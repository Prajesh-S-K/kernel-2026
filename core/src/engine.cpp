#include "nodx/engine.hpp"
#include <algorithm>
#include <cmath>

namespace nodx {
const char* name(SystemState s) {
    switch (s) {
    case SystemState::Boot: return "BOOT";
    case SystemState::CalibrationRequired: return "CALIBRATION_REQUIRED";
    case SystemState::Calibrating: return "CALIBRATING";
    case SystemState::Ready: return "READY";
    case SystemState::Active: return "ACTIVE";
    case SystemState::Paused: return "PAUSED";
    case SystemState::SafeState: return "SAFE_STATE";
    }
    return "SAFE_STATE";
}
const char* name(CalPhase p) {
    switch (p) {
    case CalPhase::Idle: return "IDLE";
    case CalPhase::Rest: return "REST";
    case CalPhase::Left: return "LEFT";
    case CalPhase::Right: return "RIGHT";
    case CalPhase::Up: return "UP";
    case CalPhase::Down: return "DOWN";
    case CalPhase::Natural: return "NATURAL";
    case CalPhase::Analyze: return "ANALYZE";
    case CalPhase::Validate: return "VALIDATE";
    case CalPhase::Save: return "PROFILE_SAVE";
    case CalPhase::Complete: return "COMPLETE";
    case CalPhase::Failed: return "FAILED";
    }
    return "FAILED";
}
const char* name(DwellState s) {
    switch (s) {
    case DwellState::Idle: return "IDLE";
    case DwellState::Arming: return "ARMING";
    case DwellState::Progress: return "PROGRESS";
    case DwellState::Click: return "CLICK";
    case DwellState::Lockout: return "LOCKOUT";
    }
    return "IDLE";
}
void Statistics::add(float x) {
    ++count;
    double delta = x - mean;
    mean += delta / count;
    m2 += delta * (x - mean);
}
float Statistics::sigma() const { return count > 1 ? std::sqrt(m2 / (count - 1)) : 0.f; }
void CalibrationEngine::start(uint32_t now) {
    *this = CalibrationEngine{};
    phase = CalPhase::Rest; phaseStart_ = now; reason = "collecting";
}
void CalibrationEngine::cancel() { phase = CalPhase::Failed; reason = "cancelled"; }
float CalibrationEngine::progress(uint32_t now) const {
    if (phase == CalPhase::Complete) return 1.f;
    if (phase < CalPhase::Rest || phase > CalPhase::Save) return 0.f;
    if (phase >= CalPhase::Analyze) return .95f;
    return (int(phase) - 1 + std::min(1.f, float(uint32_t(now - phaseStart_)) / start::phaseMs)) / 6.f;
}
void CalibrationEngine::tick(const MotionSample& s, uint32_t now) {
    if (phase < CalPhase::Rest || phase > CalPhase::Save) return;
    if (!s.valid) { phase = CalPhase::Failed; reason = "invalid sample"; return; }
    for (float v : s.gyro) {
        if (!std::isfinite(v) || std::abs(v) > start::maxGyro) {
            phase = CalPhase::Failed; reason = "invalid sample"; return;
        }
    }
    if (phase == CalPhase::Analyze) { analyze(); return; }
    if (phase == CalPhase::Validate) {
        phase = candidate.valid() ? CalPhase::Save : CalPhase::Failed;
        reason = phase == CalPhase::Save ? "validated" : "profile invalid";
        return;
    }
    if (phase == CalPhase::Save) return; // System owns transactional persistence
    if (phase == CalPhase::Rest) {
        for (unsigned i = 0; i < 3; ++i) rest_[i].add(s.gyro[i]);
    } else if (phase == CalPhase::Natural) {
        ++natural_;
    } else {
        unsigned i = unsigned(phase) - unsigned(CalPhase::Left);
        unsigned axis = i < 2 ? 0 : 1;
        float sign = (i == 0 || i == 2) ? -1.f : 1.f;
        directions_[i].add(sign * (s.gyro[axis] - float(rest_[axis].mean)));
    }
    if (uint32_t(now - phaseStart_) < start::phaseMs) return;
    unsigned count = phase == CalPhase::Rest ? rest_[0].count
        : phase == CalPhase::Natural ? natural_ : directions_[unsigned(phase) - 2].count;
    if (count < start::minPhaseSamples) { phase = CalPhase::Failed; reason = "insufficient samples"; return; }
    phase = static_cast<CalPhase>(int(phase) + 1);
    phaseStart_ = now;
}
void CalibrationEngine::analyze() {
    for (unsigned i = 0; i < 3; ++i) {
        if (rest_[i].sigma() > start::maxRestSigma || std::abs(rest_[i].mean) > start::maxRestBias) {
            phase = CalPhase::Failed; reason = "rest too unstable"; return;
        }
        candidate.bias[i] = float(rest_[i].mean);
        if (i < 2) candidate.deadzone[i] = start::deadzoneBase + start::noiseMultiplier * rest_[i].sigma();
    }
    for (unsigned i = 0; i < 4; ++i) {
        float range = float(directions_[i].mean);
        if (range < start::minimumRange || range < 2 * candidate.deadzone[i / 2]
            || directions_[i].sigma() > range * .8f) {
            phase = CalPhase::Failed; reason = "direction not controllable"; return;
        }
        candidate.gain[i] = std::clamp(600.f / range, 1.f, 120.f);
    }
    // Filtering responds to measured rest noise; bounded candidate, still requires user testing.
    candidate.alpha = std::clamp(.5f / (1 + rest_[0].sigma() + rest_[1].sigma()), .15f, .5f);
    phase = CalPhase::Validate; reason = "analyzed";
}
void MotionProcessor::reset() { filtered_ = {}; roll_ = 0; }
Motion MotionProcessor::process(const MotionSample& s, const UserProfile& p, float dt) {
    for (unsigned i = 0; i < 3; ++i) filtered_[i] += p.alpha * (s.gyro[i] - p.bias[i] - filtered_[i]);
    // Complementary roll: short-term gyro + long-term gravity. Mount convention is START.
    float gravityRoll = std::atan2(s.accel[1], s.accel[2]) * 57.2957795f;
    roll_ = .98f * (roll_ + filtered_[2] * dt) + .02f * gravityRoll;
    auto zone = [](float v, float d) { return std::copysign(std::max(0.f, std::abs(v) - d), v); };
    float speed = std::hypot(filtered_[0], filtered_[1]);
    return {zone(filtered_[0], p.deadzone[0]), zone(filtered_[1], p.deadzone[1]), roll_, 1.f / (1.f + speed)};
}
Intent AdaptiveEngine::apply(const Motion& m, const UserProfile& p, float dt) const {
    float speed = std::hypot(m.x, m.y);
    // Continuous response through precision/normal/travel regions.
    float factor = .35f + .65f * std::min(1.f, speed / p.precisionThreshold)
        + std::clamp((speed - p.precisionThreshold) / (p.fastThreshold - p.precisionThreshold), 0.f, 1.f);
    Intent out;
    out.dx = m.x * p.gain[m.x < 0 ? 0 : 1] * factor * dt;
    out.dy = m.y * p.gain[m.y < 0 ? 2 : 3] * factor * dt;
    out.mode = speed < p.precisionThreshold ? "PRECISION" : speed > p.fastThreshold ? "TRAVEL" : "NORMAL";
    if (p.scrollEnabled && std::abs(m.roll) > p.scrollThreshold) {
        out.wheel = std::copysign((std::abs(m.roll) - p.scrollThreshold) * p.scrollGain * dt, m.roll);
        out.mode = "SCROLL";
    }
    return out;
}
Intent IntentEngine::resolve(Intent i, bool allowed, bool scrolling) const {
    if (!allowed) return {};
    if (scrolling) { i.dx = 0; i.dy = 0; }
    return i;
}
bool DebouncedSwitch::update(bool raw, uint32_t now) {
    if (raw != raw_) { raw_ = raw; changed_ = now; }
    if (uint32_t(now - changed_) >= start::debounceMs) stable_ = raw_;
    return stable_;
}
void DebouncedSwitch::reset(bool raw, uint32_t now) { raw_ = raw; stable_ = false; changed_ = now; }
void SelectionManager::reset(bool raw, uint32_t now) {
    if (dwell == DwellState::Arming || dwell == DwellState::Progress) ++cancellations;
    dwell = DwellState::Idle;
    switch_.reset(raw, now); requireRelease_ = raw;
}
float SelectionManager::progress(uint32_t now, const UserProfile& p) const {
    if (dwell != DwellState::Progress) return 0;
    return std::min(1.f, float(uint32_t(now - since_)) / p.dwellMs);
}
Selection SelectionManager::update(bool raw, double x, double y, bool allowed, bool scrolling,
                                   const UserProfile& p, uint32_t now) {
    if (!allowed) { reset(raw, now); return {}; }
    bool pressed = switch_.update(raw, now);
    if (requireRelease_) {
        if (!raw && !pressed) requireRelease_ = false;
        return {};
    }
    if (raw || pressed || scrolling || !p.dwellEnabled) {
        if (dwell == DwellState::Arming || dwell == DwellState::Progress) ++cancellations;
        dwell = DwellState::Idle;
        return {pressed, false};
    }
    double distance = std::hypot(x - anchorX_, y - anchorY_);
    if (dwell == DwellState::Click) dwell = DwellState::Lockout;
    if (dwell == DwellState::Lockout) {
        if (distance > p.dwellTolerance * 1.5f) dwell = DwellState::Idle;
        return {};
    }
    if (dwell != DwellState::Idle && distance > p.dwellTolerance) {
        ++cancellations; dwell = DwellState::Idle; return {};
    }
    if (dwell == DwellState::Idle) {
        anchorX_ = x; anchorY_ = y; since_ = now; dwell = DwellState::Arming;
    } else if (dwell == DwellState::Arming && uint32_t(now - since_) >= start::armMs) {
        dwell = DwellState::Progress; since_ = now;
    } else if (dwell == DwellState::Progress && uint32_t(now - since_) >= p.dwellMs) {
        dwell = DwellState::Click; return {false, true};
    }
    return {};
}
Command SafetyManager::gate(Command c, bool active, bool healthy, bool valid, bool connected) {
    calculationFault = !std::isfinite(c.dx) || !std::isfinite(c.dy) || !std::isfinite(c.wheel);
    if (calculationFault || !active || !healthy || !valid || !connected) return {};
    c.dx = std::clamp(c.dx, -float(start::maxPointer), float(start::maxPointer));
    c.dy = std::clamp(c.dy, -float(start::maxPointer), float(start::maxPointer));
    c.wheel = std::clamp(c.wheel, -float(start::maxWheel), float(start::maxWheel));
    return c;
}
void HIDManager::reset() { remainderX_ = remainderY_ = remainderWheel_ = 0; }
bool HIDManager::emit(Command c) {
    auto quantize = [](float v, float& remainder, int bound) {
        float total = std::clamp(v + remainder, -float(bound), float(bound));
        int whole = int(total);
        remainder = total - whole;
        return static_cast<int8_t>(whole);
    };
    last = {quantize(c.dx, remainderX_, start::maxPointer), quantize(c.dy, remainderY_, start::maxPointer),
        quantize(c.wheel, remainderWheel_, start::maxWheel), c.down || c.pulse};
    if (!transport_.connected()) { reset(); last = {}; return false; }
    bool sent = transport_.send(last);
    if (c.pulse) {
        Report release{};
        bool released = transport_.send(release);
        sent = sent && released;
    }
    return sent;
}
void Feedback::update(SystemState state, uint32_t now) {
    if (state != previous_) {
        until_ = now + (state == SystemState::SafeState ? 300 : 80);
        previous_ = state;
    }
    buzzer = int32_t(until_ - now) > 0;
}
System::System(HIDTransport& t, ProfileRepository& r) : hid(t), transport_(t), repository_(r) {
    hasProfile = repository_.load(profile);
    state = hasProfile ? SystemState::Ready : SystemState::CalibrationRequired;
    diagnostics.reason = hasProfile ? "profile loaded; explicit resume required" : "calibration required";
}
void System::safe(const char* reason, uint32_t now) {
    if (state == SystemState::Calibrating) { calibration.cancel(); calibration.reason = reason; }
    if (state != SystemState::SafeState) ++diagnostics.faults;
    state = SystemState::SafeState; diagnostics.reason = reason;
    selection.reset(lastRaw_, now); processor_.reset(); hid.reset();
}
void System::calibrate(uint32_t now) {
    calibration.start(now); state = SystemState::Calibrating;
    diagnostics.reason = "calibration collecting";
    selection.reset(lastRaw_, now); processor_.reset(); hid.reset();
}
void System::cancelCalibration() {
    calibration.cancel(); state = hasProfile ? SystemState::Ready : SystemState::CalibrationRequired;
    diagnostics.reason = "calibration cancelled; previous profile preserved";
}
bool System::resume() {
    if (state != SystemState::Ready && state != SystemState::Paused) return false;
    if (!hasProfile || !profile.valid() || sensors_.healthy < start::recoverySamples || !transport_.connected()) return false;
    state = SystemState::Active; diagnostics.reason = "active";
    processor_.reset(); selection.reset(lastRaw_, lastTick_); hid.reset();
    return true;
}
void System::pause() {
    if (state == SystemState::Active) state = SystemState::Paused;
    diagnostics.reason = "paused; explicit resume required";
    selection.reset(lastRaw_, lastTick_); processor_.reset(); hid.reset();
}
bool System::setProfile(const UserProfile& p, bool persist) {
    if (!p.valid() || (persist && !repository_.save(p))) return false;
    profile = p; hasProfile = true;
    state = SystemState::Ready; processor_.reset(); selection.reset(lastRaw_, lastTick_); hid.reset();
    diagnostics.reason = "valid profile; explicit resume required";
    return true;
}
void System::invalidateProfile() { hasProfile = false; safe("profile invalid", lastTick_); }
void System::tick(MotionSample raw, uint32_t now, bool pressed) {
    ++diagnostics.ticks; lastRaw_ = pressed;
    bool timing = !hadTick_ || (uint32_t(now - lastTick_) > 0 && uint32_t(now - lastTick_) <= start::timeoutMs);
    float dt = hadTick_ ? float(uint32_t(now - lastTick_)) / 1000.f : .01f;
    hadTick_ = true; lastTick_ = now;
    bool healthy = sensors_.check(raw, now) && axes.valid() && timing;
    if (!healthy) safe(!timing ? "loop timing fault" : !axes.valid() ? "axis mapping invalid" : sensors_.reason, now);
    if (!transport_.connected() || transportFailed_) safe(transportFailed_ ? "HID send failed" : "BLE disconnected", now);
    if (hasProfile && !profile.valid()) safe("profile invalid", now);
    if (healthy && state == SystemState::SafeState && sensors_.healthy >= start::recoverySamples && transport_.connected()
        && (!hasProfile || profile.valid())) {
        transportFailed_ = false;
        state = hasProfile ? SystemState::Ready : SystemState::CalibrationRequired;
        diagnostics.reason = "recovered; explicit resume required";
    }
    MotionSample mapped = axes.apply(raw);
    if (state == SystemState::Calibrating && healthy) {
        calibration.tick(mapped, now);
        if (calibration.phase == CalPhase::Save) {
            if (setProfile(calibration.candidate)) calibration.phase = CalPhase::Complete;
            else { calibration.phase = CalPhase::Failed; calibration.reason = "save failed"; state = hasProfile ? SystemState::Ready : SystemState::CalibrationRequired; }
        } else if (calibration.phase == CalPhase::Failed) {
            state = hasProfile ? SystemState::Ready : SystemState::CalibrationRequired;
        }
    }
    bool active = state == SystemState::Active;
    Motion motion = healthy ? processor_.process(mapped, profile, dt) : Motion{};
    diagnostics.motion = motion;
    Intent i = adaptive_.apply(motion, profile, dt);
    bool scrolling = i.wheel != 0;
    i = intent_.resolve(i, active, scrolling);
    // Dwell observes bounded, quantized relative output from the preceding tick.
    // Host cursor acceleration is unknown; this is explicitly an estimate.
    x_ += hid.last.dx; y_ += hid.last.dy;
    if (!std::isfinite(x_) || !std::isfinite(y_)) { safe("pointer accumulator invalid", now); x_ = y_ = 0; active = false; }
    Selection selected = selection.update(pressed, x_, y_, active, scrolling, profile, now);
    Command command=interaction_.compose(i,selected);
    // REQUIRED FINAL ORDER: safety gate -> HID manager -> transport. No bypass.
    Command safeCommand = safety_.gate(command, active, healthy, hasProfile && profile.valid(), transport_.connected());
    if (safety_.calculationFault) safe("calculation invalid", now);
    if (!active || !healthy || safety_.calculationFault) hid.reset();
    bool sent = hid.emit(safeCommand);
    if (!sent && transport_.connected()) { transportFailed_ = true; safe("HID send failed", now); }
    diagnostics.cursor = state == SystemState::SafeState ? "SAFE_STATE" : state == SystemState::Paused ? "PAUSED"
        : !active ? "WARNING" : selected.pulse ? "CLICK" : selected.down ? "DRAG" : scrolling ? "SCROLL"
        : selection.dwell == DwellState::Progress ? "DWELL_PROGRESS" : selection.dwell == DwellState::Arming ? "DWELL_ARMING" : i.mode;
    feedback.update(state, now);
}
}
