# BENCH_PLAN · breadboard bring-up stages (prepared, NOT yet run on hardware)

Everything here is local preparation. **Nothing has been flashed**, no USB port has been confirmed, and no
hardware reading exists yet. Do not flash until the exact USB port is confirmed and an explicit instruction
to flash is given. Hardware logs stay on this machine in `hardware-evidence/` (git-ignored) and are not
published without review.

Fixed rules for every stage: the buzzer stays disconnected (`NODX_BUZZER=-1`); no 3V3/5V reaches the button;
the bench pins (SDA GPIO8, SCL GPIO9, enable button GPIO4) are **START candidates** that exist only in the
`bench-*` PlatformIO environments, while the released and general environments keep every GPIO disabled;
nothing is described as validated until a reading says so.

## Stage 0 · before power (no USB, no tools beyond a multimeter)

| Check | How | Record |
|---|---|---|
| Button contact pairs | Continuity test with the button **not** pressed: find the two pin pairs that beep with each other, then confirm pressing connects the pairs. Appearance proves nothing | Which physical pins are pair A and pair B (photo + note) |
| Button wiring | Pair A to GPIO4, pair B to GND. **No 3V3/5V** | Photo of the breadboard |
| MPU6050 breakout | Identify the VCC arrangement (3.3 V vs 5 V input and regulator), AD0 to GND, SDA GPIO8, SCL GPIO9 | Breakout markings, schematic or photo |
| Board identification | Module marking (expect ESP32-S3-WROOM-1 N16R8), which USB-C is UART and which is native USB, pins GPIO8/9/4 exposed and not strapping/PSRAM/USB (avoid 35-37, 19/20) | Photos, exact silkscreen |
| Buzzer | Not connected | Photo |

Gate: wiring photographed and continuity facts written down. Otherwise stop.

## Stage 1 · diagnostic firmware: board facts (`bench-diag`, serial only)

Build (allowed now): `pio run -e bench-diag`. It prints `DIAG,...` lines only; it compiles no BLE or HID code
and never moves anything. Upload is **not** run until the port is confirmed and a go-ahead is given:

```bash
pio run -e bench-diag -t upload --upload-port <CONFIRMED_PORT>   # NOT RUN
python3 scripts/bench_log.py --port <CONFIRMED_PORT> --label stage1 --send info --seconds 15
```

Record: chip/revision, flash size and mode, PSRAM size, USB CDC-on-boot, reset reason, free heap. This is the
first real evidence on the open **N16R8 memory/USB question and the DIO/QIO image-header question**. START
check: flash 16 MB, PSRAM 8 MB. If either differs, stop and fix the configuration before anything else.

## Stage 2 · I2C bus and MPU6050 (`bench-diag`)

`scan` then `imu 10` (board still on the bench):

```bash
python3 scripts/bench_log.py --port <CONFIRMED_PORT> --label stage2 --send scan --send "imu 10" --seconds 40   # NOT RUN
```

Reads: SDA/SCL idle levels, device at 0x68, WHO_AM_I, the firmware's own init register sequence, **how often
the data-ready bit (register 0x3A) is seen with and without INT_ENABLE (0x38)**, about 100 distinct frames/s,
read errors, 10 ms poll jitter, gyro bias and noise, accelerometer magnitude, temperature.

Why the data-ready row matters: `MPU6050Sensor::read` only accepts a frame when that status bit is set, and
`begin()` does not set INT_ENABLE. If the bit only appears after INT_ENABLE, the shipped sensor path would
never produce a sample. That fix belongs to the sensor-initialisation task (not part of this preparation);
this stage only measures it. Gate: WHO_AM_I 0x68, frames about 100/s, no read errors. Otherwise stop.

## Stage 3 · the enable button (`bench-diag`)

`button 20`, pressing and releasing a few times (a quick tap, a normal press, a long hold):

```bash
python3 scripts/bench_log.py --port <CONFIRMED_PORT> --label stage3 --send "button 20" --seconds 30   # NOT RUN
```

Reads: idle level HIGH, every press/release **burst** with its edge count and span in microseconds (the real
bounce), and the real `EnableGate` toggling (`gateToggle`, latched). Expect one toggle per press of 30 ms or
more and none for a shorter tap. Also do by hand and note: held while pressing reset (boot with the button
held; permission must stay off), a floating-pin check (disconnect GPIO4 briefly: the pull-up must read HIGH).
Gate: bounce within START limits (20 edges, 10 ms) or the debounce values are revisited with the evidence.

## Stage 4 · BLE (`bench-ble-probe`)

Uses the firmware's real BLE adapter but no control engine; reports leave **only on a serial command**:

```bash
pio run -e bench-ble-probe                                  # build is allowed now
pio run -e bench-ble-probe -t upload --upload-port <CONFIRMED_PORT>   # NOT RUN
python3 scripts/bench_log.py --port <CONFIRMED_PORT> --label stage4 --send status --send ping --send nudge --seconds 90   # NOT RUN
```

Pair from the host's Bluetooth settings, then record: advertising and connect, encryption (`secured`),
subscription, `ping` and `nudge` delivery, reconnect after disconnect, and (with a host window that can show
a held button) `down confirm`, drop the link, then check the host released it (`up` afterwards). Also count
whether the host sees unsolicited traffic while idle. Gate: a secured, subscribed link delivering commanded
reports, and a known host-side result for a link lost with the button down.

## Stage 5 · the real firmware (`bench-firmware`) with the companion

Build is allowed now: `pio run -e bench-firmware`. After a go-ahead: flash it, run
`python3 desktop/server.py --serial <CONFIRMED_PORT>` and use the companion (source label `HARDWARE`).
Order: status and telemetry; the enable button latch and chips; calibration with the real axes (the START axis
mapping and signs must be checked against real motion first); gesture training; commit; resume by gesture;
dwell; drag; every release cause including BLE disconnect during drag; reboot with the button held; fault
recovery; NVS persistence across power cycles and (separately) interrupted saves.

## Stage 6 · accidental activation and comfort

Long unscripted sessions per [TEST_PLAN](TEST_PLAN.md): count candidates, rejections and executions per hour;
training burden; comfort. Only this stage can say anything about suitability.

## Not done here

Items outside this preparation and still needed before stage 5 can pass: the MPU data-ready initialisation
(if stage 2 shows it), idle BLE report traffic and the Device Information service, local HTTP `Host`
validation, version-string consistency (separate tasks). V1 hardware readiness is **not** claimed until stages
0-5 have real readings and stage 6 has been run.

## Local evidence layout

`hardware-evidence/<timestamp>-<label>/raw.log` (host-timestamped serial text), `summary.txt` and
`summary.json` from `scripts/bench_analyze.py`. Analyse an existing log without a port:
`python3 scripts/bench_log.py --analyze <raw.log>`. `bench_log.py` needs `--port` explicitly, only sends the
allow-listed diagnostic commands, never flashes and never guesses a port.
