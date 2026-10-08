#pragma once
#include "calibration.hpp"
#include "fault.hpp"
#include "handsfree.hpp"
#include "mapping.hpp"
#include "output.hpp"

namespace nodx {
enum class SystemState {
    Boot,
    CalibrationRequired,
    Calibrating,
    Ready,
    Active,
    Paused,
    SafeState,
    Training,
    Teaching
};
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
    // With a configuration repository the system can run the hands-free interaction mode.
    System(HIDTransport& transport, ProfileRepository& repository, HandsFreeRepository& config);
    System(const System&) = delete;
    System& operator=(const System&) = delete;

    void tick(MotionSample sample, uint32_t now, bool rawSwitch);
    void calibrate(uint32_t now, uint32_t leadMs = 0);
    void cancelCalibration();
    bool resume();
    void pause();
    bool setProfile(const UserProfile& profile, bool persist = true);
    bool temporarySettings(bool dwellEnabled, bool scrollEnabled);
    void invalidateProfile();

    // Hands-free mode. Everything here is a validated command; none of it resumes control.
    void configureEnableInput(bool present); // adapter: is an enable switch/button wired?
    // Adapter, every loop pass: the RAW input (switch ON / button pressed). What it means depends
    // on the stored EnableKind; a disable is applied here at once, without waiting for a sample.
    void setControlSwitch(bool active, uint32_t now);
    bool trainStart(GestureId id, uint32_t now); // helper: stops output, begins training
    void trainCancel();
    bool trainAccept();                    // stage the validated pattern (not saved)
    bool commitHandsFree();                // helper: convert + save, READY afterwards
    bool useLegacyMode();                  // helper: explicit compatibility mode
    void stageSwitchless(bool qualified);  // helper: alternative to the enable switch
    void stageEnableKind(EnableKind kind); // helper: maintained switch or momentary button
    // Temporary movement-only demo (RAM only, off at every boot, never saved): no dwell clicks, no
    // drag, no wheel, and pointer steps bounded. Every safety check and the enable input stay as
    // they are. Changing it while control is active pauses control (explicit resume afterwards).
    bool setDemoMovementOnly(bool on);
    bool demoMovementOnly() const {
        return demoMovementOnly_;
    }
    // Temporary UNCALIBRATED pointer demo: real sensor, AxisTransform, filtering, output bounds,
    // SafetyManager -> HIDManager, but a validated RAM-only demo profile instead of a user profile.
    // It needs the physical enable button's permission (momentary latch) and is stopped by it. Movement only. Never saved, never a calibration, never
    // started by a connection or a reboot; any stop needs an explicit restart.
    bool startUncalibratedDemo(uint32_t now);
    void stopUncalibratedDemo(const char* reason);
    bool uncalibratedDemo() const {
        return uncal_;
    }
    const char* uncalibratedDemoBlocker() const; // nullptr when a start would be accepted
    // Optional dwell clicking inside the running uncalibrated demo: off by default, accepted only
    // while the demo is active, cleared by every stop. One primary-button click per completed
    // dwell; no drag, double-click, right-click or scrolling. Settings are RAM only.
    bool setUncalibratedDwell(bool on, uint32_t now);
    bool setUncalibratedDwellSettings(uint32_t dwellMs, float tolerance);
    // Fallback pointing controls (RAM only): swap the sign of horizontal and/or vertical movement.
    void setUncalibratedReversal(bool horizontal, bool vertical);
    // Physical enable permission for the fallback demo. On by default; only tests relax it.
    void setUncalibratedNeedsEnable(bool required) {
        uncalNeedsEnable_ = required;
    }
    // Guided mapping and configured control. Teaching is never started by a reboot, reconnect or
    // fault; its result lives in RAM until saved explicitly; saving never blocks using it.
    void setControlRepository(ControlRepository& repository); // loads the stored settings
    bool teachStart(uint32_t now);
    void teachCancel();
    bool teachAccept();                          // preview accepted: active in RAM, not saved
    bool teachSave();                            // explicit, transactional; failure keeps RAM use
    void clearLearned();                         // forget the RAM settings (the stored record stays)
    bool startConfiguredControl(uint32_t now);
    const char* configuredBlocker() const;       // nullptr when a start would be accepted
    const char* teachBlocker() const;
    bool configuredControl() const {
        return uncal_ && configured_;
    }
    MappingStatus mappingStatus(uint32_t now) const;
    const LearnedControl& learned() const {
        return learned_;
    }
    bool learnedValid() const {
        return learnedValid_;
    }
    bool uncalibratedDwell() const {
        return uncal_ && uncalDwell_;
    }
    float dwellProgress(uint32_t now) const; // progress of the profile that is actually in use
    ProfileState profileState() const;
    UserProfile uncalibratedDemoProfile() const; // validated demo configuration (RAM only)
    UserProfile configuredProfile() const;       // gains from the learned mapping
    HandsFreeStatus handsFreeStatus() const;
    const char* activationBlocker() const; // nullptr when resume would be allowed

    // Read-only compatibility views. State changes must pass through commands.
    const SystemState& state = state_;
    const UserProfile& profile = profile_;
    const bool& hasProfile = hasProfile_;
    const CalibrationEngine& calibration = calibration_;
    const SelectionManager& selection = selection_;
    const Diagnostics& diagnostics = diagnostics_;
    const Feedback& feedback = feedback_;
    const HIDManager& hid = hid_;
    const InteractionMode& interaction = mode_;
    const bool& dragging = dragging_;
    const GestureRecognizer& recognizer = recognizer_;
    const GestureTrainer& trainer = trainer_;
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
    // hands-free state
    HandsFreeRepository* configRepo_ = nullptr;
    HandsFreeConfig config_;
    ConfigState configState_ = ConfigState::Missing;
    InteractionMode mode_ = InteractionMode::Legacy;
    EnableGate enable_;
    bool enablePresent_ = false;
    GestureRecognizer recognizer_;
    GestureTrainer trainer_;
    std::array<bool, gestureCount> staged_{};
    std::array<GestureTemplate, gestureCount> stagedTemplates_{};
    float stagedNeutral_ = 0;
    bool stagedSwitchless_ = false;
    EnableKind stagedKind_ = EnableKind::Momentary; // a new setup assumes the push button
    bool dragging_ = false;
    bool demoMovementOnly_ = false;
    bool uncal_ = false;
    bool configured_ = false; // the running session uses the learned mapping (not the default one)
    ControlRepository* controlRepo_ = nullptr;
    LearnedControl learned_;
    bool learnedValid_ = false, learnedSaved_ = false;
    ControlRecordState controlRecord_ = ControlRecordState::Missing;
    const char* saveResult_ = "";
    MappingTeacher teacher_;
    ControlProcessor controlProc_;
    Vec3 lastAccel_{0, 0, 1};
    bool uncalDwell_ = false;
    uint32_t uncalDwellMs_ = start::uncalDwellMs;
    float uncalDwellTolerance_ = start::uncalDwellTolerance;
    uint32_t uncalClicks_ = 0;
    bool profileInvalidated_ = false;
    UserProfile uncalProfile_;
    EnableGate uncalGate_; // the momentary enable button's permission latch for the fallback demo
    bool uncalNeedsEnable_ = true; // physical enable permission is required (tests may relax it)
    bool uncalPrevPress_ = false;  // without the requirement a press still only STOPS the demo
    bool uncalReverseX_ = false, uncalReverseY_ = false; // RAM only, user controls
    uint32_t refused_ = 0;

    const char* sessionBlocker(bool configured) const;
    bool startSession(uint32_t now, bool configured);
    void enterSafe(FaultCode fault, uint32_t now);
    void resetInteraction(uint32_t now);
    bool emitStationary();
    void stop(SystemState next, uint32_t now);
    void advanceCalibration(const MotionSample& mapped, uint32_t now);
    void advanceTraining(const MotionSample& mapped, uint32_t now);
    void loadConfig();
    bool updateGestures(const MotionSample& mapped, uint32_t now, bool inputsValid);
    void executeGesture(GestureId id);
    GestureTemplate effectiveTemplate(unsigned id) const;
    bool setupAllowed() const;
    bool commitFailed(const char* reason, bool storage);
};
} // namespace nodx
