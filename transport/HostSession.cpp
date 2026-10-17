#include "HostSession.hpp"

namespace rtw {
HostSession* HostSession::create(IOUSBHostInterface* host) {
    auto* loop=host?host->getWorkLoop():nullptr;
    if (!loop) return nullptr;
    auto* s=static_cast<HostSession*>(IOMalloc(sizeof(HostSession)));
    if (!s) return nullptr;
    s->references=1;s->host=host;s->loop=loop;s->client=nullptr;s->state=Fresh;
    s->opening=false;s->draining=false;s->workerScheduled=false;
    s->activeActions=0;s->pendingCompletions=0;
    s->waiters=0;s->callbackFrames=nullptr;s->actionFrames=nullptr;
    for (auto& event:s->events) event={nullptr,nullptr,nullptr,nullptr,Empty};
    s->workerCall=thread_call_allocate(dispatchThread,s);
    if (!s->workerCall) { IOFree(s,sizeof(*s));return nullptr; }
    s->drainGate=IOCommandGate::commandGate(host);
    if (!s->drainGate || loop->addEventSource(s->drainGate)!=kIOReturnSuccess) {
        if (s->drainGate) s->drainGate->release();
        thread_call_free(s->workerCall);
        IOFree(s,sizeof(*s));return nullptr;
    }
    host->retain();loop->retain();
    return s;
}
void HostSession::retain() { __atomic_add_fetch(&references,1,__ATOMIC_RELAXED); }
void HostSession::release() {
    if (__atomic_sub_fetch(&references,1,__ATOMIC_ACQ_REL)) return;
    // A queued notification owns a session reference until its callback ends.
    // The kernel API explicitly permits freeing a call from its own callback.
    thread_call_free(workerCall);
    loop->removeEventSource(drainGate);drainGate->release();
    host->release();loop->release();IOFree(this,sizeof(HostSession));
}
bool HostSession::open(IOService* forClient) {
    if (!forClient) return false;
    struct Arguments { HostSession* session; IOService* client; } args={this,forClient};
    retain();
    const auto result=loop->runAction([](OSObject*,void* arg,void*,void*,void*) -> IOReturn {
        auto& a=*static_cast<Arguments*>(arg);auto* s=a.session;
        if (s->state!=Fresh) return kIOReturnNotOpen;
        s->state=Opening;s->opening=true;
        if (!s->host->open(a.client)) {
            s->state=Retired;s->opening=false;s->notifyIfQuiescent();
            return kIOReturnNotOpen;
        }
        // A native open may sleep/drop the gate or reenter termination. A
        // close during Opening must never be undone when native open returns.
        if (s->state!=Opening) {
            s->draining=true;s->host->close(a.client);s->draining=false;
            s->opening=false;s->notifyIfQuiescent();return kIOReturnNotOpen;
        }
        s->opening=false;s->client=a.client;s->state=Open;return kIOReturnSuccess;
    },host,&args);
    release();
    return result==kIOReturnSuccess;
}
void HostSession::close() {
    retain();
    loop->runAction([](OSObject*,void* arg,void*,void*,void*) -> IOReturn {
        auto* s=static_cast<HostSession*>(arg);
        s->closeGated();
        return kIOReturnSuccess;
    },host,this);
    release();
}
void HostSession::closeGated() {
    const bool wasOpen=state==Open;
    auto* oldClient=client;
    state=Retired;client=nullptr;
    // Mark draining before native close can reenter closeAndNotify(). A
    // recursive close must not deliver its notification inside that callback.
    if (wasOpen) {
        draining=true;host->close(oldClient);draining=false;
    }
    notifyIfQuiescent();
}
void HostSession::notifyIfQuiescent() {
    if (waiters) drainGate->commandWakeup(&pendingCompletions,false);
    if (opening || draining || activeActions || pendingCompletions || workerScheduled) return;
    for (const auto& event:events) if (event.state==Pending) {
        workerScheduled=true;retain();thread_call_enter(workerCall);break;
    }
}
void HostSession::dispatchThread(thread_call_param_t context,thread_call_param_t) {
    auto* s=static_cast<HostSession*>(context);
    for (;;) {
        struct Selection { HostSession* session;DeferredAction* event; } selected={s,nullptr};
        s->loop->runAction([](OSObject*,void* arg,void*,void*,void*) -> IOReturn {
            auto& a=*static_cast<Selection*>(arg);auto* s=a.session;
            if (!s->opening && !s->draining && !s->activeActions && !s->pendingCompletions) {
                // One worker serializes notice -> client close -> provider
                // finalization. A notice handler may enqueue the later phases.
                for (auto& event:s->events) if (event.state==Pending) {
                    event.state=Running;a.event=&event;return kIOReturnSuccess;
                }
            }
            s->workerScheduled=false;return kIOReturnSuccess;
        },s->host,&selected);
        if (!selected.event) break;
        auto* event=selected.event;
        auto* target=event->target;auto* provider=event->provider;
        event->action(target,provider,event->argument); // No USB gate held.
        s->loop->runAction([](OSObject*,void* arg,void*,void*,void*) -> IOReturn {
            auto* event=static_cast<DeferredAction*>(arg);event->state=Finished;
            event->target=nullptr;event->provider=nullptr;return kIOReturnSuccess;
        },s->host,event);
        if (provider) provider->release();
        target->release();
    }
    s->release();
}
IOReturn HostSession::closeAndQueue(EventKind kind,IOService* target,IOService* provider,
                                  void* argument,ClosedAction action) {
    if (!target || !action || (kind!=TerminationNotice && !provider)) return kIOReturnBadArgument;
    struct Arguments { HostSession* session;EventKind kind;IOService* target;IOService* provider;
        void* argument;ClosedAction action;
    } args={this,kind,target,provider,argument,action};
    retain();
    const auto result=loop->runAction([](OSObject*,void* arg,void*,void*,void*) -> IOReturn {
        auto& a=*static_cast<Arguments*>(arg);auto* s=a.session;auto& event=s->events[a.kind];
        if (event.state==Empty) {
            a.target->retain();if (a.provider) a.provider->retain();
            event={a.action,a.target,a.provider,a.argument,Pending};
        }
        s->closeGated();return kIOReturnSuccess;
    },host,&args);
    release();return result;
}
IOReturn HostSession::closeAndNotify(IOService* target,IOService* provider,
                                   void* argument,ClosedAction action) {
    return closeAndQueue(TerminationNotice,target,provider,argument,action);
}
IOReturn HostSession::closeAndFinalize(IOService* target,IOService* provider,
                                     void* options,ClosedAction action) {
    return closeAndQueue(ProviderFinalization,target,provider,options,action);
}
IOReturn HostSession::closeAndReleaseClient(IOService* target,IOService* client,
                                          void* options,ClosedAction action) {
    return closeAndQueue(ClientClose,target,client,options,action);
}
IOReturn HostSession::perform(Action action,void* context) {
    if (!action) return kIOReturnBadArgument;
    struct Arguments { HostSession* session; Action action; void* context; } args={this,action,context};
    retain(); // An inline completion can release the caller's pipe wrapper.
    const auto result=loop->runAction([](OSObject*,void* arg,void*,void*,void*) -> IOReturn {
        auto& a=*static_cast<Arguments*>(arg);
        if (a.session->state!=Open) return kIOReturnNotOpen;
        // The native call acquires this same recursive workloop gate. There
        // is no admission gap and no additional mutex across native callbacks.
        ++a.session->activeActions;
        CompletionFrame frame={current_thread(),a.session->actionFrames};
        a.session->actionFrames=&frame;
        const auto result=a.action(a.context);
        // Other threads can run while native commandSleep drops the gate.
        // Remove by identity, not by assuming global LIFO completion order.
        auto** link=&a.session->actionFrames;
        while (*link && *link!=&frame) link=&(*link)->next;
        if (*link) *link=frame.next;
        --a.session->activeActions;
        a.session->notifyIfQuiescent();
        return result;
    },host,&args);
    release();return result;
}
CompletionLease HostSession::completionLease() {
    return {this, [](void* context) { static_cast<HostSession*>(context)->beginCompletion(); },
                  [](void* context,CompletionFrame* frame) { static_cast<HostSession*>(context)->endCompletion(frame); },
                  [](void* context,CompletionFrame* frame) { static_cast<HostSession*>(context)->enterCompletion(frame); }};
}
void HostSession::beginCompletion() {
    retain(); // Owned until endCompletion, even if the pipe wrapper is freed.
    loop->runAction([](OSObject*,void* arg,void*,void*,void*) -> IOReturn {
        ++static_cast<HostSession*>(arg)->pendingCompletions;
        return kIOReturnSuccess;
    },host,this);
}
void HostSession::enterCompletion(CompletionFrame* frame) {
    struct Arguments { HostSession* session;CompletionFrame* frame; } args={this,frame};
    loop->runAction([](OSObject*,void* arg,void*,void*,void*) -> IOReturn {
        auto& a=*static_cast<Arguments*>(arg);
        a.frame->thread=current_thread();a.frame->next=a.session->callbackFrames;
        a.session->callbackFrames=a.frame;return kIOReturnSuccess;
    },host,&args);
}
void HostSession::endCompletion(CompletionFrame* frame) {
    struct Arguments { HostSession* session;CompletionFrame* frame; } args={this,frame};
    loop->runAction([](OSObject*,void* arg,void*,void*,void*) -> IOReturn {
        auto& a=*static_cast<Arguments*>(arg);auto* s=a.session;
        if (a.frame) {
            auto** link=&s->callbackFrames;
            while (*link && *link!=a.frame) link=&(*link)->next;
            if (*link) *link=a.frame->next;
        }
        --s->pendingCompletions;
        s->notifyIfQuiescent();return kIOReturnSuccess;
    },host,&args);
    release();
}
IOReturn HostSession::quiesceForStop(uint32_t timeoutMs) {
    if (!timeoutMs) return kIOReturnBadArgument;
    // An outer native gate or its work thread may be required to complete the
    // very work we would wait for. Never begin a blocking drain there.
    if (loop->inGate() || loop->onThread()) return kIOReturnNotReady;
    struct Arguments { HostSession* session;uint32_t timeoutMs; } args={this,timeoutMs};
    retain();
    const auto result=loop->runAction([](OSObject*,void* arg,void*,void*,void*) -> IOReturn {
        auto& a=*static_cast<Arguments*>(arg);auto* s=a.session;const auto thread=current_thread();
        for (auto* frame=s->callbackFrames;frame;frame=frame->next)
            if (frame->thread==thread) return kIOReturnNotReady;
        for (auto* frame=s->actionFrames;frame;frame=frame->next)
            if (frame->thread==thread) return kIOReturnNotReady;
        // Native open is not represented as a tracked perform action. Reject
        // this phase without retiring it; its caller must handle the failure.
        if (s->opening) return kIOReturnNotReady;
        s->closeGated();
        uint64_t deadline=0;
        clock_interval_to_deadline(a.timeoutMs,kMillisecondScale,&deadline);
        ++s->waiters;
        IOReturn status=kIOReturnSuccess;
        while (s->opening || s->draining || s->activeActions || s->pendingCompletions) {
            status=s->drainGate->commandSleep(&s->pendingCompletions,deadline,THREAD_UNINT);
            if (status!=kIOReturnSuccess) break;
        }
        --s->waiters;
        return status;
    },host,&args);
    release();return result;
}
}
