#pragma once
#include "adapters.hpp"
#include <string>
extern NVSStorage storage;
extern I2CBus bus;
extern MPU6050Sensor mpu;
extern SimulatedSensor simulator;
extern BLEHID ble;
extern ProfileRepository repository;
extern NVSConfigStorage configStorage;
extern HandsFreeRepository configRepository;
#ifdef NODX_SIMULATED
extern bool simulatedEnable;             // simulated raw enable input (default released)
extern std::vector<Rates> gestureScript; // synthetic gesture samples still to play
#endif
extern System* systemEngine;
// Hardware builds keep the last raw sensor frame (sensor coordinates, before any axis mapping) and
// the running integral of each gyro axis so a bench session can read axis directions from
// telemetry.
struct SensorSnapshot {
    MotionSample last;
    bool seen = false;
    uint32_t frames = 0, lastAtMs = 0;
    double angle[3] = {0, 0, 0}; // degrees, integral of the raw gyro per sensor axis since boot
};
extern SensorSnapshot sensorSnapshot;
// Bench capture of the RAW sensor stream (sensor coordinates, integer register units: gyro 131 LSB per
// deg/s, accel 16384 LSB per g) at the sensor rate, for offline replay of the recognizers. RAM only,
// passive (it never affects control), at most 20 seconds, read back in pages with `capture get`.
constexpr size_t captureCapacity = 2000;
constexpr size_t capturePage = 16;
struct CaptureRow {
    uint32_t t = 0;
    int16_t g[3] = {0, 0, 0}, a[3] = {0, 0, 0};
};
struct CaptureState {
    bool active = false;
    size_t count = 0;
    uint32_t untilMs = 0;
    size_t pageOffset = size_t(-1); // set by `capture get`; the next frame carries that page
};
extern CaptureState captureState;
extern CaptureRow captureRows[captureCapacity];
void captureSample(const MotionSample& sample, uint32_t now);
extern const char* bootResetReason; // why the chip last restarted (set by main.cpp)
void initializeRuntime();
void serviceRuntime();
void command(const std::string& line, uint32_t now, bool truncated = false);
void diagnostic(uint32_t now, bool ok = true, uint32_t requestId = 0);
bool acknowledgementCapacity();
void transmitTelemetry();
