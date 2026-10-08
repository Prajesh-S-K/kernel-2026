#pragma once
// EXPERIMENTAL quick tilt-and-return click for the temporary bench demo. One practice tilt fixes a
// three-dimensional gyro direction and a size; the recognizer then looks for: neutral -> outward stroke
// -> return stroke -> settled confirmation -> click -> rearm. It integrates the bias-corrected gyro over
// the real sample intervals, requires a clear outward excursion, an opposite return stroke and a small
// FINAL three-axis angular residual (gravity is never used as proof of returning). RAM only, off until
// explicitly enabled. Every number is an EXPERIMENTAL START value.
#include "mapping.hpp"
#include <cstdint>

namespace nodx {
struct QuickSettings {
    float sensitivity = start::quickSensitivity;         // 0.5 (stricter) .. 2.0 (more sensitive)
    float returnTolerance = start::quickReturnTolerance; // final residual / excursion
    // A candidate may only start while the rotation points within this angle of the designated
    // direction (and with the right sign); more deviation than tan(angle) of the excursion cancels it.
    float directionToleranceDeg = start::quickDirectionToleranceDeg; // validated 10 .. 60
    bool valid() const;
};

// What one practice tilt established, plus the thresholds derived from it.
struct QuickProfile {
    Vec3 direction{0, 0, 1};  // unit outward direction in the control frame (3-D, any orientation)
    Vec3 bias{};              // bias-corrected: gyro bias at rest, same frame
    float sigma = 1.f;        // worst-axis noise at rest, deg/s
    float practiceDeg = 0.f;  // practice excursion along the direction, degrees
    float practicePeak = 0.f; // practice peak rate along the direction, deg/s
    bool valid() const;
};
struct QuickThresholds {
    float enterRate, exitRate, returnRate, minExcursionDeg;
};
QuickThresholds deriveThresholds(const QuickProfile& profile, const QuickSettings& settings);

enum class QuickReject {
    None,
    TooSmall,      // never reached a clear outward excursion
    CrossAxis,     // too much motion off the practiced direction
    NoReturn,      // no opposite stroke, or stopped before coming back
    Residual,      // came back to a different orientation
    Timeout,       // not settled within the maximum time
    InvalidSample, // bad or irregular sample interval during a candidate
};
const char* name(QuickReject reason);

class QuickRecognizer {
public:
    enum class State { Neutral, Armed, Outward, Return, Settling };
    struct Event {
        bool accepted = false, rejected = false;
    };
    void configure(const QuickProfile& profile, const QuickSettings& settings);
    // After any stop, fault, reconnect or mode change: the candidate is dropped and a fresh neutral
    // period is required.
    void reset(uint32_t now);
    // `frame` is the control-frame gyro vector, NOT bias corrected.
    Event update(const Vec3& frame, uint32_t now);
    // True from candidate detection until acceptance or rejection: pointer output must be frozen and
    // the frozen movement discarded, never replayed.
    bool suppressing() const {
        return state_ == State::Outward || state_ == State::Return || state_ == State::Settling;
    }
    State state() const {
        return state_;
    }
    uint32_t accepted = 0, rejected = 0, candidates = 0, suppressedMs = 0;
    QuickReject lastReject = QuickReject::None;
    float lastExcursionDeg = 0, lastResidualDeg = 0, lastDurationMs = 0, lastCrossDeg = 0;

private:
    QuickProfile profile_;
    QuickSettings settings_;
    QuickThresholds th_{};
    float cosTolerance_ = .866f, crossRatio_ = .577f;
    State state_ = State::Neutral;
    uint32_t lastMs_ = 0, onsetMs_ = 0, calmSince_ = 0, nextAllowedMs_ = 0;
    Vec3 theta_{};
    float maxAlong_ = 0, maxCross_ = 0;
    bool retStarted_ = false;
    void finishCandidate(Event& event, bool accept, QuickReject reason, uint32_t now);
};

enum class QuickPhase { Idle, Rest, Tilt, Preview, Done, Failed };
const char* name(QuickPhase phase);
enum class QuickCue { None, HoldStill, Countdown, Go, Recording, ReturnToCentre, Preview };
const char* name(QuickCue cue);

struct QuickStatus {
    QuickPhase phase = QuickPhase::Idle;
    QuickCue cue = QuickCue::None;
    uint32_t cueMs = 0;
    float progress = 0;
    const char* reason = "idle";
    // practice result / preview
    float practiceDeg = 0, practiceResidualDeg = 0, practiceCrossDeg = 0, planeShare = 0;
    bool ready = false, enabled = false, suppressing = false, configuredFrame = false;
    const char* state = "OFF";
    uint32_t accepted = 0, rejected = 0, candidates = 0, clicks = 0, suppressedMs = 0;
    const char* lastReject = "NONE";
    float lastExcursionDeg = 0, lastResidualDeg = 0, lastDurationMs = 0;
    float sensitivity = 1.f, returnTolerance = .35f, directionToleranceDeg = 30.f;
    Vec3 direction{0, 0, 0}; // the designated (signed, unit) gyro direction; zero when none
    bool designated = false;
    const char* blocked = "";
};
size_t quickJson(char* out, size_t capacity, const QuickStatus& status);
constexpr size_t quickJsonCapacity = 1024;

// Guided practice: stationary noise, then ONE comfortable tilt-and-return. The result can be tried in
// a preview (the live recognizer runs, nothing clicks), retried, or accepted.
class QuickPractice {
public:
    void begin(uint32_t now, const QuickSettings& settings = QuickSettings{});
    void cancel();
    void retry(uint32_t now); // from the preview: capture another tilt (keeps the stillness)
    void tick(const Vec3& frame, uint32_t now);
    bool accept(); // Preview -> Done
    bool active() const {
        return phase_ != QuickPhase::Idle && phase_ != QuickPhase::Done &&
               phase_ != QuickPhase::Failed;
    }
    QuickPhase phase() const {
        return phase_;
    }
    QuickStatus status(uint32_t now) const;
    const QuickProfile& profile() const {
        return profile_;
    }
    const QuickRecognizer& preview() const {
        return preview_;
    }

private:
    enum class Sub { Countdown, WaitOnset, Moving };
    QuickPhase phase_ = QuickPhase::Idle;
    Sub sub_ = Sub::Countdown;
    uint32_t phaseStart_ = 0, subStart_ = 0, lastMs_ = 0, moveStart_ = 0, calmSince_ = 0;
    const char* reason_ = "idle";
    unsigned runCount_ = 0, onsetCount_ = 0;
    uint32_t runStart_ = 0;
    Vec3 runMean_{}, runM2_{}, bias_{};
    float sigma_ = 1.f;
    QuickProfile profile_;
    QuickSettings settings_;
    QuickRecognizer preview_;
    std::vector<Vec3> rates_;
    std::vector<float> dts_;
    float residualDeg_ = 0, crossDeg_ = 0, planeShare_ = 0;
    void beginTilt(uint32_t now);
    bool analyze();
    void fail(const char* reason);
    void tickRest(const Vec3& frame, uint32_t now);
    void tickTilt(const Vec3& frame, uint32_t now);
};
} // namespace nodx
