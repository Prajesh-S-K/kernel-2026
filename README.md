# NodX Adapt · v0.2.0 pre-hardware software

Head pointing, accessible switch selection/drag, optional dwell, roll scrolling and pause for an ESP32-S3 + MPU6050 prototype. One C++17 engine runs both the desktop simulator and the firmware. The companion uses a dark cyan/teal visual language and records Fitts-style trials without inventing performance claims.

**Status:** software implemented and checked in simulation; ESP32 firmware cross-compiled. Physical sensor, NVS power-loss behavior, BLE delivery/pairing, mounting, comfort and real-user performance remain hardware-required. Every device-dependent number is a **START value**, not a validated final setting.

```sh
cmake -S . -B build
cmake --build build -j 4
ctest --test-dir build --output-on-failure
python3 desktop/server.py
```

Open http://127.0.0.1:8765. Start calibration, open Control studio, then explicitly Resume. WASD points; Q/E rolls; Space selects; P pauses. The browser drives the same engine compiled for the ESP32. No OS input is emitted by the desktop simulator.

Install the pinned development tools in BUILD_GUIDE, then run all software checks: `sh scripts/check.sh`. Build both firmware variants: `pio run`. Start with [BUILD_GUIDE](docs/BUILD_GUIDE.md) for prerequisites and detailed instructions.

| Read | Purpose |
|---|---|
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
