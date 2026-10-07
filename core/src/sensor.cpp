#include "nodx/sensor.hpp"
#include <cmath>
#include <limits>
#include <string>

namespace nodx {
MotionSample SimulatedSensor::read(uint32_t now) {
    MotionSample s{now, gyro, accel, true};
    if (fault == Fault::NaN) s.gyro[0] = std::numeric_limits<float>::quiet_NaN();
    if (fault == Fault::Extreme) s.gyro[0] = 10000;
    if (fault == Fault::Disconnect) s.valid = false;
    if (fault == Fault::Timeout) s.timestampMs = now - start::timeoutMs - 1;
    if (fault == Fault::FrozenTimestamp) s.timestampMs = 0;
    return s;
}
MotionSample ReplaySensor::read(uint32_t now) {
    if (next_ >= samples_.size() || samples_[next_].timestampMs > now) return {now, {}, {0,0,1}, false};
    return samples_[next_++];
}
bool MPU6050Sensor::begin() {
    uint8_t who = 0;
    ready_ = bus_.read(0x75, &who, 1) && who == 0x68
        && bus_.write(0x6b, 0x01) // wake, PLL gyro X
        && bus_.write(0x1a, 0x03) // DLPF START
        && bus_.write(0x19, 0x09) // 1kHz / (9+1) = 100Hz START
        && bus_.write(0x1b, 0x00) // ±250 deg/s
        && bus_.write(0x1c, 0x00); // ±2g
    return ready_;
}
MotionSample MPU6050Sensor::read(uint32_t now) {
    MotionSample s{now, {}, {0,0,1}, false};
    uint8_t status = 0;
    if (!ready_ || !bus_.read(0x3a, &status, 1) || !(status & 1)) return s;
    uint8_t b[14];
    if (!bus_.read(0x3b, b, sizeof(b))) { ready_ = false; return s; }
    auto word = [&](unsigned at) {
        return static_cast<int16_t>((uint16_t(b[at]) << 8) | b[at + 1]);
    };
    for (unsigned i = 0; i < 3; ++i) {
        s.accel[i] = word(i * 2) / 16384.f;
        s.gyro[i] = word(8 + i * 2) / 131.f;
    }
    s.valid = true;
    return s;
}
bool AxisTransform::valid() const {
    auto permutation=[](const std::array<unsigned,3>& a){return a[0]<3 && a[1]<3 && a[2]<3 && a[0]!=a[1] && a[1]!=a[2] && a[0]!=a[2];};
    auto signsValid=[](const std::array<float,3>& s){for(float v:s)if(v!=1 && v!=-1)return false;return true;};
    return permutation(axes) && permutation(accelAxes) && signsValid(signs) && signsValid(accelSigns);
}
MotionSample AxisTransform::apply(const MotionSample& in) const {
    if (!valid()) { MotionSample s = in; s.valid = false; return s; }
    MotionSample s = in;
    for (unsigned i = 0; i < 3; ++i) {
        s.gyro[i] = in.gyro[axes[i]] * signs[i];
        s.accel[i] = in.accel[accelAxes[i]] * accelSigns[i];
    }
    return s;
}
bool SensorManager::check(const MotionSample& s, uint32_t now) {
    reason = "healthy";
    if (!s.valid) reason = "sensor unavailable";
    else if (uint32_t(now - s.timestampMs) > start::timeoutMs) reason = "sensor timeout";
    else if (seen_ && int32_t(s.timestampMs - last_) <= 0) reason = "non-increasing timestamp";
    else {
        for (float v : s.gyro) if (!std::isfinite(v) || std::abs(v) > start::maxGyro) reason = "invalid gyro";
        for (float v : s.accel) if (!std::isfinite(v) || std::abs(v) > start::maxAccel) reason = "invalid acceleration";
        float norm = std::hypot(s.accel[0], std::hypot(s.accel[1], s.accel[2]));
        if (norm < .2f || norm > 3.f) reason = "invalid gravity vector";
    }
    if (std::string(reason) != "healthy") { healthy = 0; return false; }
    last_ = s.timestampMs; seen_ = true;
    if (healthy < start::recoverySamples) ++healthy;
    return true;
}
}
