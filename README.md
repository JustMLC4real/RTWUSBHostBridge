# RTWUSBHostBridge

Experimental legacy-USB ABI bridge for the **ASUS USB-AC68 (`0b05:1817`, RTL8814AU)** on Intel macOS Tahoe, using the native `IOUSBHostFamily` stack.

**Status: `v0.1.0-alpha.1`, experimental source prerelease.** One adapter and one Intel laptop have been tested. Sleep/wake, hot unplug, failed-start cleanup and complete USB dispatch coverage are not qualified. These paths can hang or crash a kernel. Keep a separately tested boot/recovery EFI.

## What it does

`RTWUSBHostBridge.kext` provides compatibility facades for the legacy USB interfaces used by a specific Realtek driver. A local patcher redirects one metaclass import in a separately supplied `RtWlanU.kext` to the bridge. It preserves the original radio/data-path machine instructions and changes bundle matching/dependencies.

The output is two bundles, loaded in this order:

1. `RTWUSBHostBridge.kext`
2. `RtWlanU_HostCandidate.kext`

The candidate retains `com.realtek.driver.RtWlanU`, the original module identity. **Disable every original copy of that driver before loading the candidate.** They must not be loaded together.

This repository contains no Realtek executable, firmware, connection application, complete EFI, Apple binaries or device-specific OpenCore configuration. Users provide their own compatible original driver and connection utility.

## Tested scope

| Item | Observed |
| --- | --- |
| Adapter | ASUS USB-AC68, `0b05:1817` |
| Computer | Lenovo 320S-13IKB, Intel x86_64 |
| System | macOS 26.7.1, build `25G241`, Darwin `25.6.0` |
| Native USB | Loaded IOUSBHostFamily, AppleUSBCommon and AppleUSBXHCI UUIDs matched the native KDK; legacy IOUSBFamily absent |
| Radio | Scan, association and HTTPS data transfer succeeded |
| Short transfer | A 10 MB HTTPS download through the ASUS on native USB measured approximately 164 Mbit/s; one short sample, not a maximum-speed benchmark |
| Other root patches | Intel Wi-Fi/audio patches were reapplied with legacy USB disabled; ASUS HTTPS still worked after reboot |
| USB link | High Speed observed; SuperSpeed operation not established |

Only this device ID is enabled. Apple Silicon, other Realtek variants and other macOS builds are untested. See [known issues](docs/KNOWN_ISSUES.md) and [test scope](docs/TESTING.md).

## Build locally

Requirements: macOS, Xcode with Intel kernel USB headers and `libkmod.a`, Python 3, and a separately obtained original driver. The accepted executable is **RtWlanU 1830.32.b27**, SHA-256:

```text
7e8e7a4d6ced1b85b3854c896011feb6227a430c49fd738322f76b1b0d819ba3
```

Other executables are rejected. The original distribution used during development was [chris1111/Wireless-USB-OC-Big-Sur-Adapter V17](https://github.com/chris1111/Wireless-USB-OC-Big-Sur-Adapter). Its Realtek code and utility are separate third-party components; see [third-party notices](THIRD_PARTY.md).

Run the component tests first:

```sh
python3 tools/test.py
```

Build the two bundles into a **new local output directory**, explicitly enabling the experimental ASUS personality:

```sh
python3 tools/build.py --driver /path/to/original/RtWlanU.kext --enable-alpha
```

The output is `dist/`. Without `--enable-alpha`, bridge matching stays disabled. The tools never install/load a module, change EFI/security settings or patch system files. Built bundles use local ad-hoc signatures, not Apple production signing/notarization. A new local build has not automatically inherited the original hardware test result.

## External-EFI test installation

1. Back up your EFI and verify an independent boot/recovery path. Initially test on a separate EFI.
2. Ensure original `RtWlanU` copies are disabled, including copies in `/Library/Extensions` or other injected bundles.
3. Add both generated bundles to `EFI/OC/Kexts`. Merge the entries from [the Kernel/Add example](examples/Kernel-Add.plist) into your own config, bridge first. Do not replace your entire config with this fragment.
4. Validate your own configuration with the `ocvalidate` matching your OpenCore version.
5. Boot the test EFI with the ASUS disconnected, then attach it after macOS starts. Use the separately supplied USB-WiFi utility to join a network.

The intended configuration uses native Tahoe USB, **without a legacy USB root patch**. Removing existing root patches also affects unrelated patches; follow your patcher's supported revert/reapply procedure and preserve a system recovery route. This project does not automate root-patch removal or disable SIP/AMFI/Gatekeeper. Its signatures have not been qualified under all security configurations.

Verify `RTUSBInterface → RtWlanU → IOEthernetInterface` in IORegistry and confirm real traffic through that interface. A network list or successful OpenCore injection alone is insufficient. Current receive byte/packet counters are unreliable; use interface-bound traffic with alternate connections disabled for a route-exclusive test.

## Recovery

Boot your known-working recovery EFI and remove/disable both experimental entries. On native Tahoe USB, restoring the original driver alone will not make it compatible. Restoring an EFI also does not restore removed system root patches. Keep your patcher's supported system recovery method available.

## License and attribution

The original bridge source, local patcher and documentation are offered under [MIT](LICENSE). This does not license the separately supplied Realtek driver, utility, Apple SDK or their generated/linked components. No third-party executable is included in the source release. Credits and source references are in [THIRD_PARTY.md](THIRD_PARTY.md).
