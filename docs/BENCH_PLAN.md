# BENCH_PLAN · breadboard bring-up stages (prepared, NOT yet run on hardware)

Everything here is local preparation. **Nothing has been flashed** and no hardware reading exists yet. The
native USB port `/dev/cu.usbmodem101` was seen present and unoccupied (read-only check). Do not flash until an explicit instruction to flash is given (and the port is re-checked first). Hardware logs stay on this machine in `hardware-evidence/` (git-ignored) and are not
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

## Native USB connection and upload (current setup: one cable, the board's USB port)

The only cable goes to the board's native USB port, which enumerates as Espressif "USB JTAG/serial debug unit"
(303a:1001). `bench-diag` is built for it: `ARDUINO_USB_MODE=1` and `ARDUINO_USB_CDC_ON_BOOT=1` are set
explicitly in `platformio.ini` (the board definition sets only the mode, and without CDC-on-boot `Serial`
would be UART0, invisible over this cable). Verified from the build, not assumed: both flags, the pins
(SDA 8, SCL 9, button 4), `NODX_BUZZER=-1`, `HWCDC` linked, and **no BLE/HID code** (zero `NimBLEDevice`/`BLEHID`
symbols in the ELF). It also repeats a banner every 3 s until the first command, because anything printed
before the host opens the port is lost.

1. Check the port without opening it (read-only: USB registry, `ls`, `lsof`):
   ```bash
   python3 scripts/bench_port.py --expect /dev/cu.usbmodem101
   ```
   It said `READY` on the day it was prepared (one Espressif device, one port, no process holding it).
2. Build (allowed): `pio run -e bench-diag`.
3. **Upload (NOT RUN: needs your explicit flash instruction):**
   ```bash
   pio run -e bench-diag -t upload --upload-port /dev/cu.usbmodem101
   ```
   The USB-JTAG/serial unit normally resets into the ROM bootloader by itself. If upload cannot connect (the
   board may currently run something that uses USB), hold BOOT, tap RESET, release BOOT, re-identify the port
   (it changes in download mode) and repeat. The `qio_opi`/`opi` settings of the N16R8 candidate and the
   unresolved DIO/QIO header question apply: a wrong memory setting shows up at boot, not at upload.
4. **After upload the port may change** (the chip re-enumerates). Re-identify before any stage; do not reuse the
   old name blindly:
   ```bash
   python3 scripts/bench_port.py          # note the port it reports as <PORT>
   ```
5. Stages 1-3 use `scripts/bench_log.py` with the repo's virtualenv (it has pyserial 3.5). It opens the port with
   DTR and RTS held low so that watching the board does not reset it, echoes every line live and saves a local,
   git-ignored session under `hardware-evidence/`. A port is only ever used when you pass it with `--port`.

## Stage 1 · board facts (`bench-diag`)

```bash
.venv/bin/python scripts/bench_log.py --port <PORT> --label stage1 --send info --seconds 20   # NOT RUN
```

Record: chip/revision, flash size and mode, PSRAM size, USB CDC-on-boot and mode, reset reason, free heap. This
is the first real evidence on the open **N16R8 memory/USB question and the DIO/QIO image-header question**.
START check: flash 16 MB, PSRAM 8 MB. If either differs, stop and fix the configuration before anything else.

## Stage 2 · I2C bus and MPU6050 (`bench-diag`)

Board still on the bench:

```bash
.venv/bin/python scripts/bench_log.py --port <PORT> --label stage2 --send scan --send "imu 10" --seconds 45   # NOT RUN
```

Reads: SDA/SCL idle levels, device at 0x68, WHO_AM_I, the firmware's own init register sequence, **how often
the data-ready bit (register 0x3A) is seen with and without INT_ENABLE (0x38)**, about 100 distinct frames/s,
read errors, 10 ms poll jitter, gyro bias and noise, accelerometer magnitude, temperature.

Why the data-ready row matters: `MPU6050Sensor::read` only accepts a frame when that status bit is set, and
`begin()` does not set INT_ENABLE. If the bit only appears after INT_ENABLE, the shipped sensor path would
never produce a sample. If the measurement shows that, the fix is made locally in `core/src/sensor.cpp` with a
regression test and recorded before/after evidence (the diagnostic's own numbers, the same stage re-run on a
`bench-firmware` telemetry check). Gate: WHO_AM_I 0x68, frames about 100/s, no read errors. Otherwise stop.

## Stage 3 · the enable button (`bench-diag`)

Press and release a few times (a quick tap, a normal press, a long hold) when the prompt line appears:

```bash
.venv/bin/python scripts/bench_log.py --port <PORT> --label stage3 --send "button 20" --seconds 30   # NOT RUN
```

Reads: idle level HIGH, every press/release **burst** with its edge count and span in microseconds (the real
bounce), and the real `EnableGate` toggling (`gateToggle`, latched). Expect one toggle per press of 30 ms or
more and none for a shorter tap. Also do by hand and note: reset while the button is held (permission must stay
off), a floating-pin check (disconnect GPIO4 briefly: the pull-up must read HIGH).
Gate: bounce within START limits (20 edges, 10 ms) or the debounce values are revisited with the evidence.

## Stage 4 · BLE (`bench-ble-probe`) — DEFERRED, not to be run until instructed

Uses the firmware's real BLE adapter but no control engine; reports leave **only on a serial command**:

```bash
pio run -e bench-ble-probe                                  # build is allowed now
pio run -e bench-ble-probe -t upload --upload-port <PORT>   # NOT RUN
python3 scripts/bench_log.py --port <PORT> --label stage4 --send status --send ping --send nudge --seconds 90   # NOT RUN
```

Pair from the host's Bluetooth settings, then record: advertising and connect, encryption (`secured`),
subscription, `ping` and `nudge` delivery, reconnect after disconnect, and (with a host window that can show
a held button) `down confirm`, drop the link, then check the host released it (`up` afterwards). Also count
whether the host sees unsolicited traffic while idle. Gate: a secured, subscribed link delivering commanded
reports, and a known host-side result for a link lost with the button down.

## Stage 5 · the real firmware (`bench-firmware`) with the companion — DEFERRED, not to be run until instructed

Build is allowed now: `pio run -e bench-firmware`. After a go-ahead: flash it, run
`python3 desktop/server.py --serial <PORT>` and use the companion (source label `HARDWARE`).
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
