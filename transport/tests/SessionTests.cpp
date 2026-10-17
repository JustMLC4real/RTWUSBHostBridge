#include "../HostSession.hpp"
#include <cassert>
#include <cstdio>
#include <future>
#include <thread>
#include <chrono>

int testAllocations=0;
bool testAllocationFailure=false;
static IOReturn count(void* context) { ++*static_cast<int*>(context);return 0; }

static void retiredSession() {
    IOWorkLoop loop;IOUSBHostInterface host;IOService client;host.loop=&loop;
    auto* old=rtw::HostSession::create(&host);assert(old);
    int submissions=0;
    assert(old->perform(count,&submissions)==kIOReturnNotOpen);
    assert(old->open(&client));assert(!old->open(&client));
    assert(old->perform(count,&submissions)==0 && submissions==1);
    struct Drain { rtw::HostSession* session;int* submissions;int calls; } drain={old,&submissions,0};
    host.onClose=[](void* context) {
        auto& d=*static_cast<Drain*>(context);++d.calls;
        // Native close can synchronously invoke callbacks. Neither submitting
        // nor reopening this identity may succeed from such a callback.
        assert(d.session->perform(count,d.submissions)==kIOReturnNotOpen);
        assert(!d.session->open(reinterpret_cast<IOService*>(0x1234)));
        d.session->close(); // Recursive close must not issue native close twice.
    };
    host.onCloseContext=&drain;
    old->close();old->close();assert(host.closes==1 && drain.calls==1);
    host.onClose=nullptr;
    auto* current=rtw::HostSession::create(&host);assert(current->open(&client));
    // Reproduce the previously unexcluded stale-control-wrapper schedule:
    // an old identity must not submit into the newly opened host session.
    assert(old->perform(count,&submissions)==kIOReturnNotOpen && submissions==1);
    assert(current->perform(count,&submissions)==0 && submissions==2);
    current->close();old->release();current->release();
    assert(host.references==1 && loop.references==1 && testAllocations==0);
}

static void submissionVersusClose() {
    IOWorkLoop loop;IOUSBHostInterface host;IOService client;host.loop=&loop;
    auto* s=rtw::HostSession::create(&host);assert(s->open(&client));
    std::promise<void> admitted,releaseSubmission,closeStarted;
    auto admittedFuture=admitted.get_future();
    auto releaseFuture=releaseSubmission.get_future();
    auto closeFuture=closeStarted.get_future();
    struct Submission { std::promise<void>* admitted;std::future<void>* release;
        IOUSBHostInterface* host;
    } args={&admitted,&releaseFuture,&host};
    auto submit=std::async(std::launch::async,[&] {
        return s->perform([](void* context) -> IOReturn {
            auto& a=*static_cast<Submission*>(context);a.admitted->set_value();
            a.release->wait();
            assert(a.host->client); // Native close must not pass admission.
            ++a.host->syncRequests;return kIOReturnSuccess;
        },&args);
    });
    admittedFuture.wait();
    auto close=std::async(std::launch::async,[&] {closeStarted.set_value();s->close();});
    closeFuture.wait();
    // If close uses a different gate, it will finish while submission is held.
    assert(close.wait_for(std::chrono::milliseconds(20))==std::future_status::timeout);
    releaseSubmission.set_value();assert(submit.get()==0);close.get();
    assert(host.syncRequests==1 && host.closes==1);
    int n=0;assert(s->perform(count,&n)==kIOReturnNotOpen && n==0);
    s->release();assert(testAllocations==0);
}

static void inlineCloseAndFailures() {
    IOWorkLoop loop;IOUSBHostInterface host;IOService client;
    assert(!rtw::HostSession::create(nullptr));
    assert(!rtw::HostSession::create(&host));
    host.loop=&loop;testAllocationFailure=true;
    assert(!rtw::HostSession::create(&host));testAllocationFailure=false;
    testThreadCallAllocationFailure()=true;
    assert(!rtw::HostSession::create(&host));testThreadCallAllocationFailure()=false;
    assert(testAllocations==0 && host.references==1 && loop.references==1);
    auto* failed=rtw::HostSession::create(&host);host.openAllowed=false;
    assert(!failed->open(&client));host.openAllowed=true;
    assert(!failed->open(&client));failed->close();failed->release();assert(host.closes==0);
    auto* s=rtw::HostSession::create(&host);assert(s->open(&client));
    assert(s->perform([](void* p) -> IOReturn {
        auto* session=static_cast<rtw::HostSession*>(p);
        session->close();
        int n=0;assert(session->perform(count,&n)==kIOReturnNotOpen && n==0);
        return 0;
    },s)==0);
    s->release();assert(host.references==1 && loop.references==1 && testAllocations==0);
    // A completion can release the last external wrapper/session reference
    // before the native submit returns. perform() must keep itself alive.
    s=rtw::HostSession::create(&host);assert(s->open(&client));
    assert(s->perform([](void* p) -> IOReturn {
        auto* session=static_cast<rtw::HostSession*>(p);
        session->close();session->release();return 0;
    },s)==0);
    assert(host.references==1 && loop.references==1 && testAllocations==0);
}
static void terminationDuringOpen() {
    IOWorkLoop loop;IOUSBHostInterface host;IOService client;host.loop=&loop;
    auto* s=rtw::HostSession::create(&host);
    host.onOpen=[](void* context) { static_cast<rtw::HostSession*>(context)->close(); };
    host.onOpenContext=s;
    assert(!s->open(&client)); // A terminated pending open cannot resurrect.
    assert(!host.client && host.opens==1 && host.closes==1);
    int n=0;assert(s->perform(count,&n)==kIOReturnNotOpen && n==0);
    s->close();assert(host.closes==1);s->release();
    assert(host.references==1 && loop.references==1 && testAllocations==0);
}
struct NoticeTarget : IOService {
    rtw::HostSession* session=nullptr;
    IOService* provider=nullptr;
    int phase=0;
    int notices=0;
};
static void notified(IOService* target,IOService* provider,void* argument) {
    auto* n=static_cast<NoticeTarget*>(target);
    assert(n->phase==2 && provider==n->provider);
    assert(argument==reinterpret_cast<void*>(0x1234));
    ++n->notices;
    // Both services stay retained for this call, even if a native callback
    // reentered termination or the notification reenters it again.
    assert(n->references==2 && provider->references>=2);
    int submissions=0;
    assert(n->session->perform(count,&submissions)==kIOReturnNotOpen);
    assert(n->session->closeAndNotify(target,provider,argument,notified)==0);
    n->session->close();
    assert(n->notices==1);
}
static void noticeDuringDrain() {
    IOWorkLoop loop;IOUSBHostInterface host;IOService client,provider;
    NoticeTarget target;host.loop=&loop;
    auto* s=rtw::HostSession::create(&host);assert(s->open(&client));
    target.session=s;target.provider=&provider;
    host.onClose=[](void* context) {
        auto* t=static_cast<NoticeTarget*>(context);t->phase=1;
        assert(t->session->closeAndNotify(t,t->provider,
            reinterpret_cast<void*>(0x1234),notified)==0);
        assert(t->notices==0); // The native drain has not returned yet.
        // A duplicate may carry another argument; preserve the first event.
        assert(t->session->closeAndNotify(t,t->provider,nullptr,notified)==0);
        assert(t->notices==0);t->phase=2;
    };
    host.onCloseContext=&target;
    s->close();assert(target.notices==0 && host.closes==1);
    testRunThreadCalls();assert(target.notices==1);
    assert(target.references==1 && provider.references==1);
    assert(s->closeAndNotify(&target,&provider,nullptr,notified)==0);
    assert(target.notices==1);s->release();assert(testAllocations==0);
}
static void noticeDuringOpening() {
    IOWorkLoop loop;IOUSBHostInterface host;IOService client,provider;
    NoticeTarget target;host.loop=&loop;
    auto* s=rtw::HostSession::create(&host);
    target.session=s;target.provider=&provider;
    host.onOpen=[](void* context) {
        auto* t=static_cast<NoticeTarget*>(context);t->phase=1;
        assert(t->session->closeAndNotify(t,t->provider,
            reinterpret_cast<void*>(0x1234),notified)==0);
        assert(t->notices==0); // Native open has not returned or rolled back.
    };
    host.onOpenContext=&target;
    host.onClose=[](void* context) {
        auto* t=static_cast<NoticeTarget*>(context);
        assert(t->notices==0);t->phase=2;
    };
    host.onCloseContext=&target;
    assert(!s->open(&client));
    assert(target.notices==0 && host.opens==1 && host.closes==1 && !host.client);
    testRunThreadCalls();assert(target.notices==1);
    assert(target.references==1 && provider.references==1);
    s->release();assert(testAllocations==0);
}
static void freshAndNormalNotice() {
    IOWorkLoop loop;IOUSBHostInterface host;IOService client,provider;
    host.loop=&loop;NoticeTarget target;
    auto* fresh=rtw::HostSession::create(&host);
    target.session=fresh;target.provider=&provider;target.phase=2;
    assert(fresh->closeAndNotify(nullptr,&provider,nullptr,notified)==kIOReturnBadArgument);
    assert(fresh->closeAndNotify(&target,&provider,nullptr,nullptr)==kIOReturnBadArgument);
    assert(fresh->closeAndNotify(&target,&provider,reinterpret_cast<void*>(0x1234),notified)==0);
    assert(target.notices==0);testRunThreadCalls();
    assert(target.notices==1 && host.closes==0);fresh->release();
    auto* open=rtw::HostSession::create(&host);assert(open->open(&client));
    target.session=open;target.notices=0;target.phase=1;
    host.onClose=[](void* context) {
        auto* t=static_cast<NoticeTarget*>(context);assert(t->notices==0);t->phase=2;
    };
    host.onCloseContext=&target;
    assert(open->closeAndNotify(&target,&provider,reinterpret_cast<void*>(0x1234),notified)==0);
    assert(target.notices==0);testRunThreadCalls();
    assert(target.notices==1 && host.closes==1);
    assert(target.references==1 && provider.references==1);
    open->release();assert(testAllocations==0);
}
static void noticeWhileDrainDropsGate() {
    IOWorkLoop loop;IOUSBHostInterface host;IOService client,provider;
    NoticeTarget target;host.loop=&loop;
    auto* s=rtw::HostSession::create(&host);assert(s->open(&client));
    target.session=s;target.provider=&provider;
    struct Context { NoticeTarget* target;IOWorkLoop* loop; } context={&target,&loop};
    host.onClose=[](void* pointer) {
        auto& c=*static_cast<Context*>(pointer);c.target->phase=1;
        // Model one commandSleep interleaving: native close temporarily drops
        // this gate, so another thread can request termination before close
        // has finished. This is not a model of the complete native scheduler.
        c.loop->gate.unlock();
        auto requester=std::async(std::launch::async,[&] {
            return c.target->session->closeAndNotify(c.target,c.target->provider,
                reinterpret_cast<void*>(0x1234),notified);
        });
        assert(requester.get()==0);
        c.loop->gate.lock();
        assert(c.target->notices==0);c.target->phase=2;
    };
    host.onCloseContext=&context;
    s->close();assert(target.notices==0 && host.closes==1);
    testRunThreadCalls();assert(target.notices==1);
    assert(target.references==1 && provider.references==1);
    s->release();assert(testAllocations==0);
}
static void queuedNoticeOwnsSession() {
    IOWorkLoop loop;IOUSBHostInterface host;IOService client,provider;
    NoticeTarget target;host.loop=&loop;
    auto* s=rtw::HostSession::create(&host);assert(s->open(&client));
    target.session=s;target.provider=&provider;target.phase=2;
    assert(s->closeAndNotify(&target,&provider,reinterpret_cast<void*>(0x1234),notified)==0);
    s->release(); // The queued worker is now the only session owner.
    assert(target.notices==0 && target.references==2 && provider.references==2);
    testRunThreadCalls(); // Includes freeing its own call in final release.
    assert(target.notices==1 && target.references==1 && provider.references==1);
    assert(host.references==1 && loop.references==1 && testAllocations==0);
}
static void notificationDoesNotInvertArbitration() {
    IOWorkLoop loop;IOUSBHostInterface host;IOService client,provider;
    host.loop=&loop;auto* s=rtw::HostSession::create(&host);assert(s->open(&client));
    std::mutex arbitration;
    struct Target : IOService { std::mutex* arbitration;rtw::HostSession* session;int notices=0; } target;
    target.arbitration=&arbitration;target.session=s;
    std::promise<void> gateHeld,arbitrationHeld,allowClose,queued;
    auto gateFuture=gateHeld.get_future();auto arbFuture=arbitrationHeld.get_future();
    auto closeFuture=allowClose.get_future();auto queuedFuture=queued.get_future();
    auto closer=std::async(std::launch::async,[&] {
        std::lock_guard<std::recursive_mutex> gate(loop.gate);
        gateHeld.set_value();closeFuture.wait();
        assert(s->closeAndNotify(&target,&provider,nullptr,
            [](IOService* service,IOService*,void*) {
                auto* t=static_cast<Target*>(service);
                std::lock_guard<std::mutex> arbitration(*t->arbitration);
                t->session->close();++t->notices;
            })==0);
        queued.set_value();
    });
    gateFuture.wait();
    auto opener=std::async(std::launch::async,[&] {
        std::lock_guard<std::mutex> lock(arbitration);
        arbitrationHeld.set_value();
        int count=0;return s->perform(::count,&count);
    });
    arbFuture.wait();allowClose.set_value();
    auto worker=std::async(std::launch::async,[&] {
        queuedFuture.wait();testRunThreadCalls();
    });
    // The previous inline notification waits for arbitration while the opener
    // holds it and waits for the USB gate. These bounded waits catch that cycle.
    assert(closer.wait_for(std::chrono::seconds(2))==std::future_status::ready);
    closer.get();
    assert(opener.wait_for(std::chrono::seconds(2))==std::future_status::ready);
    assert(opener.get()==kIOReturnNotOpen);
    assert(worker.wait_for(std::chrono::seconds(2))==std::future_status::ready);
    worker.get();assert(target.notices==1);
    assert(target.references==1 && provider.references==1);
    s->release();assert(testAllocations==0);
}
struct CompletionNotice : IOService {
    rtw::HostSession* session;
    IOService* provider;
    bool callbackActive=false;
    bool submitting=false;
    int callbacks=0;
    int expected=0;
    int notices=0;
};
static void completionNotice(IOService* target,IOService* provider,void*) {
    auto* t=static_cast<CompletionNotice*>(target);
    assert(provider==t->provider && !t->callbackActive && !t->submitting);
    assert(t->callbacks==t->expected);
    ++t->notices;
}
static void requestNotice(CompletionNotice* t) {
    assert(t->session->closeAndNotify(t,t->provider,nullptr,completionNotice)==0);
}
static void callbackReturnBarrier() {
    // Exercise all three facade asynchronous entry points, including the
    // schedule where native close returns before a queued callback, and close
    // from the callback itself. Running the fake worker early must do nothing.
    for (int mode=0;mode<3;++mode) for (int scenario=0;scenario<4;++scenario) {
        IOWorkLoop loop;IOUSBHostInterface host;host.loop=&loop;
        IOService client,provider;OSObject owner;IOMemoryDescriptor buffer;
        IOUSBHostPipe pipe;
        auto* s=rtw::HostSession::create(&host);assert(s->open(&client));
        CompletionNotice target;target.session=s;target.provider=&provider;
        target.expected=scenario<2?1:0;
        auto* native=mode==0?static_cast<FakeSubmission*>(&pipe):static_cast<FakeSubmission*>(&host);
        native->immediate=scenario==1;
        if (scenario==2) native->submitStatus=kIOReturnNotReady;
        struct Arguments { CompletionNotice* target;OSObject* owner;
            IOUSBHostInterface* host;IOUSBHostPipe* pipe;IOMemoryDescriptor* buffer;
            int mode,scenario;
        } args={&target,&owner,&host,&pipe,&buffer,mode,scenario};
        const auto result=s->perform([](void* context) -> IOReturn {
            auto& a=*static_cast<Arguments*>(context);auto* t=a.target;
            const auto lease=t->session->completionLease();
            rtw::LegacyCompletion completion={t,[](void* target,void*,IOReturn,uint32_t) {
                auto* t=static_cast<CompletionNotice*>(target);
                t->callbackActive=true;
                requestNotice(t); // Native close does not wait for itself.
                testRunThreadCalls();assert(t->notices==0);
                ++t->callbacks;t->callbackActive=false;
            },nullptr};
            // OSObject ownership and legacy callback target are separate in
            // this test; both remain live until the completion returns.
            rtw::LegacyRequest request={};request.length=16;request.data=a.buffer;
            t->submitting=true;
            if (a.scenario==3) testAllocationFailure=true;
            IOReturn result;
            if (a.mode==0) result=rtw::pipeAsync(a.pipe,a.owner,a.buffer,16,completion,0,0,nullptr,&lease);
            else if (a.mode==1) result=rtw::controlAsync(a.host,a.owner,request,a.buffer,completion,0,nullptr,&lease);
            else result=rtw::controlRawAsync(a.host,a.owner,request,completion,0,nullptr,&lease);
            testAllocationFailure=false;
            if (a.scenario>=2) requestNotice(t);
            testRunThreadCalls();assert(t->notices==0);
            t->submitting=false;
            return result;
        },&args);
        if (scenario<2) assert(result==0);
        else assert(result==(scenario==2?kIOReturnNotReady:kIOReturnNoMemory));
        if (scenario==0) {
            requestNotice(&target);
            testRunThreadCalls();assert(target.notices==0);
            // The pending transfer must keep the retired session alive even
            // after its external reference goes away.
            s->release();s=nullptr;
            native->complete();
        }
        testRunThreadCalls();assert(target.notices==1);
        if (s) s->release();
        assert(owner.references==1 && buffer.references==1 && pipe.references==1);
        assert(host.references==1 && loop.references==1 && target.references==1 && provider.references==1);
        assert(testAllocations==0);
    }
}

static void multipleCompletionBarrier() {
    IOWorkLoop loop;IOUSBHostInterface host;host.loop=&loop;
    IOService client,provider;OSObject owner;IOMemoryDescriptor buffer;
    IOUSBHostPipe first,second;
    auto* s=rtw::HostSession::create(&host);assert(s->open(&client));
    CompletionNotice target;target.session=s;target.provider=&provider;target.expected=2;
    const auto lease=s->completionLease();
    rtw::LegacyCompletion completion={&target,[](void* value,void*,IOReturn,uint32_t) {
        auto* t=static_cast<CompletionNotice*>(value);++t->callbacks;
    },nullptr};
    assert(rtw::pipeAsync(&first,&owner,&buffer,16,completion,0,0,nullptr,&lease)==0);
    assert(rtw::pipeAsync(&second,&owner,&buffer,16,completion,0,0,nullptr,&lease)==0);
    requestNotice(&target);testRunThreadCalls();assert(target.notices==0);
    first.complete();testRunThreadCalls();assert(target.notices==0);
    second.complete();testRunThreadCalls();assert(target.notices==1);
    s->release();assert(testAllocations==0);
}

static void callbackOnAnotherThread() {
    IOWorkLoop loop;IOUSBHostInterface host;host.loop=&loop;
    IOService client,provider;OSObject owner;IOMemoryDescriptor buffer;IOUSBHostPipe pipe;
    auto* s=rtw::HostSession::create(&host);assert(s->open(&client));
    CompletionNotice target;target.session=s;target.provider=&provider;target.expected=1;
    std::promise<void> entered,resume;auto enteredFuture=entered.get_future();auto resumeFuture=resume.get_future();
    struct Blocked { CompletionNotice* target;std::promise<void>* entered;std::future<void>* resume; }
        blocked={&target,&entered,&resumeFuture};
    rtw::LegacyCompletion completion={&target,[](void*,void* value,IOReturn,uint32_t) {
        auto& b=*static_cast<Blocked*>(value);b.target->callbackActive=true;
        b.entered->set_value();b.resume->wait();
        ++b.target->callbacks;b.target->callbackActive=false;
    },&blocked};
    const auto lease=s->completionLease();
    assert(rtw::pipeAsync(&pipe,&owner,&buffer,16,completion,0,0,nullptr,&lease)==0);
    auto callback=std::async(std::launch::async,[&] { pipe.complete(); });
    enteredFuture.wait();
    requestNotice(&target);testRunThreadCalls();assert(target.notices==0);
    resume.set_value();callback.get();testRunThreadCalls();assert(target.notices==1);
    s->release();assert(testAllocations==0);
}

static void quiesceBarrier() {
    IOWorkLoop loop;IOUSBHostInterface host;host.loop=&loop;IOService client;
    for (int failure=0;failure<2;++failure) {
        testGateAllocationFailure()=failure==0;testGateAttachmentFailure()=failure==1;
        assert(!rtw::HostSession::create(&host));
        testGateAllocationFailure()=false;testGateAttachmentFailure()=false;
        assert(host.references==1 && loop.references==1 && testAllocations==0);
    }
    auto* s=rtw::HostSession::create(&host);assert(s->open(&client));
    assert(s->quiesceForStop(0)==kIOReturnBadArgument);
    loop.worker=current_thread();assert(s->quiesceForStop()==kIOReturnNotReady);loop.worker=nullptr;
    assert(s->perform([](void* context) -> IOReturn {
        assert(static_cast<rtw::HostSession*>(context)->quiesceForStop()==kIOReturnNotReady);
        return kIOReturnSuccess;
    },s)==0);
    assert(host.closes==0);
    OSObject owner;IOMemoryDescriptor buffer;IOUSBHostPipe pipe;
    const auto lease=s->completionLease();
    rtw::LegacyCompletion callback={s,[](void* value,void*,IOReturn,uint32_t) {
        auto* session=static_cast<rtw::HostSession*>(value);
        // Fake completion is deliberately outside the gate/workloop thread:
        // only the explicit callback-frame identity prevents self-wait here.
        assert(session->quiesceForStop()==kIOReturnNotReady);
    },nullptr};
    assert(rtw::pipeAsync(&pipe,&owner,&buffer,16,callback,0,0,nullptr,&lease)==0);
    pipe.complete();assert(host.closes==0);
    assert(s->quiesceForStop()==0 && host.closes==1);
    assert(s->quiesceForStop()==0 && host.closes==1);
    int countCalls=0;assert(s->perform(count,&countCalls)==kIOReturnNotOpen);
    s->release();assert(testAllocations==0);
}

static void quiesceWaitsForCallback() {
    IOWorkLoop loop;IOUSBHostInterface host;host.loop=&loop;IOService client;
    OSObject owner;IOMemoryDescriptor buffer;IOUSBHostPipe pipe;
    auto* s=rtw::HostSession::create(&host);assert(s->open(&client));
    const auto lease=s->completionLease();
    rtw::LegacyCompletion callback={s,[](void* value,void*,IOReturn,uint32_t) {
        auto* session=static_cast<rtw::HostSession*>(value);int submissions=0;
        assert(session->perform(count,&submissions)==kIOReturnNotOpen && !submissions);
        assert(session->quiesceForStop()==kIOReturnNotReady);
    },nullptr};
    assert(rtw::pipeAsync(&pipe,&owner,&buffer,16,callback,0,0,nullptr,&lease)==0);
    std::promise<void> closed;auto closedFuture=closed.get_future();
    host.onClose=[](void* value) { static_cast<std::promise<void>*>(value)->set_value(); };
    host.onCloseContext=&closed;
    auto wait=std::async(std::launch::async,[&] { return s->quiesceForStop(); });
    closedFuture.wait();
    assert(wait.wait_for(std::chrono::milliseconds(20))==std::future_status::timeout);
    pipe.complete();
    assert(wait.wait_for(std::chrono::seconds(2))==std::future_status::ready && wait.get()==0);
    s->release();assert(testAllocations==0);
}

static void quiesceWaitsForAction() {
    IOWorkLoop loop;IOUSBHostInterface host;host.loop=&loop;IOService client;
    auto* s=rtw::HostSession::create(&host);assert(s->open(&client));
    std::promise<void> dropped,resume,closed;
    auto droppedFuture=dropped.get_future();auto resumeFuture=resume.get_future();auto closedFuture=closed.get_future();
    struct Dropped { rtw::HostSession* session;IOWorkLoop* loop;
        std::promise<void>* dropped;std::future<void>* resume;
    } context={s,&loop,&dropped,&resumeFuture};
    auto active=std::async(std::launch::async,[&] {
        return s->perform([](void* value) -> IOReturn {
            auto& c=*static_cast<Dropped*>(value);
            // Model a native call temporarily releasing its gate. The active
            // action frame must still reject quiescing on that same thread.
            IOWorkLoop::held().pop_back();c.loop->gate.unlock();
            assert(c.session->quiesceForStop()==kIOReturnNotReady);
            c.dropped->set_value();c.resume->wait();
            c.loop->gate.lock();IOWorkLoop::held().push_back(c.loop);
            return kIOReturnSuccess;
        },&context);
    });
    droppedFuture.wait();
    host.onClose=[](void* value) { static_cast<std::promise<void>*>(value)->set_value(); };
    host.onCloseContext=&closed;
    auto wait=std::async(std::launch::async,[&] { return s->quiesceForStop(); });
    closedFuture.wait();
    assert(wait.wait_for(std::chrono::milliseconds(20))==std::future_status::timeout);
    resume.set_value();assert(active.get()==0);
    assert(wait.wait_for(std::chrono::seconds(2))==std::future_status::ready && wait.get()==0);
    s->release();assert(testAllocations==0);
}

static void overlappingCallbackFrames() {
    IOWorkLoop loop;IOUSBHostInterface host;host.loop=&loop;IOService client;
    OSObject owner;IOMemoryDescriptor buffer;IOUSBHostPipe first,second;
    auto* s=rtw::HostSession::create(&host);assert(s->open(&client));
    std::promise<void> firstEntered,secondEntered,firstResume,secondResume;
    auto fa=firstEntered.get_future(),fb=secondEntered.get_future();
    auto ra=firstResume.get_future(),rb=secondResume.get_future();
    struct Context { rtw::HostSession* session;std::promise<void>* entered;std::future<void>* resume; };
    Context a={s,&firstEntered,&ra},b={s,&secondEntered,&rb};
    auto action=[](void*,void* value,IOReturn,uint32_t) {
        auto& c=*static_cast<Context*>(value);c.entered->set_value();c.resume->wait();
        assert(c.session->quiesceForStop()==kIOReturnNotReady);
    };
    const auto lease=s->completionLease();
    rtw::LegacyCompletion ca={s,action,&a},cb={s,action,&b};
    assert(rtw::pipeAsync(&first,&owner,&buffer,16,ca,0,0,nullptr,&lease)==0);
    assert(rtw::pipeAsync(&second,&owner,&buffer,16,cb,0,0,nullptr,&lease)==0);
    auto one=std::async(std::launch::async,[&] {first.complete();});fa.wait();
    auto two=std::async(std::launch::async,[&] {second.complete();});fb.wait();
    // Finish the older frame first. The newer thread must remain registered
    // or its self-quiesce would wait forever on its own pending completion.
    firstResume.set_value();assert(one.wait_for(std::chrono::seconds(2))==std::future_status::ready);one.get();
    secondResume.set_value();assert(two.wait_for(std::chrono::seconds(2))==std::future_status::ready);two.get();
    assert(s->quiesceForStop()==0);s->release();assert(testAllocations==0);
}

static void quiesceTimeoutKeepsPendingAlive() {
    IOWorkLoop loop;IOUSBHostInterface host;host.loop=&loop;IOService client;
    OSObject owner;IOMemoryDescriptor buffer;IOUSBHostPipe pipe;
    auto* s=rtw::HostSession::create(&host);assert(s->open(&client));
    int callbacks=0;
    rtw::LegacyCompletion completion={nullptr,[](void*,void* value,IOReturn,uint32_t) {
        ++*static_cast<int*>(value);
    },&callbacks};
    const auto lease=s->completionLease();
    assert(rtw::pipeAsync(&pipe,&owner,&buffer,16,completion,0,0,nullptr,&lease)==0);
    assert(s->quiesceForStop(5)==kIOReturnTimeout);
    int submissions=0;assert(s->perform(count,&submissions)==kIOReturnNotOpen && !submissions);
    assert(owner.references==2 && buffer.references==2 && callbacks==0);
    s->release(); // Timeout must not discard the pending callback's ownership.
    pipe.complete();assert(callbacks==1 && owner.references==1 && buffer.references==1);
    assert(testAllocations==0);
}

struct FinalizeTarget : IOService {
    rtw::HostSession* session;
    IOService* provider;
    IOWorkLoop* loop;
    IOService* client=nullptr;
    bool callbackActive=false;
    bool requireNotice=false;
    bool noticeDone=false;
    bool requireClose=false;
    bool openRecord=true;
    int closes=0;
    int notices=0;
    int finalizations=0;
};
static void finalized(IOService* target,IOService* provider,void* options) {
    auto* t=static_cast<FinalizeTarget*>(target);
    assert(provider==t->provider && options==reinterpret_cast<void*>(0x4567));
    assert(!t->callbackActive && !t->loop->inGate());
    assert(!t->requireNotice || t->noticeDone);
    assert(!t->requireClose || !t->openRecord);
    ++t->finalizations;
    // A duplicate callback-phase request must not deliver twice.
    assert(t->session->closeAndFinalize(t,provider,nullptr,finalized)==0);
}
static void clientClosed(IOService* target,IOService* client,void* options) {
    auto* t=static_cast<FinalizeTarget*>(target);
    assert(client==t->client && options==reinterpret_cast<void*>(0x3456));
    assert(!t->callbackActive && !t->loop->inGate());
    assert(!t->requireNotice || t->noticeDone);
    t->openRecord=false;++t->closes;
    assert(t->session->closeAndReleaseClient(t,client,nullptr,clientClosed)==0);
    testRunThreadCalls();assert(!t->requireClose || !t->finalizations);
}
static void noticeBeforeFinalize(IOService* target,IOService* provider,void*) {
    auto* t=static_cast<FinalizeTarget*>(target);
    assert(provider==t->provider && !t->callbackActive && !t->loop->inGate());
    ++t->notices;
    if (t->requireClose)
        assert(t->session->closeAndReleaseClient(t,t->client,reinterpret_cast<void*>(0x3456),clientClosed)==0);
    assert(t->session->closeAndFinalize(t,provider,nullptr,finalized)==0);
    // Scheduling is insufficient: finalization must wait until this notice
    // handler RETURNS, even if another worker is available immediately.
    testRunThreadCalls();assert(!t->finalizations);
    t->noticeDone=true;
}
static void deferredDidTerminate() {
    IOWorkLoop loop;IOUSBHostInterface host;host.loop=&loop;IOService client,provider;
    for (int withNotice=0;withNotice<2;++withNotice) {
        auto* s=rtw::HostSession::create(&host);assert(s->open(&client));
        FinalizeTarget target;target.session=s;target.provider=&provider;target.loop=&loop;
        target.requireNotice=withNotice;
        OSObject owner;IOMemoryDescriptor buffer;IOUSBHostPipe pipe;
        const auto lease=s->completionLease();
        rtw::LegacyCompletion callback={&target,[](void* value,void*,IOReturn,uint32_t) {
            auto* t=static_cast<FinalizeTarget*>(value);t->callbackActive=true;
            if (t->requireNotice)
                assert(t->session->closeAndNotify(t,t->provider,nullptr,noticeBeforeFinalize)==0);
            assert(t->session->closeAndFinalize(t,t->provider,reinterpret_cast<void*>(0x4567),finalized)==0);
            assert(t->session->closeAndFinalize(t,t->provider,nullptr,finalized)==0);
            testRunThreadCalls();assert(!t->finalizations && !t->notices);
            t->callbackActive=false;
        },nullptr};
        assert(rtw::pipeAsync(&pipe,&owner,&buffer,16,callback,0,0,nullptr,&lease)==0);
        pipe.complete();
        s->release(); // Pending workers now own the final session references.
        testRunThreadCalls();
        assert(target.finalizations==1 && target.notices==withNotice);
        assert(target.references==1 && provider.references==1);
        assert(owner.references==1 && buffer.references==1 && testAllocations==0);
    }
    auto* fresh=rtw::HostSession::create(&host);
    FinalizeTarget target;target.session=fresh;target.provider=&provider;target.loop=&loop;
    assert(fresh->closeAndFinalize(nullptr,&provider,nullptr,finalized)==kIOReturnBadArgument);
    assert(fresh->closeAndFinalize(&target,nullptr,nullptr,finalized)==kIOReturnBadArgument);
    assert(fresh->closeAndFinalize(&target,&provider,nullptr,nullptr)==kIOReturnBadArgument);
    assert(fresh->closeAndFinalize(&target,&provider,reinterpret_cast<void*>(0x4567),finalized)==0);
    fresh->release();testRunThreadCalls();assert(target.finalizations==1 && testAllocations==0);
}

static void finalizeAfterOpenRollback() {
    IOWorkLoop loop;IOUSBHostInterface host;host.loop=&loop;IOService client,provider;
    auto* s=rtw::HostSession::create(&host);
    FinalizeTarget target;target.session=s;target.provider=&provider;target.loop=&loop;
    host.onOpen=[](void* value) {
        auto* t=static_cast<FinalizeTarget*>(value);t->callbackActive=true;
        assert(t->session->closeAndFinalize(t,t->provider,reinterpret_cast<void*>(0x4567),finalized)==0);
        testRunThreadCalls();assert(!t->finalizations);
        t->callbackActive=false;
    };
    host.onOpenContext=&target;
    assert(!s->open(&client) && host.closes==1);
    s->release();testRunThreadCalls();assert(target.finalizations==1 && testAllocations==0);
}

static void clientOpenRecordDefersStop() {
    for (int notice=0;notice<2;++notice) {
        IOWorkLoop loop;IOUSBHostInterface host;host.loop=&loop;IOService client,provider;
        auto* s=rtw::HostSession::create(&host);assert(s->open(&client));
        FinalizeTarget target;target.session=s;target.provider=&provider;target.loop=&loop;
        target.client=&client;target.requireClose=true;target.requireNotice=notice;
        OSObject owner;IOMemoryDescriptor buffer;IOUSBHostPipe pipe;const auto lease=s->completionLease();
        rtw::LegacyCompletion completion={&target,[](void* value,void*,IOReturn,uint32_t) {
            auto* t=static_cast<FinalizeTarget*>(value);t->callbackActive=true;
            // Register finalization first while a callback is still active.
            // Notice handling may register client-close later; phase priority
            // must still close the client before finalizing the provider.
            assert(t->session->closeAndFinalize(t,t->provider,reinterpret_cast<void*>(0x4567),finalized)==0);
            if (t->requireNotice)
                assert(t->session->closeAndNotify(t,t->provider,nullptr,noticeBeforeFinalize)==0);
            else assert(t->session->closeAndReleaseClient(t,t->client,reinterpret_cast<void*>(0x3456),clientClosed)==0);
            testRunThreadCalls();assert(t->openRecord && !t->finalizations && !t->closes);
            t->callbackActive=false;
        },nullptr};
        assert(rtw::pipeAsync(&pipe,&owner,&buffer,16,completion,0,0,nullptr,&lease)==0);
        pipe.complete();assert(target.openRecord);
        s->release();testRunThreadCalls();
        assert(!target.openRecord && target.closes==1 && target.finalizations==1);
        assert(client.references==1 && provider.references==1 && target.references==1 && testAllocations==0);
    }
    IOWorkLoop loop;IOUSBHostInterface host;host.loop=&loop;IOService client,provider;
    auto* s=rtw::HostSession::create(&host);
    FinalizeTarget target;target.session=s;target.provider=&provider;target.loop=&loop;target.client=&client;
    assert(s->closeAndReleaseClient(&target,nullptr,nullptr,clientClosed)==kIOReturnBadArgument);
    assert(s->closeAndFinalize(&target,&provider,reinterpret_cast<void*>(0x4567),finalized)==0);
    testRunThreadCalls();assert(target.finalizations==1);
    // Provider finalization does not imply a late client close is invalid.
    assert(s->closeAndReleaseClient(&target,&client,reinterpret_cast<void*>(0x3456),clientClosed)==0);
    testRunThreadCalls();assert(target.closes==1 && !target.openRecord);
    s->release();assert(testAllocations==0);
}

int main() {
    clientOpenRecordDefersStop();
    deferredDidTerminate();finalizeAfterOpenRollback();
    quiesceTimeoutKeepsPendingAlive();
    overlappingCallbackFrames();
    quiesceBarrier();quiesceWaitsForCallback();quiesceWaitsForAction();
    callbackReturnBarrier();multipleCompletionBarrier();callbackOnAnotherThread();
    retiredSession();submissionVersusClose();inlineCloseAndFailures();terminationDuringOpen();
    noticeDuringDrain();noticeDuringOpening();freshAndNormalNotice();noticeWhileDrainDropsGate();
    queuedNoticeOwnsSession();notificationDoesNotInvertArbitration();
    std::puts("PASS: stale session rejection, concurrent admission/close, deferred termination, queued lifetime, arbitration/gate inversion regression");
}
