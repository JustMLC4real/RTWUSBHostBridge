"""Generate the audited legacy dispatch slots, without importing legacy kexts.

Unimplemented slots are explicit barriers. This is a development component, not
an installable driver: validate_abi.py must also establish complete call coverage
before any candidate is eligible for hardware deployment.
"""
from pathlib import Path
import json

ROOT = Path(__file__).resolve().parent
CLASSES = {
    'RTUSBPipe': ('OSObject', 0x118, {
        0x120: 'uint8_t status()',
        0x128: 'IOReturn abort()',
        0x130: 'IOReturn reset()',
        0x138: 'IOReturn clearStall()',
        0x140: 'IOReturn readDefault(IOMemoryDescriptor*, rtw::LegacyCompletion*, uint64_t*)',
        0x148: 'IOReturn writeDefault(IOMemoryDescriptor*, rtw::LegacyCompletion*)',
        0x160: 'IOReturn controlDescDefault(rtw::LegacyRequest*, rtw::LegacyCompletion*)',
        0x168: 'IOReturn controlDefault(rtw::LegacyRequest*, rtw::LegacyCompletion*)',
        0x178: 'const StandardUSB::EndpointDescriptor* endpointDescriptor()',
        0x180: 'uint8_t direction()',
        0x188: 'uint8_t type()',
        0x190: 'uint8_t endpointNumber()',
        0x198: 'uint16_t address()',
        0x1a0: 'uint16_t maxPacketSize()',
        0x1a8: 'uint8_t interval()',
        0x1b0: 'IOReturn readTimed(IOMemoryDescriptor*, uint32_t, uint32_t, rtw::LegacyCompletion*, uint64_t*)',
        0x1b8: 'IOReturn writeTimed(IOMemoryDescriptor*, uint32_t, uint32_t, rtw::LegacyCompletion*)',
        0x1c0: 'IOReturn controlDesc(rtw::LegacyRequest*, uint32_t, uint32_t, rtw::LegacyCompletion*)',
        0x1c8: 'IOReturn control(rtw::LegacyRequest*, uint32_t, uint32_t, rtw::LegacyCompletion*)',
        0x1d0: 'IOReturn read(IOMemoryDescriptor*, uint32_t, uint32_t, uint64_t, rtw::LegacyCompletion*, uint64_t*)',
        0x1d8: 'IOReturn write(IOMemoryDescriptor*, uint32_t, uint32_t, uint64_t, rtw::LegacyCompletion*)',
        0x1e0: 'IOReturn pipeStatus()',
        0x1e8: 'IOReturn clearPipeStall(bool)',
    }),
    'RTUSBDevice': ('IOService', 0x850, {
        0x890: 'IOReturn findInterfaceDescriptor(const StandardUSB::ConfigurationDescriptor*, const StandardUSB::InterfaceDescriptor*, const InterfaceQuery*, StandardUSB::InterfaceDescriptor**)',
        0x8a8: 'const StandardUSB::ConfigurationDescriptor* configuration(uint8_t)',
        0x8c8: 'uint16_t address()',
        0x8d0: 'uint8_t speed()',
        0x8e8: 'uint8_t maxPacketSize()',
        0x8f0: 'uint16_t vendor()',
        0x8f8: 'uint16_t product()',
        0x900: 'uint16_t revision()',
        0x908: 'uint8_t configurationCount()',
        0x910: 'uint8_t manufacturerIndex()',
        0x918: 'uint8_t productIndex()',
        0x920: 'uint8_t serialIndex()',
        0x928: 'RTUSBPipe* controlPipe()',
        0x958: 'IOReturn stringDescriptor(uint8_t, char*, int, uint16_t)',
    }),
    'RTUSBInterface': ('IOService', 0x850, {
        0x858: 'void closePipes()',
        0x860: 'IOReturn createPipes()',
        0x868: 'void updateProperties()',
        0x880: 'RTUSBPipe* findPipe(RTUSBPipe*, EndpointQuery*)',
        0x890: 'IOReturn alternate(IOService*, uint16_t)',
        0x898: 'RTUSBPipe* pipe(uint8_t)',
        0x8a0: 'uint8_t configurationValue()',
        0x8a8: 'RTUSBDevice* device()',
        0x8b0: 'uint8_t interfaceNumber()',
        0x8b8: 'uint8_t alternateSetting()',
        0x8c0: 'uint8_t endpointCount()',
        0x8c8: 'uint8_t interfaceClass()',
        0x8d0: 'uint8_t interfaceSubClass()',
        0x8d8: 'uint8_t interfaceProtocol()',
        0x8e0: 'uint8_t interfaceStringIndex()',
        0x8e8: 'IOReturn control(rtw::LegacyRequest*, rtw::LegacyCompletion*)',
        0x8f0: 'IOReturn controlDesc(rtw::LegacyRequest*, rtw::LegacyCompletion*)',
        0x8f8: 'IOReturn endpointProperties(uint8_t, uint8_t, uint8_t, uint8_t*, uint16_t*, uint8_t*)',
        0x900: 'RTUSBPipe* findPipeRetained(RTUSBPipe*, EndpointQuery*, bool)',
        0x918: 'void unlinkPipes()',
        0x920: 'void reopenPipes()',
    }),
}
EXTRA = {
    'RTUSBPipe': '''
    static RTUSBPipe* create(IOUSBHostInterface*, rtw::HostSession*, IOUSBHostPipe* = nullptr);
    IOReturn transfer(IOMemoryDescriptor*, uint32_t, uint32_t, uint64_t,
        rtw::LegacyCompletion*, uint64_t*);
protected:
    void free() override;
private:
    rtw::HostSession* session = nullptr;
    IOReturn transferGated(IOMemoryDescriptor*, uint32_t, uint32_t, uint64_t,
        rtw::LegacyCompletion*, uint64_t*);
    IOReturn controlGated(rtw::LegacyRequest*, uint32_t, uint32_t,
        rtw::LegacyCompletion*, bool);
    IOUSBHostInterface* host = nullptr;
    IOUSBHostPipe* endpoint = nullptr;
''',
    'RTUSBDevice': '''
    static RTUSBDevice* create(IOUSBHostInterface*);
    bool prepareSession(rtw::HostSession*);
protected:
    void free() override;
private:
    IOUSBHostInterface* host = nullptr;
    RTUSBPipe* zero = nullptr;
    const StandardUSB::DeviceDescriptor* descriptor();
''',
    'RTUSBInterface': '''
    bool start(IOService*) override;
    void stop(IOService*) override;
    void close(IOService*, IOOptionBits = 0) override;
    bool willTerminate(IOService*, IOOptionBits) override;
    bool didTerminate(IOService*, IOOptionBits, bool*) override;
    IOReturn message(uint32_t, IOService*, void*) override;
    IOReturn powerStateWillChangeTo(IOPMPowerFlags, unsigned long, IOService*) override;
    IOReturn powerStateDidChangeTo(IOPMPowerFlags, unsigned long, IOService*) override;
protected:
    bool handleOpen(IOService*, IOOptionBits, void*) override;
    void handleClose(IOService*, IOOptionBits) override;
    void free() override;
private:
    IOUSBHostInterface* host = nullptr;
    RTUSBDevice* proxy = nullptr;
    rtw::HostSession* session = nullptr;
    RTUSBPipe* endpoints[32] = {};
    uint32_t count = 0;
    IOService* client = nullptr;
    IOService* pendingCloseClient = nullptr; // Protected by IOService arbitration.
    bool hostOpen = false;
    bool closing = false;
    bool stopping = false;
    bool powerActive = false;
    IOUSBHostDevice* powerProvider = nullptr;
    rtw::PowerEvents powerEvents;
    void stopPowerNotifications();
    void closeSession();
    void finishClientClose(IOService*, IOOptionBits);
    const StandardUSB::InterfaceDescriptor* descriptor();
''',
}

header = '''// Generated by generate_abi.py. Keep dispatch order frozen.
#pragma once
#include "../transport/HostTransport.hpp"
#include "../transport/HostSession.hpp"
#include "../transport/PowerEvents.hpp"
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSBoolean.h>
class RTUSBPipe;
class RTUSBDevice;
class RTUSBInterface;
struct EndpointQuery { uint8_t type; uint8_t direction; uint16_t maxPacketSize; uint8_t interval; };
struct InterfaceQuery { uint16_t klass; uint16_t subclass; uint16_t protocol; uint16_t alternate; };
uintptr_t rtwUnsupportedABI(const char*, uint32_t);
'''
manifest = {}
for cls, (base, start, methods) in CLASSES.items():
    end = max(methods)
    header += f'\nclass {cls} : public {base} {{\n    OSDeclareDefaultStructors({cls})\npublic:\n'
    manifest[cls] = {'base': base, 'start': start, 'methods': {}}
    for slot in range(start, end + 8, 8):
        if slot in methods:
            decl = methods[slot]
            ownership = 'LIBKERN_RETURNS_NOT_RETAINED ' if (
                decl.startswith('RTUSBDevice*') or
                (decl.startswith('RTUSBPipe*') and 'findPipeRetained' not in decl)) else ''
            header += f'    {ownership}virtual {decl}; // 0x{slot:x}\n'
            manifest[cls]['methods'][f'{slot:#x}'] = decl
        else:
            header += f'    virtual uintptr_t blocked_{slot:x}() {{ return rtwUnsupportedABI("{cls}", 0x{slot:x}); }}\n'
    header += EXTRA[cls] + '};\n'
(ROOT / 'LegacyABI.hpp').write_text(header)
(ROOT / 'expected_abi.json').write_text(json.dumps(manifest, indent=2))
print('Generated exact-slot facade declarations. Unmapped slots remain deployment blockers.')
