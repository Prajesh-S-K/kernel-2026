# TEST_PLAN

## Software gate

Run `sh scripts/check.sh`. Named invariant results are emitted by `nodx_tests`; failures return a nonzero exit. Deterministic tests avoid medical or usability assertions.

| Coverage | Check |
|---|---|
| Calibration | Low/high rest noise; asymmetry; too-small range; unstable/sparse input; save failure retains last profile |
| Processing | Bias correction; EMA response/reset; deadzone boundaries; continuous acceleration; directional gain; roll arbitration/disable |
| Selection | Debounce bounce/press/hold/release; pause-held switch lock; dwell one-click lockout, meaningful unlock, cancellation at 95%, switch/scroll cancel |
| Profiles | Wire round-trip; every-byte corruption; schema with recomputed CRC; NaN/range validation; failed/torn slot; fallback; generation overflow |
| Sensors | NaN, extreme, absent, stale/frozen timestamp; gravity bounds; wrap/backward clocks; MPU signed conversion and absent device; replay timestamps/EOF |
| Safety/output | Stationary fault reports; calibration abort on fault; 20-sample recovery plus explicit resume; BLE loss/reconnect; notification failure; invalid profile; finite output bounds; fractional output; dwell release; 100,000-tick stress |
| Desktop integration | Full calibration → save → process restart/load; corruption repair; fault recovery; raw record/replay; NaN replay; protocol refusal; HTTP argument bounds; generic preserves adaptive |
| Lab metrics | Empty evidence remains blank; misses/time/accuracy; aborted exclusion; aggregate ΣID/Σtime; nested profile CSV quoting |

## Browser acceptance

Open the loopback companion. Check desktop and narrow/mobile layouts, readable labels, keyboard focus, high zoom, reduced motion and glow toggles. Confirm real progress/profile generation; saved asymmetric profile reloading; animated cursor state labels; pointing/switch/dwell/scroll/pause/fault behavior. Run a complete 12-trial block and a deliberate miss/abort. Confirm raw disk JSONL, CSV export, profile hash and no console errors. Automation-derived host-pointer trials are software smoke evidence only, never participant results.

## Hardware gate (all still required)

| Order | Acceptance evidence to record |
|---|---|
| Board | Correct ESP32-S3 variant/flash configuration; upload/boot/reset logs; no IMU boots safely; measured loop timing/watchdog behavior |
| GPIO/electrical | Qualified 3.3V sensor bus/pull-ups/address; actual pins; clean switch waveform and debounce; buzzer driver/current/feedback behavior |
| MPU6050 | WHO_AM_I; ±250°/s and ±2g configuration; gravity sign and orientation; known motion sign; fresh sample cadence; bias/noise/clipping logs |
| Mounting | Rigid axes; roll gyro agrees with roll gravity sign; yaw/pitch isolation; cable strain and movement comfort checked |
| NVS | Save/reload; interrupted writes at different timings; fallback slot behavior after power cycling; corrupted/missing storage inhibits active control |
| BLE | Encryption/subscription before output; press/release and dwell pulse order; disconnect during drag; reconnect released; notification congestion/failure; host sleep/resume; OS-specific pairing matrix |
| Safety | Physical sensor removal, bus lock, delayed loop, values outside bounds, invalid profile and held switch during recovery; no movement before explicit resume |
| Tuning | Change only registered START values, replay same raw input before/after, record latency/noise tradeoff and user-selected dwell/scroll preferences |
| User/lab | Comfortable short session, accessible pause available, generic/adaptive order counterbalanced, practice consistent, device/profile/source/version logged; report improvements or lack of them honestly |

Do not claim cross-platform compatibility, comfort, effective Fitts throughput or adaptation benefit from software tests. On hardware, measure actual emitted HID rate and host cursor behavior; the core's relative-output dwell estimate does not know host pointer acceleration or screen edges.

## v0.2.0 regressions and release gate

All original 51 named checks remain. Additional cases cover immediate stopping without a new sample,
repeated transport failures, recovery interruption, invalid mapping/profile finite telemetry,
settings-save failure, strict request IDs/truncation/counts/booleans/trailing fields, native dead
process/malformed reply/deadline, partial serial replies/missing acknowledgement, temporary settings,
frozen Lab context/grouping, accurate simulation labels, coalesced polling, separate persistence,
browser timeout and dirty/stale/tampered release refusal. The gate checks four-space C++/Python,
two-space JS, format/lint, native UBSAN and both ESP32-S3 builds.

Browser acceptance additionally checks block abort after profile/state/geometry changes, matched
selection toggles between conditions, per-block results, CSV persistence status and mobile controls.
The GitHub workflow installs pinned development tools; remote execution is not claimed as evidence.

## Hands-free revision (unreleased)

Software gate additions (all deterministic, synthetic input; none proves an accidental-trigger rate,
comfort or suitability):

| Coverage | Where |
|---|---|
| Recognition: exactly once, partial/wrong-order/too-fast/too-slow/weak/strong/diagonal/gap/total rejections, neutral rearm, held posture, rollover, jitter, ambiguity, NaN, determinism, normal-movement corpus (150+ sequences) | `tests/test_handsfree.cpp` |
| Configuration: bounds, distinctness, round trip and identity, every-byte corruption, unsupported/out-of-bounds/truncated, two-slot, save failures, torn write, interrupted multi-record setup (power-loss model), legacy preservation | `tests/test_handsfree.cpp` |
| Training: repeated examples, too few strokes, noise, insufficient motion, off-axis, inconsistent, NaN/inf/extreme/timing faults, indistinguishable or prefix patterns, cancel, immediate release | `tests/test_handsfree.cpp` |
| Pointing vs recognition candidates (learned templates, keyed pointing): bounded `TOO_SLOW` rejection, discarded-never-replayed movement, no candidate below entry or in non-starting directions, ramp onset, corpus with 0 executions, roll-first mitigation | `tests/test_handsfree.cpp` (section J) |
| Conflicts, drag (11 release causes), enable switch, recovery, unconfigured switch, dwell lockout | `tests/test_handsfree.cpp` |
| Enable push button (section K): boot released/held, bounce on press/release/disable, long hold, repeated presses, one toggle per press, immediate disable during movement/scroll/dwell/drag, failed neutral delivery, enable then explicit resume, reboot resets permission, fault recovery not bypassed, invalid config and unwired input stay inhibited, maintained-switch and pre-button record compatibility, kind change at commit | `tests/test_handsfree.cpp`, `tests/test_handsfree_protocol.py`, `tests/handsfree.test.mjs`, `tests/test_firmware_commands.cpp` |
| Native protocol: setup and daily workflow, strict command validation, truthful storage failures, corrupt/oversized records, switch semantics, transport failure, replay determinism, trial-context enforcement, telemetry size | `tests/test_handsfree_protocol.py` |
| Firmware command path compiled and driven natively with the real `firmware/src/{runtime,commands,telemetry}.cpp` over stubbed Arduino/Wire/NVS/BLE headers: parser bounds and backpressure, every new command's valid/invalid arguments, hardware-configuration refusal of simulation commands, the simulated whole pipeline (setup, gesture resume, switch OFF/ON, reboot, fault, corrupt record) | `tests/test_firmware_commands.cpp` (two ctest programs) |
| Companion logic: setup steps, training view, recovery guidance, Lab freezing/invalidation/grouping/raw columns, command queueing | `tests/handsfree.test.mjs` |
| Browser Lab block (manual, recorded): a full 12-trial hands-free block and an aborted block, persisted context/grouping/interruption rows checked | `evidence/handsfree-lab-block.json` |
| Mutation spot checks run during development: removing suppression, switch-OFF release, neutral rearm, the enable gate or drag release each makes the suite fail | recorded in EVIDENCE |

Browser acceptance additions (manual, recorded in EVIDENCE): helper setup → training with retry → save;
daily gesture resume, drag and switch OFF/ON; dwell click without a button; save-failure display;
desktop and mobile layout, keyboard-accessible named controls, quiet live regions, reduced motion.

### Hardware gate additions (all still required)

The stage-by-stage bench procedure, bench PlatformIO environments and local logging tools are in
[BENCH_PLAN](BENCH_PLAN.md) (prepared; nothing has been run on hardware).

| Order | Acceptance evidence to record |
|---|---|
| Enable button / switch | Wiring (GPIO4 to one contact pair, GND to the other, no 3V3/5V; pairs found by continuity testing), clean waveform and real bounce of the exact tactile switch on press and release; the disabling press releases within one loop pass with the sensor stalled; held-at-power-up; unconfigured/disconnected input inhibits; behaviour with a floating input; whether a glitch disabling control is acceptable |
| Gestures | Per user: comfortable distinct patterns, training burden, repeatability across sessions and head mounts, noise at rest, drift; stroke thresholds against real gyro scale |
| Accidental activation | Long unscripted sessions of normal pointing, scrolling, talking, reading, eating and fatigue; count candidates, rejections and executions per hour; report honestly |
| Dwell + drag | Missed/unwanted clicks, lockout comfort, drag release on every stop cause including BLE disconnect during drag and host sleep |
| Recovery | Fault, disconnect and reconnect never resume; qualification; resume by gesture only; held button after power cycle |
| Storage | Hands-free record power-loss behaviour on NVS; corrupt/missing record handling; conversion interrupted at each step |
| Companion | Hardware source labels, training over USB telemetry latency, recovery guidance accuracy |
| Prior hardware items | Sensor initialisation/data-ready, exact N16R8 memory and USB configuration, BLE behaviour remain open |
