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
    enable_.configure(enablePresent_, config_.switchlessQualified);
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
    healthyChecks_ = 0;
    diagnostics_.faultCode = fault;
    diagnostics_.reason = description(fault);
    diagnostics_.cursor = "SAFE_STATE";
    resetInteraction(now);
    feedback_.update(state_, now);
}

void System::stop(SystemState next, uint32_t now) {
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

void System::calibrate(uint32_t now) {
    stop(SystemState::Calibrating, now);
    if (state_ != SystemState::Calibrating) {
        return;
    }
    calibration_.start(now);
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
    if (handsFree && state_ == SystemState::Active && !enable_.permitted()) {
        stop(SystemState::Paused, now);
        diagnostics_.reason = "control switch OFF; explicit resume required";
    }
    const bool active = state_ == SystemState::Active && inputsValid && hasProfile_;
    Intent intent;
    diagnostics_.motion = {};
    // Invalid input/profile never reaches control math or telemetry numbers.
    if (inputsValid && profile_.valid()) {
        diagnostics_.motion = processor_.process(mapped, profile_, dtSeconds);
        intent = adaptive_.apply(diagnostics_.motion, profile_, dtSeconds);
    }
    bool scrolling = intent.wheel != 0;
    if (recognizing) {
        // Candidate movement is discarded, never replayed after a rejection.
        intent = Intent{};
        scrolling = false;
    }
    intent = intent_.resolve(intent, active, scrolling);
    x_ += hid_.last.dx;
    y_ += hid_.last.dy;
    if (!std::isfinite(x_) || !std::isfinite(y_)) {
        x_ = y_ = 0;
        enterSafe(FaultCode::Calculation, now);
    }
    const bool outputAllowed = active && state_ == SystemState::Active;
    Selection selected;
    if (!handsFree || !outputAllowed) {
        selected = selection_.update(lastRaw_, x_, y_, outputAllowed, scrolling, profile_, now);
    } else if (recognizing || dragging_) {
        selection_.interrupt(); // no dwell click while recognizing or dragging
    } else {
        selected = selection_.update(false, x_, y_, true, scrolling, profile_, now);
    }
    if (handsFree && outputAllowed && dragging_) {
        selected.down = true;
    }
    const Command command = interaction_.compose(intent, selected);
    // REQUIRED FINAL ORDER: safety gate -> HID manager -> transport.
    const Command safeCommand =
        safety_.gate(command, outputAllowed, inputsValid, hasProfile_ && profile_.valid(),
                     transport_.connected());
    if (!outputAllowed || recognizing) {
        hid_.reset(); // no fractional movement survives a suppressed or inactive period
    }
    const bool delivered = hid_.emit(safeCommand);
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
    enable_.configure(present, config_.switchlessQualified);
}

void System::setControlSwitch(bool on, uint32_t now) {
    const bool before = enable_.permitted();
    enable_.update(on, now);
    if (mode_ != InteractionMode::HandsFree || !before || enable_.permitted()) {
        return;
    }
    // Permitted -> inhibited. Release now: no further sensor sample is needed.
    if (state_ == SystemState::Active) {
        stop(SystemState::Paused, now);
        diagnostics_.reason = "control switch OFF; explicit resume required";
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
    if (state_ != SystemState::Active) {
        ++refused_;
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
    return s;
}
} // namespace nodx
