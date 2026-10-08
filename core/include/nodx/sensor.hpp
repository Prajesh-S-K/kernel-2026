#pragma once
#include "fault.hpp"
#include "parameters.hpp"
#include <array>
#include <cstddef>
#include <utility>
#include <vector>

namespace nodx {
struct MotionSample {
    uint32_t timestampMs = 0;
    std::array<float, 3> gyro{};         // degrees/second, sensor coordinates
    std::array<float, 3> accel{0, 0, 1}; // g
    bool valid = true;
};
class Sensor {
public:
    virtual ~Sensor() = default;
    virtual MotionSample read(uint32_t now) = 0;
};
enum class Fault { None, NaN, Extreme, Timeout, Disconnect, FrozenTimestamp };
class SimulatedSensor : public Sensor {
public:
    std::array<float, 3> gyro{};
    std::array<float, 3> accel{0, 0, 1};
    Fault fault = Fault::None;
    MotionSample read(uint32_t now) override;
};
class ReplaySensor : public Sensor {
public:
    explicit ReplaySensor(std::vector<MotionSample> samples) : samples_(std::move(samples)) {}
    MotionSample read(uint32_t now) override;

private:
    std::vector<MotionSample> samples_;
    size_t next_ = 0;
};
class RegisterBus {
public:
    virtual ~RegisterBus() = default;
    virtual bool write(uint8_t reg, uint8_t value) = 0;
    virtual bool read(uint8_t reg, uint8_t* bytes, size_t count) = 0;
};
// ±250 deg/s and ±2g. Conversion isolated from all downstream modules.
class MPU6050Sensor : public Sensor {
public:
    explicit MPU6050Sensor(RegisterBus& bus) : bus_(bus) {}
    bool begin();
    MotionSample read(uint32_t now) override;

private:
    RegisterBus& bus_;
    bool ready_ = false;
};
struct AxisTransform {
    std::array<unsigned, 3> axes{2, 0, 1}; // yaw pitch roll START mount assumption
    std::array<float, 3> signs{1, 1, 1};
    std::array<unsigned, 3> accelAxes{1, 0, 2}; // roll-Y gravity uses -sensor X / sensor Z
    std::array<float, 3> accelSigns{1, -1, 1};
    bool valid() const;
    MotionSample apply(const MotionSample& input) const;
};
class SensorManager {
public:
    bool check(const MotionSample& sample, uint32_t now);
    unsigned healthy = 0;
    const char* reason = "boot";
    FaultCode faultCode = FaultCode::None;

private:
    uint32_t last_ = 0;
    bool seen_ = false;
};
} // namespace nodx
