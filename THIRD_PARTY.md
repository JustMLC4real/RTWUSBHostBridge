# Third-party components

## Realtek / upstream packaging

The accepted external input is a Realtek `RtWlanU` executable, version 1830.32.b27, originally obtained during development from [chris1111/Wireless-USB-OC-Big-Sur-Adapter V17](https://github.com/chris1111/Wireless-USB-OC-Big-Sur-Adapter). Credit Realtek for the radio/data-path driver and chris1111 for that distribution and connection-utility packaging.

The upstream README states that the driver source is unavailable. No separate permission establishing redistribution rights for our modified Realtek binary was identified during this release preparation. This repository therefore includes neither the original nor modified Realtek executable, firmware or Wi-Fi utility. Users supply a compatible original locally. The patcher preserves vendor files/notices and changes only the documented metaclass import and bundle metadata. The MIT license here does not grant rights over those third-party components.

## Apple

Kernel interfaces and headers are supplied by the user's Xcode SDK. Local builds link the SDK's `libkmod.a`; no Apple library, KDK, system kext, framework or full Apple source file is included in this repository. Apple components retain their own terms. Refer to the SDK terms before distributing locally linked binaries.

The framework-lifecycle research referenced [Apple's XNU IOService source](https://github.com/apple-oss-distributions/xnu/blob/main/iokit/Kernel/IOService.cpp). The tests contain a small behavioral model, not the complete native framework.

## OpenCore / patching tools

OpenCore configuration is used for injection; this repository does not bundle OpenCore. Refer to [OpenCorePkg](https://github.com/acidanthera/OpenCorePkg) and its documentation for configuration and validation. Unrelated audio/Intel Wi-Fi root patches may remain; the tested native USB configuration excluded legacy USB root patches.
