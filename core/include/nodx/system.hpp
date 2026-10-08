#pragma once
#include "calibration.hpp"
#include "fault.hpp"
#include "output.hpp"

namespace nodx {
enum class SystemState { Boot, CalibrationRequired, Calibrating, Ready, Active, Paused, SafeState };
const char* name(SystemState state);
class Feedback {
public:
    void update(SystemState state, uint32_t now);
    bool buzzer = false;

private:
    SystemState previous_ = SystemState::Boot;
    uint32_t until_ = 0;
};
struct Diagnostics {
    uint64_t ticks = 0;
    uint32_t faults = 0;
    FaultCode faultCode = FaultCode::None;
    const char* reason = "boot";
    Motion motion;
    const char* cursor = "WARNING";
};
class System {
private:
    SystemState state_ = SystemState::Boot;
    UserProfile profile_;
    bool hasProfile_ = false;
    CalibrationEngine calibration_;
    SelectionManager selection_;
    Diagnostics diagnostics_;
    Feedback feedback_;
    HIDManager hid_;

public:
    System(HIDTransport& transport, ProfileRepository& repository);
    System(const System&) = delete;
    System& operator=(const System&) = delete;

    void tick(MotionSample sample, uint32_t now, bool rawSwitch);
    void calibrate(uint32_t now);
    void cancelCalibration();
    bool resume();
    void pause();
    bool setProfile(const UserProfile& profile, bool persist = true);
    bool temporarySettings(bool dwellEnabled, bool scrollEnabled);
    void invalidateProfile();

    // Read-only compatibility views. State changes must pass through commands.
    const SystemState& state = state_;
    const UserProfile& profile = profile_;
    const bool& hasProfile = hasProfile_;
    const CalibrationEngine& calibration = calibration_;
    const SelectionManager& selection = selection_;
    const Diagnostics& diagnostics = diagnostics_;
    const Feedback& feedback = feedback_;
    const HIDManager& hid = hid_;
    AxisTransform axes;

private:
    HIDTransport& transport_;
    ProfileRepository& repository_;
    SensorManager sensors_;
    MotionProcessor processor_;
    AdaptiveEngine adaptive_;
    IntentEngine intent_;
    InteractionEngine interaction_;
    SafetyManager safety_;
    uint32_t lastTick_ = 0;
    unsigned healthyChecks_ = 0;
    bool hadTick_ = false;
    double x_ = 0, y_ = 0;
    bool lastRaw_ = false;

    void enterSafe(FaultCode fault, uint32_t now);
    void resetInteraction(uint32_t now);
    bool emitStationary();
    void stop(SystemState next, uint32_t now);
    void advanceCalibration(const MotionSample& mapped, uint32_t now);
};
} // namespace nodx
