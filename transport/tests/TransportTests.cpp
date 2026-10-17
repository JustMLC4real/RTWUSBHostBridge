#include "../HostTransport.hpp"
#include <cassert>
#include <cstdio>

int testAllocations = 0;
bool testAllocationFailure = false;

struct Result { int calls = 0; IOReturn status = 0; uint32_t remaining = 0; };
static void capture(void* target, void* parameter, IOReturn status, uint32_t remaining) {
    assert(target == reinterpret_cast<void*>(0x1234));
    auto* r = static_cast<Result*>(parameter);
    ++r->calls; r->status = status; r->remaining = remaining;
}
static rtw::LegacyCompletion completion(Result& r) {
    return {reinterpret_cast<void*>(0x1234), capture, &r};
}
static void balanced(OSObject& owner, OSObject& source, IOMemoryDescriptor& buffer) {
    assert(owner.references == 1 && source.references == 1 && buffer.references == 1);
    assert(testAllocations == 0);
}

static void transfers() {
    OSObject owner;
    IOUSBHostPipe pipe;
    IOMemoryDescriptor buffer;
    Result r;
    pipe.transferred = 1500;
    assert(rtw::pipeAsync(&pipe, &owner, &buffer, 1536, completion(r), 0, 5000) == 0);
    assert(owner.references == 2 && pipe.references == 2 && buffer.references == 2);
    assert(r.calls == 0 && pipe.timeout == 5000 && pipe.length == 1536);
    pipe.complete();
    assert(r.calls == 1 && r.remaining == 36 && r.status == 0);
    balanced(owner, pipe, buffer);

    OSObject wrapper;
    r = {}; pipe.transferred = 0;
    pipe.callbackStatus = static_cast<IOReturn>(0xe0005000U);
    assert(rtw::pipeAsync(&pipe,&owner,&buffer,512,completion(r),0,0,&wrapper)==0);
    assert(wrapper.references==2);
    pipe.complete();
    assert(wrapper.references==1 && static_cast<uint32_t>(r.status)==0xe000404fU && r.remaining==512);
    balanced(owner,pipe,buffer);
    assert(static_cast<uint32_t>(rtw::legacyStatus(static_cast<IOReturn>(0xe00002d6U)))==0xe0004051U);
    assert(static_cast<uint32_t>(rtw::legacyStatus(static_cast<IOReturn>(0xe0005001U)))==0xe000405dU);
    pipe.callbackStatus=0;

    r = {};
    pipe.transferred = 513; pipe.callbackStatus = kIOReturnAborted;
    assert(rtw::pipeAsync(&pipe, &owner, &buffer, 2048, completion(r), 0, 0) == 0);
    pipe.complete();
    assert(r.status == kIOReturnAborted && r.remaining == 1535);
    balanced(owner, pipe, buffer);

    // A completion may happen before io() returns. Exercise this with ASan.
    r = {}; pipe.immediate = true; pipe.transferred = 1024; pipe.callbackStatus = 0;
    assert(rtw::pipeAsync(&pipe, &owner, &buffer, 1024, completion(r), 0, 0) == 0);
    assert(r.calls == 1 && r.remaining == 0);
    balanced(owner, pipe, buffer);

    r = {}; pipe.transferred = 2048;
    assert(rtw::pipeAsync(&pipe, &owner, &buffer, 1024, completion(r), 0, 0) == 0);
    assert(r.status == kIOReturnOverrun && r.remaining == 0);
    balanced(owner, pipe, buffer);

    r = {}; pipe.immediate = false; pipe.submitStatus = kIOReturnNotReady;
    assert(rtw::pipeAsync(&pipe, &owner, &buffer, 1024, completion(r), 0, 0) == kIOReturnNotReady);
    assert(r.calls == 0);
    balanced(owner, pipe, buffer);
    pipe.submitStatus = 0;

    testAllocationFailure = true;
    assert(rtw::pipeAsync(&pipe, &owner, &buffer, 1024, completion(r), 0, 0) == kIOReturnNoMemory);
    testAllocationFailure = false;
    balanced(owner, pipe, buffer);
    const auto before = pipe.submissions;
    assert(rtw::pipeAsync(&pipe, &owner, &buffer, 4097, completion(r), 0, 0) == kIOReturnBadArgument);
    buffer.size = 0x100000000ULL;
    assert(rtw::pipeAsync(&pipe, &owner, &buffer, buffer.size, completion(r), 0, 0) == kIOReturnBadArgument);
    buffer.size = 4096;
    assert(rtw::pipeAsync(&pipe, &owner, &buffer, 1024, completion(r), 10, 0) == kIOReturnUnsupported);
    pipe.descriptor.bmAttributes = 3;
    assert(rtw::pipeAsync(&pipe, &owner, &buffer, 1024, completion(r), 0, 100) == kIOReturnBadArgument);
    pipe.descriptor.bmAttributes = 1;
    assert(rtw::pipeAsync(&pipe, &owner, &buffer, 1024, completion(r), 0, 0) == kIOReturnUnsupported);
    assert(pipe.submissions == before);
    balanced(owner, pipe, buffer);
}

static void controls() {
    IOUSBHostInterface iface;
    OSObject owner;
    IOMemoryDescriptor buffer;
    uint8_t data[4] = {};
    rtw::LegacyRequest req = {0xc0, 5, 0x1234, 0x5678, 4, data, 99};
    iface.syncTransferred = 3;
    assert(rtw::controlSync(&iface, req, 5000) == 0);
    assert(req.bytesDone == 3 && iface.setup.bmRequestType == 0xc0 &&
        iface.setup.bRequest == 5 && iface.setup.wValue == 0x1234 &&
        iface.setup.wIndex == 0x5678 && iface.setup.wLength == 4 && iface.timeout == 5000);
    iface.syncTransferred = 5;
    assert(rtw::controlSync(&iface, req, 5000) == kIOReturnOverrun && req.bytesDone == 4);
    req.data = nullptr;
    assert(rtw::controlSync(&iface, req, 5000) == kIOReturnBadArgument && req.bytesDone == 0);
    req.data = data;
    Result r;
    iface.transferred = 2;
    assert(rtw::controlAsync(&iface, &owner, req, &buffer, completion(r), 5000) == 0);
    iface.complete();
    assert(r.calls == 1 && r.remaining == 2);
    balanced(owner, iface, buffer);
    iface.submitStatus = kIOReturnNotReady; r = {};
    assert(rtw::controlAsync(&iface, &owner, req, &buffer, completion(r), 5000) == kIOReturnNotReady);
    assert(r.calls == 0);
    balanced(owner, iface, buffer);
    iface.submitStatus = 0; iface.transferred = 4; iface.immediate = true; r = {};
    OSObject wrapper;
    assert(rtw::controlRawAsync(&iface, &owner, req, completion(r), 5000,&wrapper) == 0);
    assert(r.calls == 1 && r.remaining == 0 && owner.references == 1 && iface.references == 1);
    assert(wrapper.references==1);
    assert(testAllocations == 0);
    iface.immediate = false; iface.submitStatus = kIOReturnNotReady; r = {};
    assert(rtw::controlRawAsync(&iface, &owner, req, completion(r), 5000,&wrapper) == kIOReturnNotReady);
    assert(!r.calls && testAllocations == 0 && owner.references == 1 && iface.references == 1);
    assert(wrapper.references==1);
}

static void configuration(IOUSBHostInterface& iface, IOUSBHostPipe& in, IOUSBHostPipe& out) {
    // The second interface's endpoint must not leak into the first one's pipes.
    iface.config = {9,2,0,0,2,1,0,0x80,50, 9,4,0,0,2,255,255,255,0,
        7,5,0x81,2,0,2,0, 7,5,0x02,2,0,2,0,
        9,4,1,0,1,255,255,255,0, 7,5,0x83,3,8,0,1};
    iface.config[2] = static_cast<uint8_t>(iface.config.size());
    iface.pipes[0x81] = &in; iface.pipes[0x02] = &out;
}
static void descriptors() {
    IOUSBHostInterface iface;
    IOUSBHostPipe in, out;
    configuration(iface, in, out);
    rtw::Endpoint endpoints[4] = {};
    uint32_t count = 0;
    assert(rtw::copyEndpoints(&iface, endpoints, 4, count) == 0 && count == 2);
    assert(endpoints[0].address == 0x81 && endpoints[1].address == 2 &&
        endpoints[0].maxPacketSize == 512 && in.references == 2 && out.references == 2);
    rtw::releaseEndpoints(endpoints, count);
    assert(in.references == 1 && out.references == 1);
    iface.config[23] = 0x12; // HS transaction bits are not part of packet size.
    assert(rtw::copyEndpoints(&iface, endpoints, 4, count) == 0);
    assert(endpoints[0].maxPacketSize == 512);
    rtw::releaseEndpoints(endpoints, count);
    assert(rtw::copyEndpoints(&iface, endpoints, 1, count) == kIOReturnNoSpace && count == 0);
    assert(in.references == 1 && out.references == 1 && !endpoints[0].pipe);

    iface.pipes[2] = nullptr;
    assert(rtw::copyEndpoints(&iface, endpoints, 4, count) == kIOReturnNotReady && count == 0);
    assert(in.references == 1);
    configuration(iface, in, out);
    iface.config[25] = 0; // zero descriptor length after first retained pipe
    assert(rtw::copyEndpoints(&iface, endpoints, 4, count) == kIOReturnBadArgument && count == 0);
    assert(in.references == 1);
    configuration(iface, in, out);
    iface.config[27] = 0x81; // duplicate endpoint
    assert(rtw::copyEndpoints(&iface, endpoints, 4, count) == kIOReturnBadArgument && count == 0);
    assert(in.references == 1);
    configuration(iface, in, out);
    iface.config[13] = 3; // endpoint count mismatch
    assert(rtw::copyEndpoints(&iface, endpoints, 4, count) == kIOReturnBadArgument && count == 0);
    assert(in.references == 1 && out.references == 1);
    configuration(iface, in, out);
    iface.selected = static_cast<uint32_t>(iface.config.size() - 2);
    assert(rtw::copyEndpoints(&iface, endpoints, 4, count) == kIOReturnBadArgument);
}

static void asusObservedConfiguration() {
    IOUSBHostInterface iface;
    // Descriptor shape observed on this user's 0b05:1817 with a cache-only
    // query: one configuration, interface 0, alt 0, five USB2 endpoints.
    iface.config={9,2,53,0,1,1,0,0x80,50, 9,4,0,0,5,255,255,255,0,
        7,5,0x81,2,0,2,0, 7,5,0x02,2,0,2,0, 7,5,0x03,2,0,2,0,
        7,5,0x04,2,0,2,0, 7,5,0x85,3,64,0,1};
    IOUSBHostPipe p[5];
    const uint8_t addresses[]={0x81,0x02,0x03,0x04,0x85};
    for (int i=0;i<5;++i) iface.pipes[addresses[i]]=&p[i];
    rtw::Endpoint endpoints[32]={};uint32_t count=0;
    assert(rtw::copyEndpoints(&iface,endpoints,32,count)==0 && count==5);
    int bulkIn=0,bulkOut=0,interruptIn=0;
    for (uint32_t i=0;i<count;++i) {
        assert(endpoints[i].address==addresses[i]);
        const auto type=endpoints[i].attributes&3;
        const bool input=endpoints[i].address&0x80;
        if (type==2) { assert(endpoints[i].maxPacketSize==512); if (input) ++bulkIn;else ++bulkOut; }
        if (type==3 && input) { assert(endpoints[i].maxPacketSize==64);++interruptIn; }
    }
    assert(bulkIn==1 && bulkOut==3 && interruptIn==1);
    rtw::releaseEndpoints(endpoints,count);
    for (const auto& endpoint:p) assert(endpoint.references==1);
}

int main() {
    transfers(); controls(); descriptors(); asusObservedConfiguration();
    assert(testAllocations == 0);
    std::puts("PASS: transfer callbacks, cancellation, failure cleanup, control setup, endpoint bounds and ownership");
}
