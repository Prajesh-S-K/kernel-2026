#pragma once
// Optional gesture click for the uncalibrated and configured control sessions: one deliberate,
// user-taught movement (default: a side tilt and return) produces one primary-button click.
// Bounded, time-normalised template matching on the bias-removed 3-axis gyro vector. Trained in
// RAM, never saved, off until explicitly enabled. All numbers are START values.
#include "mapping.hpp"
#include <array>
#include <cstdint>
#include <vector>

namespace nodx {
constexpr unsigned clickTracePoints = 16;

struct ClickTemplate {
    std::array<Vec3, clickTracePoints> trace{}; // mean example, deg/s, bias removed
    Vec3 bias{};                                // gyro bias in the trained frame at rest
    float sigma = 1.f;                          // worst-axis noise at rest
    float peak = 0.f;                           // largest |component| of the trace
    float threshold = .5f;                      // accepted relative distance
    float meanMs = 0.f;                         // mean gesture duration
    unsigned axis = 0;                          // principal axis of the gesture
    float firstSign = 1.f;                      // sign of the first strong lobe on that axis
    bool valid() const;
};

enum class ClickPhase { Idle, Rest, Example, Confusion, Ready, Done, Failed };
const char* name(ClickPhase phase);
enum class ClickCue { None, HoldStill, Countdown, Go, Recording, ReturnToCentre, PointNormally, Ready };
const char* name(ClickCue cue);

struct ClickTrainStatus {
    ClickPhase phase = ClickPhase::Idle;
    ClickCue cue = ClickCue::None;
    unsigned step = 0, steps = 0, retries = 0;
    bool validation = false;
    uint32_t cueMs = 0;
    float progress = 0;
    const char* reason = "idle";
    uint32_t activityMs = 0; // confusion check: time of ordinary pointing observed
};

// Normalised comparison shared by training and recognition.
std::array<Vec3, clickTracePoints> resampleTrace(const std::vector<Vec3>& samples,
                                                 const std::vector<uint32_t>& times);
float relativeDistance(const std::array<Vec3, clickTracePoints>& a,
                       const std::array<Vec3, clickTracePoints>& b);

class ClickRecognizer {
public:
    enum class State { Armed, Candidate, NeutralWait };
    struct Event {
        bool accepted = false;
    };
    void configure(const ClickTemplate& tmpl);
    void reset(uint32_t now);
    // `frame` is the 3-axis gyro vector in the trained frame, NOT bias corrected.
    Event update(const Vec3& frame, uint32_t now);
    // True while a candidate is being collected: pointer output must be suppressed and the
    // suppressed movement discarded, never replayed.
    bool suppressing() const {
        return state_ == State::Candidate;
    }
    State state() const {
        return state_;
    }
    uint32_t accepted = 0, rejected = 0, candidates = 0;
    const char* lastReject = "NONE";

private:
    ClickTemplate tmpl_;
    State state_ = State::Armed;
    uint32_t lastMs_ = 0, startMs_ = 0, calmSince_ = 0, neutralSince_ = 0;
    std::vector<Vec3> samples_;
    std::vector<uint32_t> times_;
    float enterRate() const;
    float exitRate() const;
    bool isCandidateStart(const Vec3& rates) const;
    bool finish(uint32_t now);
};

struct ClickStatus {
    ClickTrainStatus train;
    bool ready = false, enabled = false, suppressing = false, configuredFrame = false;
    const char* state = "ARMED";
    const char* lastReject = "NONE";
    uint32_t accepted = 0, rejected = 0, candidates = 0, clicks = 0;
    const char* blocked = "";
};
size_t clickJson(char* out, size_t capacity, const ClickStatus& status);
constexpr size_t clickJsonCapacity = 768;

class ClickTrainer {
public:
    void begin(uint32_t now);
    void cancel();
    void tick(const Vec3& frame, uint32_t now);
    bool accept(); // Ready -> Done
    bool active() const {
        return phase_ != ClickPhase::Idle && phase_ != ClickPhase::Done &&
               phase_ != ClickPhase::Failed;
    }
    ClickPhase phase() const {
        return phase_;
    }
    ClickTrainStatus status(uint32_t now) const;
    const ClickTemplate& result() const {
        return result_;
    }

private:
    enum class Sub { Countdown, WaitOnset, Moving, Settle };
    ClickPhase phase_ = ClickPhase::Idle;
    Sub sub_ = Sub::Countdown;
    uint32_t phaseStart_ = 0, subStart_ = 0, lastMs_ = 0, moveStart_ = 0, calmSince_ = 0;
    uint32_t activityMs_ = 0;
    bool pendingAdvance_ = false;
    const char* reason_ = "idle";
    // rest
    unsigned runCount_ = 0;
    uint32_t runStart_ = 0;
    Vec3 runMean_{}, runM2_{};
    Vec3 bias_{};
    float sigma_ = 1.f;
    // examples
    unsigned step_ = 0, retries_ = 0, onsetCount_ = 0;
    std::vector<Vec3> samples_;
    std::vector<uint32_t> times_;
    std::vector<std::array<Vec3, clickTracePoints>> examples_;
    std::vector<float> durations_;
    ClickTemplate result_;
    ClickRecognizer confusion_;
    float enterRate() const;
    float exitRate() const;
    void beginStep(uint32_t now);
    void reject(const char* reason, uint32_t now);
    void finishMovement(uint32_t now);
    bool buildTemplate();
    void fail(const char* reason);
    void tickRest(const Vec3& frame, uint32_t now);
    void tickExample(const Vec3& frame, uint32_t now);
    void tickConfusion(const Vec3& frame, uint32_t now);
};
} // namespace nodx
