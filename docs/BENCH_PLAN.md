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

### Stage 2b · the firmware's own sensor driver (`imuinit`)

After `imuregs` has captured the as-found state, `imuinit 10` runs `MPU6050Sensor::begin()` (identity, the
documented register writes, read-back of every one) and then reads frames through the driver for 10 s. The
analyser judges: begin accepted with a variant, about 100 valid frames/s, accelerometer magnitude about 1 g.
It writes sensor registers, which is why it comes after the read-only dump. **Written locally, not yet
flashed or run.**

```bash
.venv/bin/python scripts/bench_log.py --port <PORT> --label stage2b --send imuregs --send "imuinit 10" --seconds 30   # NOT RUN
```

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

## Stage 4 · BLE (`bench-ble-probe`) — PREPARED, NOT FLASHED, NOT RUN; wait for an explicit instruction

Prerequisites already met on the bench: stages 1-3 pass (board facts, sensor via the firmware driver, enable button).
The probe uses the firmware's REAL BLE adapter (`BLEHID`: report-protocol mouse, bonded pairing, encrypted
reports) and none of the control engine. **Reports leave only on a serial command**; the probe never moves the host
pointer by itself. It builds without warnings and contains no bond-deleting code. The enable button, the sensor and
the buzzer are not touched.

Host-side precautions (the host is this Mac or another computer, because the board will appear as a real Bluetooth mouse):
* Close anything that a stray click could activate. Park the pointer over an empty, harmless window before `down confirm`.
* Pairing creates a **bond**, stored in the board's NVS by the BLE stack and on the host. Never erase the whole flash/NVS to
  "clear" it. To start clean, remove "NodX Adapt" in the host's Bluetooth settings, and use `bonds` to read how many pairings
  the board holds (read-only). The real-control firmware will see the same bond.
* The native USB serial link and the Bluetooth link are independent: keep the USB cable connected for logging.

```bash
pio run -e bench-ble-probe                                               # build is allowed now
pio run -e bench-ble-probe -t upload --upload-port /dev/cu.usbmodem101  # NOT RUN: needs the explicit go (re-check the port first)
python3 scripts/bench_port.py                                            # re-identify the port after the upload: <PORT>
.venv/bin/python scripts/bench_log.py --port <PORT> --label stage4-pair --send status --send bonds --seconds 60   # NOT RUN
```

Checklist, in order, each recorded with the host-side result written next to the board's `BLE,` lines:
1. **Advertising and boot:** the heartbeat shows `state` lines every 3 s with `connected=0`; the host lists "NodX Adapt".
2. **Pair from the host's Bluetooth settings:** expect `secured=1`, then `subscribed=1`, `connected=1` in `link-change` lines,
   in that order. Record whether the host asks for confirmation or a PIN (the adapter declares no input/output).
3. **Delivery:** `ping` (no movement, no buttons) reports `delivered=1`; `nudge` (+2 then -2) moves the host pointer by nothing net;
   `delivered=0` before the link is secured and subscribed is the expected, correct refusal.
4. **Reconnect:** disconnect from the host side, then reconnect; the board advertises again by itself and the link returns
   secured and subscribed without re-pairing. Also drop the link by moving the board out of range or turning host Bluetooth off.
5. **Button down during a lost link:** with the pointer over a harmless spot, `down confirm` (host button pressed), turn host
   Bluetooth off, then back on and `up`. Record what the host did (released on disconnect? stuck?). The probe prints
   `link-lost-with-button-down` as a warning. This is the case the real firmware must handle (drag during disconnect).
6. **Idle traffic:** with a secured link and no command, watch the host for unsolicited reports (expect none).
7. **Both sides' view:** the host's Bluetooth details (device name, appearance) next to the probe's `state` lines.

Gate: a secured, subscribed link that delivers commanded reports and survives disconnect/reconnect, and a recorded host-side
result for the button-down link loss. Known gaps this stage will measure but not fix: idle report traffic and the missing
Device Information service (separate tasks).

## Stage 5 · the real firmware (`bench-firmware`), attended movement-only demo

`bench-firmware` is the real firmware with the START bench pins (SDA 8, SCL 9, enable button GPIO4, buzzer off, native USB
serial), the MPU-6500-class driver, BLE mouse and the hands-free engine. Order, each step recorded in `hardware-evidence/`:

1. **Flash** (explicit authorization given; port re-checked first; no full erase, NVS and the BLE bond preserved):
   `pio run -e bench-firmware -t upload --upload-port /dev/cu.usbmodem101`, then `python3 scripts/bench_port.py`.
2. **Prerequisites seen in telemetry** (`status`): `source` HARDWARE, `sensor.seen` with frames increasing and a small `ageMs`,
   `connected` true (BLE secured + subscribed), `state` never ACTIVE at boot.
3. **Axis mapping from measurement while output is inhibited.** The operator performs three isolated movements, each separated
   by stillness: yaw (turn left, return, turn right, return), pitch (tilt up, return, tilt down, return), roll (tilt right, return,
   tilt left, return). `scripts/bench_axes.py` finds the dominant raw gyro axis and the first-lobe direction of each, and the
   gravity axes, and prints build flags; it refuses a capture whose movements were not isolated. The flags go into the bench
   environment and the board is re-flashed with them. Conventions it targets: yaw left negative, pitch up negative, roll right positive.
4. **Real calibration and gesture training** through the companion in hardware mode (`python3 desktop/server.py --serial <PORT>`):
   no synthetic gestures (`gesture` is refused on hardware), no bypassed prerequisites; the operator performs the movements.
5. **Movement-only demo**: `handsfree demo on` (or the checkbox in the setup view): no dwell click, no drag, no wheel, steps
   bounded to 6 px per report, the saved profile unchanged, off after every restart.
6. **Enable permission and intentional resume**: press the enable button (latch), then the resume gesture; gently rotate/tilt the
   assembly while watching the pointer; press the button to disable and confirm the pointer stops.
Not run in this stage: clicking, dragging, held-button disconnect.

## Stage 5 (original outline) — superseded by the list above

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

## Uncalibrated pointer demo (temporary bench aid)

Purpose: show the real sensor moving the Mac pointer while calibration is missing or FAILED. It is not a
calibration, saves nothing and never reports a profile. Start values are in `docs/PARAMETERS.md`.

* Start: companion, Setup page, "Start without calibration" (hardware device only). It needs the physical
  enable button's permission (its own momentary latch: disabled at every boot, a button held at boot is
  ignored, cleared by every fault, the next press disables), a healthy sensor (20 good samples), a valid axis
  mapping, an unfaulted BLE link and no calibration, training or active control in progress. It works when
  calibration is missing or failed, or the saved profile is corrupt (reported separately).
* Reversal: "Reverse horizontal" / "Reverse vertical" flip the default mapping's pointer direction (RAM only,
  cleared by a reboot). The fallback uses the default axis mapping and promises nothing for other mountings.
* Real sensor -> AxisTransform -> filtering -> bounded output -> SafetyManager -> HIDManager. Clicks,
  dwell, drag and wheel are removed; pointer steps are at most 4 px per report.
* Stop: "Stop demo" (banner and panel), the next press of the enable button (at the press edge, no sample needed),
  starting a calibration, pause, any fault (sensor, mapping, timing, calculation), BLE disconnect or
  delivery failure. After any stop the demo stays off until started again; reconnecting or rebooting
  never starts it. Pause-on-focus-loss is suspended only while this demo runs, so the pointer can be
  watched in another window.
* A corrupt saved profile stays reported as CORRUPT (`profileState`); the demo uses its own RAM profile
  and never repairs, replaces or accepts the record. It uses no NVS writes.
* Banner text while active: "UNCALIBRATED DEMO — LIVE SENSOR". The Performance Lab refuses to start
  while it is active, so it cannot enter a comparison.

Pointer direction for the measured bench mounting (USB end = back; axis mapping measured with
`scripts/bench_axes.py`, not inferred from the sensor identity). The mapping the firmware actually uses is
shown on the panel from the `axes` telemetry (gyro axes 2,0,1 signs +,+,+ for the bench build):

| Board motion | Pointer |
| --- | --- |
| Turn left (yaw rate negative) | left |
| Turn right | right |
| Tilt front end up (pitch rate negative) | up |
| Tilt front end down | down |
| Sideways roll | nothing (no scrolling) |

A different mounting needs a new measured mapping, never an inferred one.

### BLE report rate (found while preparing the demo)

The control loop used to hand the BLE adapter a report on every 10 ms sensor tick, including all-zero
reports while idle (about 100 notifications per second). A BLE link carries far fewer; this matched the
repeated "HID connection or delivery failed" faults and may be linked to the serial stalls (unproven).
Now the core sends an idle zero report only once (a stop always sends its release) and the adapter
coalesces movement to one notification per 20 ms; a button change or an all-zero report is sent at once.

### Loop watchdog and reset reason

After the serial link was seen to go silent for good (35 s to 6 min after boot, sensor healthy and 100 Hz
until the last reply, no fault counted), the loop task watchdog is enabled and the last reset reason is
printed at boot (`[BOOT] reset reason: ...`) and reported as `reset` in hardware telemetry. A hung control
loop now reboots the board (the panic text with its backtrace goes to serial) and the reason shows TASK_WDT,
which separates a firmware lock-up from a USB-serial glitch. Watchdog timeout is the framework default.

## Uncalibrated demo: dwell clicking (attended)

Optional, explicitly enabled, off at every start of the demo. No motion calibration or gesture training is
needed, and sensor health, the safety gate and the HID manager are unchanged. Reuses `SelectionManager`:
250 ms arming, then 1200 ms progress, then one primary click (press, then release), then a lockout until the
pointer has moved more than 1.5 x the tolerance (12 units at the default 8). Staying still never repeats the
click; a deliberate move away re-arms it. If the release is not delivered the system enters the existing
fault/recovery path and the demo and dwell stay off until you restart them.

Cancelled by: movement beyond the tolerance, pause, the enable button, an invalid or stale sensor sample, BLE
disconnect or delivery failure, Stop demo, starting a calibration. Reconnecting never restarts anything.
Drag, double-click, right-click and scrolling stay disabled. The trained pause/resume gesture is NOT offered:
gesture training and recognition both require a valid saved motion profile, which this demo does not have.

Short attended demonstration (harmless target only, for example an empty text editor or a button that just
counts clicks):
1. Reload the companion; wait until the demo panel says Ready. Click "Start uncalibrated pointer demo"
   (movement only; banner "UNCALIBRATED DEMO — LIVE SENSOR"). Confirm left/right/up/down as before.
2. Park the pointer over the harmless target and keep the board still.
3. Tick "Enable dwell clicking" (banner becomes "UNCALIBRATED DEMO — DWELL CLICK"). Watch the progress bar fill
   (about 1.5 s including arming); expect exactly one click on the target and "Clicks this run: 1".
4. Stay still for 10 s: no second click (status says it is locked out).
5. Move the board clearly away and back to the target, then hold still: one more click (rearmed).
6. Untick the box or click "Stop demo" (or press the enable button): clicking stops at once.
Do not test drag, held buttons or disconnects with the button held.
