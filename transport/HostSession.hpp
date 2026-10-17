#pragma once
#include "HostTransport.hpp"
#ifndef RTW_TEST_HOST
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOCommandGate.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#endif

namespace rtw {
// One immutable open-session identity. Never reactivate a retired instance.
// The matching-build native USB stack uses host->getWorkLoop() for its gate.
class HostSession {
public:
    using Action = IOReturn (*)(void*);
    using ClosedAction = void (*)(IOService*, IOService*, void*);
    static HostSession* create(IOUSBHostInterface*);
    void retain();
    void release();
    bool open(IOService*);
    void close();
    // Coalesce one termination notification per session. Retain its services
    // and schedule it only after native open/rollback and close have returned,
    // including when requested recursively by a draining completion.
    // Delivery runs on a kernel thread call without holding our USB gate;
    // acquiring IOService arbitration locks under that gate can deadlock open.
    // argument is an opaque notification value; pointee ownership is not taken.
    IOReturn closeAndNotify(IOService* target, IOService* provider,
                          void* argument, ClosedAction);
    // Separate didTerminate continuation. Never retain the caller's bool*
    // defer; copy only its opaque options value. If a termination notice has
    // already been registered, finish that delivery before this continuation.
    IOReturn closeAndFinalize(IOService* target, IOService* provider,
                            void* options, ClosedAction);
    IOReturn closeAndReleaseClient(IOService* target, IOService* client,
                                 void* options, ClosedAction);
    IOReturn perform(Action, void*);
    CompletionLease completionLease();
    // Explicit blocking barrier, NOT yet wired into the original stop hook.
    // Reject reentrant/workloop-thread contexts rather than waiting on self.
    // timeoutMs bounds our accounting wait after native close returns; it
    // cannot impose a deadline on the native close implementation itself.
    IOReturn quiesceForStop(uint32_t timeoutMs=5000);
private:
    enum State { Fresh, Opening, Open, Retired };
    uint32_t references;
    IOUSBHostInterface* host;
    IOWorkLoop* loop;
    IOCommandGate* drainGate;
    IOService* client;
    State state; // Read/written only under loop->runAction().
    bool opening;
    bool draining;
    bool workerScheduled;
    uint32_t activeActions;
    uint32_t pendingCompletions;
    uint32_t waiters;
    CompletionFrame* callbackFrames;
    CompletionFrame* actionFrames;
    enum EventKind { TerminationNotice, ClientClose, ProviderFinalization, EventCount };
    enum EventState { Empty, Pending, Running, Finished };
    struct DeferredAction {
        ClosedAction action;
        IOService* target;
        IOService* provider;
        void* argument;
        EventState state;
    } events[EventCount];
    thread_call_t workerCall;
    static void dispatchThread(thread_call_param_t, thread_call_param_t);
    IOReturn closeAndQueue(EventKind,IOService*,IOService*,void*,ClosedAction);
    void closeGated();
    void notifyIfQuiescent();
    void beginCompletion();
    void enterCompletion(CompletionFrame*);
    void endCompletion(CompletionFrame*);
};
}
