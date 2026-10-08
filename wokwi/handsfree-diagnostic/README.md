# NodX Adapt hands-free diagnostic (Wokwi)

## 1. What it is
A tiny simulation of an ESP32-S3, an MPU6050 motion sensor and ONE four-pin momentary tactile pushbutton (the enable button). It only prints sensor readings and the button state to the serial monitor. It is a DIAGNOSTIC: no BLE, no mouse output, no other buttons, no buzzer.

The button is only a control-ENABLE toggle: it never selects, pauses or resumes anything. The serial output keeps two things apart:
- **raw pressed state**: `button PRESSED` / `button released` (what the pin does right now);
- **latched control permission**: `control ENABLED` / `control DISABLED` (what presses toggled). `toggles=N` counts accepted toggles.

The sketch applies the same rules as the firmware's momentary enable gate: permission is DISABLED at every boot, a button held at boot enables nothing (release it, then press once), one debounced (30 ms) press enables, the next press disables at its first edge, holding toggles only once, and a stable release is needed before the next press.

## 2. Paste and run
1. Go to https://wokwi.com and start a new **ESP32-S3** project (Arduino).
2. Open `diagram.json` in the editor and replace its whole contents with this folder's `diagram.json`.
3. Open `sketch.ino` and replace its whole contents with this folder's `sketch.ino`.
4. Press **Start** and open the serial monitor.
5. You should see the DIAGNOSTIC banner, `CONTROL DISABLED`, `Press the enable button once to enable control.`, `Sensor OK: WHO_AM_I = 0x68`, then a line about 5 times a second ending `button released | control DISABLED | toggles=0 | ok=N fail=0`.
6. Press the green ENABLE button (click it, or hold the `E` key while the diagram has focus). A click is very short: **hold it for at least a second** (Ctrl-click, Cmd-click on Mac, keeps it pressed until the next click). `CONTROL ENABLED (latched; control is NOT resumed by this)` is printed and lines read `control ENABLED`. Keep holding: `toggles` stays 1. Release: the line reads `button released | control ENABLED` - the permission outlives the press. Press again: `CONTROL DISABLED`.
7. Held at boot: press **Restart**, then immediately Ctrl-click the button (the sketch waits one second at boot before it reads the pin), so it is held when the sketch reports its state. The sketch prints `Button is HELD at boot: release it, then press once to enable.` and stays `control DISABLED` while held and after release, until a new press.
8. Click the MPU6050 to open its control panel and drag its sliders (acceleration, rotation, temperature); the numbers on the next serial line change to match. If the canvas shows only part of the circuit, drag empty background to pan.
9. To see the error handling, delete the `imu:SDA` line from `diagram.json` and restart: the sketch prints `no I2C reply` and `SENSOR NOT FOUND / WRONG ID`, then re-checks every 5 seconds instead of hanging.

If the serial monitor covers the button, collapse it with the arrow at its bottom right. Wokwi simulates contact bounce by default; if the simulator runs slowly (a low percentage next to the timer) the 30 ms debounce takes correspondingly longer in real time.

## 3. Wokwi simulation wiring vs approved physical wiring
- The Wokwi wiring is an **idealised simulation**: MPU6050 SDA to GPIO8, SCL to GPIO9, VCC to 3V3, GND to GND, AD0 to GND (address 0x68); the button's contact `1.l` to GPIO4 and its contact `2.l` to GND (internal pull-up, pressed = LOW). `1.l`/`1.r` are one contact and `2.l`/`2.r` the other (Wokwi docs for `wokwi-pushbutton-6mm`).
- **No 3V3 or 5V goes to the button.** On a real four-pin tactile button identify which pins are internally connected **by continuity testing, not by appearance**: one pair goes to GPIO4, the other pair to GND.
- GPIO8, GPIO9 and GPIO4 are only **START / proposed** values, not approved physical wiring.
- Before wiring real hardware you MUST check, from photos and schematics: the real MPU6050 breakout's VCC pin and regulator arrangement (3.3 V vs 5 V input), and which pins the exact ESP32-S3 N16R8 board really exposes.
- Avoid GPIO35-37 (octal PSRAM), GPIO19/20 (USB) and strapping/UART pins unless they have been checked.
- A successful simulation does **not** qualify the hardware.

## 4. What it does NOT show
No BLE, no mouse/HID output, no calibration, no gesture detection, no real-world noise, drift or electrical behaviour, no real switch bounce or contact resistance.

## 5. What was verified, and what was not
Checked in the Wokwi web editor (no sign-in, nothing saved): the diagram loads without wiring errors, the sketch compiles (also with `-Wall -Wextra` under PlatformIO), and the serial output follows the rules above: boot with the button held stays disabled and a release alone enables nothing; one press enables (`toggles=1`) and holding for 5 s toggles once; release keeps the permission; the second press disables (`toggles=2`) and a third enables (`toggles=3`), with Wokwi's default bounce on. Evidence: [transcript](../../evidence/wokwi-enable-button.txt), [screenshot](../../evidence/wokwi-enable-button.jpg). Earlier (slide-switch version) checks that still apply: the live accel lines, the missing-sensor message after removing the SDA wire, and moving the MPU6050 sliders. Pin names were checked against Wokwi's published `wokwi-boards` ESP32-S3-DevKitC-1 pin map; the part types against docs.wokwi.com (`wokwi-mpu6050`, `wokwi-pushbutton-6mm`). The ESP32-S3 board part type was confirmed from Wokwi's published board definition and a public project, not from a docs.wokwi.com part page. **Not verified:** the keyboard shortcut `E`, the missing-sensor message and the sliders with the button version of the sketch, and anything about real hardware.
