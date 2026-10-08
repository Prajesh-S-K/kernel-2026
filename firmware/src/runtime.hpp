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
void initializeRuntime();
void serviceRuntime();
void command(const std::string& line, uint32_t now, bool truncated = false);
void diagnostic(uint32_t now, bool ok = true, uint32_t requestId = 0);
bool acknowledgementCapacity();
void transmitTelemetry();
