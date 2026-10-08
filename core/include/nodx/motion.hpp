#pragma once
#include "profile.hpp"
#include "sensor.hpp"

namespace nodx {
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
struct Intent {
    float dx = 0, dy = 0, wheel = 0;
    const char* mode = "NORMAL";
};
class AdaptiveEngine {
public:
    Intent apply(const Motion& motion, const UserProfile& profile, float dt) const;
};
class IntentEngine {
public:
    Intent resolve(Intent intent, bool allowed, bool scrolling) const;
};
} // namespace nodx
