# Evidence and test scope

Hardware observations on 2026-10-17 used an ASUS USB-AC68 (`0b05:1817`) and Lenovo 320S-13IKB, x86_64, macOS 26.7.1 build 25G241 / Darwin 25.6.0.

The first bridge personality omitted `bConfigurationValue` and `bInterfaceNumber`. A read-only `IOServiceMatchPropertyTable` comparison against the actual provider returned no match for the original profile, configuration alone, or interface alone, and a match with both configuration 1 and interface 0. Both fields are included in the build metadata here.

After that correction the new `RTUSBInterface` service had a live `RtWlanU` child and network interface. Scan, association and HTTPS data transfer worked. A Wi-Fi-only 10 MB transfer, with other physical network interfaces inactive, succeeded on the earlier patched USB-stack stage.

After removing legacy USB root patches, the loaded Intel module UUIDs matched the 25G241 native KDK:

| Module | UUID |
| --- | --- |
| IOUSBHostFamily | AB1CBC87-FCA9-34C1-8F95-30E490B92D2D |
| AppleUSBCommon | 5032090B-9D24-3409-8CB3-72DAD9C5C94F |
| AppleUSBXHCI | 4F5E24C5-4FEA-32DA-A8AD-D2FEFEF3ABF2 |

Legacy IOUSBFamily was absent; the native device was IOUSBHostDevice. The bridge and candidate were active. Interface-bound HTTPS returned HTTP 200 and a short 10 MB native-stack download measured about 164 Mbit/s. Ethernet remained attached during this native-stack download; the tool explicitly selected the ASUS interface. Do not turn that one sample into a maximum-speed claim.

Intel Wi-Fi/audio patches were then reapplied with USB disabled. After a subsequent reboot, patch metadata listed only those groups, the native host-family UUID remained active, both trial drivers were loaded, and ASUS-bound HTTPS still succeeded.

## Component tests

`python3 tools/test.py` runs AddressSanitizer/UndefinedBehaviorSanitizer tests of transport, sessions and power events. It also extracts unchanged production close/termination method bodies into a fake-framework fixture, runs ASan/UBSan and ThreadSanitizer, and checks that a deliberate arbitration-lock regression fails.

`tools/build.py` compiles the x86_64 kernel classes and verifies 58 selected legacy vtable slots. The local patcher rejects an unexpected original executable and checks that the original executable instruction section is preserved.

These checks do not reproduce the complete kernel, prove every callback ownership edge, or qualify sleep/unplug/error paths. Public CI does not have a Realtek binary or USB hardware. Private EFI backups, network identifiers, addresses and raw logs are excluded from this repository.
