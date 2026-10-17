# v0.1.0-alpha.1 — experimental source prerelease

Initial source release of RTWUSBHostBridge for ASUS USB-AC68 `0b05:1817` on Intel macOS Tahoe using native USBHost APIs.

- Legacy USB ABI facades over IOUSBHostInterface/Device/Pipe.
- Session retirement, tracked completion accounting and deferred normal client-close/termination handling.
- Correct ASUS configuration/interface matching: configuration 1, interface 0.
- Local, exact-hash patcher for a separately obtained Realtek 1830.32.b27 executable; original radio/data-path instructions preserved.
- Component sanitizers, selected compiler ABI validation, minimal OpenCore entry example and recovery instructions.
- One-machine native Tahoe scan, association and HTTPS transfer observations documented with their limits.

This prerelease contains source and tooling only. It includes no Realtek or Apple binaries, Wi-Fi utility, full EFI or private machine configuration. Sleep/wake, unplug/reconnect, failed-start lifetime handling and complete dispatch coverage remain unqualified and may cause hangs or kernel crashes. See `docs/KNOWN_ISSUES.md` before experimenting.

No stable-release or universal compatibility claim is made.
