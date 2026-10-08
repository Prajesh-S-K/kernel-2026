# Module walkthrough

Read the engine from the outside inward: `system.hpp` exposes commands and read-only views;
`system.cpp` coordinates the pipeline. It owns state, accepted profile, recovery qualification
and interaction resets. Every ordinary report and immediate stop goes through SafetyManager,
then HIDManager. A failed delivery enters Safe State; healthy input alone cannot resume control.

| File/module | Responsibility | Useful check |
|---|---|---|
| `sensor` | Common samples, simulator/replay/MPU register adapter, freshness and finite checks, axis transform | Sensor/conversion/replay cases in `test_main.cpp` |
| `calibration` | Sample windows, Welford statistics, bounded candidate profile; no storage/HID | Noise, asymmetry, rejection and save-failure cases |
| `motion` | Bias/EMA/deadzone, complementary roll, directional gain, intent arbitration | EMA, directional gain and scroll tests |
| `selection` | Switch debounce, release qualification, dwell arming/cancel/lockout | Selection and held-switch tests |
| `profile` | Stable 84-byte encoding, CRC, numeric validation, two-slot repository | Every-byte corruption and torn-write tests |
| `output` | Final safety gate, output bounds, fractional HID accumulation and pulse release | Safety bounds and immediate stopping tests |
| `system` | State machine, validated commands, fault/recovery coordination; hands-free arbitration | Recovery and stop-delivery regressions |
| `gesture` | Stroke templates, deterministic recognizer, helper training | `test_handsfree.cpp` recognition and training cases |
| `handsfree` | Hands-free record + CRC, two-slot repository, enable gate, status JSON | Configuration, interrupted-save and gate cases |
| `gesture_script` | Synthetic head motion shared by the simulators and tests | Replay determinism and corpus cases |
| `protocol.hpp` | Bounded ASCII envelope and strict request ID parsing, independent of Arduino | Envelope/truncation regression |
| `desktop/main.cpp` | POSIX profile storage, synthetic HID, native commands and JSON, raw/replay runner | `test_protocol.py` save/restart/replay |
| `desktop/transport.py` | Two-second native/serial replies, fragments, correlation, bounded shutdown | Deadline and partial-reply tests |
| `desktop/server.py` | Loopback HTTP validation and separate device/trial locks | Strict HTTP command conversion tests |
| `firmware/adapters.hpp` | NVS, I2C, BLE hardware boundaries and disabled GPIO defaults | Cross-build; physical adapter acceptance still required |
| `firmware/commands.cpp` | Command validation and truthful acknowledgement | Core envelope check; board serial acceptance later |
| `firmware/telemetry.cpp` | Fixed-size frames, acknowledgement priority, bounded nonblocking transmission | Cross-build; UART/USB timing measurement later |
| `firmware/runtime.cpp` | Initialization, sensor cadence, physical controls and scheduling | Both firmware builds; board timing later |
| `firmware/main.cpp` | Arduino initialization/coordination entry points | No device/control logic here |
| `ui/transport.js` | Three-second browser deadline, serialized commands and coalesced polls | Hardening JS tests |
| `ui/app.js` | Setup/control presentation, input events and page coordination | Browser walkthrough |
| `ui/cursor.js`, `source.js` | Cursor presentation and honest source labels | Source-label tests and browser states |
| `ui/handsfree.js`, `handsfree-view.js` | Setup steps, training view-model, recovery guidance, live status chips | `tests/handsfree.test.mjs` and the browser walkthrough |
| `ui/lab.js`, `lab-view.js` | Immutable contexts, block invalidation/grouping, trials and lab presentation | Hardening tests and 12-target browser smoke |
| `ui/metrics.js` | Pure raw CSV and explicitly nominal metrics | Original five metric tests |
| `scripts/release.py`, `package.py` | Verified-source/build hashes and clean-source packaging | Dirty/stale/tampered release regression |

The `engine.hpp` umbrella retains existing includes. There is no alternate desktop control algorithm,
framework, cloud dependency or hidden profile import. Headers describe the public contract; source
files implement it. Profiles are copied for proposed changes and accepted only by `setProfile`.

## Development routine

Install development tools as shown in BUILD_GUIDE. Run `sh scripts/check.sh` after a change.
Use `sh scripts/release_gate.sh` for a complete release check, including both firmware targets.
Individual suites: `./build/nodx_tests`, `./build/nodx_handsfree_tests`, `./build/nodx_firmware_sim_tests` and `./build/nodx_firmware_hw_tests` (the real firmware command path on the host over stubbed Arduino/NVS/BLE headers), `python3 -m unittest discover -s tests -p 'test_*.py' -v`,
and `node --test tests/*.test.mjs`. Use isolated runtime directories for demos and replay.

Format C++ with `clang-format -i`, Python with `ruff format`, and UI/test JS with
`ui/node_modules/.bin/prettier --write`. Code uses four-space C++/Python and two-space JS,
with a 100-character target width. Names should identify meaning and units; guard clauses
reject invalid work before state changes. Comments explain constraints or decisions.

## Reading the hands-free code

Start with [HANDS_FREE_SPEC](HANDS_FREE_SPEC.md), then `system.hpp` (new commands and
`handsFreeStatus`), the `tick` order in `system.cpp` (health → gate → recognition → action →
suppressed composition → SafetyManager → HIDManager), `gesture.cpp` (recognizer, then trainer),
and `handsfree.cpp` (record, repository, gate). Tests mirror this order in `tests/test_handsfree.cpp`
(recognition, configuration, training, conflicts, drag, switch and recovery).
`tests/hf_support.hpp` holds the rigs, including a power-loss storage model. The legacy
`test_main.cpp` is unchanged and still runs.
