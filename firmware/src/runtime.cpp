#include "runtime.hpp"
NVSStorage storage;
I2CBus bus;
MPU6050Sensor mpu(bus);
SimulatedSensor simulator;
BLEHID ble;
ProfileRepository repository(storage);
NVSConfigStorage configStorage;
HandsFreeRepository configRepository(configStorage);
NVSControlStorage controlStorage;
ControlRepository controlRepository(controlStorage);
#ifdef NODX_SIMULATED
bool simulatedEnable = false; // raw enable input: released
std::vector<Rates> gestureScript;
size_t gesturePosition = 0;
#endif
System* systemEngine = nullptr;
SensorSnapshot sensorSnapshot;
DebouncedSwitch pauseSwitch, calSwitch;
uint32_t lastPoll = 0, lastSample = 0, lastProbe = 0, lastDiagnostic = 0;
bool previousPause = false, previousCal = false;
std::string serialLine;
bool serialOverflow = false;
bool pressed(int pin) {
    return pin >= 0 && digitalRead(pin) == LOW;
}
const char* bootResetReason = "UNKNOWN";
void initializeRuntime() {
    Serial.begin(115200);
    Serial.println("[NODX] 0.2.0 pre-hardware; START parameters; ESP32-S3");
    bool storageOK = storage.begin() && configStorage.begin() && controlStorage.begin();
    Serial.println(storageOK ? "[STORAGE] initialized" : "[STORAGE] failed");
    for (int pin : {NODX_SWITCH, NODX_PAUSE, NODX_CALIBRATE, NODX_ENABLE}) {
        if (pin >= 0) {
            pinMode(pin, INPUT_PULLUP);
        }
    }
    if (NODX_BUZZER >= 0) {
        pinMode(NODX_BUZZER, OUTPUT);
        digitalWrite(NODX_BUZZER, LOW);
    }
    if (NODX_SDA >= 0 && NODX_SCL >= 0) {
        bus.enabled = Wire.begin(NODX_SDA, NODX_SCL, 100000);
        Wire.setTimeOut(20);
    }
    bool imuOK = mpu.begin();
    Serial.println(imuOK ? (std::string("[IMU] detected ") + nodx::name(mpu.variant())).c_str()
                         : "[IMU] unavailable; outputs inhibited");
    ble.begin();
    systemEngine = new System(ble, repository, configRepository);
    systemEngine->setControlRepository(controlRepository);
    // Hands-free needs the enable input (switch or push button); without NODX_ENABLE control stays
    // inhibited unless setup qualified an alternative.
#ifdef NODX_SIMULATED
    systemEngine->configureEnableInput(true);
#else
    systemEngine->configureEnableInput(NODX_ENABLE >= 0);
#endif
#ifndef NODX_SIMULATED
    // Optional MEASURED sensor-to-head axis mapping from the build (bench sessions): gyro
    // axes/signs for yaw, pitch, roll and accel axes/signs for mapped x, lateral y, vertical z.
    // Without these flags the START mount assumption in AxisTransform applies. An invalid mapping
    // makes every sample invalid.
#ifdef NODX_GYRO_AXES
    systemEngine->axes.axes = {NODX_GYRO_AXES};
#endif
#ifdef NODX_GYRO_SIGNS
    systemEngine->axes.signs = {NODX_GYRO_SIGNS};
#endif
#ifdef NODX_ACCEL_AXES
    systemEngine->axes.accelAxes = {NODX_ACCEL_AXES};
#endif
#ifdef NODX_ACCEL_SIGNS
    systemEngine->axes.accelSigns = {NODX_ACCEL_SIGNS};
#endif
    Serial.println(systemEngine->axes.valid() ? "[AXES] mapping valid"
                                              : "[AXES] mapping INVALID; samples will be rejected");
#endif
#ifdef NODX_SIMULATED
    systemEngine->axes.axes = {0, 1, 2};
    systemEngine->axes.accelAxes = {0, 1, 2};
    systemEngine->axes.accelSigns = {1, 1, 1};
    Serial.println("[INPUT] simulated; motion and fault serial commands enabled");
#endif
    diagnostic(millis());
}
void serviceRuntime() {
    uint32_t now = millis();
    auto& s = *systemEngine;
    // Bounded nonblocking serial parser.
    constexpr size_t commandBytes = 80;
    for (unsigned i = 0; i < 64 && Serial.available() && acknowledgementCapacity(); ++i) {
        char character = char(Serial.read());
        if (character == '\n') {
            command(serialLine, now, serialOverflow);
            serialLine.clear();
            serialOverflow = false;
        } else if (character != '\r') {
            if (serialLine.size() < commandBytes) {
                serialLine += character;
            } else {
                serialOverflow = true;
            }
        }
    }
    // The raw enable input is read on every pass so a disable releases without waiting for a
    // sample.
#ifdef NODX_SIMULATED
    s.setControlSwitch(simulatedEnable, now);
#else
    s.setControlSwitch(pressed(NODX_ENABLE), now);
#endif
    // Physical pause and calibration buttons exist only in the legacy compatibility mode.
    const bool legacyControls = s.interaction == InteractionMode::Legacy;
    bool pauseNow = legacyControls && pauseSwitch.update(pressed(NODX_PAUSE), now);
    bool calNow = legacyControls && calSwitch.update(pressed(NODX_CALIBRATE), now);
    if (pauseNow && !previousPause) {
        if (s.state == SystemState::Active) {
            s.pause();
        } else {
            s.resume();
        }
    }
    if (calNow && !previousCal) {
        s.calibrate(now);
    }
    previousPause = pauseNow;
    previousCal = calNow;
    const bool selectionPressed = legacyControls && pressed(NODX_SWITCH);
    // Poll faster than data-ready cadence; tick only on fresh 100Hz sensor frames.
#ifdef NODX_SIMULATED
    constexpr uint32_t pollMs = start::sampleMs;
#else
    constexpr uint32_t pollMs = 2;
#endif
    if (uint32_t(now - lastPoll) >= pollMs) {
        lastPoll = now;
#ifdef NODX_SIMULATED
        if (gesturePosition < gestureScript.size()) {
            const Rates& rate = gestureScript[gesturePosition++];
            simulator.gyro = {rate[0], rate[1], rate[2]};
            if (gesturePosition == gestureScript.size()) {
                gestureScript.clear();
                gesturePosition = 0;
                simulator.gyro = {0, 0, 0};
            }
        }
        s.tick(simulator.read(now), now, selectionPressed);
#else
        MotionSample sample = mpu.read(now);
        if (sample.valid) {
            const double dt =
                sensorSnapshot.seen ? double(uint32_t(now - sensorSnapshot.lastAtMs)) / 1000.0 : 0;
            for (unsigned i = 0; i < 3 && dt > 0 && dt <= 0.05; ++i) {
                sensorSnapshot.angle[i] += double(sample.gyro[i]) * dt;
            }
            sensorSnapshot.last = sample;
            sensorSnapshot.seen = true;
            sensorSnapshot.lastAtMs = now;
            ++sensorSnapshot.frames;
            lastSample = now;
            s.tick(sample, now, selectionPressed);
        } else if (uint32_t(now - lastSample) > start::timeoutMs) {
            s.tick(sample, now, selectionPressed);
        }
        if (uint32_t(now - lastSample) > start::timeoutMs && uint32_t(now - lastProbe) >= 1000) {
            lastProbe = now;
            mpu.begin();
        }
#endif
    }
    if (NODX_BUZZER >= 0) {
        digitalWrite(NODX_BUZZER, s.feedback.buzzer ? HIGH : LOW);
    }
    if (uint32_t(now - lastDiagnostic) >= 200) {
        lastDiagnostic = now;
        diagnostic(now);
    }
    transmitTelemetry();
}
