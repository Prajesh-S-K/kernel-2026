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
    case SystemState::Teaching:
        return "TEACHING";
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
    controlProc_.reset();
    clickRec_.reset(now);
    quickRec_.reset(now);
    hid_.reset();
    diagnostics_.motion = {};
    dragging_ = false;
    palette_.reset(); // pending actions, a held drag and a scroll never survive a reset
    hoverSeen_ = PaletteTarget::None;
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
    if (state_ == SystemState::Teaching) {
        teacher_.cancel();
    }
    if (state_ == SystemState::Teaching && teachKind_ == TeachKind::Click) {
        clickTrainer_.cancel();
    }
    if (state_ == SystemState::Teaching && teachKind_ == TeachKind::Quick) {
        quickPractice_.cancel();
    }
    configured_ = false;
    clickEnabled_ = false;
    quickEnabled_ = false;
    actionsEnabled_ = false;
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
    if (state_ == SystemState::Teaching && next != SystemState::Teaching) {
        teacher_.cancel();
    }
    configured_ = false;
    clickEnabled_ = false;
    quickEnabled_ = false;
    actionsEnabled_ = false;
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
    if (inputsValid) {
        lastAccel_ = {raw.accel[0], raw.accel[1], raw.accel[2]};
    }
    const MotionSample mapped = axes.apply(raw);
    if (state_ == SystemState::Teaching && inputsValid && teachKind_ == TeachKind::Quick) {
        quickPractice_.tick(clickFrame(raw, mapped, quickTrainingConfigured_), now);
        if (quickPractice_.phase() == QuickPhase::Failed) {
            const char* why = quickPractice_.status(now).reason;
            stop(hasProfile_ ? SystemState::Ready : SystemState::CalibrationRequired, now);
            diagnostics_.reason = why;
        }
    } else if (state_ == SystemState::Teaching && inputsValid && teachKind_ == TeachKind::Click) {
        clickTrainer_.tick(clickFrame(raw, mapped, clickTrainingConfigured_), now);
        if (clickTrainer_.phase() == ClickPhase::Failed) {
            const char* why = clickTrainer_.status(now).reason;
            stop(hasProfile_ ? SystemState::Ready : SystemState::CalibrationRequired, now);
            diagnostics_.reason = why;
        }
    } else if (state_ == SystemState::Teaching && inputsValid) {
        teacher_.tick(raw, now);
        if (teacher_.phase() == MapPhase::Failed) {
            const char* why = teacher_.status(now).reason;
            stop(hasProfile_ ? SystemState::Ready : SystemState::CalibrationRequired, now);
            diagnostics_.reason = why; // the plain-words reason stays visible
        }
    }
    if (state_ == SystemState::Calibrating && inputsValid) {
        advanceCalibration(mapped, now);
    }
    if (state_ == SystemState::Training && inputsValid) {
        advanceTraining(mapped, now);
    }
    bool recognizing = handsFree && updateGestures(mapped, now, inputsValid);
    bool gestureClick = false;
    const bool clickActive = uncal_ && clickEnabled_ && inputsValid && state_ == SystemState::Active;
    if (clickActive) {
        gestureClick = clickRec_.update(clickFrame(raw, mapped, configured_), now).accepted;
        recognizing = recognizing || clickRec_.suppressing(); // candidate: pointer stops, no replay
    }
    const bool quickActive = uncal_ && quickEnabled_ && inputsValid && state_ == SystemState::Active;
    if (quickActive) {
        // Candidate detection freezes the pointer from this very sample until accept or reject; what
        // is frozen is discarded, never replayed. Movement before detection cannot be undone.
        const auto event = quickRec_.update(clickFrame(raw, mapped, configured_), now);
        gestureClick = gestureClick || event.accepted;
        recognizing = recognizing || quickRec_.suppressing();
    }

    // Defensive: control never stays active while the maintained switch inhibits it.
    if (handsFree && !uncal_ && state_ == SystemState::Active && !enable_.permitted()) {
        stop(SystemState::Paused, now);
        diagnostics_.reason = "control switch OFF; explicit resume required";
    }
    if (uncal_ && configured_ && uncalNeedsEnable_ && !uncalGate_.permitted()) {
        stopUncalibratedDemo("control stopped: enable permission lost; explicit restart required");
    }
    const bool movementOnly = demoMovementOnly_ || uncal_;
    const UserProfile& control = uncal_ ? uncalProfile_ : profile_;
    const bool profileOk = uncal_ || (hasProfile_ && profile_.valid());
    const bool active = state_ == SystemState::Active && inputsValid && (hasProfile_ || uncal_);
    Intent intent;
    diagnostics_.motion = {};
    // Invalid input/profile never reaches control math or telemetry numbers.
    if (inputsValid && control.valid()) {
        // Configured control projects the RAW gyro on the learned rows; everything else uses the
        // default axis mapping and the EMA processor.
        diagnostics_.motion = uncal_ && configured_ ? controlProc_.process(raw, learned_)
                                                    : processor_.process(mapped, control, dtSeconds);
        intent = adaptive_.apply(diagnostics_.motion, control, dtSeconds);
    }
    bool scrolling = intent.wheel != 0;
    if (recognizing) {
        // Candidate movement is discarded, never replayed after a rejection.
        intent = Intent{};
        scrolling = false;
    }
    intent = intent_.resolve(intent, active, scrolling);
    if (uncal_ && !configured_) { // learned mapping needs no reversal: relearn instead
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
    const bool actionsActive = uncal_ && actionsEnabled_ && !demoMovementOnly_;
    ActionOutput actionOut;
    if (actionsActive && outputAllowed) {
        // Dwell action palette: the SelectionManager times the one dwell; the palette decides what a
        // completed dwell means. Over the palette it selects a control and is never an OS click.
        const PaletteTarget over = palette_.hover(now);
        if (over != hoverSeen_) {
            selection_.reset(false, now); // a fresh dwell that starts where the pointer now is
            if (over == PaletteTarget::None) {
                selection_.lockAt(x_, y_); // left the palette: cleared, re-armed only by real movement
            }
            hoverSeen_ = over;
        }
        bool completed = false;
        if (palette_.scrollActive()) {
            selection_.interrupt(); // scrolling is not selecting; the exit is timed by the palette
        } else {
            completed = selection_.update(false, x_, y_, true, false, control, now).pulse;
        }
        ActionInput in;
        in.dwellPulse = completed;
        in.verticalRate = (uncal_ && !configured_ && uncalReverseY_) ? -diagnostics_.motion.y
                                                                     : diagnostics_.motion.y;
        in.lateralRate = (uncal_ && !configured_ && uncalReverseX_) ? -diagnostics_.motion.x
                                                                    : diagnostics_.motion.x;
        in.dt = dtSeconds;
        in.dwellMs = control.dwellMs;
        in.x = float(x_);
        in.y = float(y_);
        in.tolerance = control.dwellTolerance;
        actionOut = palette_.update(in, now);
        if (actionOut.resetDwell) {
            selection_.reset(false, now);
        }
        if (actionOut.lockAfter) {
            selection_.lockAt(x_, y_);
        }
        selected = {};
        selected.down = actionOut.down;
        selected.pulse = actionOut.pulse;
        if (actionOut.freezePointer) {
            intent.dx = intent.dy = 0;
        }
    } else if (dwellClicking && outputAllowed) {
        // Explicitly enabled dwell in the uncalibrated demo: the normal selection manager with the
        // demo profile. No raw switch, no scrolling; hold/drag can never come out of it.
        if (recognizing) {
            selection_.interrupt(); // no dwell click while a gesture candidate is open
        } else {
            selected = selection_.update(false, x_, y_, true, false, control, now);
        }
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
    const bool gestureClicking = gestureClick && outputAllowed; // trained or quick recognizer
    if (gestureClicking) {
        selection_.interrupt();
        selected.pulse = true; // exactly one press/release pair through the normal pipeline
        selected.down = false;
    }
    Command command = interaction_.compose(intent, selected);
    if (movementOnly) {
        const float limit = uncal_ ? start::uncalDemoMaxStep : start::demoMaxStep;
        command.down = false;
        command.pulse = command.pulse && (dwellClicking || gestureClicking || actionsActive);
        command.wheel = 0;
        if (actionsActive) {
            command.down = actionOut.down && outputAllowed; // a drag holds the primary button
            command.right = actionOut.right;
            command.twice = actionOut.twice;
            command.wheel = actionOut.wheel; // the gate and the HIDManager bound it
        }
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
    if ((dwellClicking || gestureClicking || actionsActive) && safeCommand.pulse) {
        ++uncalClicks_;
    }
    if (actionsActive) {
        actionsWheel_ += hid_.last.wheel < 0 ? -hid_.last.wheel : hid_.last.wheel; // notches sent, either way
        // A drag released by a palette entry is only "released" once that report was really delivered with
        // the button up; until then no palette selection is possible.
        if (palette_.releaseUnconfirmed() && delivered && !hid_.last.down && !hid_.last.right) {
            palette_.confirmRelease();
        }
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

    if (actionOut.stop && state_ == SystemState::Active) {
        stopUncalibratedDemo("stopped from the action palette; explicit restart required");
    }
    if (state_ == SystemState::SafeState) {
        diagnostics_.cursor = "SAFE_STATE";
    } else if (state_ == SystemState::Paused) {
        diagnostics_.cursor = "PAUSED";
    } else if (!outputAllowed) {
        diagnostics_.cursor = "WARNING";
    } else if (recognizing) {
        diagnostics_.cursor = "GESTURE";
    } else if (actionsActive && palette_.dragging()) {
        diagnostics_.cursor = "DRAGGING";
    } else if (actionsActive && palette_.scrollActive()) {
        diagnostics_.cursor = "SCROLL_ACTIVE";
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
    if (uncal_ && ((configured_ && uncalNeedsEnable_ && !uncalGate_.permitted()) || pressEdge)) {
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

UserProfile System::configuredProfile() const {
    UserProfile configured; // learned gains; thresholds stay at the START values; no scrolling
    configured.gain = learned_.gain;
    configured.deadzone = {learned_.deadzoneEnter[0], learned_.deadzoneEnter[1]};
    return configured;
}
UserProfile System::uncalibratedDemoProfile() const {
    // bias 0: no rest measurement exists in the fallback, so its deadzone must cover idle bias
    UserProfile demo = configured_ ? configuredProfile() : UserProfile{};
    if (!configured_) {
        demo.deadzone = {start::uncalDemoDeadzone, start::uncalDemoDeadzone};
        demo.gain = {start::uncalDemoGain, start::uncalDemoGain, start::uncalDemoGain,
                     start::uncalDemoGain};
        demo.alpha = start::uncalDemoAlpha;
    }
    demo.dwellEnabled = uncalDwell_ || actionsEnabled_; // movement-only unless dwell clicking was explicitly enabled
    demo.dwellMs = uncalDwellMs_;
    demo.dwellTolerance = uncalDwellTolerance_;
    demo.scrollEnabled = false;
    return demo;
}

const char* System::sessionBlocker(bool configured) const {
    if (uncal_ || state_ == SystemState::Active) {
        return "control is already active";
    }
    if (state_ == SystemState::Calibrating) {
        return "calibration in progress";
    }
    if (state_ == SystemState::Training) {
        return "gesture training in progress";
    }
    if (state_ == SystemState::Teaching) {
        return "mapping teaching in progress";
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
    if (configured) {
        if (!learnedValid_) {
            return "no learned mapping: teach the movements first";
        }
        // A gross re-orientation (beyond mapMountingBlockDeg) means the gyro axes no longer mean
        // left/right/up/down. Ordinary head tilt stays well inside that and never blocks.
        if (mountingAngleDeg() > start::mapMountingBlockDeg) {
            return "mounting changed: teach the movements again";
        }
    }
    // Configured control keeps the physical enable permission; the temporary fallback is authorised by
    // the explicit website start instead.
    if (configured && uncalNeedsEnable_ && !uncalGate_.present()) {
        return "enable button not present";
    }
    if (configured && uncalNeedsEnable_ && !uncalGate_.permitted()) {
        return "press the enable button first";
    }
    if (!configured && !uncalibratedDemoProfile().valid()) {
        return "demo configuration invalid";
    }
    return nullptr;
}
const char* System::uncalibratedDemoBlocker() const {
    return sessionBlocker(false);
}
const char* System::configuredBlocker() const {
    return sessionBlocker(true);
}

bool System::startSession(uint32_t now, bool configured) {
    if (const char* blocker = sessionBlocker(configured)) {
        diagnostics_.reason = blocker;
        return false;
    }
    uncalDwell_ = false; // every start is movement-only until dwell is enabled again
    clickEnabled_ = false; // ... and until the gesture click is enabled again
    quickEnabled_ = false; // ... or the quick gesture click
    actionsEnabled_ = false; // ... or the dwell action palette
    palette_.clearCounters();
    actionsWheel_ = 0;
    configured_ = configured;
    uncalProfile_ = uncalibratedDemoProfile();
    if (!emitStationary()) {
        configured_ = false;
        enterSafe(FaultCode::Transport, now);
        return false;
    }
    resetInteraction(now);
    state_ = SystemState::Active;
    uncal_ = true; // after resetInteraction(); never persisted, never reported as calibration
    uncalClicks_ = 0;
    diagnostics_.faultCode = FaultCode::None;
    diagnostics_.reason = configured ? "configured control active; movement only"
                                     : "uncalibrated demo active; movement only";
    return true;
}
bool System::startUncalibratedDemo(uint32_t now) {
    return startSession(now, false);
}
bool System::startConfiguredControl(uint32_t now) {
    return startSession(now, true);
}

Vec3 System::clickFrame(const MotionSample& raw, const MotionSample& mapped,
                        bool configuredFrame) const {
    if (configuredFrame) {
        const Vec3 g{raw.gyro[0], raw.gyro[1], raw.gyro[2]};
        return {dot(learned_.horizontal, g), dot(learned_.vertical, g), dot(learned_.rollRow(), g)};
    }
    return {mapped.gyro[0], mapped.gyro[1], mapped.gyro[2]};
}
const char* System::clickBlocker() const {
    if (!uncal_ || state_ != SystemState::Active) {
        return "start the control session first";
    }
    if (!clickReady_) {
        return "teach the click gesture first";
    }
    if (clickConfiguredFrame_ != configured_) {
        return clickConfiguredFrame_ ? "this gesture was taught for configured control"
                                     : "this gesture was taught for the uncalibrated fallback";
    }
    return nullptr;
}
bool System::clickTrainStart(uint32_t now, bool configuredFrame) {
    if (configuredFrame && !learnedValid_) {
        diagnostics_.reason = "no learned mapping: teach the movements first";
        return false;
    }
    if (const char* blocker = teachBlocker()) {
        diagnostics_.reason = blocker;
        return false;
    }
    stop(SystemState::Teaching, now);
    if (state_ != SystemState::Teaching) {
        return false;
    }
    teachKind_ = TeachKind::Click;
    clickTrainingConfigured_ = configuredFrame;
    clickTrainer_.begin(now);
    diagnostics_.reason = "gesture click training: hold the assembly completely still";
    return true;
}
void System::clickTrainCancel() {
    if (state_ != SystemState::Teaching || teachKind_ != TeachKind::Click) {
        return;
    }
    stop(hasProfile_ ? SystemState::Ready : SystemState::CalibrationRequired, lastTick_);
    if (state_ != SystemState::SafeState) {
        diagnostics_.reason = "gesture training cancelled; the previous gesture is kept";
    }
}
bool System::clickTrainAccept() {
    if (state_ != SystemState::Teaching || teachKind_ != TeachKind::Click ||
        !clickTrainer_.accept()) {
        return false;
    }
    const ClickTemplate result = clickTrainer_.result();
    const bool frame = clickTrainingConfigured_;
    stop(hasProfile_ ? SystemState::Ready : SystemState::CalibrationRequired, lastTick_);
    if (state_ == SystemState::SafeState || !result.valid()) {
        return false;
    }
    clickTemplate_ = result;
    clickReady_ = true;
    clickConfiguredFrame_ = frame;
    diagnostics_.reason = "gesture taught (in memory only); enable it explicitly while control runs";
    return true;
}
void System::clickClear() {
    clickEnabled_ = false;
    clickReady_ = false;
}
bool System::setClickGesture(bool on, uint32_t now) {
    if (!on) {
        clickEnabled_ = false;
        return true;
    }
    if (const char* blocker = clickBlocker()) {
        diagnostics_.reason = blocker;
        return false;
    }
    clickRec_.configure(clickTemplate_); // needs a neutral stretch before it arms
    clickRec_.reset(now);
    clickEnabled_ = true;
    if (actionsEnabled_) {
        actionsEnabled_ = false; // ... and not together with the action palette
        releaseHeldButtons(now);
    }
    quickEnabled_ = false; // one click recognizer at a time
    if (uncalDwell_) {
        uncalDwell_ = false; // ... and not together with dwell
        uncalProfile_ = uncalibratedDemoProfile();
        selection_.reset(false, now);
    }
    diagnostics_.reason = "gesture click enabled";
    return true;
}
ClickStatus System::clickStatus(uint32_t now) const {
    ClickStatus st;
    st.train = clickTrainer_.status(now);
    st.ready = clickReady_;
    st.enabled = uncal_ && clickEnabled_;
    st.configuredFrame = clickReady_ ? clickConfiguredFrame_ : clickTrainingConfigured_;
    st.suppressing = st.enabled && clickRec_.suppressing();
    st.state = !st.enabled ? "OFF"
               : clickRec_.state() == ClickRecognizer::State::Armed       ? "ARMED"
               : clickRec_.state() == ClickRecognizer::State::Candidate    ? "CANDIDATE"
                                                                           : "NEUTRAL_WAIT";
    st.lastReject = clickRec_.lastReject;
    st.accepted = clickRec_.accepted;
    st.rejected = clickRec_.rejected;
    st.candidates = clickRec_.candidates;
    st.clicks = uncalClicks_;
    const char* blocker = clickBlocker();
    st.blocked = st.enabled ? "" : (blocker ? blocker : "");
    return st;
}

const char* System::quickBlocker() const {
    if (!uncal_ || state_ != SystemState::Active) {
        return "start the control session first";
    }
    if (!quickReady_) {
        return "practice the quick gesture first";
    }
    if (quickConfiguredFrame_ != configured_) {
        return quickConfiguredFrame_ ? "this practice was done for configured control"
                                     : "this practice was done for the uncalibrated fallback";
    }
    return nullptr;
}
bool System::quickPracticeStart(uint32_t now, bool configuredFrame) {
    if (configuredFrame && !learnedValid_) {
        diagnostics_.reason = "no learned mapping: teach the movements first";
        return false;
    }
    if (uncal_) {
        // An explicit practice start ends a running session first: pending gestures are cancelled
        // and held output is released through the normal stop path.
        stopUncalibratedDemo("session stopped: starting the quick gesture practice");
    }
    if (const char* blocker = teachBlocker()) {
        diagnostics_.reason = blocker;
        return false;
    }
    stop(SystemState::Teaching, now); // a practice start cancels any pending gesture and output
    if (state_ != SystemState::Teaching) {
        return false;
    }
    teachKind_ = TeachKind::Quick;
    quickTrainingConfigured_ = configuredFrame;
    quickPractice_.begin(now, quickSettings_);
    diagnostics_.reason = "quick gesture practice: hold the assembly completely still";
    return true;
}
void System::quickPracticeRetry(uint32_t now) {
    if (state_ == SystemState::Teaching && teachKind_ == TeachKind::Quick) {
        quickPractice_.retry(now);
    }
}
void System::quickPracticeCancel() {
    if (state_ != SystemState::Teaching || teachKind_ != TeachKind::Quick) {
        return;
    }
    stop(hasProfile_ ? SystemState::Ready : SystemState::CalibrationRequired, lastTick_);
    if (state_ != SystemState::SafeState) {
        diagnostics_.reason = "quick gesture practice cancelled";
    }
}
bool System::quickPracticeAccept() {
    if (state_ != SystemState::Teaching || teachKind_ != TeachKind::Quick ||
        !quickPractice_.accept()) {
        return false;
    }
    const QuickProfile profile = quickPractice_.profile();
    const bool frame = quickTrainingConfigured_;
    stop(hasProfile_ ? SystemState::Ready : SystemState::CalibrationRequired, lastTick_);
    if (state_ == SystemState::SafeState || !profile.valid()) {
        return false;
    }
    quickProfile_ = profile;
    quickReady_ = true;
    quickConfiguredFrame_ = frame;
    diagnostics_.reason = "quick gesture practised (memory only); enable it explicitly while control runs";
    return true;
}
void System::quickClear() {
    quickEnabled_ = false;
    quickReady_ = false;
    if (!quickPractice_.active()) {
        quickPractice_ = QuickPractice{}; // forget the designated direction and the old preview
    }
}
bool System::setQuickSettings(float sensitivity, float returnTolerance) {
    return setQuickSettings(sensitivity, returnTolerance, quickSettings_.directionToleranceDeg);
}
bool System::setQuickSettings(float sensitivity, float returnTolerance, float directionToleranceDeg) {
    const QuickSettings candidate{sensitivity, returnTolerance, directionToleranceDeg};
    if (!candidate.valid()) {
        return false;
    }
    quickSettings_ = candidate;
    if (uncal_ && quickEnabled_) {
        quickRec_.configure(quickProfile_, quickSettings_); // restarts: a fresh neutral period
        quickRec_.reset(lastTick_);
    }
    return true;
}
bool System::setQuickGesture(bool on, uint32_t now) {
    if (!on) {
        quickEnabled_ = false;
        quickRec_.reset(now);
        return true;
    }
    if (const char* blocker = quickBlocker()) {
        diagnostics_.reason = blocker;
        return false;
    }
    uncalDwell_ = false; // mutually exclusive with dwell and with the trained gesture
    clickEnabled_ = false;
    if (actionsEnabled_) {
        actionsEnabled_ = false; // ... and with the action palette
        releaseHeldButtons(now);
    }
    uncalProfile_ = uncalibratedDemoProfile();
    selection_.reset(false, now);
    quickRec_.configure(quickProfile_, quickSettings_);
    quickRec_.reset(now); // needs a neutral period before it arms
    quickEnabled_ = true;
    diagnostics_.reason = "EXPERIMENTAL quick gesture click enabled";
    return true;
}
QuickStatus System::quickStatus(uint32_t now) const {
    QuickStatus st = quickPractice_.status(now);
    if (st.phase != QuickPhase::Preview) {
        st.designated = false; // only a live preview or an accepted practice designates a direction
        st.direction = {};
    }
    st.ready = quickReady_;
    st.enabled = uncal_ && quickEnabled_;
    st.configuredFrame = quickReady_ ? quickConfiguredFrame_ : quickTrainingConfigured_;
    st.sensitivity = quickSettings_.sensitivity;
    st.returnTolerance = quickSettings_.returnTolerance;
    st.directionToleranceDeg = quickSettings_.directionToleranceDeg;
    if (quickReady_) {
        st.direction = quickProfile_.direction; // the designated direction, signed, 3-D
        st.designated = true;
    }
    if (st.enabled) {
        st.suppressing = quickRec_.suppressing();
        st.accepted = quickRec_.accepted;
        st.rejected = quickRec_.rejected;
        st.candidates = quickRec_.candidates;
        st.suppressedMs = quickRec_.suppressedMs;
        st.lastReject = name(quickRec_.lastReject);
        st.lastExcursionDeg = quickRec_.lastExcursionDeg;
        st.lastResidualDeg = quickRec_.lastResidualDeg;
        st.lastDurationMs = quickRec_.lastDurationMs;
        st.practiceDeg = quickProfile_.practiceDeg;
        using S = QuickRecognizer::State;
        const S state = quickRec_.state();
        st.state = state == S::Armed      ? "READY"
                   : state == S::Outward  ? "OUTWARD"
                   : state == S::Return   ? "RETURN"
                   : state == S::Settling ? "SETTLING"
                                          : "NEUTRAL";
    } else if (st.phase == QuickPhase::Idle || st.phase == QuickPhase::Done ||
               st.phase == QuickPhase::Failed) {
        st.state = "OFF";
    }
    st.clicks = uncalClicks_;
    const char* blocker = quickBlocker();
    st.blocked = st.enabled ? "" : (blocker ? blocker : "");
    return st;
}

void System::setControlRepository(ControlRepository& repository) {
    controlRepo_ = &repository;
    LearnedControl stored;
    if (repository.load(stored)) {
        learned_ = stored; // available in RAM, never activated by itself
        learnedValid_ = true;
        learnedSaved_ = true;
    }
    controlRecord_ = repository.state();
}
float System::mountingAngleDeg() const {
    const float n = norm(lastAccel_);
    if (!learnedValid_ || !(n > 1e-3f)) {
        return 0.f;
    }
    const float c = std::clamp(dot(lastAccel_, learned_.gravity) / n, -1.f, 1.f);
    return std::acos(c) * 57.2957795f;
}
const char* System::teachBlocker() const {
    if (uncal_ || state_ == SystemState::Active) {
        return "control is active: stop it first";
    }
    if (state_ == SystemState::Calibrating || state_ == SystemState::Training ||
        state_ == SystemState::Teaching) {
        return "another setup is in progress";
    }
    if (state_ == SystemState::SafeState) {
        return "safe state: wait for the fault to clear";
    }
    if (!transport_.connected()) {
        return "BLE link unavailable";
    }
    if (healthyChecks_ < start::recoverySamples) {
        return "waiting for healthy sensor samples";
    }
    return nullptr;
}
bool System::teachStart(uint32_t now) {
    if (const char* blocker = teachBlocker()) {
        diagnostics_.reason = blocker;
        return false;
    }
    stop(SystemState::Teaching, now); // releases output first
    if (state_ != SystemState::Teaching) {
        return false;
    }
    teachKind_ = TeachKind::Mapping;
    teacher_.begin(now);
    diagnostics_.reason = "mapping teaching: hold the assembly completely still";
    return true;
}
void System::teachCancel() {
    if (state_ != SystemState::Teaching || teachKind_ != TeachKind::Mapping) {
        return;
    }
    stop(hasProfile_ ? SystemState::Ready : SystemState::CalibrationRequired, lastTick_);
    if (state_ != SystemState::SafeState) {
        diagnostics_.reason = "teaching cancelled; previous settings kept";
    }
}
bool System::teachAccept() {
    if (state_ != SystemState::Teaching || teachKind_ != TeachKind::Mapping || !teacher_.accept()) {
        return false;
    }
    const LearnedControl candidate = teacher_.candidate();
    stop(hasProfile_ ? SystemState::Ready : SystemState::CalibrationRequired, lastTick_);
    if (state_ == SystemState::SafeState || !candidate.valid()) {
        return false;
    }
    learned_ = candidate;
    learnedValid_ = true;
    learnedSaved_ = false;
    if (clickConfiguredFrame_) {
        clickReady_ = false; // a gesture taught in the old learned frame no longer applies
    }
    if (quickConfiguredFrame_) {
        quickReady_ = false;
    }
    saveResult_ = "";
    diagnostics_.reason = "mapping accepted and active in memory; not saved yet";
    return true;
}
bool System::teachSave() {
    if (!learnedValid_) {
        saveResult_ = "NOTHING_TO_SAVE";
        return false;
    }
    if (!controlRepo_) {
        saveResult_ = "NO_STORAGE";
        return false;
    }
    if (uncal_ || state_ == SystemState::Active || state_ == SystemState::Teaching) {
        saveResult_ = "STOP_CONTROL_FIRST";
        return false;
    }
    if (controlRepo_->save(learned_)) {
        learnedSaved_ = true;
        controlRecord_ = controlRepo_->state();
        saveResult_ = "SAVED";
        diagnostics_.reason = "mapping saved";
        return true;
    }
    // Previous stored settings are untouched and the RAM settings keep working.
    saveResult_ = "SAVE_FAILED_RAM_ONLY";
    diagnostics_.reason = "saving failed; the mapping works in memory only";
    return false;
}
void System::clearLearned() {
    if (uncal_ && configured_) {
        stopUncalibratedDemo("learned mapping cleared; explicit restart required");
    }
    learnedValid_ = false;
    learnedSaved_ = false;
    saveResult_ = "";
    if (clickConfiguredFrame_) {
        clickReady_ = false;
    }
    if (quickConfiguredFrame_) {
        quickReady_ = false;
    }
}
MappingStatus System::mappingStatus(uint32_t now) const {
    MappingStatus st = teacher_.status(now);
    st.learnedValid = learnedValid_;
    st.unsaved = learnedValid_ && !learnedSaved_;
    st.stored = name(controlRecord_);
    st.mode = !uncal_ ? "OFF" : configured_ ? "CONFIGURED" : "UNCALIBRATED_DEMO";
    const char* blocker = configuredBlocker();
    st.blocked = uncal_ ? "" : (blocker ? blocker : "");
    st.saveResult = saveResult_;
    st.mountingDeg = mountingAngleDeg();
    st.mountingWarning = learnedValid_ && st.mountingDeg > start::mapMountingWarnDeg;
    if (learnedValid_ && teacher_.phase() == MapPhase::Idle) {
        st.bias = learned_.bias;
        st.noise = learned_.noise;
    }
    return st;
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
    if (on) {
        quickEnabled_ = false; // dwell and the recognizers are mutually exclusive
        clickEnabled_ = false;
        if (actionsEnabled_) {
            actionsEnabled_ = false; // ... and the action palette has its own dwell
            releaseHeldButtons(now);
        }
    }
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

void System::releaseHeldButtons(uint32_t now) {
    const bool held = palette_.dragging();
    palette_.reset();
    hoverSeen_ = PaletteTarget::None;
    if (held && state_ != SystemState::SafeState) {
        // Switching modes while a drag holds the button: release it now, through the same final
        // boundary. A failed release inhibits further output (transport fault, explicit restart).
        if (!emitStationary()) {
            enterSafe(FaultCode::Transport, now);
        }
    }
}
const char* System::actionBlocker() const {
    if (!uncal_ || state_ != SystemState::Active) {
        return "start the control session first";
    }
    return nullptr;
}
bool System::setActionPalette(bool on, uint32_t now) {
    if (!on) {
        if (actionsEnabled_) {
            actionsEnabled_ = false;
            uncalProfile_ = uncalibratedDemoProfile();
            selection_.reset(false, now);
            releaseHeldButtons(now);
            diagnostics_.reason = "dwell action palette off";
        }
        return true;
    }
    if (const char* blocker = actionBlocker()) {
        diagnostics_.reason = blocker;
        return false;
    }
    uncalDwell_ = false; // competing recognizers and dwell clicking are off while it is on
    clickEnabled_ = false;
    quickEnabled_ = false;
    actionsEnabled_ = true;
    uncalProfile_ = uncalibratedDemoProfile();
    palette_.reset();
    hoverSeen_ = PaletteTarget::None;
    selection_.reset(false, now);
    diagnostics_.reason = "EXPERIMENTAL dwell action palette on; Left-click is selected";
    return true;
}
bool System::setActionHover(PaletteTarget target, uint32_t now) {
    if (!uncal_ || !actionsEnabled_) {
        return false;
    }
    palette_.setHover(target, now);
    return true;
}
ActionsStatus System::actionsStatus(uint32_t now) const {
    ActionsStatus st;
    st.enabled = uncal_ && actionsEnabled_;
    const char* blocker = actionBlocker();
    st.blocked = blocker ? blocker : "";
    st.dwellMs = uncalDwellMs_;
    st.tolerance = uncalDwellTolerance_;
    st.neutral = start::actionScrollNeutral;
    st.left = palette_.left;
    st.right = palette_.right;
    st.doubles = palette_.doubles;
    st.dragStarts = palette_.dragStarts;
    st.dragReleases = palette_.dragReleases;
    st.scrollStarts = palette_.scrollStarts;
    st.scrollExits = palette_.scrollExits;
    st.selections = palette_.selections;
    st.inhibited = palette_.inhibited;
    st.cancelled = palette_.cancelled;
    st.paletteReleases = palette_.paletteReleases;
    st.commitMs = start::actionCommitMs;
    st.drops = palette_.drops;
    st.cancels = palette_.cancels;
    st.confirmedReleases = palette_.confirmedReleases;
    st.last = name(palette_.last);
    st.wheelUnits = uint32_t(actionsWheel_);
    if (st.enabled) {
        const PaletteTarget over = palette_.hover(now);
        st.mode = name(palette_.mode());
        st.dragging = palette_.dragging();
        st.scroll = name(palette_.scroll());
        st.frozen = palette_.scrollActive();
        st.hover = name(over);
        st.inPalette = over != PaletteTarget::None;
        st.dwellState = name(selection_.dwell);
        st.dwellProgress = selection_.progress(lastTick_, uncalProfile_);
        st.exitProgress = palette_.exitProgress(lastTick_, uncalDwellMs_);
        st.locked = name(palette_.lockedTarget());
        st.reporting = palette_.reporting(lastTick_);
        st.reportAgeMs = palette_.reportAge(lastTick_);
        st.pending = palette_.pending();
    }
    return st;
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
    s.uncalNeedsEnable = uncalNeedsEnable_; // applies to configured control only
    s.uncalPermission = !uncal_ ? "NONE" : configured_ ? "ENABLE_BUTTON" : "WEBSITE_START";
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
