#include "nodx/system.hpp"
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

void System::resetInteraction(uint32_t now) {
    selection_.reset(lastRaw_, now);
    processor_.reset();
    hid_.reset();
    diagnostics_.motion = {};
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
    state_ = SystemState::SafeState;
    healthyChecks_ = 0;
    diagnostics_.faultCode = fault;
    diagnostics_.reason = description(fault);
    diagnostics_.cursor = "SAFE_STATE";
    resetInteraction(now);
    feedback_.update(state_, now);
}

void System::stop(SystemState next, uint32_t now) {
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
    if (!hasProfile_ || !profile_.valid() || healthyChecks_ < start::recoverySamples) {
        return false;
    }
    if (!emitStationary()) {
        enterSafe(FaultCode::Transport, lastTick_);
        return false;
    }
    resetInteraction(lastTick_);
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
        if (setProfile(calibration_.candidate)) {
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
    ++diagnostics_.ticks;
    lastRaw_ = pressed;
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
    if (state_ == SystemState::Calibrating && inputsValid) {
        advanceCalibration(axes.apply(raw), now);
    }

    const bool active = state_ == SystemState::Active && inputsValid && hasProfile_;
    Intent intent;
    diagnostics_.motion = {};
    // Invalid input/profile never reaches control math or telemetry numbers.
    if (inputsValid && profile_.valid()) {
        diagnostics_.motion = processor_.process(axes.apply(raw), profile_, dtSeconds);
        intent = adaptive_.apply(diagnostics_.motion, profile_, dtSeconds);
    }
    const bool scrolling = intent.wheel != 0;
    intent = intent_.resolve(intent, active, scrolling);
    x_ += hid_.last.dx;
    y_ += hid_.last.dy;
    if (!std::isfinite(x_) || !std::isfinite(y_)) {
        x_ = y_ = 0;
        enterSafe(FaultCode::Calculation, now);
    }
    const bool outputAllowed = active && state_ == SystemState::Active;
    const Selection selected =
        selection_.update(pressed, x_, y_, outputAllowed, scrolling, profile_, now);
    const Command command = interaction_.compose(intent, selected);
    // REQUIRED FINAL ORDER: safety gate -> HID manager -> transport.
    const Command safeCommand =
        safety_.gate(command, outputAllowed, inputsValid, hasProfile_ && profile_.valid(),
                     transport_.connected());
    if (!outputAllowed) {
        hid_.reset();
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
} // namespace nodx
