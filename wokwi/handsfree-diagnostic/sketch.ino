// NodX Adapt - Wokwi DIAGNOSTIC (sensor + ONE momentary enable button).
// No BLE, no mouse output. Just prints what the sensor and the button report.
//
// The button is a four-pin momentary tactile pushbutton wired GPIO4 <-> GND (INPUT_PULLUP, pressed =
// LOW). It is only a control-ENABLE toggle: it never selects, pauses or resumes anything. The sketch
// keeps the RAW pressed state apart from the LATCHED permission it toggles, mirroring the firmware:
//   * the permission is DISABLED at every boot, whatever the button is doing;
//   * a button held at boot enables nothing: release it (stably), then press once;
//   * one debounced press enables; the next press disables at its first edge;
//   * holding toggles only once; a stable release is required before the next press.
#include <Wire.h>

const int PIN_SDA = 8;       // START value, not qualified wiring
const int PIN_SCL = 9;       // START value, not qualified wiring
const int PIN_BUTTON = 4;    // START value, not qualified wiring
const uint8_t MPU_ADDR = 0x68;   // AD0 tied to GND
const uint8_t REG_WHO_AM_I = 0x75, REG_PWR_MGMT_1 = 0x6B, REG_DATA = 0x3B;
const uint8_t EXPECTED_ID = 0x68;
const unsigned long PERIOD_MS = 200;     // 5 Hz
const unsigned long DEBOUNCE_MS = 30;    // press / release must be stable this long

bool sensorOk = false;
bool pressed = false;             // RAW button state (LOW on GPIO4); not a permission
bool latched = false;             // LATCHED permission toggled by accepted presses
bool armed = false;               // a stable release has been seen since the last accepted press
bool pressTracked = false, releaseTracked = false;
unsigned long pressSince = 0, releaseSince = 0, toggles = 0;
unsigned long lastReadAt = 0, lastProbeAt = 0;
unsigned long goodReads = 0, failedReads = 0;

bool readBytes(uint8_t reg, uint8_t *buf, uint8_t n) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)MPU_ADDR, (int)n) != n) return false;
  for (uint8_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

bool writeByte(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

// Try to identify and wake the sensor. Returns true on success.
bool initSensor(int attempts) {
  for (int i = 1; i <= attempts; i++) {
    uint8_t id = 0;
    if (!readBytes(REG_WHO_AM_I, &id, 1)) {
      Serial.printf("Sensor init try %d/%d: no I2C reply\n", i, attempts);
    } else if (id != EXPECTED_ID) {
      Serial.printf("SENSOR NOT FOUND / WRONG ID (0x%02X)\n", id);
    } else if (!writeByte(REG_PWR_MGMT_1, 0x00)) {
      Serial.printf("Sensor init try %d/%d: wake-up write failed\n", i, attempts);
    } else {
      Serial.println("Sensor OK: WHO_AM_I = 0x68, device awake");
      return true;
    }
    delay(100);
  }
  Serial.println("SENSOR NOT FOUND / WRONG ID - will re-check every 5 s");
  return false;
}

void printPermission() {
  Serial.println(latched ? "CONTROL ENABLED (latched; control is NOT resumed by this)"
                         : "CONTROL DISABLED");
}

// Same rules as the firmware's momentary enable gate.
void updateButton() {
  unsigned long now = millis();
  pressed = (digitalRead(PIN_BUTTON) == LOW);
  if (!pressed) {
    pressTracked = false;
    if (!releaseTracked) { releaseTracked = true; releaseSince = now; }
    if (!armed && now - releaseSince >= DEBOUNCE_MS) armed = true;   // stable release seen
    return;
  }
  releaseTracked = false;
  if (!armed) return;              // held at boot, same press, or bouncing: nothing to accept
  if (latched) {                   // disable at the first press edge (fail safe)
    latched = false; armed = false; pressTracked = false; toggles++;
    printPermission();
    return;
  }
  if (!pressTracked) { pressTracked = true; pressSince = now; }
  if (now - pressSince >= DEBOUNCE_MS) {   // one debounced press enables, exactly once
    latched = true; armed = false; pressTracked = false; toggles++;
    printPermission();
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println("=== NodX Adapt DIAGNOSTIC: no BLE, no mouse output ===");
  Serial.println("GPIO4 (enable button), GPIO8 (SDA), GPIO9 (SCL) are START values,");
  Serial.println("not qualified wiring. Check real hardware before wiring.");
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(100000);
  Wire.setTimeOut(20);   // ms, bounded I2C wait
  // Control is DISABLED at boot whatever the button state; a held button must be released first.
  pressed = (digitalRead(PIN_BUTTON) == LOW);
  latched = false;
  armed = false;
  printPermission();
  Serial.println(pressed ? "Button is HELD at boot: release it, then press once to enable."
                         : "Press the enable button once to enable control.");
  sensorOk = initSensor(5);
  lastProbeAt = millis();
}

void loop() {
  delay(1);   // keeps the simulator light; far finer than the 30 ms debounce
  updateButton();
  unsigned long now = millis();

  if (!sensorOk) {
    if (now - lastProbeAt >= 5000) { lastProbeAt = now; sensorOk = initSensor(1); }
    return;
  }
  if (now - lastReadAt < PERIOD_MS) return;
  lastReadAt = now;

  uint8_t b[14];
  if (!readBytes(REG_DATA, b, 14)) {
    failedReads++;
    Serial.printf("I2C read FAILED (failures so far: %lu)\n", failedReads);
    if (failedReads % 10 == 0) sensorOk = false;   // re-probe after repeated failures
    return;
  }
  goodReads++;
  auto s16 = [&](int i) { return (int16_t)((b[i] << 8) | b[i + 1]); };
  float ax = s16(0) / 16384.0f, ay = s16(2) / 16384.0f, az = s16(4) / 16384.0f;
  float tempC = s16(6) / 340.0f + 36.53f;
  float gx = s16(8) / 131.0f, gy = s16(10) / 131.0f, gz = s16(12) / 131.0f;
  Serial.printf("accel g: %6.2f %6.2f %6.2f | gyro dps: %7.1f %7.1f %7.1f | %.1f C | button %s | control %s | toggles=%lu | ok=%lu fail=%lu\n",
                ax, ay, az, gx, gy, gz, tempC, pressed ? "PRESSED" : "released",
                latched ? "ENABLED" : "DISABLED", toggles, goodReads, failedReads);
}
