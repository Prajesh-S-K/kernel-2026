# NodX Adapt · v0.2.0 pre-hardware software

Head pointing, accessible switch selection/drag, optional dwell, roll scrolling and pause for an ESP32-S3 + MPU6050 prototype. One C++17 engine runs both the desktop simulator and the firmware. The companion uses a dark cyan/teal visual language and records Fitts-style trials without inventing performance claims.

**Hands-free revision (unreleased, on top of v0.2.0):** everyday operation needs no physical button. Dwell selects; two helper-trained head gestures pause/resume and toggle drag; at most one maintained control-enable switch permits or inhibits control. The earlier physical-switch behaviour remains as an explicit *legacy compatibility mode*. Read [HANDS_FREE_SPEC](docs/HANDS_FREE_SPEC.md). Everything about gestures, the enable switch and their parameters is simulation-checked only: **no hardware, comfort or accidental-activation evidence exists.**

**Desktop action overlay (optional, macOS, experimental):** a floating **NodX ▾** tile with a dwell menu (Left/Right/Double-click, Drag/Drop, Scroll, Keyboard, Cancel, Pause / Stop) and a dwell-typed NodX keyboard, so the head-controlled pointer can pick actions in any application. It is a separate program that talks only to the local companion bridge (no second serial connection, no mouse injection) and is launched with `scripts/run_overlay.sh` after you start a session. Setup, permissions, limits and what is and is not verified: [docs/OVERLAY.md](docs/OVERLAY.md).

**Status:** software implemented and checked in simulation; ESP32 firmware cross-compiled. Physical sensor, NVS power-loss behavior, BLE delivery/pairing, mounting, comfort and real-user performance remain hardware-required. Every device-dependent number is a **START value**, not a validated final setting.

```sh
cmake -S . -B build
cmake --build build -j 4
ctest --test-dir build --output-on-failure
python3 desktop/server.py
```

Open http://127.0.0.1:8765. Start calibration, then (helper) open Hands-free setup to train the two gestures and save; or, in legacy compatibility mode, open Control studio and Resume. WASD points; Q/E rolls; P is the helper pause. In legacy mode Space selects. The browser drives the same engine compiled for the ESP32. No OS input is emitted by the desktop simulator.

Install the pinned development tools in BUILD_GUIDE, then run all software checks: `sh scripts/check.sh`. Build both firmware variants: `pio run`. Start with [BUILD_GUIDE](docs/BUILD_GUIDE.md) for prerequisites and detailed instructions.

| Read | Purpose |
|---|---|
| [HANDS_FREE_SPEC](docs/HANDS_FREE_SPEC.md) | Hands-free interaction, gestures, enable button/switch, configuration and recovery |
| [BENCH_PLAN](docs/BENCH_PLAN.md) | Breadboard bring-up stages, bench environments and local logging (prepared, not yet run) |
| [MASTER_BLUEPRINT](docs/MASTER_BLUEPRINT.md) | Frozen scope, architecture, module responsibilities and state transitions |
| [BUILD_GUIDE](docs/BUILD_GUIDE.md) | Desktop, replay, firmware and later USB companion setup |
| [TEST_PLAN](docs/TEST_PLAN.md) | Test coverage and physical acceptance gates |
| [CRISIS_PLAN](docs/CRISIS_PLAN.md) | Fault behavior, recovery and demonstration fallback |
| [DECISION_LOG](docs/DECISION_LOG.md) | Small implementation decisions and tradeoffs |
| [EVIDENCE](docs/EVIDENCE.md) | What was verified and what remains unproven |
| [DEMO_GUIDE](docs/DEMO_GUIDE.md) | A reproducible demonstration |
| [PARAMETERS](docs/PARAMETERS.md) | START registry, bounds, units and tuning methods |
| [PARAMETER_CHANGELOG](docs/PARAMETER_CHANGELOG.md) | Parameter history |
| [MODULE_WALKTHROUGH](docs/MODULE_WALKTHROUGH.md) | Reading order, responsibilities and individual checks |
| [PROTOCOL](docs/PROTOCOL.md) | Desktop/USB transport and raw data schemas |

The custom NodX cursor belongs to the companion. Standard BLE HID uses the host's normal cursor outside it. The prototype is USB-powered; battery, Wi-Fi and cloud features are outside V1.

Release gate: `sh scripts/release_gate.sh`. Package only its clean, committed source with `python3 scripts/package.py`. The prior release tag is preserved.

Standalone Wokwi diagnostic (one four-pin momentary pushbutton as the enable button, MPU6050, no other buttons, no buzzer, no BLE): [wokwi/handsfree-diagnostic](wokwi/handsfree-diagnostic/README.md). It is a simulation aid, not hardware qualification.
