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
// InvenSense/TDK six-axis parts this driver knows by their WHO_AM_I value. Both are accepted as
// DISTINCT variants; any other identity is rejected and nothing is written to it.
//   0x68  MPU-6000/6050  (RM-MPU-6000A-00 rev 4.0, register 117: default 0x68)
//   0x70  MPU-6500       (RM-MPU-6500A-00 rev 2.1, register 117: default 0x70)
enum class ImuVariant : uint8_t { Unknown, Mpu6050, Mpu6500 };
const char* name(ImuVariant variant);
ImuVariant identifyImu(uint8_t whoAmI);
// Die temperature from the raw TEMP_OUT word. MPU-6050: raw/340 + 36.53 (RM-MPU-6000A-00 reg
// 65-66). MPU-6500: raw/333.87 + 21 with a room-temperature offset of 0 LSB (PS-MPU-6500A rev 1.3).
// Not used by control; the two formulas differ, so the variant decides. Unknown gives NaN.
float imuTemperatureC(ImuVariant variant, int16_t raw);
// ±250 deg/s (131 LSB/(deg/s)) and ±2 g (16384 LSB/g) on both variants. Conversion isolated from
// all downstream modules. Equal consecutive frames are NOT a fault: a still sensor produces them.
class MPU6050Sensor : public Sensor {
public:
    explicit MPU6050Sensor(RegisterBus& bus) : bus_(bus) {}
    // Identify, initialise and READ BACK every configuration register; false (and not ready) on an
    // unknown identity, a bus error or any register that did not take the value written.
    bool begin();
    MotionSample read(uint32_t now) override;
    ImuVariant variant() const {
        return variant_;
    }

private:
    RegisterBus& bus_;
    bool ready_ = false;
    ImuVariant variant_ = ImuVariant::Unknown;
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
