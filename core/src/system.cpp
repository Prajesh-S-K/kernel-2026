#include "nodx/system.hpp"
#include <algorithm>
#include <cmath>

namespace nodx {
const char* name(SystemState state) {
    switch (state) {
    case SystemState::Boot:
        return "BOOT";
    case SystemState::CalibrationRequired:
        return "CALIBRATION_REQUIRED";
    case SystemState::Calibrating:
        return "CALIBRATING";
    case SystemState::Ready:
        return "READY";
    case SystemState::Active:
        return "ACTIVE";
    case SystemState::Paused:
        return "PAUSED";
    case SystemState::SafeState:
        return "SAFE_STATE";
    case SystemState::Training:
        return "TRAINING";
    }
    return "SAFE_STATE";
}

void Feedback::update(SystemState state, uint32_t now) {
    if (state != previous_) {
        until_ = now + (state == SystemState::SafeState ? 300 : 80);
        previous_ = state;
    }
    buzzer = int32_t(until_ - now) > 0;
}

System::System(HIDTransport& transport, ProfileRepository& repository)
    : hid_(transport), transport_(transport), repository_(repository) {
    hasProfile_ = repository_.load(profile_);
    state_ = hasProfile_ ? SystemState::Ready : SystemState::CalibrationRequired;
    diagnostics_.reason =
        hasProfile_ ? "profile loaded; explicit resume required" : "calibration required";
}

System::System(HIDTransport& transport, ProfileRepository& repository, HandsFreeRepository& config)
    : System(transport, repository) {
    configRepo_ = &config;
    loadConfig();
}

void System::loadConfig() {
    configState_ = configRepo_->load(config_);
    if (configState_ == ConfigState::Valid) {
        mode_ = config_.enabled ? InteractionMode::HandsFree : InteractionMode::Legacy;
    } else {
        config_ = HandsFreeConfig{};
        mode_ = configState_ == ConfigState::Missing ? InteractionMode::Legacy
                                                     : InteractionMode::ConfigInvalid;
    }
    stagedSwitchless_ = config_.switchlessQualified;
    // A stored record keeps the kind it was saved with (a pre-button record is MAINTAINED). Only a
    // setup with no usable record assumes the push button.
    stagedKind_ = configState_ == ConfigState::Valid ? config_.enableKind : EnableKind::Momentary;
    enable_.configure(enablePresent_, config_.switchlessQualified, config_.enableKind);
    recognizer_.configure(config_.gestures, (1u << gestureCount) - 1);
    if (mode_ == InteractionMode::ConfigInvalid) {
        diagnostics_.reason =
            "hands-free configuration invalid; helper setup or legacy mode needed";
    } else if (mode_ == InteractionMode::HandsFree) {
        diagnostics_.reason =
            hasProfile_ ? "hands-free ready; explicit resume required" : "calibration required";
    }
}

void System::resetInteraction(uint32_t now) {
    selection_.reset(lastRaw_, now);
    processor_.reset();
    hid_.reset();
    diagnostics_.motion = {};
    dragging_ = false;
    recognizer_.reset(now);
}

bool System::emitStationary() {
    // Stop commands use the same final safety boundary as ordinary motion.
    hid_.reset();
    hid_.requireReport(); // an explicit stop always sends its release
    Command stationary = safety_.gate({}, false, false, false, transport_.connected());
    return hid_.emit(stationary);
}

void System::enterSafe(FaultCode fault, uint32_t now) {
    if (state_ != SystemState::SafeState) {
        ++diagnostics_.faults;
    }
    if (state_ == SystemState::Calibrating) {
        calibration_.cancel();
        calibration_.reason = description(fault);
    }
    if (state_ == SystemState::Training) {
        trainer_.cancel();
    }
    state_ = SystemState::SafeState;
    uncal_ = false; // a fault always ends the demo; restarting is explicit
    uncalDwell_ = false;
    uncalGate_.clearLatch(); // a button permission never survives a fault; press again after recovery
    enable_.clearLatch(); // a button permission never survives a fault; press again after recovery
    healthyChecks_ = 0;
    diagnostics_.faultCode = fault;
    diagnostics_.reason = description(fault);
    diagnostics_.cursor = "SAFE_STATE";
    resetInteraction(now);
    feedback_.update(state_, now);
}

void System::stop(SystemState next, uint32_t now) {
    uncal_ = false; // every stop path (pause, calibration, training, ...) ends the demo
    uncalDwell_ = false; // dwell clicking never outlives the demo
    if (state_ == SystemState::Training && next != SystemState::Training) {
        trainer_.cancel();
    }
    state_ = next;
    resetInteraction(now);
    diagnostics_.cursor = next == SystemState::Paused ? "PAUSED" : "WARNING";
    if (!emitStationary()) {
        enterSafe(FaultCode::Transport, now);
    }
    feedback_.update(state_, now);
}

void System::calibrate(uint32_t now, uint32_t leadMs) {
    stop(SystemState::Calibrating, now);
    if (state_ != SystemState::Calibrating) {
        return;
    }
    calibration_.start(now, leadMs);
    diagnostics_.reason = "calibration collecting";
}

void System::cancelCalibration() {
    calibration_.cancel();
    stop(hasProfile_ ? SystemState::Ready : SystemState::CalibrationRequired, lastTick_);
    if (state_ != SystemState::SafeState) {
        diagnostics_.reason = "calibration cancelled; previous profile preserved";
    }
}

bool System::resume() {
    if (state_ != SystemState::Ready && state_ != SystemState::Paused) {
        return false;
    }
    if (activationBlocker()) {
        return false;
    }
    if (!emitStationary()) {
        enterSafe(FaultCode::Transport, lastTick_);
        return false;
    }
    resetInteraction(lastTick_);
    if (mode_ == InteractionMode::HandsFree) {
        // A resume gesture leaves the user still; require movement before the first dwell click.
        selection_.lockAt(x_, y_);
    }
    state_ = SystemState::Active;
    diagnostics_.faultCode = FaultCode::None;
    diagnostics_.reason = "active";
    return true;
}

void System::pause() {
    // Pause must never erase a fault or turn an unqualified device into READY.
    if (state_ != SystemState::Active && state_ != SystemState::Paused) {
        if (!emitStationary()) {
            enterSafe(FaultCode::Transport, lastTick_);
        }
        return;
    }
    stop(SystemState::Paused, lastTick_);
    if (state_ == SystemState::Paused) {
        diagnostics_.reason = "paused; explicit resume required";
    }
}

bool System::setProfile(const UserProfile& candidate, bool persist) {
    if (!candidate.valid()) {
        return false;
    }
    if (persist && mode_ == InteractionMode::HandsFree && !candidate.dwellEnabled) {
        return false; // hands-free selection is dwell; turning it off needs legacy mode
    }
    stop(hasProfile_ ? SystemState::Ready : SystemState::CalibrationRequired, lastTick_);
    if (state_ == SystemState::SafeState) {
        return false;
    }
    // Release before potentially slow storage. Failed writes retain the old profile.
    if (persist && !repository_.save(candidate)) {
        diagnostics_.faultCode = FaultCode::Storage;
        diagnostics_.reason = description(FaultCode::Storage);
        return false;
    }
    profile_ = candidate;
    hasProfile_ = true;
    profileInvalidated_ = false;
    state_ = SystemState::Ready;
    diagnostics_.faultCode = FaultCode::None;
    diagnostics_.reason = "valid profile; explicit resume required";
    return true;
}

bool System::temporarySettings(bool dwellEnabled, bool scrollEnabled) {
    if (!hasProfile_) {
        return false;
    }
    UserProfile candidate = profile_;
    candidate.dwellEnabled = dwellEnabled;
    candidate.scrollEnabled = scrollEnabled;
    return setProfile(candidate, false);
}

void System::invalidateProfile() {
    hasProfile_ = false;
    profileInvalidated_ = true;
    enterSafe(FaultCode::Profile, lastTick_);
    emitStationary();
}

void System::advanceCalibration(const MotionSample& mapped, uint32_t now) {
    calibration_.tick(mapped, now);
    if (calibration_.phase == CalPhase::Save) {
        UserProfile candidate = calibration_.candidate;
        if (mode_ == InteractionMode::HandsFree) {
            candidate.dwellEnabled = true; // new hands-free profiles use dwell selection
        }
        if (setProfile(candidate)) {
            calibration_.phase = CalPhase::Complete;
        } else {
            calibration_.phase = CalPhase::Failed;
            calibration_.reason = "save or output release failed";
        }
    }
    if (calibration_.phase == CalPhase::Failed && state_ != SystemState::SafeState) {
        state_ = hasProfile_ ? SystemState::Ready : SystemState::CalibrationRequired;
    }
}

void System::tick(MotionSample raw, uint32_t now, bool pressed) {
    const bool handsFree = mode_ == InteractionMode::HandsFree;
    ++diagnostics_.ticks;
    lastRaw_ = pressed && !handsFree; // physical selection is legacy compatibility only
    const uint32_t elapsedMs = uint32_t(now - lastTick_);
    const bool timingValid = !hadTick_ || (elapsedMs > 0 && elapsedMs <= start::timeoutMs);
    const float dtSeconds = hadTick_ ? float(elapsedMs) / 1000.f : float(start::sampleMs) / 1000.f;
    hadTick_ = true;
    lastTick_ = now;

    FaultCode fault = FaultCode::None;
    if (!sensors_.check(raw, now)) {
        fault = sensors_.faultCode;
    }
    if (!axes.valid()) {
        fault = FaultCode::AxisMapping;
    }
    if (!timingValid) {
        fault = FaultCode::LoopTiming;
    }
    if (hasProfile_ && !profile_.valid()) {
        fault = FaultCode::Profile;
    }
    // The demo profile is RAM-only and re-checked on every pass; it can never be silently invalid.
    if (uncal_ && !uncalProfile_.valid()) {
        fault = FaultCode::Profile;
    }
    if (!transport_.connected()) {
        fault = FaultCode::Transport;
    }
    if (fault != FaultCode::None) {
        enterSafe(fault, now);
    }

    const bool inputsValid = fault == FaultCode::None;
    const MotionSample mapped = axes.apply(raw);
    if (state_ == SystemState::Calibrating && inputsValid) {
        advanceCalibration(mapped, now);
    }
    if (state_ == SystemState::Training && inputsValid) {
        advanceTraining(mapped, now);
    }
    const bool recognizing = handsFree && updateGestures(mapped, now, inputsValid);

    // Defensive: control never stays active while the maintained switch inhibits it.
    if (handsFree && !uncal_ && state_ == SystemState::Active && !enable_.permitted()) {
        stop(SystemState::Paused, now);
        diagnostics_.reason = "control switch OFF; explicit resume required";
    }
    if (uncal_ && uncalNeedsEnable_ && !uncalGate_.permitted()) {
        stopUncalibratedDemo("demo stopped: enable permission lost; explicit restart required");
    }
    const bool movementOnly = demoMovementOnly_ || uncal_;
    const UserProfile& control = uncal_ ? uncalProfile_ : profile_;
    const bool profileOk = uncal_ || (hasProfile_ && profile_.valid());
    const bool active = state_ == SystemState::Active && inputsValid && (hasProfile_ || uncal_);
    Intent intent;
    diagnostics_.motion = {};
    // Invalid input/profile never reaches control math or telemetry numbers.
    if (inputsValid && control.valid()) {
        diagnostics_.motion = processor_.process(mapped, control, dtSeconds);
        intent = adaptive_.apply(diagnostics_.motion, control, dtSeconds);
    }
    bool scrolling = intent.wheel != 0;
    if (recognizing) {
        // Candidate movement is discarded, never replayed after a rejection.
        intent = Intent{};
        scrolling = false;
    }
    intent = intent_.resolve(intent, active, scrolling);
    if (uncal_) {
        intent.dx = uncalReverseX_ ? -intent.dx : intent.dx;
        intent.dy = uncalReverseY_ ? -intent.dy : intent.dy;
    }
    if (movementOnly) {
        intent.wheel = 0; // scrolling still suppresses pointing, but nothing is scrolled
    }
    x_ += hid_.last.dx;
    y_ += hid_.last.dy;
    if (!std::isfinite(x_) || !std::isfinite(y_)) {
        x_ = y_ = 0;
        enterSafe(FaultCode::Calculation, now);
    }
    const bool outputAllowed = active && state_ == SystemState::Active;
    Selection selected;
    const bool dwellClicking = uncal_ && uncalDwell_ && !demoMovementOnly_;
    if (dwellClicking && outputAllowed) {
        // Explicitly enabled dwell in the uncalibrated demo: the normal selection manager with the
        // demo profile. No raw switch, no scrolling; hold/drag can never come out of it.
        selected = selection_.update(false, x_, y_, true, false, control, now);
        selected.down = false;
    } else if (movementOnly && outputAllowed) {
        selection_.interrupt(); // movement-only demo: no dwell click, no selection, no drag
    } else if (!handsFree || !outputAllowed) {
        selected = selection_.update(lastRaw_, x_, y_, outputAllowed, scrolling, profile_, now);
    } else if (recognizing || dragging_) {
        selection_.interrupt(); // no dwell click while recognizing or dragging
    } else {
        selected = selection_.update(false, x_, y_, true, scrolling, profile_, now);
    }
    if (handsFree && outputAllowed && dragging_) {
        selected.down = true;
    }
    Command command = interaction_.compose(intent, selected);
    if (movementOnly) {
        const float limit = uncal_ ? start::uncalDemoMaxStep : start::demoMaxStep;
        command.down = false;
        command.pulse = command.pulse && dwellClicking;
        command.wheel = 0;
        command.dx = std::clamp(command.dx, -limit, limit);
        command.dy = std::clamp(command.dy, -limit, limit);
    }
    // REQUIRED FINAL ORDER: safety gate -> HID manager -> transport.
    const Command safeCommand =
        safety_.gate(command, outputAllowed, inputsValid, profileOk, transport_.connected());
    if (!outputAllowed || recognizing) {
        hid_.reset(); // no fractional movement survives a suppressed or inactive period
    }
    const bool delivered = hid_.emit(safeCommand);
    if (dwellClicking && safeCommand.pulse) {
        ++uncalClicks_;
    }
    if (safety_.calculationFault) {
        enterSafe(FaultCode::Calculation, now);
        emitStationary();
    } else if (!delivered) {
        enterSafe(FaultCode::Transport, now);
    } else if (inputsValid) {
        if (healthyChecks_ < start::recoverySamples) {
            ++healthyChecks_;
        }
        if (state_ == SystemState::SafeState && healthyChecks_ >= start::recoverySamples) {
            state_ = hasProfile_ ? SystemState::Ready : SystemState::CalibrationRequired;
            diagnostics_.faultCode = FaultCode::None;
            diagnostics_.reason = "recovered; explicit resume required";
        }
    }

    if (state_ == SystemState::SafeState) {
        diagnostics_.cursor = "SAFE_STATE";
    } else if (state_ == SystemState::Paused) {
        diagnostics_.cursor = "PAUSED";
    } else if (!outputAllowed) {
        diagnostics_.cursor = "WARNING";
    } else if (recognizing) {
        diagnostics_.cursor = "GESTURE";
    } else if (selected.pulse) {
        diagnostics_.cursor = "CLICK";
    } else if (selected.down) {
        diagnostics_.cursor = "DRAG";
    } else if (scrolling) {
        diagnostics_.cursor = "SCROLL";
    } else if (selection_.dwell == DwellState::Progress) {
        diagnostics_.cursor = "DWELL_PROGRESS";
    } else if (selection_.dwell == DwellState::Arming) {
        diagnostics_.cursor = "DWELL_ARMING";
    } else {
        diagnostics_.cursor = intent.mode;
    }
    feedback_.update(state_, now);
}

// ------------------------------------------------------------------ hands-free
const char* System::activationBlocker() const {
    if (!hasProfile_ || !profile_.valid()) {
        return "calibrated profile required";
    }
    if (mode_ == InteractionMode::ConfigInvalid) {
        return "hands-free configuration invalid";
    }
    if (mode_ == InteractionMode::HandsFree) {
        if (!profile_.dwellEnabled) {
            return "dwell selection required in hands-free mode";
        }
        if (const char* gate = enable_.blocked()) {
            return gate;
        }
    }
    if (healthyChecks_ < start::recoverySamples) {
        return "waiting for healthy sensor samples";
    }
    return nullptr;
}

void System::configureEnableInput(bool present) {
    enablePresent_ = present;
    uncalGate_.configure(present, false, EnableKind::Momentary);
    enable_.configure(present, config_.switchlessQualified, config_.enableKind);
}

void System::setControlSwitch(bool active, uint32_t now) {
    uncalGate_.update(active, now);
    const bool pressEdge = active && !uncalPrevPress_;
    uncalPrevPress_ = active;
    if (uncal_ && ((uncalNeedsEnable_ && !uncalGate_.permitted()) || pressEdge)) {
        // Disable at the press edge: release now, without waiting for another sensor sample.
        stopUncalibratedDemo("demo stopped by the enable button; explicit restart required");
    }
    const bool before = enable_.permitted();
    enable_.update(active, now);
    if (mode_ != InteractionMode::HandsFree || !before || enable_.permitted()) {
        return;
    }
    // Permitted -> inhibited. Release now: no further sensor sample is needed.
    if (state_ == SystemState::Active) {
        stop(SystemState::Paused, now);
        diagnostics_.reason =
            enable_.kind() == EnableKind::Momentary
                ? "control disabled by the enable button; explicit resume required"
                : "control switch OFF; explicit resume required";
    } else {
        resetInteraction(now);
        if (!emitStationary()) {
            enterSafe(FaultCode::Transport, now);
        }
    }
    feedback_.update(state_, now);
}

bool System::updateGestures(const MotionSample& mapped, uint32_t now, bool inputsValid) {
    const bool listening = inputsValid && hasProfile_ && enable_.permitted() &&
                           (state_ == SystemState::Ready || state_ == SystemState::Paused ||
                            state_ == SystemState::Active);
    if (!listening) {
        recognizer_.reset(now);
        return false;
    }
    const Rates rate{mapped.gyro[0] - profile_.bias[0], mapped.gyro[1] - profile_.bias[1],
                     mapped.gyro[2] - profile_.bias[2]};
    const GestureEvent event = recognizer_.update(rate, now);
    if (event.executed) {
        executeGesture(event.id);
        return false;
    }
    return recognizer_.suppressing();
}

void System::executeGesture(GestureId id) {
    if (id == GestureId::PauseResume) {
        if (state_ == SystemState::Active) {
            pause();
        } else if (!resume()) {
            ++refused_; // a gesture can never override a failed safety condition
        }
        return;
    }
    if (state_ != SystemState::Active || demoMovementOnly_) {
        ++refused_; // no drag outside active control, and none in the movement-only demo
        return;
    }
    if (dragging_) {
        dragging_ = false; // the release report is composed later in this same tick
        selection_.lockAt(x_, y_);
        diagnostics_.reason = "drag released";
    } else {
        dragging_ = true;
        selection_.interrupt();
        diagnostics_.reason = "drag started";
    }
}

GestureTemplate System::effectiveTemplate(unsigned id) const {
    if (staged_[id]) {
        return stagedTemplates_[id];
    }
    if (configState_ == ConfigState::Valid) {
        return config_.gestures.templates[id];
    }
    return {};
}

bool System::setupAllowed() const {
    return configRepo_ && state_ != SystemState::Calibrating && state_ != SystemState::SafeState &&
           state_ != SystemState::Training;
}

void System::advanceTraining(const MotionSample& mapped, uint32_t now) {
    trainer_.tick({mapped.gyro[0] - profile_.bias[0], mapped.gyro[1] - profile_.bias[1],
                   mapped.gyro[2] - profile_.bias[2]},
                  now);
    diagnostics_.reason = trainer_.reason;
}

bool System::trainStart(GestureId id, uint32_t now) {
    if (!configRepo_ || !hasProfile_ || !profile_.valid() || state_ == SystemState::SafeState ||
        state_ == SystemState::Calibrating) {
        return false;
    }
    stop(SystemState::Training, now); // inhibits and releases output immediately
    if (state_ != SystemState::Training) {
        return false;
    }
    trainer_.begin(id, now, effectiveTemplate(1 - unsigned(id)));
    diagnostics_.reason = trainer_.reason;
    return true;
}

void System::trainCancel() {
    if (state_ != SystemState::Training) {
        return;
    }
    stop(hasProfile_ ? SystemState::Ready : SystemState::CalibrationRequired, lastTick_);
    if (state_ != SystemState::SafeState) {
        diagnostics_.reason = "training cancelled; prior configuration preserved";
    }
}

bool System::trainAccept() {
    if (state_ != SystemState::Training || trainer_.phase != TrainPhase::Ready ||
        !trainer_.validated) {
        return false;
    }
    const unsigned id = static_cast<unsigned>(trainer_.id);
    const GestureTemplate learned = trainer_.candidate;
    const float neutral = trainer_.neutralRate;
    stop(SystemState::Ready, lastTick_);
    if (state_ != SystemState::Ready) {
        return false;
    }
    staged_[id] = true;
    stagedTemplates_[id] = learned;
    stagedNeutral_ = std::max(stagedNeutral_, neutral);
    diagnostics_.reason = "pattern staged, not saved; commit hands-free setup to keep it";
    return true;
}

void System::stageSwitchless(bool qualified) {
    stagedSwitchless_ = qualified;
}
void System::stageEnableKind(EnableKind kind) {
    stagedKind_ = kind;
}

bool System::commitFailed(const char* reason, bool storage) {
    if (storage) {
        diagnostics_.faultCode = FaultCode::Storage;
    }
    diagnostics_.reason = reason;
    return false;
}

bool System::commitHandsFree() {
    if (!setupAllowed() || !hasProfile_) {
        return false;
    }
    HandsFreeConfig next;
    next.enabled = true;
    next.switchlessQualified = stagedSwitchless_;
    next.enableKind = stagedKind_;
    next.gestures.neutralRate = std::max(
        stagedNeutral_, configState_ == ConfigState::Valid ? config_.gestures.neutralRate : 0.f);
    for (unsigned id = 0; id < gestureCount; ++id) {
        next.gestures.templates[id] = effectiveTemplate(id);
    }
    if (!next.valid()) {
        return commitFailed("train both gestures with distinct patterns first", false);
    }
    UserProfile saved;
    if (!repository_.load(saved)) {
        return commitFailed("save a calibrated profile before hands-free setup", false);
    }
    stop(hasProfile_ ? SystemState::Ready : SystemState::CalibrationRequired, lastTick_);
    if (state_ == SystemState::SafeState) {
        return false;
    }
    UserProfile converted = saved;
    converted.dwellEnabled = true;
    const bool changed = !saved.dwellEnabled;
    // Order matters: the profile is converted first, the configuration record is the single
    // commit point. A failure restores the prior profile and leaves the prior record untouched.
    if (changed && !repository_.save(converted)) {
        return commitFailed("hands-free setup not saved: profile write failed", true);
    }
    if (!configRepo_->save(next)) {
        if (changed) {
            repository_.save(saved); // best effort; the in-memory profile is unchanged anyway
        }
        return commitFailed("hands-free setup not saved: configuration write failed", true);
    }
    profile_ = converted;
    hasProfile_ = true;
    config_ = next;
    configState_ = ConfigState::Valid;
    mode_ = InteractionMode::HandsFree;
    staged_ = {};
    stagedNeutral_ = 0;
    enable_.setSwitchless(next.switchlessQualified);
    if (enable_.kind() != next.enableKind) {
        enable_.setKind(next.enableKind); // a different input kind starts disabled again
    }
    recognizer_.configure(next.gestures, (1u << gestureCount) - 1);
    recognizer_.reset(lastTick_);
    diagnostics_.faultCode = FaultCode::None;
    diagnostics_.reason = "hands-free setup saved; explicit resume required";
    return true;
}

bool System::useLegacyMode() {
    if (!setupAllowed()) {
        return false;
    }
    HandsFreeConfig next = configState_ == ConfigState::Valid ? config_ : HandsFreeConfig{};
    next.enabled = false;
    if (configState_ != ConfigState::Valid) {
        next.enableKind = stagedKind_; // no stored record to keep: use what the helper staged
    }
    stop(hasProfile_ ? SystemState::Ready : SystemState::CalibrationRequired, lastTick_);
    if (state_ == SystemState::SafeState) {
        return false;
    }
    if (!configRepo_->save(next)) {
        return commitFailed("legacy mode not saved: configuration write failed", true);
    }
    config_ = next;
    configState_ = ConfigState::Valid;
    mode_ = InteractionMode::Legacy;
    recognizer_.reset(lastTick_);
    diagnostics_.faultCode = FaultCode::None;
    diagnostics_.reason = "legacy compatibility mode saved; explicit resume required";
    return true;
}

bool System::setDemoMovementOnly(bool on) {
    if (on == demoMovementOnly_) {
        return true;
    }
    if (state_ == SystemState::Active || dragging_) {
        stop(SystemState::Paused, lastTick_); // releases everything before the rules change
        if (state_ == SystemState::SafeState) {
            return false;
        }
        diagnostics_.reason = "movement-only demo mode changed; explicit resume required";
    }
    demoMovementOnly_ = on;
    return true;
}

ProfileState System::profileState() const {
    if (hasProfile_) {
        return ProfileState::Valid;
    }
    // A stored record that fails to decode (or was invalidated at run time) stays CORRUPT; neither
    // the demo nor calibration-free control ever repairs or accepts it.
    return profileInvalidated_ || repository_.state() == ProfileState::Corrupt
               ? ProfileState::Corrupt
               : ProfileState::Missing;
}

UserProfile System::uncalibratedDemoProfile() const {
    UserProfile demo; // bias 0: no rest measurement exists, so the deadzone must cover idle bias
    demo.deadzone = {start::uncalDemoDeadzone, start::uncalDemoDeadzone};
    demo.gain = {start::uncalDemoGain, start::uncalDemoGain, start::uncalDemoGain,
                 start::uncalDemoGain};
    demo.alpha = start::uncalDemoAlpha;
    demo.dwellEnabled = uncalDwell_; // movement-only unless dwell clicking was explicitly enabled
    demo.dwellMs = uncalDwellMs_;
    demo.dwellTolerance = uncalDwellTolerance_;
    demo.scrollEnabled = false;
    return demo;
}

const char* System::uncalibratedDemoBlocker() const {
    if (uncal_ || state_ == SystemState::Active) {
        return "control is already active";
    }
    if (state_ == SystemState::Calibrating) {
        return "calibration in progress";
    }
    if (state_ == SystemState::Training) {
        return "gesture training in progress";
    }
    if (state_ == SystemState::SafeState) {
        return "safe state: wait for the fault to clear";
    }
    if (!axes.valid()) {
        return "axis mapping invalid";
    }
    if (!transport_.connected()) {
        return "BLE link unavailable";
    }
    if (healthyChecks_ < start::recoverySamples) {
        return "waiting for healthy sensor samples";
    }
    if (uncalNeedsEnable_ && !uncalGate_.present()) {
        return "enable button not present";
    }
    if (uncalNeedsEnable_ && !uncalGate_.permitted()) {
        return "press the enable button first";
    }
    if (!uncalibratedDemoProfile().valid()) {
        return "demo configuration invalid";
    }
    return nullptr;
}

bool System::startUncalibratedDemo(uint32_t now) {
    if (const char* blocker = uncalibratedDemoBlocker()) {
        diagnostics_.reason = blocker;
        return false;
    }
    uncalDwell_ = false; // every start is movement-only until dwell is enabled again
    uncalProfile_ = uncalibratedDemoProfile();
    if (!emitStationary()) {
        enterSafe(FaultCode::Transport, now);
        return false;
    }
    resetInteraction(now);
    state_ = SystemState::Active;
    uncal_ = true; // after resetInteraction(); never persisted, never reported as calibration
    uncalClicks_ = 0;
    diagnostics_.faultCode = FaultCode::None;
    diagnostics_.reason = "uncalibrated demo active; movement only";
    return true;
}

void System::stopUncalibratedDemo(const char* reason) {
    if (!uncal_) {
        return;
    }
    stop(hasProfile_ ? SystemState::Ready : SystemState::CalibrationRequired, lastTick_);
    if (state_ != SystemState::SafeState) {
        diagnostics_.reason = reason;
    }
    feedback_.update(state_, lastTick_);
}

void System::setUncalibratedReversal(bool horizontal, bool vertical) {
    uncalReverseX_ = horizontal;
    uncalReverseY_ = vertical;
}

bool System::setUncalibratedDwell(bool on, uint32_t now) {
    if (!uncal_) {
        return false; // movement-only demo is the default; dwell needs a running demo
    }
    if (on == uncalDwell_) {
        return true;
    }
    uncalDwell_ = on;
    uncalProfile_ = uncalibratedDemoProfile();
    selection_.reset(false, now); // fresh dwell: progress 0, nothing carried over
    diagnostics_.reason = on ? "uncalibrated demo: dwell clicking enabled"
                             : "uncalibrated demo: movement only";
    return true;
}

bool System::setUncalibratedDwellSettings(uint32_t dwellMs, float tolerance) {
    UserProfile candidate;
    candidate.dwellMs = dwellMs;
    candidate.dwellTolerance = tolerance;
    if (!std::isfinite(tolerance) || !candidate.valid()) {
        return false; // bounds are the profile's own: 500-5000 ms, tolerance 2-50
    }
    uncalDwellMs_ = dwellMs;
    uncalDwellTolerance_ = tolerance;
    uncalProfile_ = uncalibratedDemoProfile();
    if (uncal_) {
        selection_.reset(false, lastTick_); // a changed threshold restarts the dwell
    }
    return true;
}

float System::dwellProgress(uint32_t now) const {
    return selection_.progress(now, uncal_ ? uncalProfile_ : profile_);
}

HandsFreeStatus System::handsFreeStatus() const {
    HandsFreeStatus s;
    s.mode = name(mode_);
    s.config = name(configState_);
    s.configId = configState_ == ConfigState::Valid ? configId(config_) : 0;
    s.switchPresent = enable_.present();
    s.switchOn = enable_.on();
    s.permitted = enable_.permitted();
    s.switchless = enable_.switchless();
    s.switchlessStaged = stagedSwitchless_;
    s.switchKind = name(enable_.kind());
    s.switchKindStaged = name(stagedKind_);
    s.switchPressed = enable_.pressed();
    s.switchLatched = enable_.kind() == EnableKind::Momentary && enable_.on();
    s.switchArmed = enable_.armed();
    s.recognizer = name(recognizer_.state());
    s.lastGesture = recognizer_.lastGesture < 0
                        ? "NONE"
                        : name(static_cast<GestureId>(recognizer_.lastGesture));
    s.lastReject = name(recognizer_.lastReject);
    s.candidates = recognizer_.candidates;
    s.rejected = recognizer_.rejected;
    s.executed = recognizer_.executed;
    s.refused = refused_;
    s.suppressing = recognizer_.suppressing();
    s.dragging = dragging_;
    s.demoMovementOnly = demoMovementOnly_;
    s.trainPhase = name(trainer_.phase);
    s.trainGesture = trainer_.phase == TrainPhase::Idle ? "NONE" : name(trainer_.id);
    s.trainReason = trainer_.reason;
    s.trainAccepted = trainer_.accepted;
    s.trainRequired = start::trainExamples;
    s.trainRejects = trainer_.rejects;
    s.trainValidated = trainer_.validated;
    for (unsigned id = 0; id < gestureCount; ++id) {
        s.staged[id] = staged_[id];
        s.stored[id] =
            configState_ == ConfigState::Valid && config_.gestures.templates[id].configured();
    }
    const char* blocker = activationBlocker();
    s.blocked = blocker ? blocker : "";
    const char* uncalBlocker = uncalibratedDemoBlocker();
    s.uncalActive = uncal_;
    s.uncalBlocked = uncal_ ? "" : (uncalBlocker ? uncalBlocker : "");
    s.profileState = name(profileState());
    const UserProfile shown = uncalibratedDemoProfile();
    s.uncalGain = shown.gain[0];
    s.uncalDeadzone = shown.deadzone[0];
    s.uncalMaxStep = start::uncalDemoMaxStep;
    s.uncalNeedsEnable = uncalNeedsEnable_;
    s.uncalPresent = uncalGate_.present();
    s.uncalPermitted = uncalGate_.permitted();
    s.uncalReverseX = uncalReverseX_;
    s.uncalReverseY = uncalReverseY_;
    s.uncalDwellEnabled = uncal_ && uncalDwell_;
    s.uncalDwellMs = uncalDwellMs_;
    s.uncalDwellTolerance = uncalDwellTolerance_;
    s.uncalClicks = uncalClicks_;
    s.uncalDwellState = uncal_ && uncalDwell_ ? name(selection_.dwell) : "IDLE";
    s.uncalDwellProgress = uncal_ && uncalDwell_ ? selection_.progress(lastTick_, uncalProfile_) : 0.f;
    return s;
}
} // namespace nodx
