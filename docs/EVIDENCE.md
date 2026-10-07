# EVIDENCE · pre-hardware baseline

Verified 8 October 2026 on this macOS host. Source version 0.1.0; local release tag `v0.1.0-prehardware`. This document distinguishes software checks from physical qualification.

| Item | Evidence / status |
|---|---|
| Shared C++ engine | Built with AppleClang 17, C++17; core warnings treated as errors |
| Core invariants | **36 passed, 0 failed**; [named results](../evidence/control-tests.log); includes 100,000 simulated ticks, fault and persistence checks |
| Desktop/serial protocol | **10 passed**; full save/restart/replay integration plus simulated serial acknowledgement correlation and virtual-input isolation |
| Lab metric/export tests | **5 passed**; empty/miss/abort cases, summed nominal rate, nested-profile CSV |
| Combined software run | **51 named checks passed**; [software log](../evidence/software-checks.log); undefined-behavior sanitizer enabled for C++ |
| Address sanitizer | Runtime stalled on this macOS host; stopped; **not counted as passed**. Optional ASAN configuration retained |
| ESP32-S3 cross-build | **Both real-sensor and simulated-sensor environments built successfully** with pinned PlatformIO/NimBLE; [build log](../evidence/firmware-build.log) |
| Browser setup | Actual native calibration generated/saved a valid synthetic asymmetric profile (left≈50, right≈25, up≈60, down≈30 px/degree); no real-person inference |
| Browser control | Observed ACTIVE → injected disconnect → SAFE_STATE → healthy recovery READY; explicit resume; dwell ARMING and LOCKOUT observed; pause exercised |
| Browser lab | 12-target automated host-pointer block completed; a separate deliberate miss and abort retained raw; [automated rows](../evidence/automated-browser-trials.jsonl) are **UI smoke data, never participant performance** |
| Visual checks | Desktop and narrow companion layouts inspected; generated profile, motion trace, state labels and responsive controls; final [setup screenshot](../evidence/setup-desktop.jpg) |
| Versioning | Independent local repository; no remote publication; build/runtime outputs ignored |

## Hardware-required status

Physical upload/boot, exact GPIO assignment, MPU6050 axis/gravity signs and rate/noise, NVS power-loss transactions, BLE pairing/encryption/subscription/report ordering, switch electrical debounce, buzzer circuit, USB bridge transport, host disconnect-release behavior, watchdog/brownout recovery, latency, user comfort and performance all remain **UNVALIDATED**. No battery/Wi-Fi/cloud feature was added.

The firmware image is compile-ready with disabled GPIOs; physical integration still requires assigning qualified pins and measuring the actual mount/parameters. No percentage-complete hardware claim is made. A standard HID host uses its own cursor outside the companion. The lab displays observed nominal target metrics, not certified Fitts throughput or adaptation benefit.

## Primary implementation references

MPU6050 register settings and conversion constants follow the manufacturer's [MPU-6000/MPU-6050 register map](https://invensense.tdk.com/wp-content/uploads/2015/02/MPU-6000-Register-Map1.pdf). The BLE adapter was checked against the pinned [NimBLE-Arduino 2.3.6 server example](https://github.com/h2zero/NimBLE-Arduino/blob/2.3.6/examples/NimBLE_Server/NimBLE_Server.ino), [server API](https://github.com/h2zero/NimBLE-Arduino/blob/2.3.6/src/NimBLEServer.h) and [characteristic API](https://github.com/h2zero/NimBLE-Arduino/blob/2.3.6/src/NimBLECharacteristic.h). These references support implementation choices, not physical validation.

Reproduce: `sh scripts/check.sh`, then `pio run -e esp32s3 -e esp32s3-sim`. Build outputs and verification logs are separate from the source. Firmware hashes are recorded in the delivered artifact manifest. The GitHub workflow is prepared but has not run remotely.
