#pragma once
#include <cstdint>
#include <cstdlib>
#include <vector>
#include <mutex>
#include <cassert>
#include <condition_variable>
#include <new>
#include <chrono>

using IOReturn = int;
constexpr IOReturn kIOReturnSuccess = 0;
constexpr IOReturn kIOReturnBadArgument = -1;
constexpr IOReturn kIOReturnOverrun = -2;
constexpr IOReturn kIOReturnNoMemory = -3;
constexpr IOReturn kIOReturnUnsupported = -4;
constexpr IOReturn kIOReturnNotReady = -5;
constexpr IOReturn kIOReturnNoSpace = -6;
constexpr IOReturn kIOReturnAborted = -7;
constexpr IOReturn kIOReturnNotOpen = -8;
constexpr IOReturn kIOReturnTimeout = -9;
extern int testAllocations;
extern bool testAllocationFailure;
inline void* IOMalloc(size_t size) {
    if (testAllocationFailure) return nullptr;
    auto* p = std::malloc(size);
    if (p) ++testAllocations;
    return p;
}
inline void IOFree(void* p, size_t) { --testAllocations; std::free(p); }

using thread_call_param_t = void*;
using thread_call_func_t = void (*)(void*,void*);
struct FakeThreadCall { thread_call_func_t action;void* context;bool queued; };
using thread_call_t = FakeThreadCall*;
inline std::mutex& testThreadCallMutex() { static std::mutex value;return value; }
inline std::vector<thread_call_t>& testThreadCallQueue() {
    static std::vector<thread_call_t> value;return value;
}
inline bool& testThreadCallAllocationFailure() { static bool value=false;return value; }
inline thread_call_t thread_call_allocate(thread_call_func_t action,void* context) {
    if (testThreadCallAllocationFailure()) return nullptr;
    auto* call=static_cast<thread_call_t>(IOMalloc(sizeof(FakeThreadCall)));
    if (call) { call->action=action;call->context=context;call->queued=false; }
    return call;
}
inline bool thread_call_enter(thread_call_t call) {
    std::lock_guard<std::mutex> lock(testThreadCallMutex());
    if (call->queued) return true;
    call->queued=true;testThreadCallQueue().push_back(call);return false;
}
inline bool thread_call_free(thread_call_t call) {
    {
        std::lock_guard<std::mutex> lock(testThreadCallMutex());
        if (call->queued) return false;
    }
    IOFree(call,sizeof(*call));return true;
}
inline void testRunThreadCalls() {
    for (;;) {
        thread_call_func_t action;void* context;
        {
            std::lock_guard<std::mutex> lock(testThreadCallMutex());
            auto& queue=testThreadCallQueue();if (queue.empty()) return;
            auto* call=queue.front();queue.erase(queue.begin());call->queued=false;
            action=call->action;context=call->context;
        }
        // Callback may free its own call. Never access that call afterwards.
        action(context,nullptr);
    }
}

struct OSObject {
    int references = 1;
    void retain() { ++references; }
    void release() { --references; }
};
using IOOptionBits = uint32_t;
inline void IOLog(const char*,...) {}
struct IOService : OSObject {
    mutable std::recursive_mutex arbitration;
    IOService* openOwner=nullptr;
    bool phase3=false;
    int stopRequests=0,baseDidTerminateCalls=0;
    void (*onStopRequest)(void*,IOService*)=nullptr;
    void* stopContext=nullptr;
    virtual bool lockForArbitration(bool=true) { arbitration.lock();return true; }
    virtual void unlockForArbitration() { arbitration.unlock(); }
    virtual void close(IOService* client,IOOptionBits options=0) {
        lockForArbitration();
        const bool wasOpen=handleIsOpen(client);
        if (wasOpen) handleClose(client,options);
        const bool last=wasOpen && phase3;
        unlockForArbitration();
        // Match kernel 25G241: there is NO post-handleClose isOpen recheck.
        if (last) client->scheduleStop(this);
    }
    virtual void scheduleStop(IOService* provider) {
        ++stopRequests;if(onStopRequest) onStopRequest(stopContext,provider);
    }
    virtual bool didTerminate(IOService*,IOOptionBits,bool* defer) {
        if (!*defer) ++baseDidTerminateCalls;
        return true;
    }
protected:
    virtual bool handleIsOpen(const IOService* client) const {
        return client?openOwner==client:openOwner!=nullptr;
    }
    virtual void handleClose(IOService* client,IOOptionBits) {
        if (openOwner==client) openOwner=nullptr;
    }
};
using thread_t = void*;
inline thread_t current_thread() { static thread_local char token;return &token; }
constexpr int THREAD_UNINT=0;
constexpr uint32_t kMillisecondScale=1000000;
inline void clock_interval_to_deadline(uint32_t interval,uint32_t scale,uint64_t* deadline) {
    *deadline=std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count()+static_cast<uint64_t>(interval)*scale;
}
struct IOCommandGate;
struct IOWorkLoop : OSObject {
    using Action = IOReturn (*)(OSObject*,void*,void*,void*,void*);
    std::recursive_mutex gate;
    thread_t worker=nullptr;
    static std::vector<IOWorkLoop*>& held() { static thread_local std::vector<IOWorkLoop*> value;return value; }
    bool onThread() const { return worker==current_thread(); }
    bool inGate() const { for (auto* loop:held()) if (loop==this) return true;return false; }
    IOReturn addEventSource(IOCommandGate*);
    void removeEventSource(IOCommandGate*);
    IOReturn runAction(Action action,OSObject* owner,void* a=nullptr,
        void* b=nullptr,void* c=nullptr,void* d=nullptr) {
        std::lock_guard<std::recursive_mutex> lock(gate);
        held().push_back(this);
        const auto result=action(owner,a,b,c,d);
        held().pop_back();return result;
    }
};
inline bool& testGateAllocationFailure() { static bool value=false;return value; }
inline bool& testGateAttachmentFailure() { static bool value=false;return value; }
struct IOCommandGate : OSObject {
    IOWorkLoop* loop=nullptr;
    std::condition_variable_any ready;
    static IOCommandGate* commandGate(OSObject*) {
        if (testGateAllocationFailure()) return nullptr;
        auto* memory=IOMalloc(sizeof(IOCommandGate));
        return memory?new(memory) IOCommandGate:nullptr;
    }
    void release() {
        if (--references) return;
        this->~IOCommandGate();IOFree(this,sizeof(IOCommandGate));
    }
    IOReturn commandSleep(void*,uint64_t deadline,int) {
        assert(loop && loop->inGate());
        // These tests enter the blocking barrier at gate depth one; they do
        // not model native recursive-lock sleep or native interruption codes.
        std::unique_lock<std::recursive_mutex> lock(loop->gate,std::adopt_lock);
        const auto result=ready.wait_until(lock,std::chrono::steady_clock::time_point(std::chrono::nanoseconds(deadline)));
        lock.release();return result==std::cv_status::timeout?kIOReturnTimeout:kIOReturnSuccess;
    }
    void commandWakeup(void*,bool) { ready.notify_all(); }
};
inline IOReturn IOWorkLoop::addEventSource(IOCommandGate* source) {
    if (testGateAttachmentFailure()) return kIOReturnNoMemory;
    source->retain();source->loop=this;return kIOReturnSuccess;
}
inline void IOWorkLoop::removeEventSource(IOCommandGate* source) {
    source->loop=nullptr;source->release();
}
struct IOMemoryDescriptor : OSObject {
    uint64_t size = 4096;
    uint64_t getLength() { return size; }
};
namespace StandardUSB {
struct DeviceRequest {
    uint8_t bmRequestType, bRequest;
    uint16_t wValue, wIndex, wLength;
};
struct EndpointDescriptor {
    uint8_t bLength, bDescriptorType, bEndpointAddress, bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t bInterval;
};
struct ConfigurationDescriptor {};
struct InterfaceDescriptor {};
}
struct IOUSBHostCompletion {
    void* owner;
    void (*action)(void*, void*, IOReturn, uint32_t);
    void* parameter;
};
struct FakeSubmission {
    IOReturn submitStatus = kIOReturnSuccess;
    IOReturn callbackStatus = kIOReturnSuccess;
    uint32_t transferred = 0;
    bool immediate = false;
    bool queued = false;
    int submissions = 0;
    uint32_t length = 0;
    uint32_t timeout = 0;
    IOUSBHostCompletion saved = {};
    IOReturn submit(IOUSBHostCompletion* c, uint32_t bytes, uint32_t ms) {
        ++submissions;
        length = bytes;
        timeout = ms;
        if (submitStatus != kIOReturnSuccess) return submitStatus;
        saved = *c;
        queued = true;
        if (immediate) complete();
        return kIOReturnSuccess;
    }
    void complete() {
        queued = false;
        const auto c = saved;
        c.action(c.owner, c.parameter, callbackStatus, transferred);
    }
};
struct IOUSBHostPipe : OSObject, FakeSubmission {
    StandardUSB::EndpointDescriptor descriptor = {7, 5, 0x81, 2, 512, 0};
    const StandardUSB::EndpointDescriptor* getEndpointDescriptor() { return &descriptor; }
    IOReturn io(IOMemoryDescriptor*, uint32_t length, IOUSBHostCompletion* c, uint32_t timeout) {
        return submit(c, length, timeout);
    }
};
struct IOUSBHostInterface : OSObject, FakeSubmission {
    IOWorkLoop* loop = nullptr;
    bool openAllowed = true;
    IOService* client = nullptr;
    int opens = 0, closes = 0, syncRequests = 0;
    void (*onClose)(void*) = nullptr;
    void* onCloseContext = nullptr;
    void (*onOpen)(void*) = nullptr;
    void* onOpenContext = nullptr;
    IOWorkLoop* getWorkLoop() { return loop; }
    bool open(IOService* c) {
        ++opens;
        if (!openAllowed || client) return false;
        client=c;
        if (onOpen) onOpen(onOpenContext);
        return true;
    }
    void close(IOService*) {
        ++closes;client=nullptr;
        if (onClose) onClose(onCloseContext);
    }
    std::vector<uint8_t> config;
    uint32_t selected = 9;
    uint32_t syncTransferred = 0;
    IOReturn syncStatus = kIOReturnSuccess;
    StandardUSB::DeviceRequest setup = {};
    IOUSBHostPipe* pipes[256] = {};
    IOReturn deviceRequest(StandardUSB::DeviceRequest& r, void*, uint32_t& done, uint32_t ms) {
        ++syncRequests;
        setup = r; done = syncTransferred; timeout = ms; return syncStatus;
    }
    IOReturn deviceRequest(StandardUSB::DeviceRequest& r, IOMemoryDescriptor*, IOUSBHostCompletion* c, uint32_t ms) {
        setup = r; return submit(c, r.wLength, ms);
    }
    IOReturn deviceRequest(StandardUSB::DeviceRequest& r, void*, IOUSBHostCompletion* c, uint32_t ms) {
        setup = r; return submit(c, r.wLength, ms);
    }
    const StandardUSB::ConfigurationDescriptor* getConfigurationDescriptor() {
        return reinterpret_cast<const StandardUSB::ConfigurationDescriptor*>(config.data());
    }
    const StandardUSB::InterfaceDescriptor* getInterfaceDescriptor() {
        return reinterpret_cast<const StandardUSB::InterfaceDescriptor*>(config.data() + selected);
    }
    IOUSBHostPipe* copyPipe(uint8_t address) {
        auto* p = pipes[address];
        if (p) p->retain();
        return p;
    }
};
