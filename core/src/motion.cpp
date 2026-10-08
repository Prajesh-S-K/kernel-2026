#include "nodx/motion.hpp"
#include <algorithm>
#include <cmath>

namespace nodx {
void MotionProcessor::reset() {
    filtered_ = {};
    roll_ = 0;
}
Motion MotionProcessor::process(const MotionSample& sample, const UserProfile& profile,
                                float dtSeconds) {
    for (unsigned axis = 0; axis < 3; ++axis) {
        filtered_[axis] +=
            profile.alpha * (sample.gyro[axis] - profile.bias[axis] - filtered_[axis]);
    }
    // Complementary roll: short-term gyro + long-term gravity. Mount convention is START.
    float gravityRoll = std::atan2(sample.accel[1], sample.accel[2]) * 57.2957795f;
    roll_ = .98f * (roll_ + filtered_[2] * dtSeconds) + .02f * gravityRoll;
    auto zone = [](float value, float deadzone) {
        return std::copysign(std::max(0.f, std::abs(value) - deadzone), value);
    };
    float speed = std::hypot(filtered_[0], filtered_[1]);
    return {zone(filtered_[0], profile.deadzone[0]), zone(filtered_[1], profile.deadzone[1]), roll_,
            1.f / (1.f + speed)};
}
Intent AdaptiveEngine::apply(const Motion& motion, const UserProfile& profile,
                             float dtSeconds) const {
    float speed = std::hypot(motion.x, motion.y);
    // Continuous response through precision/normal/travel regions.
    float factor = .35f + .65f * std::min(1.f, speed / profile.precisionThreshold) +
                   std::clamp((speed - profile.precisionThreshold) /
                                  (profile.fastThreshold - profile.precisionThreshold),
                              0.f, 1.f);
    Intent out;
    out.dx = motion.x * profile.gain[motion.x < 0 ? 0 : 1] * factor * dtSeconds;
    out.dy = motion.y * profile.gain[motion.y < 0 ? 2 : 3] * factor * dtSeconds;
    out.mode = speed < profile.precisionThreshold ? "PRECISION"
               : speed > profile.fastThreshold    ? "TRAVEL"
                                                  : "NORMAL";
    if (profile.scrollEnabled && std::abs(motion.roll) > profile.scrollThreshold) {
        out.wheel = std::copysign((std::abs(motion.roll) - profile.scrollThreshold) *
                                      profile.scrollGain * dtSeconds,
                                  motion.roll);
        out.mode = "SCROLL";
    }
    return out;
}
Intent IntentEngine::resolve(Intent intent, bool allowed, bool scrolling) const {
    if (!allowed) {
        return {};
    }
    if (scrolling) {
        intent.dx = 0;
        intent.dy = 0;
    }
    return intent;
}
} // namespace nodx
