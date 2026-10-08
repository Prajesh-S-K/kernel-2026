# START parameter registry

All numeric values below are experimental starting settings. Bounds are defensive software limits, not medical limits. Changes require entry in PARAMETER_CHANGELOG and evidence before hardware release. Source: `core/include/nodx/parameters.hpp`, `UserProfile`, and explicitly named firmware/processing constants.

| Parameter | START | Unit / bound | Hardware/user measurement needed |
|---|---:|---|---|
| Sample cadence | 10 | ms, nominal 100Hz | Data-ready cadence, scheduler jitter, processing cost |
| I²C poll | 2 | ms real driver; fresh samples only | CPU load and actual sensor cadence |
| IMU/loop timeout | 100 | ms | Measured longest healthy interval vs stopping latency |
| Recovery samples | 20 | consecutive valid | Fault recovery latency; never automatic ACTIVE |
| Input gyro maximum | 240 | °/s per raw axis | Clipping and qualified ±250°/s range |
| Input accel maximum | 4 | g per raw axis | Reject nonfinite/extreme; physical range is ±2g |
| Gravity norm window | 0.2–3 | g | Motion/impact logs; not a wearer's safety rating |
| Filter alpha | 0.35 generic | valid 0.05–1 | Jitter vs latency |
| Calibrated alpha | `clamp(0.5/(1+σX+σY),0.15,0.5)` | dimensionless heuristic | Real noise and preference |
| Deadzone base / noise multiplier | 0.6 / 3 | °/s; `D=base+kσ`, profile 0.1–15 | Preserve intentional movement while suppressing rest |
| Direction gain | 30 generic | px/degree; valid 1–120 | Screen travel vs comfortable movement |
| Calibrated gain target | 600 / direction mean | px/(°/s), clamped 1–120 px/degree | Task/user tests; limited motion acceptance |
| Precision / fast thresholds | 5 / 35 | °/s; 1–20 / >precision+1 to100 | Speed-region usability |
| Speed gain | 0.35 base; +0.65 precision ramp; +1 fast ramp | dimensionless continuous 0.35–2 | Movement precision/travel and fatigue |
| Dwell arming | 250 | ms | Accidental arming vs response |
| Dwell time | 1000 | ms; valid 500–5000 | User-selected preference, cancellation/Midas touch |
| Dwell tolerance | 8 | estimated output px; valid 2–50 | Host acceleration/edges, stability |
| Dwell unlock multiplier | 1.5 | ×tolerance | Re-arm with deliberate movement |
| Dwell enabled | false | opt-in | User preference |
| Switch debounce | 30 | ms | Scope/logic-analyzer contact waveform |
| Scroll angle threshold | 12 | degrees; valid 5–45 | Roll isolation and comfortable tilt |
| Scroll gain | 0.8 | wheel units/s/degree beyond threshold; 0.05–3 | Host wheel behavior, intended speed |
| Scroll enabled | true | independently configurable | User suitability |
| Roll complementary blend | 0.98 / 0.02 | gyro / gravity each sample | Mount axis/sign, drift, acceleration rejection |
| Calibration window | 1500 | ms each of six windows | Sustainable comfortable motion and sample cadence |
| Minimum window samples | 100 | count | Actual rate; fail sparse acquisition |
| Minimum direction mean | 3 | °/s; also >2×deadzone | Limited range/noise tradeoff |
| Max direction variation | 0.8×mean | standard deviation | Repeatability and movement quality |
| Rest sigma / bias maximum | 4 / 10 | °/s | Bias/environment and noise |
| Output pointer/wheel limits | ±40 / ±5 | counts per report; HID descriptor ±127 | Emitted rate/host motion; no claim of px/s cap |
| I²C frequency/timeout | 100000 / 20 | Hz / ms | Wiring, pull-ups, bus recovery |
| MPU DLPF / divider | 3 / 9 | registers; nominal 100Hz | Actual register settings and response |
| GPIOs | -1 | disabled | Board/pin/circuit qualification |
| MPU address / WHO_AM_I | 0x68 / 0x68 | register constants | AD0 wiring and device identity |
| MPU scale | ±250°/s / ±2g | 131 LSB/(°/s), 16384 LSB/g | Raw conversion/real sensor checks |
| Gyro map | yaw=Z, pitch=X, roll=Y; signs + | START mount | Verify all six signed motions |
| Gravity map | mapped `{sensorY,-sensorX,sensorZ}` | START roll-Y convention | Gravity roll agrees with gyro roll |
| Reprobe/telemetry | 1000 / 200 | ms (telemetry 5 Hz, see v0.2.0 table; earlier value was 500) | Recovery and serial bandwidth |
| Buzzer transition pulse | 80 normal /300 Safe State | ms; buzzer deferred, type and rating unknown, `NODX_BUZZER=-1` | Identify buzzer type, voltage and current first |

The lab's 32/56/80px target diameters, 12 trials and geometry are experiment design constants, not validated accessibility settings. The stability readout `1/(1+filtered speed)` is an illustrative motion metric, not a measured personal ability/comfort score.

## v0.2.0 transport/scheduling constants (not user tuning)

| Value | Setting / units | Acceptance |
|---|---|---|
| Native/serial reply | 2 seconds | Deadline regression; physical USB reconnect still required |
| Browser reply | 3 seconds | Aborted-request regression |
| Firmware periodic telemetry | 200 ms / 5 Hz | Cross-built; board sampling jitter must be measured |
| Firmware command line | 80 bytes; 64 read bytes per iteration | Envelope/truncation check; board parser acceptance later |
| Telemetry frame | 2048 bytes; two pending acknowledgements | Bounded fixed storage; physical saturation later |
| Firmware transmit budget | ≤64 available bytes per iteration | Nonblocking code; actual driver behavior requires board measurement |

All V1 motion/filter/selection defaults and wire bytes are preserved. Recovery requires 20
consecutive healthy checks including successful neutral output while connected. A fault resets
qualification; a held switch must release and the user must resume explicitly.

## Hands-free START values (unreleased, unvalidated)

| Parameter | START | Unit / bound | Measurement needed |
|---|---:|---|---|
| Gesture smoothing | 0.5 | EMA alpha | Noise vs latency on the real sensor |
| Neutral hold before arming | 300 | ms | Accidental-trigger rate vs responsiveness |
| Slow-motion disarm | 120 | ms in [0.5·enter, enter) | Pointing that resembles a stroke start |
| Stroke end / cross-axis ratio | 0.5 / 0.6 | × enterRate / × peak | Single-axis cleanliness of real nods and tilts |
| Minimum trained peak | 20 | °/s | Weakest comfortable movement of target users |
| Training examples / rejects allowed | 4 / 8 | count | Training burden |
| Rest capture | 1000 ms, ≥80 samples | σ ≤ 4 °/s, \|mean\| ≤ 10 °/s | Real rest noise and bias |
| Example wait / capture / validation | 6000 / 3000 / 20000 | ms | Helper pacing |
| Learned neutralRate | clamp(3σ+1.5, 2, 8) | °/s, record bounds 1–10 | Real noise |
| Learned template | peakMin 0.5×min, peakMax min(240,1.5×max); enter 0.5×peakMin; stroke 0.5×min–2×max; gap max(2×max,150); total 1.5×max | see spec bounds | Whether these margins fit real repeats |
| Enable debounce | ON stable 30 ms, OFF immediate | ms | Switch bounce on a scope |
| Enable GPIO | -1 (disabled); candidate GPIO4 | pin | Board schematic, wiring |
| Dwell in hands-free | existing dwell values; lockout after resume/drag | ms / px | Unwanted-click rate, comfort |

Templates learned in training are also START values: they are stored per user and marked unvalidated
until measured. The record format bounds are defensive software limits, not human limits.
