#pragma once

namespace nodx {
enum class FaultCode {
    None,
    SensorUnavailable,
    SensorTimeout,
    Timestamp,
    Gyroscope,
    Acceleration,
    Gravity,
    AxisMapping,
    LoopTiming,
    Profile,
    Calculation,
    Transport,
    Storage,
};

constexpr const char* name(FaultCode code) {
    switch (code) {
    case FaultCode::None:
        return "NONE";
    case FaultCode::SensorUnavailable:
        return "SENSOR_UNAVAILABLE";
    case FaultCode::SensorTimeout:
        return "SENSOR_TIMEOUT";
    case FaultCode::Timestamp:
        return "TIMESTAMP";
    case FaultCode::Gyroscope:
        return "GYROSCOPE";
    case FaultCode::Acceleration:
        return "ACCELERATION";
    case FaultCode::Gravity:
        return "GRAVITY";
    case FaultCode::AxisMapping:
        return "AXIS_MAPPING";
    case FaultCode::LoopTiming:
        return "LOOP_TIMING";
    case FaultCode::Profile:
        return "PROFILE";
    case FaultCode::Calculation:
        return "CALCULATION";
    case FaultCode::Transport:
        return "TRANSPORT";
    case FaultCode::Storage:
        return "STORAGE";
    }
    return "CALCULATION";
}

constexpr const char* description(FaultCode code) {
    switch (code) {
    case FaultCode::None:
        return "healthy";
    case FaultCode::SensorUnavailable:
        return "sensor unavailable";
    case FaultCode::SensorTimeout:
        return "sensor timeout";
    case FaultCode::Timestamp:
        return "non-increasing timestamp";
    case FaultCode::Gyroscope:
        return "invalid gyro";
    case FaultCode::Acceleration:
        return "invalid acceleration";
    case FaultCode::Gravity:
        return "invalid gravity vector";
    case FaultCode::AxisMapping:
        return "axis mapping invalid";
    case FaultCode::LoopTiming:
        return "loop timing fault";
    case FaultCode::Profile:
        return "profile invalid";
    case FaultCode::Calculation:
        return "calculation invalid";
    case FaultCode::Transport:
        return "HID connection or delivery failed";
    case FaultCode::Storage:
        return "profile save failed";
    }
    return "calculation invalid";
}
} // namespace nodx
