// Production facade method bodies against a deliberately small framework
// model. This is not native IOKit execution or a full-facade layout test.
#include "../HostSession.hpp"
#include <cassert>
#include <future>
#include <chrono>
#include <cstdio>

int testAllocations=0;
bool testAllocationFailure=false;
class RTUSBInterface : public IOService {
public:
    rtw::HostSession* session=nullptr;
    IOService* client=nullptr;
    IOService* pendingCloseClient=nullptr;
    bool hostOpen=false,closing=false;
    void close(IOService*,IOOptionBits=0) override;
    void handleClose(IOService*,IOOptionBits) override;
    void finishClientClose(IOService*,IOOptionBits);
    void closeSession();
    bool didTerminate(IOService*,IOOptionBits,bool*) override;
};
#ifndef RTW_FACADE_METHODS
#define RTW_FACADE_METHODS "../../DevelopmentOnly/FacadeFixture/ActualCloseMethods.inc"
#endif
#include RTW_FACADE_METHODS

static void kernelModelRejectsHandleOnlyDeferral() {
    struct HandleOnly : IOService {
        void handleClose(IOService*,IOOptionBits) override {} // Keeps open record.
    } facade;
    IOService client;facade.openOwner=&client;facade.phase3=true;
    facade.close(&client);
    // Matching-kernel close queues stop based on phase3, without rechecking
    // the open record. This catches the earlier, incorrect handle-only model.
    assert(facade.openOwner==&client && client.stopRequests==1);
}

static void callbackCloseAndProviderFinalize() {
    IOWorkLoop loop;IOUSBHostInterface host;host.loop=&loop;
    RTUSBInterface facade;IOService client,nativeProvider;
    facade.session=rtw::HostSession::create(&host);assert(facade.session->open(&facade));
    facade.client=&client;facade.openOwner=&client;facade.hostOpen=true;facade.phase3=true;
    IOUSBHostPipe pipe;IOMemoryDescriptor buffer;
    struct Context { RTUSBInterface* facade;IOService* client;IOService* provider;bool returned=false; }
        context={&facade,&client,&nativeProvider};
    client.onStopRequest=[](void* value,IOService* provider) {
        auto* c=static_cast<Context*>(value);
        assert(provider==c->facade && c->returned);
    };
    client.stopContext=&context;
    rtw::LegacyCompletion completion={&context,[](void* value,void*,IOReturn,uint32_t) {
        auto* c=static_cast<Context*>(value);
        c->facade->close(c->client,0x3456);
        c->facade->close(c->client,0x9999); // Duplicate must coalesce.
        {
            bool localDefer=false;
            assert(c->facade->didTerminate(c->provider,0x4567,&localDefer) && localDefer);
        } // The caller's bool must not escape this scope.
        testRunThreadCalls();
        assert(c->facade->openOwner==c->client && !c->client->stopRequests);
        assert(!c->facade->baseDidTerminateCalls);
        c->returned=true;
    },nullptr};
    const auto lease=facade.session->completionLease();
    assert(rtw::pipeAsync(&pipe,&client,&buffer,16,completion,0,0,nullptr,&lease)==0);
    pipe.complete();
    assert(client.stopRequests==0 && facade.pendingCloseClient==&client);
    testRunThreadCalls();
    assert(client.stopRequests==1 && facade.openOwner==nullptr && !facade.pendingCloseClient);
    assert(facade.baseDidTerminateCalls==1 && host.closes==1);
    assert(facade.references==1 && client.references==1 && nativeProvider.references==1);
    facade.session->release();facade.session=nullptr;assert(testAllocations==0);
}

static void callbackCanRecloseDuringNativeDrain() {
    IOWorkLoop loop;IOUSBHostInterface host;host.loop=&loop;
    RTUSBInterface facade;IOService client;
    facade.session=rtw::HostSession::create(&host);assert(facade.session->open(&facade));
    facade.client=&client;facade.openOwner=&client;facade.hostOpen=true;facade.phase3=true;
    struct Context { RTUSBInterface* facade;IOService* client;IOWorkLoop* loop; }
        context={&facade,&client,&loop};
    host.onClose=[](void* value) {
        auto& c=*static_cast<Context*>(value);
        // Model native commandSleep allowing a second callback thread to run.
        IOWorkLoop::held().pop_back();c.loop->gate.unlock();
        auto callback=std::async(std::launch::async,[&] {c.facade->close(c.client,0x9999);});
        assert(callback.wait_for(std::chrono::seconds(2))==std::future_status::ready);
        callback.get();c.loop->gate.lock();IOWorkLoop::held().push_back(c.loop);
    };
    host.onCloseContext=&context;
    facade.close(&client,0x3456);
    assert(facade.openOwner==&client && client.stopRequests==0);
    testRunThreadCalls();assert(client.stopRequests==1 && facade.openOwner==nullptr);
    facade.session->release();facade.session=nullptr;assert(testAllocations==0);
}

static void unopenedCloseAndNoSessionFinalize() {
    RTUSBInterface facade;IOService client,provider;
    facade.close(&client);assert(!client.stopRequests && !facade.pendingCloseClient);
    bool defer=false;assert(facade.didTerminate(&provider,0,&defer));
    assert(!defer && facade.baseDidTerminateCalls==1);
}

int main() {
    kernelModelRejectsHandleOnlyDeferral();callbackCloseAndProviderFinalize();
    callbackCanRecloseDuringNativeDrain();unopenedCloseAndNoSessionFinalize();
    std::puts("PASS: actual facade close/didTerminate bodies; kernel close model, delayed stop, copied defer state, cross-thread reclose without arbitration deadlock");
}
