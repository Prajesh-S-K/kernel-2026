# NodX Adapt hands-free diagnostic (Wokwi)

## 1. What it is
A tiny simulation of an ESP32-S3, an MPU6050 motion sensor and ONE maintained slide switch. It only prints sensor readings and the switch state to the serial monitor. It is a DIAGNOSTIC: no BLE, no mouse output, no buttons, no buzzer.

## 2. Paste and run
1. Go to https://wokwi.com and start a new **ESP32-S3** project (Arduino).
2. Open `diagram.json` in the editor and replace its whole contents with this folder's `diagram.json`.
3. Open `sketch.ino` and replace its whole contents with this folder's `sketch.ino`.
4. Press **Start** (green play button) and open the serial monitor.
5. You should see the DIAGNOSTIC banner, `Sensor OK: WHO_AM_I = 0x68`, then a line about 5 times a second.
6. The slide switch starts ON (knob to the left). Click it: `CONTROL SWITCH: OFF (inhibited)` is printed, every later line says `switch OFF`, and clicking again prints `ON (permitted)`. Click the MPU6050 to open its control panel and drag its sliders (acceleration, rotation, temperature); the numbers on the next serial line change to match. If the canvas shows only part of the circuit, drag empty background to pan.
7. To see the error handling, delete the `imu:SDA` line from `diagram.json` and restart: the sketch prints `no I2C reply` and `SENSOR NOT FOUND / WRONG ID`, then re-checks every 5 seconds instead of hanging.

If the serial monitor covers the switch, collapse it with the arrow at its bottom right.

## 3. Wokwi simulation wiring vs approved physical wiring
- The Wokwi wiring is an **idealised simulation**: MPU6050 SDA to GPIO8, SCL to GPIO9, VCC to 3V3, GND to GND, AD0 to GND (address 0x68); switch between GPIO4 and GND (ON = LOW, internal pull-up).
- GPIO8, GPIO9 and GPIO4 are only **START / proposed** values, not approved physical wiring.
- Before wiring real hardware you MUST check, from photos and schematics: the real MPU6050 breakout's VCC pin and regulator arrangement (3.3 V vs 5 V input), and which pins the exact ESP32-S3 N16R8 board really exposes.
- Avoid GPIO35-37 (octal PSRAM), GPIO19/20 (USB) and strapping/UART pins unless they have been checked.
- A successful simulation does **not** qualify the hardware.

## 4. What it does NOT show
No BLE, no mouse/HID output, no calibration, no gesture detection, no real-world noise, drift or electrical behaviour.

## 5. What was verified, and what was not
Checked in the Wokwi web editor (no sign-in, nothing saved): the diagram loads without wiring errors, the sketch compiles, the banner, `Sensor OK: WHO_AM_I = 0x68`, live accel lines, a live switch click OFF then ON, and the missing-sensor message after removing the SDA wire. Pin names were checked against Wokwi's published `wokwi-boards` ESP32-S3-DevKitC-1 pin map; the part types against docs.wokwi.com (MPU6050, slide switch). The ESP32-S3 board part type was confirmed from Wokwi's published board definition and a public project, not from a docs.wokwi.com part page. Also checked: moving the MPU6050 sliders (acceleration X 1.35 g, rotation X 191 deg/s, rotation Z -183 deg/s, temperature 68.4 C) changed the matching serial fields and nothing else. **Not verified:** the 30 ms debounce timing, and anything about real hardware.
