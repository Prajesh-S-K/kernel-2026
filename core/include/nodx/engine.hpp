#pragma once
#include "profile.hpp"
#include "sensor.hpp"
#include <string>

namespace nodx {
enum class SystemState { Boot, CalibrationRequired, Calibrating, Ready, Active, Paused, SafeState };
const char* name(SystemState state);
enum class CalPhase { Idle, Rest, Left, Right, Up, Down, Natural, Analyze, Validate, Save, Complete, Failed };
const char* name(CalPhase phase);
struct Statistics {
    unsigned count = 0;
    double mean = 0, m2 = 0;
    void add(float x);
    float sigma() const;
};
class CalibrationEngine {
public:
    void start(uint32_t now);
    void cancel();
    void tick(const MotionSample& mapped, uint32_t now);
    CalPhase phase = CalPhase::Idle;
    const char* reason = "idle";
    UserProfile candidate;
    float progress(uint32_t now) const;
private:
    uint32_t phaseStart_ = 0;
    std::array<Statistics, 3> rest_{};
    std::array<Statistics, 4> directions_{};
    unsigned natural_ = 0;
    void analyze();
};
struct Motion {
    float x = 0, y = 0, roll = 0, stability = 1;
};
class MotionProcessor {
public:
    Motion process(const MotionSample& sample, const UserProfile& profile, float dt);
    void reset();
private:
    std::array<float, 3> filtered_{};
    float roll_ = 0;
};
struct Intent { float dx = 0, dy = 0, wheel = 0; const char* mode = "NORMAL"; };
class AdaptiveEngine {
public:
    Intent apply(const Motion& motion, const UserProfile& profile, float dt) const;
};
class IntentEngine {
public:
    Intent resolve(Intent intent, bool allowed, bool scrolling) const;
};
class DebouncedSwitch {
public:
    bool update(bool raw, uint32_t now);
    void reset(bool raw, uint32_t now);
    bool pressed() const { return stable_; }
private:
    bool raw_ = false, stable_ = false;
    uint32_t changed_ = 0;
};
enum class DwellState { Idle, Arming, Progress, Click, Lockout };
const char* name(DwellState state);
struct Selection { bool down = false; bool pulse = false; };
class SelectionManager {
public:
    Selection update(bool rawSwitch, double x, double y, bool allowed,
                     bool scrolling, const UserProfile& p, uint32_t now);
    void reset(bool rawSwitch, uint32_t now);
    DwellState dwell = DwellState::Idle;
    uint32_t cancellations = 0;
    float progress(uint32_t now, const UserProfile& p) const;
private:
    DebouncedSwitch switch_;
    bool requireRelease_ = false;
    double anchorX_ = 0, anchorY_ = 0;
    uint32_t since_ = 0;
};
struct Command { float dx = 0, dy = 0, wheel = 0; bool down = false, pulse = false; };
class InteractionEngine {
public:
    Command compose(const Intent& intent, const Selection& selection) const {
        return {intent.dx, intent.dy, intent.wheel, selection.down, selection.pulse};
    }
};
struct Report { int8_t dx = 0, dy = 0, wheel = 0; bool down = false; };
class SafetyManager {
public:
    Command gate(Command command, bool active, bool healthy, bool profileValid, bool connected);
    bool calculationFault = false;
};
class HIDTransport {
public:
    virtual ~HIDTransport() = default;
    virtual bool connected() const = 0;
    virtual bool send(const Report& report) = 0;
};
class HIDManager {
public:
    explicit HIDManager(HIDTransport& transport) : transport_(transport) {}
    bool emit(Command safeCommand);
    void reset();
    Report last;
private:
    HIDTransport& transport_;
    float remainderX_ = 0, remainderY_ = 0, remainderWheel_ = 0;
};
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
    const char* reason = "boot";
    Motion motion;
    const char* cursor = "WARNING";
};
class System {
public:
    System(HIDTransport& transport, ProfileRepository& repository);
    void tick(MotionSample sample, uint32_t now, bool rawSwitch);
    void calibrate(uint32_t now);
    void cancelCalibration();
    bool resume();
    void pause();
    bool setProfile(const UserProfile& profile, bool persist = true);
    void invalidateProfile();
    SystemState state = SystemState::Boot;
    UserProfile profile;
    bool hasProfile = false;
    AxisTransform axes;
    CalibrationEngine calibration;
    SelectionManager selection;
    Diagnostics diagnostics;
    Feedback feedback;
    HIDManager hid;
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
    bool hadTick_ = false, transportFailed_ = false;
    double x_ = 0, y_ = 0;
    bool lastRaw_ = false;
    void safe(const char* reason, uint32_t now);
};
}
