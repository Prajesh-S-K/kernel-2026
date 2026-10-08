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
void initializeRuntime();
void serviceRuntime();
void command(const std::string& line, uint32_t now, bool truncated = false);
void diagnostic(uint32_t now, bool ok = true, uint32_t requestId = 0);
bool acknowledgementCapacity();
void transmitTelemetry();
