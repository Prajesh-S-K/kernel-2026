# BUILD_GUIDE

## Desktop prerequisites

CMake ≥3.16, a C++17 compiler, Python ≥3.10 and Node ≥18 for metric checks. The desktop file adapter targets macOS/Linux (POSIX fsync). No npm packages, cloud accounts, front-end bundler or downloaded visual assets are required. On Windows use WSL for the desktop process; firmware compilation can use PlatformIO on Windows.

From the repository directory:

```sh
cmake -S . -B build
cmake --build build -j 4
ctest --test-dir build --output-on-failure
python3 desktop/server.py
```

Open http://127.0.0.1:8765. Keep one control tab open; the local server has one serialized session. Port override: `--port 8766`. State lives in ignored `runtime/`. A different `--runtime /tmp/nodx-demo` creates an isolated fresh demo without changing your current saved profile.

Full software checks (36 core invariants, desktop integration, metric calculation/export):

```sh
sh scripts/check.sh
```

This enables undefined-behavior sanitization. Address sanitization is an optional separate `-DNODX_ASAN=ON` build; it stalled on the current macOS host and is not counted as passed. Use a separate build directory when trying it. Browser behavior must also be checked; see TEST_PLAN.

## Each simulation

1. **Calibration:** Start calibration in Setup. Six sample windows generate the mathematical asymmetric user; analysis, validation and transactional save follow. Load saved verifies persistence. Cancel leaves the prior profile intact.
2. **Pointing/selection:** Control studio → Resume. WASD or sliders supply yaw/pitch rates. Hold Space or the accessible-switch control to drag. Dwell toggle opts in and returns to READY; Resume explicitly. Remain still to arm/click; move beyond tolerance to unlock the next click.
3. **Scroll/pause:** Q/E or the roll slider supplies roll rate; the complementary roll estimate crosses its angle threshold. Scroll suppresses pointing and dwell. P / Pause releases selection. Resume resets filters so old motion does not carry over.
4. **Faults:** Choose NaN, extreme, timeout, disconnect, frozen timestamp or HID failure, or uncheck simulated BLE. Expect SAFE_STATE. Restore healthy input; expect READY. Resume explicitly. Profile corruption changes only demo runtime slots; recalibration repairs them.
5. **Raw recording:** Record raw samples in Control studio; Stop saves `runtime/samples.csv`. Every native 10ms sample is recorded, not just the display trace. Starting a new recording replaces the previous sample recording; copy it before the next session.
6. **Replay:** replay the bundled deterministic data with a separate runtime directory:

```sh
./build/nodx_sim /tmp/nodx-replay --replay fixtures/synthetic_motion.csv > /tmp/nodx-replay.jsonl
./build/nodx_sim /tmp/nodx-fault --replay fixtures/synthetic_fault_recovery.csv > /tmp/nodx-fault.jsonl
```

The CLI loads a transient generic profile and explicitly resumes once after initial health qualification. After any fault it stays READY and does not resume again. Samples use recorded timestamps without sleeping; traces are reproducible algorithm outputs, not wall-clock measurements. Replay EOF is invalid in the sensor abstraction; CLI exits at the end of the supplied file. Input coordinates are semantic yaw/pitch/roll for the desktop; convert recorded hardware axes using the qualified mapping before desktop replay.

7. **Performance:** choose condition and source; Start 12 trials; select the center start target, then every target. Simulated input uses WASD/Space or dwell. Host pointer uses normal pointer clicks. Misses advance/log the trial. Abort, navigation, focus loss, resizing and device faults retain an aborted attempt and exclude it from summary metrics. Export raw CSV. Disk copies append to `runtime/trials.jsonl`; UI CSV contains the current page's session. Reloading the page does not load older trials into summaries.

## ESP32 firmware

Pinned toolchain: PlatformIO 6.1.18, espressif32 platform 6.12.0, Arduino ESP32 from that platform, NimBLE-Arduino 2.3.6. Environment: `esp32-s3-devkitc-1`. Confirm the exact purchased board, USB behavior, flash/PSRAM size and pin availability before flashing.

```sh
python3 -m venv .venv
. .venv/bin/activate
pip install platformio==6.1.18
pio run -e esp32s3 -e esp32s3-sim
```

Images appear in `.pio/build/<environment>/firmware.bin`; ELF, bootloader and partition artifacts stay together in that build directory. `esp32s3` reads real MPU6050; `esp32s3-sim` supports synthetic serial `motion` and `fault` commands. Both compile the shared core. Compile success is not successful flashing or hardware validation.

All GPIOs default to **-1 (disabled)**. After pin qualification, set build flags in `platformio.ini`, for example:

```ini
build_flags = ${env.build_flags} -DNODX_SDA=8 -DNODX_SCL=9 -DNODX_SWITCH=4 -DNODX_PAUSE=5 -DNODX_CALIBRATE=6 -DNODX_BUZZER=7
```

These numbers are illustrative START candidates, not approved wiring. Selection/pause/calibration are active-low INPUT_PULLUP; buzzer is active-high logic to a qualified driver circuit. Verify voltages, pin conflicts, external pull-ups, buzzer current and sensor address independently. Verify gyro/gravity axis transforms in `AxisTransform`. Do not wear an unqualified wired assembly.

After physical qualification, build then flash with `pio run -e esp32s3 -t upload`; serial monitor: `pio device monitor`. Pair the BLE mouse and ensure encryption/report subscription before calibration. Monitor provides instructions via `status`/phase telemetry. Commands: `calibrate`, `cancel`, `resume`, `pause`, `load`, `generic`, `dwell on/off`, `scroll on/off`. Physical pause/calibration buttons trigger the same state transitions. Simulated firmware additionally accepts `motion <yaw> <pitch> <roll>` and `fault <0..5>`.

## Later USB companion connection

USB carries setup telemetry only; BLE remains the pointing path. This avoids specialized software for basic host control. Optional bridge:

```sh
pip install -r desktop/requirements-hardware.txt
python3 desktop/server.py --serial /dev/cu.usbmodemYOUR_DEVICE
```

The UI detects HARDWARE telemetry and disables virtual sensor/fault controls. Use Host pointer in the lab; the actual BLE pointer supplies selections. The bridge and serial format are implemented and compile-checked, but require board/USB/NVS/BLE validation. Physical firmware telemetry at 115200 baud competes with sampling and needs timing measurement. Desktop profile slots and ESP32 NVS are separate; profiles are not silently transferred between them.
