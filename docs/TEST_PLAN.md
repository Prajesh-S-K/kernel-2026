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
