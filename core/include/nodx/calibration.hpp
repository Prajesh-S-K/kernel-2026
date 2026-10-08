#pragma once
#include "profile.hpp"
#include "sensor.hpp"

namespace nodx {
enum class CalPhase {
    Idle,
    Rest,
    Left,
    Right,
    Up,
    Down,
    Natural,
    Analyze,
    Validate,
    Save,
    Complete,
    Failed
};
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
} // namespace nodx
