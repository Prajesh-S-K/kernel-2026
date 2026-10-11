#include "nodx/sensor.hpp"
#include <cmath>
#include <limits>

namespace nodx {
MotionSample SimulatedSensor::read(uint32_t now) {
    MotionSample s{now, gyro, accel, true};
    if (fault == Fault::NaN) {
        s.gyro[0] = std::numeric_limits<float>::quiet_NaN();
    }
    if (fault == Fault::Extreme) {
        s.gyro[0] = 10000;
    }
    if (fault == Fault::Disconnect) {
        s.valid = false;
    }
    if (fault == Fault::Timeout) {
        s.timestampMs = now - start::timeoutMs - 1;
    }
    if (fault == Fault::FrozenTimestamp) {
        s.timestampMs = 0;
    }
    return s;
}
MotionSample ReplaySensor::read(uint32_t now) {
    if (next_ >= samples_.size() || samples_[next_].timestampMs > now) {
        return {now, {}, {0, 0, 1}, false};
    }
    return samples_[next_++];
}
const char* name(ImuVariant variant) {
    switch (variant) {
    case ImuVariant::Mpu6050:
        return "MPU-6050";
    case ImuVariant::Mpu6500:
        return "MPU-6500";
    case ImuVariant::Unknown:
        break;
    }
    return "UNKNOWN";
}
ImuVariant identifyImu(uint8_t whoAmI) {
    switch (whoAmI) {
    case 0x68:
        return ImuVariant::Mpu6050;
    case 0x70:
        return ImuVariant::Mpu6500;
    default:
        return ImuVariant::Unknown;
    }
}
float imuTemperatureC(ImuVariant variant, int16_t raw) {
    switch (variant) {
    case ImuVariant::Mpu6050:
        return float(raw) / 340.f + 36.53f;
    case ImuVariant::Mpu6500:
        return float(raw) / 333.87f + 21.f;
    case ImuVariant::Unknown:
        break;
    }
    return std::numeric_limits<float>::quiet_NaN();
}
bool MPU6050Sensor::begin() {
    ready_ = false;
    variant_ = ImuVariant::Unknown;
    uint8_t who = 0;
    if (!bus_.read(0x75, &who, 1)) {
        return false;
    }
    const ImuVariant found = identifyImu(who);
    if (found == ImuVariant::Unknown) {
        return false; // an unrecognised part is never configured or trusted
    }
    // START configuration, identical in effect on both variants (values checked against the
    // register maps; every register is read back below):
    //  0x6B PWR_MGMT_1   0x01  SLEEP clear, CLKSEL=1 (auto: PLL if ready). 6050 resets to 0x40
    //  (asleep),
    //                          6500 resets to 0x01.
    //  0x1A CONFIG       0x03  DLPF_CFG=3 (gyro 41/42 Hz, 1 kHz internal rate; on the 6500 this
    //  needs
    //                          GYRO_CONFIG FCHOICE_B=00, which 0x00 below keeps).
    //  0x19 SMPLRT_DIV   0x09  1 kHz / (1+9) = 100 Hz.
    //  0x1B GYRO_CONFIG  0x00  FS_SEL=0, +-250 deg/s, 131 LSB/(deg/s), FCHOICE_B=00.
    //  0x1C ACCEL_CONFIG 0x00  AFS_SEL=0, +-2 g, 16384 LSB/g.
    //  0x1D ACCEL_CONFIG2 0x03 (6500 only; reserved on the 6050) ACCEL_FCHOICE_B=0, A_DLPF_CFG=3
    //                          (41 Hz): the accelerometer filter the 6050's CONFIG=3 already
    //                          applies.
    //  0x38 INT_ENABLE   0x01  data-ready source enabled, because this driver gates reads on
    //  INT_STATUS
    //                          bit 0 (the 6050 map lists INT_STATUS as the status of ENABLED
    //                          sources).
    struct Setting {
        uint8_t reg, value;
    };
    const Setting common[] = {{0x6b, 0x01}, {0x1a, 0x03}, {0x19, 0x09},
                              {0x1b, 0x00}, {0x1c, 0x00}, {0x38, 0x01}};
    const Setting accelFilter{0x1d, 0x03};
    std::array<Setting, 7> plan{};
    size_t count = 0;
    for (const Setting& setting : common) {
        plan[count++] = setting;
    }
    if (found == ImuVariant::Mpu6500) {
        plan[count++] = accelFilter;
    }
    for (size_t i = 0; i < count; ++i) {
        if (!bus_.write(plan[i].reg, plan[i].value)) {
            return false;
        }
    }
    for (size_t i = 0; i < count; ++i) {
        uint8_t readBack = 0xff;
        if (!bus_.read(plan[i].reg, &readBack, 1) || readBack != plan[i].value) {
            return false;
        }
    }
    variant_ = found;
    ready_ = true;
    return true;
}
MotionSample MPU6050Sensor::read(uint32_t now) {
    MotionSample s{now, {}, {0, 0, 1}, false};
    uint8_t status = 0;
    if (!ready_ || !bus_.read(0x3a, &status, 1) || !(status & 1)) {
        return s;
    }
    uint8_t b[14];
    if (!bus_.read(0x3b, b, sizeof(b))) {
        ready_ = false;
        return s;
    }
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
    auto permutation = [](const std::array<unsigned, 3>& a) {
        return a[0] < 3 && a[1] < 3 && a[2] < 3 && a[0] != a[1] && a[1] != a[2] && a[0] != a[2];
    };
    auto signsValid = [](const std::array<float, 3>& s) {
        for (float v : s) {
            if (v != 1 && v != -1) {
                return false;
            }
        }
        return true;
    };
    return permutation(axes) && permutation(accelAxes) && signsValid(signs) &&
           signsValid(accelSigns);
}
MotionSample AxisTransform::apply(const MotionSample& in) const {
    if (!valid()) {
        MotionSample s = in;
        s.valid = false;
        return s;
    }
    MotionSample s = in;
    for (unsigned i = 0; i < 3; ++i) {
        s.gyro[i] = in.gyro[axes[i]] * signs[i];
        s.accel[i] = in.accel[accelAxes[i]] * accelSigns[i];
    }
    return s;
}
bool SensorManager::check(const MotionSample& s, uint32_t now) {
    faultCode = FaultCode::None;
    if (!s.valid) {
        faultCode = FaultCode::SensorUnavailable;
    } else if (uint32_t(now - s.timestampMs) > start::timeoutMs) {
        faultCode = FaultCode::SensorTimeout;
    } else if (seen_ && int32_t(s.timestampMs - last_) <= 0) {
        faultCode = FaultCode::Timestamp;
    } else {
        for (float v : s.gyro) {
            if (!std::isfinite(v) || std::abs(v) > start::maxGyro) {
                faultCode = FaultCode::Gyroscope;
            }
        }
        for (float v : s.accel) {
            if (!std::isfinite(v) || std::abs(v) > start::maxAccel) {
                faultCode = FaultCode::Acceleration;
            }
        }
        float norm = std::hypot(s.accel[0], std::hypot(s.accel[1], s.accel[2]));
        if (std::isfinite(norm) && (norm < .2f || norm > 3.f)) {
            faultCode = FaultCode::Gravity;
        }
    }
    reason = description(faultCode);
    if (faultCode != FaultCode::None) {
        healthy = 0;
        return false;
    }
    last_ = s.timestampMs;
    seen_ = true;
    if (healthy < start::recoverySamples) {
        ++healthy;
    }
    return true;
}
} // namespace nodx
