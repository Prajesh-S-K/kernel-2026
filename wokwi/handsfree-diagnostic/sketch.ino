// NodX Adapt - Wokwi DIAGNOSTIC (sensor + control switch only).
// No BLE, no mouse output. Just prints what the sensor and switch report.
#include <Wire.h>

const int PIN_SDA = 8;       // START value, not qualified wiring
const int PIN_SCL = 9;       // START value, not qualified wiring
const int PIN_SWITCH = 4;    // START value, not qualified wiring
const uint8_t MPU_ADDR = 0x68;   // AD0 tied to GND
const uint8_t REG_WHO_AM_I = 0x75, REG_PWR_MGMT_1 = 0x6B, REG_DATA = 0x3B;
const uint8_t EXPECTED_ID = 0x68;
const unsigned long PERIOD_MS = 200;     // 5 Hz
const unsigned long ON_STABLE_MS = 30;   // ON must be stable this long

bool sensorOk = false;
bool switchOn = false;            // accepted (debounced) state
bool lastRawOn = false;
unsigned long rawChangedAt = 0, lastReadAt = 0, lastProbeAt = 0;
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

void printSwitch() {
  Serial.println(switchOn ? "CONTROL SWITCH: ON (permitted)"
                          : "CONTROL SWITCH: OFF (inhibited)");
}

void updateSwitch() {
  bool rawOn = (digitalRead(PIN_SWITCH) == LOW);   // LOW = switch ON
  unsigned long now = millis();
  if (rawOn != lastRawOn) { lastRawOn = rawOn; rawChangedAt = now; }
  bool newState = switchOn;
  if (!rawOn) newState = false;                                 // OFF: immediate
  else if (now - rawChangedAt >= ON_STABLE_MS) newState = true; // ON: after 30 ms
  if (newState != switchOn) { switchOn = newState; printSwitch(); }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println("=== NodX Adapt DIAGNOSTIC: no BLE, no mouse output ===");
  Serial.println("GPIO4 (switch), GPIO8 (SDA), GPIO9 (SCL) are START values,");
  Serial.println("not qualified wiring. Check real hardware before wiring.");
  pinMode(PIN_SWITCH, INPUT_PULLUP);
  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(100000);
  Wire.setTimeOut(20);   // ms, bounded I2C wait
  lastRawOn = switchOn = (digitalRead(PIN_SWITCH) == LOW);
  printSwitch();
  sensorOk = initSensor(5);
  lastProbeAt = millis();
}

void loop() {
  updateSwitch();
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
  Serial.printf("accel g: %6.2f %6.2f %6.2f | gyro dps: %7.1f %7.1f %7.1f | %.1f C | switch %s | ok=%lu fail=%lu\n",
                ax, ay, az, gx, gy, gz, tempC, switchOn ? "ON" : "OFF", goodReads, failedReads);
}
