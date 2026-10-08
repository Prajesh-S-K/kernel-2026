# EVIDENCE · v0.2.0 pre-hardware release

Verified 8 October 2026 on this macOS host. Source version 0.2.0; local release tag `v0.2.0-prehardware`. Prior tag `v0.1.0-prehardware` is preserved. This document distinguishes software checks from physical qualification.

| Item | Evidence / status |
|---|---|
| Shared C++ engine | Built with AppleClang 17, C++17; core warnings treated as errors |
| Core invariants | **42 passed, 0 failed**; [named results](../evidence/control-tests.log); includes 100,000 simulated ticks, fault and persistence checks |
| Desktop/serial protocol | **19 Python checks passed**; full save/restart/replay integration plus simulated serial acknowledgement correlation and virtual-input isolation |
| Lab metric/export tests | **13 JavaScript checks passed**; empty/miss/abort cases, summed nominal rate, nested-profile CSV |
| Combined software run | **74 named checks passed** (all original 51 retained); [software log](../evidence/software-checks.log); undefined-behavior sanitizer enabled for C++ |
| Address sanitizer | Runtime stalled on this macOS host; stopped; **not counted as passed**. Optional ASAN configuration retained |
| ESP32-S3 cross-build | **Both real-sensor and simulated-sensor environments built successfully** with pinned PlatformIO/NimBLE; [build log](../evidence/firmware-build.log) |
| Browser setup | Actual native calibration generated/saved a valid synthetic asymmetric profile (left≈50, right≈25, up≈60, down≈30 px/degree); no real-person inference |
| Browser control | Observed ACTIVE → injected disconnect → SAFE_STATE → healthy recovery READY; explicit resume; dwell ARMING and LOCKOUT observed; pause exercised; switch-driven drag moved a practice target 3px and logged selection/release |
| Browser lab | 12-target automated host-pointer block completed; a separate deliberate miss/user abort and a viewport-change abort retained raw; [automated rows](../evidence/automated-browser-trials.jsonl) are **UI smoke data, never participant performance** |
| Visual checks | Desktop 1280×900 and mobile 390×844 at 130% scale inspected, with reduced motion/glow controls; generated profile, motion trace, state labels and responsive controls; final [setup screenshot](../evidence/setup-v02.png) |
| Formatting/lint | Pinned clang-format, Ruff, Prettier and ESLint checks pass; runtime remains framework-free |
| Release integrity | Regression rejects dirty/stale source and modified build artifacts; full gate stamps source plus native/firmware hashes |
| Transport | Two-second native/serial and three-second browser deadlines tested; partial replies, malformed/nonfinite JSON, dead processes and missing acknowledgements checked |
| Versioning | Independent local repository; no remote publication; build/runtime outputs ignored |

## Hardware-required status

Physical upload/boot, exact GPIO assignment, MPU6050 axis/gravity signs and rate/noise, NVS power-loss transactions, BLE pairing/encryption/subscription/report ordering, switch electrical debounce, buzzer circuit, USB bridge transport, host disconnect-release behavior, watchdog/brownout recovery, latency, user comfort and performance all remain **UNVALIDATED**. No battery/Wi-Fi/cloud feature was added.

The firmware image is compile-ready with disabled GPIOs; physical integration still requires assigning qualified pins and measuring the actual mount/parameters. No percentage-complete hardware claim is made. A standard HID host uses its own cursor outside the companion. The lab displays observed nominal target metrics, not certified Fitts throughput or adaptation benefit.

## Primary implementation references

MPU6050 register settings and conversion constants follow the manufacturer's [MPU-6000/MPU-6050 register map](https://invensense.tdk.com/wp-content/uploads/2015/02/MPU-6000-Register-Map1.pdf). The BLE adapter was checked against the pinned [NimBLE-Arduino 2.3.6 server example](https://github.com/h2zero/NimBLE-Arduino/blob/2.3.6/examples/NimBLE_Server/NimBLE_Server.ino), [server API](https://github.com/h2zero/NimBLE-Arduino/blob/2.3.6/src/NimBLEServer.h) and [characteristic API](https://github.com/h2zero/NimBLE-Arduino/blob/2.3.6/src/NimBLECharacteristic.h). These references support implementation choices, not physical validation.

Reproduce: `sh scripts/check.sh`, then `pio run -e esp32s3 -e esp32s3-sim`. Build outputs and verification logs are separate from the source. Firmware hashes are recorded in the delivered artifact manifest. The GitHub workflow is prepared but has not run remotely.

## Browser verification scope

The v0.2.0 walkthrough observed native calibration/save, pointing, switch-driven drag/release,
scroll output, dwell arming/lockout, pause, HID-failure Safe State, qualified recovery, separate Lab
blocks, matched temporary selection settings and viewport abort. Desktop/mobile screenshots are
stored in `evidence/*-v02.png`. Mobile setup measured content width equal to the 390px viewport.
The final browser error log was empty. CSV generation, frozen context and persistence-failure export
are covered by JS tests; the browser export button was invoked without a console error. The in-app
browser's download-event observer timed out, so filesystem retrieval of that browser download is
not counted as verified. Raw JSONL and a reproducible smoke CSV are included instead.

Firmware UART/USB saturation, actual acknowledgements, electrical stopping and BLE notification
behavior remain physical tests. Cross-compilation does not establish real-time hardware behavior.

# Hands-free revision (unreleased) — evidence

Verified on the committed branch `claude/hands-free-revision` (`406fb0d`, three implementation commits on top of
`8454580`) in a clean clone under a neutral path. This revision is **software and simulation only**.
Nothing below establishes hardware behaviour, accidental-trigger rates, comfort, training burden or suitability.

## Checks

| Group | Baseline (unchanged, still passing) | Added by this revision | Now |
|---|---:|---:|---:|
| C++ named checks (`nodx_tests` / `nodx_handsfree_tests`) | 42 | 108 | 150 |
| Python (`unittest`) | 19 | 21 | 40 |
| JavaScript (`node --test`) | 13 | 14 | 27 |
| **Total named checks** | **74** | **143** | **217** |

Each named check counts once; a test executable is not counted as one check. Logs:
[software gate](../evidence/handsfree-software-checks.log) (formatting, lint, UBSAN native build, Python,
JavaScript), [named C++ results](../evidence/handsfree-tests.log), [firmware builds](../evidence/handsfree-firmware-build.log).
`ctest` reports 2 test programs (the two C++ executables); the 150 named C++ checks are inside them.

Firmware: `esp32s3` (774,509 B flash), `esp32s3-sim` (781,841 B) and `esp32s3-n16r8-bench` (778,189 B) all built;
GPIO defaults remain disabled and `NODX_ENABLE`/`NODX_BUZZER` default to -1. Compilation is not hardware
qualification, and the DIO/QIO image-header question from the N16R8 note remains open.

## Mutation spot checks (development-time, not committed)

Each deliberate defect made the suite fail, which shows the tests can detect these regressions:
recognition not suppressing output (2 failures), OFF edge not releasing immediately (2), no neutral
requirement after execution (1), the enable gate ignored on resume (2), drag not cleared on stop (6+). The first
attempt at the suppression and scroll test passed vacuously and was rewritten until the unmodified pattern
demonstrably scrolls and moves the pointer when nothing suppresses it. A real defect was also found by the
tests: the first `configId` hashed its own checksum and was constant.

## Browser walkthrough (manual, in-app browser; not an automated test)

Isolated companion on its own port and runtime directory: calibration; hands-free setup view; training of both
gestures through the UI including a deliberate retry (an idle timeout was rejected and recovered); explicit
conversion and save (dwell off before, on and locked after); daily use with no resume/pause/select button
(gesture resume, dwell ARMING → PROGRESS → click → LOCKOUT, drag on/off, switch OFF paused at once and ON did not
resume, gesture while OFF ignored); save-failure display with the earlier setup kept and the error cleared after a
successful retry; a Lab block aborted by a pause gesture. Layout had no horizontal overflow at a phone-width
viewport; all 14 new controls have names and are tabbable; the reduced-motion switch applies; live regions made 0
DOM mutations in 4 s of idle polling (after a fix: they were being rebuilt every poll). Bugs found and fixed by
this walkthrough: a CSS class collision (`.chip`), a nested telemetry field read at the wrong level, the cursor
showing in the new view, and live-region churn. Screenshots:
[desktop](../evidence/handsfree-setup-desktop.jpg), [mobile](../evidence/handsfree-setup-mobile.jpg).
Not exercised in the browser: a full 12-trial hands-free Lab block (covered by unit tests only), keyboard Tab
traversal by hand, and a real screen reader.

## Wokwi diagnostic

`wokwi/handsfree-diagnostic` compiles unmodified with PlatformIO (ESP32-S3, 0 warnings). In the Wokwi web
editor (no sign-in, nothing saved) it loaded without wiring errors and ran: banner, switch ON, `WHO_AM_I = 0x68`,
live accel lines with `fail=0`, a live click printed `CONTROL SWITCH: OFF (inhibited)` then `ON (permitted)`, and
with the SDA wire removed it printed `no I2C reply` / `SENSOR NOT FOUND / WRONG ID` and re-probed every 5 s.
Part types and pins were checked against docs.wokwi.com (MPU6050, slide switch) and Wokwi's published ESP32-S3
DevKitC-1 pin map (`wokwi-boards`); the board *part type* came from that repository and a public project, not a
docs.wokwi.com part page. **Not verified:** dragging the MPU6050 sliders, the 30 ms debounce timing, and anything
physical. Screenshots: [run](../evidence/wokwi-diagnostic-run.jpg), [missing sensor](../evidence/wokwi-missing-sensor.jpg).

## Known limitations and open hardware-required checks

* Gesture thresholds, the neutral rule, learned margins and the claim that they separate command from normal
  movement are START values tested only on synthetic input. A rejected candidate costs up to about one stroke of
  pointing. The suppression/discard design needs real-user accidental-activation measurement.
* Firmware command parsing (`firmware/src/commands.cpp`) is compile-checked and mirrors the tested native parser; it is
  not executed by any automated test. `nodx_handsfree_tests` does not cover the NVS adapter.
* A torn first configuration write fails closed to CONFIG_INVALID until a helper repairs it; real NVS atomicity is untested.
* Unchanged open items: MPU6050 data-ready/`INT_ENABLE` initialisation, exact N16R8 memory and USB configuration, BLE
  pairing/report behaviour including drag during disconnect, electrical enable-switch wiring and bounce, gesture comfort
  and accidental activation with real users, and every START value. **This is not V1 hardware readiness.**
