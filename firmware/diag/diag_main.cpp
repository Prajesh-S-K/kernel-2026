// NodX Adapt BENCH DIAGNOSTIC (bring-up stage 1). Serial output only.
// No BLE, no HID, no buzzer, no motion output. Nothing runs unless a serial command asks for it.
// Pins come from -DNODX_SDA / -DNODX_SCL / -DNODX_ENABLE in the bench environment: START values.
//
// Commands (one per line, 115200 baud):  help | info | scan | imu [seconds] | button [seconds] |
// all Every result line starts with "DIAG," and is key=value so scripts/bench_analyze.py can read
// it.
#include "diag_logic.hpp"
#include "nodx/handsfree.hpp"
#include <Arduino.h>
#include <Wire.h>
#include <cmath>
#include <cstring>
#include <esp_system.h>

#ifndef NODX_SDA
#define NODX_SDA -1
#endif
#ifndef NODX_SCL
#define NODX_SCL -1
#endif
#ifndef NODX_ENABLE
#define NODX_ENABLE -1
#endif

namespace {
using nodx::diag::Burst;
using nodx::diag::EdgeLog;
using nodx::diag::RunningStats;

constexpr uint8_t kMpu = 0x68;
constexpr uint32_t kQuietUs = 20000; // edges closer than this belong to one press/release burst

bool wireStarted = false;
bool pinsConfigured() {
    return NODX_SDA >= 0 && NODX_SCL >= 0;
}
bool startWire() {
    if (!pinsConfigured()) {
        return false;
    }
    if (!wireStarted) {
        wireStarted = Wire.begin(NODX_SDA, NODX_SCL, 100000);
        Wire.setTimeOut(20);
    }
    return wireStarted;
}
bool readReg(uint8_t reg, uint8_t* out, size_t count) {
    Wire.beginTransmission(kMpu);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) {
        return false;
    }
    if (Wire.requestFrom(kMpu, static_cast<uint8_t>(count)) != count) {
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        out[i] = Wire.read();
    }
    return true;
}
bool writeReg(uint8_t reg, uint8_t value) {
    Wire.beginTransmission(kMpu);
    Wire.write(reg);
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

// ---------------------------------------------------------------- stage 0: board facts
void info() {
    Serial.printf("DIAG,info,chip=%s,revision=%u,cores=%u,cpuMHz=%u\n", ESP.getChipModel(),
                  unsigned(ESP.getChipRevision()), unsigned(ESP.getChipCores()),
                  unsigned(ESP.getCpuFreqMHz()));
    Serial.printf("DIAG,info,flashBytes=%u,flashMHz=%u,flashMode=%d,psramBytes=%u,freeHeap=%u\n",
                  unsigned(ESP.getFlashChipSize()), unsigned(ESP.getFlashChipSpeed() / 1000000),
                  int(ESP.getFlashChipMode()), unsigned(ESP.getPsramSize()),
                  unsigned(ESP.getFreeHeap()));
    Serial.printf("DIAG,info,resetReason=%d,sdk=%s,arduinoEsp32=%d.%d.%d\n",
                  int(esp_reset_reason()), ESP.getSdkVersion(), ESP_ARDUINO_VERSION_MAJOR,
                  ESP_ARDUINO_VERSION_MINOR, ESP_ARDUINO_VERSION_PATCH);
#ifdef ARDUINO_USB_CDC_ON_BOOT
    Serial.printf("DIAG,info,usbCdcOnBoot=%d\n", int(ARDUINO_USB_CDC_ON_BOOT));
#else
    Serial.println("DIAG,info,usbCdcOnBoot=not-set,warning=Serial-is-UART0-not-USB");
#endif
#ifdef ARDUINO_USB_MODE
    Serial.printf("DIAG,info,usbMode=%d,note=1-is-hardware-CDC-JTAG-serial\n",
                  int(ARDUINO_USB_MODE));
#endif
    Serial.printf("DIAG,info,pinSda=%d,pinScl=%d,pinEnable=%d,buzzer=disabled,ble=not-compiled\n",
                  int(NODX_SDA), int(NODX_SCL), int(NODX_ENABLE));
}

// ---------------------------------------------------------------- stage 1: I2C bus
void scan() {
    if (!pinsConfigured()) {
        Serial.println("DIAG,scan,error=pins-not-configured");
        return;
    }
    // Idle levels before the bus is driven: with the breakout's pull-ups both should read HIGH.
    pinMode(NODX_SDA, INPUT);
    pinMode(NODX_SCL, INPUT);
    delay(5);
    Serial.printf("DIAG,scan,idleSda=%d,idleScl=%d\n", digitalRead(NODX_SDA),
                  digitalRead(NODX_SCL));
    if (!startWire()) {
        Serial.println("DIAG,scan,error=wire-begin-failed");
        return;
    }
    unsigned found = 0;
    for (uint8_t address = 1; address < 127; ++address) {
        Wire.beginTransmission(address);
        if (Wire.endTransmission() == 0) {
            ++found;
            Serial.printf("DIAG,scan,address=0x%02X\n", address);
        }
    }
    Serial.printf("DIAG,scan,found=%u\n", found);
}

// ---------------------------------------------------------------- stage 2: the MPU6050
// Mirrors the register sequence of MPU6050Sensor::begin() so the answer applies to the firmware,
// then measures what the firmware's data-ready gating (INT_STATUS bit 0 of register 0x3A) actually
// sees with and without INT_ENABLE (register 0x38), and samples raw frames at rest.
unsigned pollDataReady(unsigned milliseconds) {
    unsigned seen = 0, polls = 0;
    const uint32_t end = millis() + milliseconds;
    while (int32_t(millis() - end) < 0) {
        uint8_t status = 0;
        if (readReg(0x3a, &status, 1)) {
            ++polls;
            seen += status & 1;
        }
        delay(1);
    }
    Serial.printf("DIAG,imu,pollMs=%u,polls=%u,dataReadyBitSeen=%u\n", milliseconds, polls, seen);
    return seen;
}
void imu(unsigned seconds) {
    if (!startWire()) {
        Serial.println("DIAG,imu,error=pins-not-configured-or-wire-failed");
        return;
    }
    uint8_t who = 0;
    const bool whoOk = readReg(0x75, &who, 1);
    Serial.printf("DIAG,imu,whoAmIRead=%d,whoAmI=0x%02X,expected=0x68\n", whoOk, who);
    if (!whoOk) {
        return;
    }
    writeReg(0x6b, 0x80); // device reset, then wake with the firmware's clock source
    delay(100);
    const bool init = writeReg(0x6b, 0x01) && writeReg(0x1a, 0x03) && writeReg(0x19, 0x09) &&
                      writeReg(0x1b, 0x00) && writeReg(0x1c, 0x00);
    Serial.printf(
        "DIAG,imu,firmwareInitSequenceOk=%d,dlpf=3,sampleDiv=9,gyroRange=250,accelRange=2\n", init);
    uint8_t intEnable = 0xff;
    readReg(0x38, &intEnable, 1);
    Serial.printf("DIAG,imu,intEnableBefore=0x%02X\n", intEnable);
    delay(50);
    const unsigned withoutEnable = pollDataReady(1000); // what the firmware does today
    writeReg(0x38, 0x01);                               // DATA_RDY_EN
    delay(20);
    const unsigned withEnable = pollDataReady(1000);
    Serial.printf("DIAG,imu,dataReadyNoIntEnable=%u,dataReadyWithIntEnable=%u\n", withoutEnable,
                  withEnable);
    // Raw frames at rest, read on a 10 ms timer whether or not the status bit says ready.
    RunningStats gx, gy, gz, ax, ay, az, mag, temp, interval;
    unsigned frames = 0, changed = 0, errors = 0;
    uint8_t previous[14] = {};
    uint32_t last = micros(), next = micros() + 10000;
    const uint32_t end = millis() + seconds * 1000u;
    while (int32_t(millis() - end) < 0) {
        while (int32_t(micros() - next) < 0) {
        }
        next += 10000;
        uint8_t b[14];
        if (!readReg(0x3b, b, sizeof(b))) {
            ++errors;
            continue;
        }
        const uint32_t now = micros();
        interval.add(double(uint32_t(now - last)));
        last = now;
        ++frames;
        for (size_t i = 0; i < sizeof(b); ++i) {
            if (b[i] != previous[i]) {
                ++changed;
                break;
            }
        }
        memcpy(previous, b, sizeof(b));
        auto word = [&](size_t at) { return int16_t((uint16_t(b[at]) << 8) | b[at + 1]); };
        const double fx = word(0) / 16384.0, fy = word(2) / 16384.0, fz = word(4) / 16384.0;
        ax.add(fx);
        ay.add(fy);
        az.add(fz);
        mag.add(std::sqrt(fx * fx + fy * fy + fz * fz));
        temp.add(word(6) / 340.0 + 36.53);
        gx.add(word(8) / 131.0);
        gy.add(word(10) / 131.0);
        gz.add(word(12) / 131.0);
    }
    writeReg(0x38, 0x00); // leave INT_ENABLE as the firmware expects it
    Serial.printf("DIAG,imu,seconds=%u,frames=%u,framesChanged=%u,i2cErrors=%u\n", seconds, frames,
                  changed, errors);
    Serial.printf("DIAG,imu,intervalUsMean=%.0f,intervalUsMin=%.0f,intervalUsMax=%.0f\n",
                  interval.mean(), interval.min(), interval.max());
    Serial.printf("DIAG,imu,gyroMeanDps=%.3f/%.3f/%.3f,gyroStdDps=%.3f/%.3f/%.3f\n", gx.mean(),
                  gy.mean(), gz.mean(), gx.stddev(), gy.stddev(), gz.stddev());
    Serial.printf(
        "DIAG,imu,accelMeanG=%.4f/%.4f/%.4f,accelStdG=%.4f/%.4f/%.4f,accelMagMeanG=%.4f\n",
        ax.mean(), ay.mean(), az.mean(), ax.stddev(), ay.stddev(), az.stddev(), mag.mean());
    Serial.printf("DIAG,imu,tempC=%.2f\n", temp.mean());
    Serial.println("DIAG,imu,note=keep the board still for this test");
}

// Read-only register dump plus raw frames: what the sensor holds right now, with NO writes at all.
// Added after the first bench run, which found WHO_AM_I=0x70 and frozen near-zero accel/gyro
// values.
void imuRegisters() {
    if (!startWire()) {
        Serial.println("DIAG,regs,error=pins-not-configured-or-wire-failed");
        return;
    }
    struct Reg {
        uint8_t address;
        const char* name;
    };
    const Reg registers[] = {{0x19, "SMPLRT_DIV"},   {0x1a, "CONFIG"},        {0x1b, "GYRO_CONFIG"},
                             {0x1c, "ACCEL_CONFIG"}, {0x1d, "ACCEL_CONFIG2"}, {0x23, "FIFO_EN"},
                             {0x37, "INT_PIN_CFG"},  {0x38, "INT_ENABLE"},    {0x3a, "INT_STATUS"},
                             {0x6a, "USER_CTRL"},    {0x6b, "PWR_MGMT_1"},    {0x6c, "PWR_MGMT_2"},
                             {0x75, "WHO_AM_I"}};
    for (const Reg& reg : registers) {
        uint8_t value = 0;
        const bool ok = readReg(reg.address, &value, 1);
        Serial.printf("DIAG,regs,reg=%s,address=0x%02X,read=%d,value=0x%02X\n", reg.name,
                      reg.address, ok, value);
    }
    for (unsigned frame = 0; frame < 6; ++frame) {
        uint8_t b[14] = {};
        const bool ok = readReg(0x3b, b, sizeof(b));
        Serial.printf("DIAG,regs,frame=%u,read=%d,raw=", frame, ok);
        for (size_t i = 0; i < sizeof(b); ++i) {
            Serial.printf("%02X", b[i]);
        }
        Serial.println();
        delay(10);
    }
    Serial.println("DIAG,regs,note=read-only;accel=bytes0-5,temp=6-7,gyro=8-13");
}

// ---------------------------------------------------------------- stage 3: the enable button
// Records every level change with microsecond timestamps (bounce), and runs the REAL momentary gate
// from the firmware so the same rules are seen with the real part: raw pressed vs latched
// permission.
EdgeLog<256> edgeLog;
void button(unsigned seconds) {
    if (NODX_ENABLE < 0) {
        Serial.println("DIAG,button,error=enable-pin-not-configured");
        return;
    }
    pinMode(NODX_ENABLE, INPUT_PULLUP);
    delay(5);
    bool level = digitalRead(NODX_ENABLE);
    Serial.printf("DIAG,button,idleLevel=%d,expected=1,note=pressed-reads-0\n", level);
    nodx::EnableGate gate;
    gate.configure(true, false, nodx::EnableKind::Momentary);
    edgeLog.clear();
    unsigned toggles = 0;
    bool permitted = false;
    Serial.printf("DIAG,button,prompt=press-and-release-the-button-now,seconds=%u\n", seconds);
    const uint32_t end = millis() + seconds * 1000u;
    while (int32_t(millis() - end) < 0) {
        const uint32_t nowUs = micros();
        const bool now = digitalRead(NODX_ENABLE);
        if (now != level) {
            level = now;
            edgeLog.add(nowUs, level);
        }
        gate.update(!now, millis());
        if (gate.permitted() != permitted) {
            permitted = gate.permitted();
            ++toggles;
            Serial.printf("DIAG,button,gateToggle=%u,latched=%d,atMs=%lu\n", toggles, permitted,
                          static_cast<unsigned long>(millis()));
        }
    }
    Burst bursts[64];
    const size_t count =
        nodx::diag::groupBursts(edgeLog.data(), edgeLog.size(), kQuietUs, bursts, 64);
    for (size_t i = 0; i < count; ++i) {
        Serial.printf("DIAG,button,burst=%u,kind=%s,edges=%u,spanUs=%lu\n", unsigned(i + 1),
                      bursts[i].settledLevel ? "release" : "press", bursts[i].edges,
                      static_cast<unsigned long>(bursts[i].spanUs));
    }
    Serial.printf("DIAG,button,edges=%u,droppedEdges=%u,bursts=%u,gateToggles=%u,latchedAtEnd=%d\n",
                  unsigned(edgeLog.size()), unsigned(edgeLog.dropped()), unsigned(count), toggles,
                  permitted);
}

void help() {
    Serial.println("DIAG,help,commands=help|info|scan|imu [seconds]|imuregs|button [seconds]|all");
}
unsigned secondsArg(const String& line, unsigned fallback) {
    const int space = line.indexOf(' ');
    if (space < 0) {
        return fallback;
    }
    const long value = line.substring(space + 1).toInt();
    return value >= 1 && value <= 120 ? unsigned(value) : fallback;
}
String command;
bool commandSeen = false;
uint32_t lastHeartbeat = 0;
} // namespace

void setup() {
    // Native USB CDC: size the TX buffer before begin() so a burst of result lines is not cut
    // short. The baud rate is ignored by the USB port. Output printed before the host opens the
    // port is lost, so the idle heartbeat below repeats the banner until the first command arrives.
    Serial.setTxBufferSize(4096);
    Serial.begin(115200);
    delay(1500);
    Serial.println("DIAG,banner,name=NodX-bench-diagnostic,stage=1,ble=none,hid=none,flash=none");
    info();
    help();
}
void loop() {
    if (!commandSeen && millis() - lastHeartbeat >= 3000) {
        lastHeartbeat = millis();
        Serial.printf("DIAG,idle,atMs=%lu,waiting-for-command=help|info|scan|imu|button|all\n",
                      static_cast<unsigned long>(millis()));
    }
    while (Serial.available()) {
        const char c = char(Serial.read());
        if (c == '\n' || c == '\r') {
            command.trim();
            if (command.length()) {
                commandSeen = true;
            }
            if (command == "info") {
                info();
            } else if (command == "scan") {
                scan();
            } else if (command == "imuregs") {
                imuRegisters();
            } else if (command.startsWith("imu")) {
                imu(secondsArg(command, 10));
            } else if (command.startsWith("button")) {
                button(secondsArg(command, 20));
            } else if (command == "all") {
                info();
                scan();
                imu(10);
                button(20);
            } else if (command.length()) {
                help();
            }
            command = "";
        } else if (command.length() < 40) {
            command += c;
        }
    }
    delay(2);
}
