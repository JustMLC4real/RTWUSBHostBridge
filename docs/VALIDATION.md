# Release-preparation validation

Local checks completed on 2026-10-17:

- Transport, session and power-event fake-host tests passed under AddressSanitizer and UndefinedBehaviorSanitizer.
- Unchanged production close/didTerminate method bodies passed the fake-framework fixture under ASan/UBSan and ThreadSanitizer. The deliberate held-arbitration regression failed as expected.
- A fresh local build using the selected Xcode SDK and its libkmod.a passed compilation and all 58 selected compiler ABI slot checks.
- The rebuilt bridge executable instruction section was byte-identical to the hardware-tested bridge (28,368 bytes); the full signed binary differs. This does not qualify every future local build.
- The local Realtek patch preserved the original executable instruction section and passed local ad-hoc signature verification.
- An unsupported input executable was rejected before a candidate bundle was created.

These local checks did not load or install the newly built bundles. Hardware observations refer to the earlier tested build and the limits described in TESTING.md. Hosted CI has not run until the repository is published.
