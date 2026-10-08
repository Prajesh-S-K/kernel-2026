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
    // leadMs > 0 inserts a countdown cue before every collecting phase; samples are ignored during it.
    void start(uint32_t now, uint32_t leadMs = 0);
    void cancel();
    void tick(const MotionSample& mapped, uint32_t now);
    CalPhase phase = CalPhase::Idle;
    const char* reason = "idle";
    UserProfile candidate;
    float progress(uint32_t now) const;
    // Milliseconds left of the countdown cue before the current phase collects (0 when collecting).
    uint32_t cueRemainingMs(uint32_t now) const;

private:
    uint32_t phaseStart_ = 0; // when this phase's collection window opens
    uint32_t leadMs_ = 0;
    std::array<Statistics, 3> rest_{};
    std::array<Statistics, 4> directions_{};
    unsigned natural_ = 0;
    void analyze();
};
} // namespace nodx
