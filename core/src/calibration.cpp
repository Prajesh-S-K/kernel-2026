#include "nodx/calibration.hpp"
#include <algorithm>
#include <cmath>

namespace nodx {
const char* name(CalPhase phase) {
    switch (phase) {
    case CalPhase::Idle:
        return "IDLE";
    case CalPhase::Rest:
        return "REST";
    case CalPhase::Left:
        return "LEFT";
    case CalPhase::Right:
        return "RIGHT";
    case CalPhase::Up:
        return "UP";
    case CalPhase::Down:
        return "DOWN";
    case CalPhase::Natural:
        return "NATURAL";
    case CalPhase::Analyze:
        return "ANALYZE";
    case CalPhase::Validate:
        return "VALIDATE";
    case CalPhase::Save:
        return "PROFILE_SAVE";
    case CalPhase::Complete:
        return "COMPLETE";
    case CalPhase::Failed:
        return "FAILED";
    }
    return "FAILED";
}
void Statistics::add(float value) {
    ++count;
    double delta = value - mean;
    mean += delta / count;
    m2 += delta * (value - mean);
}
float Statistics::sigma() const {
    return count > 1 ? std::sqrt(m2 / (count - 1)) : 0.f;
}
void CalibrationEngine::start(uint32_t now, uint32_t leadMs) {
    *this = CalibrationEngine{};
    phase = CalPhase::Rest;
    leadMs_ = leadMs;
    phaseStart_ = now + leadMs;
    reason = "collecting";
}
void CalibrationEngine::cancel() {
    phase = CalPhase::Failed;
    reason = "cancelled";
}
float CalibrationEngine::progress(uint32_t now) const {
    if (phase == CalPhase::Complete) {
        return 1.f;
    }
    if (phase < CalPhase::Rest || phase > CalPhase::Save) {
        return 0.f;
    }
    if (phase >= CalPhase::Analyze) {
        return .95f;
    }
    float elapsed = std::max(0.f, float(int32_t(now - phaseStart_)));
    return (int(phase) - 1 + std::min(1.f, elapsed / start::phaseMs)) / 6.f;
}
uint32_t CalibrationEngine::cueRemainingMs(uint32_t now) const {
    if (phase < CalPhase::Rest || phase > CalPhase::Natural) {
        return 0;
    }
    int32_t left = int32_t(phaseStart_ - now);
    return left > 0 ? uint32_t(left) : 0;
}
void CalibrationEngine::tick(const MotionSample& sample, uint32_t now) {
    if (phase < CalPhase::Rest || phase > CalPhase::Save) {
        return;
    }
    if (!sample.valid) {
        phase = CalPhase::Failed;
        reason = "invalid sample";
        return;
    }
    for (float value : sample.gyro) {
        if (!std::isfinite(value) || std::abs(value) > start::maxGyro) {
            phase = CalPhase::Failed;
            reason = "invalid sample";
            return;
        }
    }
    if (phase == CalPhase::Analyze) {
        analyze();
        return;
    }
    if (phase == CalPhase::Validate) {
        phase = candidate.valid() ? CalPhase::Save : CalPhase::Failed;
        reason = phase == CalPhase::Save ? "validated" : "profile invalid";
        return;
    }
    if (phase == CalPhase::Save) {
        return; // System owns transactional persistence
    }
    if (phase <= CalPhase::Natural && int32_t(now - phaseStart_) < 0) {
        return; // countdown cue: nothing is collected yet
    }
    if (phase == CalPhase::Rest) {
        for (unsigned i = 0; i < 3; ++i) {
            rest_[i].add(sample.gyro[i]);
        }
    } else if (phase == CalPhase::Natural) {
        ++natural_;
    } else {
        unsigned i = unsigned(phase) - unsigned(CalPhase::Left);
        unsigned axis = i < 2 ? 0 : 1;
        float sign = (i == 0 || i == 2) ? -1.f : 1.f;
        directions_[i].add(sign * (sample.gyro[axis] - float(rest_[axis].mean)));
    }
    if (uint32_t(now - phaseStart_) < start::phaseMs) {
        return;
    }
    unsigned count = phase == CalPhase::Rest      ? rest_[0].count
                     : phase == CalPhase::Natural ? natural_
                                                  : directions_[unsigned(phase) - 2].count;
    if (count < start::minPhaseSamples) {
        phase = CalPhase::Failed;
        reason = "insufficient samples";
        return;
    }
    phase = static_cast<CalPhase>(int(phase) + 1);
    phaseStart_ = phase <= CalPhase::Natural ? now + leadMs_ : now;
}
void CalibrationEngine::analyze() {
    for (unsigned i = 0; i < 3; ++i) {
        if (rest_[i].sigma() > start::maxRestSigma ||
            std::abs(rest_[i].mean) > start::maxRestBias) {
            phase = CalPhase::Failed;
            reason = "rest too unstable";
            return;
        }
        candidate.bias[i] = float(rest_[i].mean);
        if (i < 2) {
            candidate.deadzone[i] = start::deadzoneBase + start::noiseMultiplier * rest_[i].sigma();
        }
    }
    for (unsigned i = 0; i < 4; ++i) {
        float range = float(directions_[i].mean);
        if (range < start::minimumRange || range < 2 * candidate.deadzone[i / 2] ||
            directions_[i].sigma() > range * .8f) {
            phase = CalPhase::Failed;
            reason = "direction not controllable";
            return;
        }
        candidate.gain[i] = std::clamp(600.f / range, 1.f, 120.f);
    }
    // Filtering responds to measured rest noise; bounded candidate, still requires user testing.
    candidate.alpha = std::clamp(.5f / (1 + rest_[0].sigma() + rest_[1].sigma()), .15f, .5f);
    phase = CalPhase::Validate;
    reason = "analyzed";
}
} // namespace nodx
