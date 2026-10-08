# MASTER_BLUEPRINT · frozen V1

Version 0.1.0 · 8 October 2026. Scope recovered from the original design discussion and the explicit build request. This is a software baseline for a physical prototype, not a hardware-qualified release.

## Product contract

ESP32-S3, MPU6050 on a rigid headband, an accessible momentary selection switch, pause/calibration controls. A buzzer is deferred: its type, voltage and current rating are unknown, it stays disconnected, and `NODX_BUZZER=-1`. USB provides power/programming/debugging. BLE HID provides target interaction. Yaw → pointer X; pitch → pointer Y; roll → wheel. Switch press/release → mouse down/up, including hold/drag. Dwell is opt-in; movement cancels it, and meaningful movement is required after a click. Pause stops pointer/wheel, releases the button and disables dwell.

Personal calibration measures rest and comfortable directional movement. It generates bias, noise-dependent deadzones, directional gain and bounded smoothing. Smaller comfortable movement receives greater directional gain; excessive noise or insufficient controllable movement fails calibration rather than producing a misleading profile. Natural movement supplies a final sample window; it does not currently infer a medical ability or comfort score.

No battery, Wi-Fi, cloud, extra gestures, AI classifier, medical claims, OS cursor replacement or universal host compatibility. Compatible-host claims require individual BLE tests. No claim that adaptation improves performance until measured user trials support it.

## Frozen pipeline

```text
SimulatedSensor / ReplaySensor / MPU6050Sensor (future sensors implement Sensor)
  → SensorManager
  → AxisTransform
  → MotionProcessor (bias, EMA, deadzone, complementary roll, stability estimate)
  → AdaptiveEngine (directional gain, continuous speed gain, scroll threshold)
  → IntentEngine (state permission, scroll/pointing arbitration)
  → InteractionEngine + SelectionManager (switch + dwell; pause owned by System)
  → SafetyManager
  → HIDManager
  → HIDTransport (simulated reports / encrypted subscribed BLE mouse)
```

**SafetyManager is immediately before HIDManager.** There is one output path. Safety checks active state, sensor health, validated profile, connection and finite calculations, then clamps movement/wheel. HIDManager quantizes fractional deltas, tracks remainders and emits a dwell press/release pair. Non-active/fault transitions reset remainder/filter/selection state. The desktop simulator never sends system mouse events.

## Module map

| Module / file | Responsibility |
|---|---|
| `core/include/nodx/sensor.hpp`, `core/src/sensor.cpp` | Standard timestamp/gyro/acceleration/valid sample; synthetic/fault input; timestamp-respecting replay; MPU register adapter; coordinate transforms; input health |
| `core/include/nodx/profile.hpp`, `core/src/profile.cpp` | Bounds, explicit versioned wire encoding, CRC32, two-slot generation selection, validate/save/read-back; in-memory fault storage |
| `core/include/nodx/calibration.hpp`, `core/src/calibration.cpp` (`CalibrationEngine`) | REST, LEFT, RIGHT, UP, DOWN, NATURAL, ANALYZE, VALIDATE, PROFILE_SAVE; candidate kept separate from active profile |
| `core/include/nodx/motion.hpp`, `core/src/motion.cpp` (`MotionProcessor`, `AdaptiveEngine`) | Sensor-to-motion processing, asymmetric gain, precision/normal/travel response, roll scroll |
| `IntentEngine` in `motion`, `InteractionEngine` in `output.hpp`, `SelectionManager` in `selection` (`core/src/selection.cpp`) | Arbitration, command composition, switch debounce/drag, dwell arming/progress/cancel/lockout |
| `core/include/nodx/output.hpp`, `core/src/output.cpp` (`SafetyManager`, `HIDManager`) | Final permission and bounds; fractional reporting and button release |
| `core/include/nodx/system.hpp`, `core/src/system.cpp` (`System`, `Diagnostics`, `Feedback`) | State transitions, persistence coordination, reason/counters/cursor state; nonblocking state-change buzzer pulse logic (no buzzer is connected) |
| `desktop/main.cpp` | Native engine process, durable POSIX file slots, line protocol, raw recording, CSV replay |
| `desktop/server.py` | Loopback HTTP; serialized engine requests; raw trial JSONL; optional USB serial adapter |
| `firmware/src/adapters.hpp`, `runtime.cpp`, `commands.cpp`, `telemetry.cpp`, `main.cpp` | `adapters.hpp`: I²C, NVS and BLE report service. `runtime.cpp`: GPIO controls and sampling scheduler. `commands.cpp`: bounded serial commands. `telemetry.cpp`: fixed-size telemetry. `main.cpp`: Arduino entry points only |
| `ui/` | Setup/calibration, live trace/profile, geometric animated state cursor, control demo, Fitts-style lab |

## System states

```text
BOOT → load/validate → READY or CALIBRATION_REQUIRED
calibrate → CALIBRATING → candidate validated and saved → READY
explicit resume + ≥20 healthy samples + valid profile + BLE ready → ACTIVE
pause → PAUSED → explicit resume → ACTIVE
critical fault → SAFE_STATE → verified healthy recovery → READY
missing/invalid profile after recovery → CALIBRATION_REQUIRED
```

SAFE_STATE never resumes automatically. Recovery means at least 20 consecutive healthy input samples and a usable connection. A held switch cannot become a new press after pause/fault; release is required first. A failed/cancelled calibration preserves the prior profile. Sampling/clock faults, invalid axes, nonfinite/extreme data, invalid required profile and failed output delivery inhibit output. Disconnection cannot transmit a release to an absent host; the first connected non-active report is released. Host behavior after loss must be physically tested.

## Sensor conventions

Gyro is degrees/second; acceleration is g. Raw sensor axes are independent of semantic yaw/pitch/roll. The default START gyro mapping is `{Z,X,Y}`. Gravity mapping uses `{-sensor X, sensor Z}` for positive roll about sensor Y; both sign/permutation sets are explicit and validated. Verify this convention against the actual rigid mount before enabling pointing. Desktop inputs are already yaw/pitch/roll and use an identity gravity convention.

MPU6050 uses address 0x68, WHO_AM_I 0x68, ±250°/s, ±2g, data-ready checking and a 14-byte read. Driver coefficients are 131 LSB/(°/s) and 16384 LSB/g. Future sensors only implement `Sensor::read`; downstream logic stays shared. Repeated identical numeric values with advancing timestamps are not called a failure: a stationary user can produce them. Frozen/non-increasing timestamps and missing data are detected; a sensor that internally freezes while continuing to assert new data-ready frames needs physical qualification/additional evidence.

## Companion and experiment contract

Connect/system-check presentation reflects a running simulated engine; it does not attest BLE hardware qualification. Synthetic User C uses different left/right and up/down input amplitudes. Calibration actually feeds the core and saves its result. UI badges distinguish SIMULATED versus HARDWARE; USB telemetry enables later live calibration without redesign. Custom cursor states include NORMAL, PRECISION, TRAVEL, DWELL_ARMING, DWELL_PROGRESS, CLICK, DRAG, SCROLL, PAUSED, WARNING and SAFE_STATE. Text labels accompany color; reduced motion, glow preference, scaling and keyboard focus are supported.

Lab blocks use 12 circular targets with three diameters. Record every attempt: condition, source, profile/hash, target/start/end geometry, distance, times, hit/miss, dwell cancellations and aborted status. Show hit rate, misses, mean selection time and **nominal successful ΣID/Σtime**, with `ID=log2(D/W+1)`. Selection time includes dwell and errors are never erased. This is exploratory, not ISO conformance or effective-width throughput. Generic versus adaptive runs keep their own actual settings; order must be counterbalanced in real experiments. Host-pointer trials are not automatically attributed to NodX hardware.

## v0.2.0 module and safety refinement

The frozen V1 pipeline is unchanged. See MODULE_WALKTHROUGH for the separated core, adapters,
transport, presentation and Lab responsibilities. System state and accepted profile are read-only
views. Proposed changes are validated commands. Pause/calibration/profile acceptance requests a
stationary release through SafetyManager → HIDManager immediately, before storage or another sample.
Delivery failure enters Safe State. Recovery requires 20 consecutive valid system checks and neutral
output delivery, then explicit resume with held-switch release. Invalid inputs/mapping/timing/profile
skip motion/adaptation math. Physical connection/release behavior still needs host validation.
