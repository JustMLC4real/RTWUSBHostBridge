#include "HostTransport.hpp"

namespace rtw {

IOReturn legacyStatus(IOReturn status) {
    // Mappings observed in Apple's installed AppleUSBPipe::Abort compatibility
    // implementation. In particular Realtek retries on legacy stall/timeout.
    switch (static_cast<uint32_t>(status)) {
        case 0xe00002d6U: return static_cast<IOReturn>(0xe0004051U);
        case 0xe0005000U: return static_cast<IOReturn>(0xe000404fU);
        case 0xe0005001U: return static_cast<IOReturn>(0xe000405dU);
        default: return status;
    }
}

static_assert(sizeof(void*) == 8, "This adapter targets the x86_64 driver");
static_assert(__builtin_offsetof(LegacyRequest, data) == 8, "Realtek request ABI");
static_assert(__builtin_offsetof(LegacyRequest, bytesDone) == 16, "Realtek request ABI");
static_assert(sizeof(LegacyRequest) == 24, "Realtek request ABI");

static uint16_t readLE16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

static StandardUSB::DeviceRequest convert(const LegacyRequest& in) {
    StandardUSB::DeviceRequest out = {};
    // Both the existing binary and target are little-endian x86_64.
    out.bmRequestType = in.type;
    out.bRequest = in.request;
    out.wValue = in.value;
    out.wIndex = in.index;
    out.wLength = in.length;
    return out;
}

struct Pending {
    OSObject* owner;
    OSObject* source;
    OSObject* lifetime;
    IOMemoryDescriptor* buffer;
    LegacyCompletion completion;
    uint32_t requested;
    CompletionLease lease;
};

static void dispose(Pending* p, CompletionFrame* frame=nullptr) {
    const auto lease=p->lease;
    if (p->buffer) p->buffer->release();
    p->source->release();
    p->owner->release();
    if (p->lifetime) p->lifetime->release();
    IOFree(p, sizeof(*p));
    if (lease.end) lease.end(lease.context,frame);
}

static Pending* pending(OSObject* owner, OSObject* source,
    IOMemoryDescriptor* buffer, const LegacyCompletion& completion,
    uint32_t requested, OSObject* lifetime, const CompletionLease* lease) {
    auto* p = static_cast<Pending*>(IOMalloc(sizeof(Pending)));
    if (!p) return nullptr;
    p->owner = owner;
    p->source = source;
    p->lifetime = lifetime;
    p->buffer = buffer;
    p->completion = completion;
    p->requested = requested;
    p->lease = lease ? *lease : CompletionLease{};
    owner->retain();
    source->retain();
    if (buffer) buffer->retain();
    if (lifetime) lifetime->retain();
    if (p->lease.begin) p->lease.begin(p->lease.context);
    return p;
}

static void completed(void*, void* parameter, IOReturn status,
    uint32_t transferred) {
    auto* p = static_cast<Pending*>(parameter);
    CompletionFrame frame={};
    if (p->lease.enter) p->lease.enter(p->lease.context,&frame);
    // Legacy callbacks take bytes REMAINING; IOUSBHost takes bytes TRANSFERRED.
    // Preserve partial transfers on cancellation and never underflow on overrun.
    if (transferred > p->requested) {
        transferred = p->requested;
        status = kIOReturnOverrun;
    }
    const auto c = p->completion;
    c.action(c.target, c.parameter, legacyStatus(status), p->requested - transferred);
    dispose(p,&frame);
}

IOReturn controlSync(IOUSBHostInterface* interface, LegacyRequest& request,
    uint32_t timeoutMs) {
    request.bytesDone = 0;
    if (!interface || (request.length && !request.data)) return kIOReturnBadArgument;
    auto setup = convert(request);
    uint32_t done = 0;
    const auto status = interface->deviceRequest(setup, request.data, done, timeoutMs);
    request.bytesDone = done > request.length ? request.length : done;
    return done > request.length ? kIOReturnOverrun : legacyStatus(status);
}

IOReturn controlAsync(IOUSBHostInterface* interface, OSObject* owner,
    const LegacyRequest& request, IOMemoryDescriptor* buffer,
    const LegacyCompletion& completion, uint32_t timeoutMs, OSObject* lifetime, const CompletionLease* lease) {
    if (!interface || !owner || !buffer || !completion.action ||
        request.length > buffer->getLength()) return kIOReturnBadArgument;
    auto* p = pending(owner, interface, buffer, completion, request.length, lifetime, lease);
    if (!p) return kIOReturnNoMemory;
    auto setup = convert(request);
    IOUSBHostCompletion host = {nullptr, completed, p};
    const auto status = interface->deviceRequest(setup, buffer, &host, timeoutMs);
    // The API guarantees an eventual callback only on successful submission.
    // On success the callback may already have run: do not touch p again.
    if (status != kIOReturnSuccess) dispose(p);
    return legacyStatus(status);
}

IOReturn controlRawAsync(IOUSBHostInterface* interface, OSObject* owner,
    const LegacyRequest& request, const LegacyCompletion& completion,
    uint32_t timeoutMs, OSObject* lifetime, const CompletionLease* lease) {
    if (!interface || !owner || !completion.action ||
        (request.length && !request.data)) return kIOReturnBadArgument;
    auto* p = pending(owner, interface, nullptr, completion, request.length, lifetime, lease);
    if (!p) return kIOReturnNoMemory;
    auto setup = convert(request);
    IOUSBHostCompletion host = {nullptr, completed, p};
    const auto status = interface->deviceRequest(setup, request.data, &host, timeoutMs);
    if (status != kIOReturnSuccess) dispose(p);
    return legacyStatus(status);
}

IOReturn pipeAsync(IOUSBHostPipe* pipe, OSObject* owner,
    IOMemoryDescriptor* buffer, uint64_t length,
    const LegacyCompletion& completion, uint32_t idleTimeoutMs,
    uint32_t completionTimeoutMs, OSObject* lifetime, const CompletionLease* lease) {
    if (!pipe || !owner || !buffer || !completion.action ||
        length > 0xffffffffULL || length > buffer->getLength())
        return kIOReturnBadArgument;
    // No equivalent idle/no-data timeout exists in the new API. Do not silently
    // reinterpret it as a completion timeout. Audited Realtek bulk paths use 0.
    if (idleTimeoutMs) return kIOReturnUnsupported;
    const auto* descriptor = pipe->getEndpointDescriptor();
    if (!descriptor) return kIOReturnNotReady;
    const uint8_t type = descriptor->bmAttributes & 3;
    if (type != 2 && type != 3) return kIOReturnUnsupported;
    if (type == 3 && completionTimeoutMs) return kIOReturnBadArgument;
    auto* p = pending(owner, pipe, buffer, completion, static_cast<uint32_t>(length), lifetime, lease);
    if (!p) return kIOReturnNoMemory;
    IOUSBHostCompletion host = {nullptr, completed, p};
    const auto status = pipe->io(buffer, static_cast<uint32_t>(length),
        &host, completionTimeoutMs);
    if (status != kIOReturnSuccess) dispose(p);
    return legacyStatus(status);
}

void releaseEndpoints(Endpoint* endpoints, uint32_t count) {
    if (!endpoints) return;
    for (uint32_t i = 0; i < count; ++i) {
        if (endpoints[i].pipe) endpoints[i].pipe->release();
        endpoints[i] = {};
    }
}

IOReturn copyEndpoints(IOUSBHostInterface* interface, Endpoint* endpoints,
    uint32_t capacity, uint32_t& count) {
    count = 0;
    if (!interface || !endpoints || !capacity) return kIOReturnBadArgument;
    const auto* config = reinterpret_cast<const uint8_t*>(interface->getConfigurationDescriptor());
    const auto* iface = reinterpret_cast<const uint8_t*>(interface->getInterfaceDescriptor());
    if (!config || !iface) return kIOReturnNotReady;
    // The host API owns and validates the configuration allocation. Still bound
    // every descriptor step and validate the selected interface within it.
    const uint32_t total = readLE16(config + 2);
    const uintptr_t base = reinterpret_cast<uintptr_t>(config);
    const uintptr_t selected = reinterpret_cast<uintptr_t>(iface);
    if (total < 9 || config[0] < 9 || config[0] > total || config[1] != 2 ||
        selected < base || selected - base > total - 9) return kIOReturnBadArgument;
    if (iface[0] < 9 || iface[0] > total - (selected - base) || iface[1] != 4)
        return kIOReturnBadArgument;
    IOReturn status = kIOReturnSuccess;
    uint32_t position = static_cast<uint32_t>(selected - base) + iface[0];
    uint32_t seenAddresses = 0;
    while (position < total) {
        if (total - position < 2) { status = kIOReturnBadArgument; break; }
        const auto* d = config + position;
        if (d[0] < 2 || d[0] > total - position) { status = kIOReturnBadArgument; break; }
        if (d[1] == 4) break;
        if (d[1] == 5) {
            if (d[0] < 7 || (d[2] & 0x70) || !(d[2] & 15)) {
                status = kIOReturnBadArgument; break;
            }
            const uint32_t bit = 1U << ((d[2] & 15) + ((d[2] & 0x80) ? 16 : 0));
            if (seenAddresses & bit) { status = kIOReturnBadArgument; break; }
            seenAddresses |= bit;
            if (count == capacity) { status = kIOReturnNoSpace; break; }
            auto* pipe = interface->copyPipe(d[2]);
            if (!pipe) { status = kIOReturnNotReady; break; }
            endpoints[count++] = {pipe, d[2], d[3],
                static_cast<uint16_t>(readLE16(d + 4) & 0x07ff)};
        }
        position += d[0];
    }
    if (status == kIOReturnSuccess && count != iface[4]) status = kIOReturnBadArgument;
    if (status != kIOReturnSuccess) {
        releaseEndpoints(endpoints, count);
        count = 0;
    }
    return status;
}

} // namespace rtw
