#pragma once

#ifdef RTW_TEST_HOST
#include "tests/FakeHost.hpp"
#else
#include <IOKit/IOLib.h>
#include <IOKit/usb/IOUSBHostInterface.h>
#include <IOKit/usb/IOUSBHostPipe.h>
#endif

namespace rtw {

// Preserve error values tested explicitly by the existing Realtek binary.
IOReturn legacyStatus(IOReturn);

// Layout used by the existing x86_64 Realtek binary's control requests.
struct LegacyRequest {
    uint8_t type;
    uint8_t request;
    uint16_t value;
    uint16_t index;
    uint16_t length;
    void* data;
    uint32_t bytesDone;
};

struct LegacyCompletion {
    void* target;
    void (*action)(void*, void*, IOReturn, uint32_t bytesRemaining);
    void* parameter;
};

struct Endpoint {
    IOUSBHostPipe* pipe;
    uint8_t address;
    uint8_t attributes;
    uint16_t maxPacketSize;
};

// Optional per-submission accounting. A successful Pending allocation calls
// begin once; end runs after the legacy callback and retained-object cleanup,
// or on native submission failure. The context must survive begin itself.
struct CompletionFrame {
    void* thread;
    CompletionFrame* next;
};
struct CompletionLease {
    void* context;
    void (*begin)(void*);
    void (*end)(void*, CompletionFrame*);
    void (*enter)(void*, CompletionFrame*);
};

// Objects passed here must belong to an open host-interface session.
// The caller serializes teardown against submission; accepted asynchronous
// requests retain their owner, endpoint and buffer until their callback.
IOReturn controlSync(IOUSBHostInterface*, LegacyRequest&, uint32_t timeoutMs);
IOReturn controlAsync(IOUSBHostInterface*, OSObject* owner,
    const LegacyRequest&, IOMemoryDescriptor*, const LegacyCompletion&,
    uint32_t timeoutMs, OSObject* lifetime = nullptr, const CompletionLease* lease = nullptr);
// Matches the original raw-pointer control-request contract: the submitting
// driver owns the data allocation until its completion callback runs.
IOReturn controlRawAsync(IOUSBHostInterface*, OSObject* owner,
    const LegacyRequest&, const LegacyCompletion&, uint32_t timeoutMs,
    OSObject* lifetime = nullptr, const CompletionLease* lease = nullptr);
IOReturn pipeAsync(IOUSBHostPipe*, OSObject* owner, IOMemoryDescriptor*,
    uint64_t length, const LegacyCompletion&, uint32_t idleTimeoutMs,
    uint32_t completionTimeoutMs, OSObject* lifetime = nullptr, const CompletionLease* lease = nullptr);

// Outputs own retained pipes. On failure, no output pipe remains retained.
// Endpoint enumeration does not change configuration or alternate setting.
IOReturn copyEndpoints(IOUSBHostInterface*, Endpoint*, uint32_t capacity,
    uint32_t& count);
void releaseEndpoints(Endpoint*, uint32_t count);

} // namespace rtw
