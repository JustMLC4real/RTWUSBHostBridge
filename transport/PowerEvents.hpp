#pragma once
#include <stdint.h>

namespace rtw {
// Called after a native IOUSBHostDevice power transition has completed.
// Native ordinals: Off=0, Suspended=1, On=2 (matching kernel header).
// No notification is synthesized for an unknown state or initial power-on.
class PowerEvents {
public:
    static constexpr uint32_t LegacyResume=0xe000400bU;
    static constexpr uint32_t LegacySuspend=0xe000400dU;
    void start() {
        __atomic_store_n(&previous,Unknown,__ATOMIC_RELAXED);
        __atomic_store_n(&enabled,true,__ATOMIC_RELEASE);
    }
    void stop() { __atomic_store_n(&enabled,false,__ATOMIC_RELEASE); }
    uint32_t didChange(unsigned long ordinal) {
        if (!__atomic_load_n(&enabled,__ATOMIC_ACQUIRE) || ordinal>2) return 0;
        const auto old=__atomic_exchange_n(&previous,static_cast<uint32_t>(ordinal),__ATOMIC_ACQ_REL);
        if (!__atomic_load_n(&enabled,__ATOMIC_ACQUIRE)) return 0;
        if (ordinal==1 && old!=1) return LegacySuspend;
        if (ordinal==2 && old==1) return LegacyResume;
        return 0;
    }
private:
    static constexpr uint32_t Unknown=0xffffffffU;
    uint32_t previous=Unknown;
    bool enabled=false;
};
}
