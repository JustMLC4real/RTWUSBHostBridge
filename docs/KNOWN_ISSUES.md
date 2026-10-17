# Known limitations

- Only ASUS `0b05:1817` with the exact supported Realtek executable is enabled. This is not a general Realtek replacement.
- Scan, association and data traffic succeeded on one Intel laptop. Sleep/wake, hot unplug/reconnect, repeated stress and failed-start teardown have not passed a full hardware qualification.
- The original driver can call its own stop path after a failed start. The bridge's deferred normal close/termination handling does not intercept that path. Private callback context teardown may overlap callbacks. A pre-stop trampoline was investigated separately but is **not integrated or included** in this release.
- Some legacy ABI slots remain unsupported. Selected vtable slots and observed call sites were audited; whole-program dispatch coverage is not proven. Default-control stall recovery remains unsupported.
- Transmit/no-data timeouts, reset/power notifications and every cancellation ordering are not generally equivalent to the legacy stack.
- Receive packet/byte interface statistics remained zero during successful traffic. Their diagnostic use is unreliable.
- USB 3/SuperSpeed operation is not established. The tested link was High Speed.
- The original radio code, firmware and Wi-Fi utility remain external dependencies. There is no replacement native macOS Wi-Fi menu implementation here.
- Production kernel authentication, every SIP/AMFI configuration, other machines and Apple Silicon are not qualified.
- A local rebuild may differ from the originally tested executable. One release-preparation rebuild had an identical executable instruction section, but toolchain/link-library differences still need to be recorded and future builds hardware-tested. Source and component checks are narrower evidence.
- Internal installation was performed on the test machine, but an independent internal-only boot had not yet been recorded when this release draft was prepared.

Do not label this alpha stable or claim universal Tahoe support.
