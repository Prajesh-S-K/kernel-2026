#pragma once
#include "parameters.hpp"
#include <array>
#include <cstdint>

namespace nodx {
// Command gestures. Patterns are helper-trained stroke sequences, not a classifier.
enum class GestureId : uint8_t { PauseResume = 0, Drag = 1 };
constexpr unsigned gestureCount = 2;
constexpr unsigned maxStrokes = 6;
using Rates = std::array<float, 3>; // yaw, pitch, roll in degrees/second
const char* name(GestureId id);

struct GestureTemplate {
    uint8_t strokes = 0;                    // 0 means not configured
    std::array<uint8_t, maxStrokes> axis{}; // 0 yaw, 1 pitch, 2 roll
    std::array<int8_t, maxStrokes> sign{};  // +1 or -1
    float enterRate = 0;
    float peakMin = 0, peakMax = 0;
    uint16_t strokeMinMs = 0, strokeMaxMs = 0, gapMaxMs = 0, totalMaxMs = 0;
    bool configured() const {
        return strokes != 0;
    }
    bool valid() const;
};
// Different sequences, and neither a prefix of the other (a prefix would fire early).
bool distinct(const GestureTemplate& a, const GestureTemplate& b);
struct GestureSet {
    float neutralRate = 0;
    std::array<GestureTemplate, gestureCount> templates{};
    bool valid(bool requireBoth) const;
};

enum class RecognizerState { WaitNeutral, Armed, Candidate };
enum class Reject {
    None,
    WrongOrder,
    TooSlow,
    TooFast,
    TooWeak,
    TooStrong,
    NotSingleAxis,
    GapTimeout,
    TotalTimeout,
    Ambiguous
};
const char* name(RecognizerState state);
const char* name(Reject reason);
struct GestureEvent {
    bool executed = false;
    GestureId id = GestureId::PauseResume;
};
// Deterministic state machine. Time comes only from sample timestamps (rollover safe).
class GestureRecognizer {
public:
    void configure(const GestureSet& set, unsigned enabledMask);
    void reset(uint32_t now);
    GestureEvent update(const Rates& rate, uint32_t now);
    // True while a candidate is open: pointer, scroll and dwell must stay suppressed.
    bool suppressing() const {
        return state_ == RecognizerState::Candidate;
    }
    RecognizerState state() const {
        return state_;
    }
    uint32_t candidates = 0, rejected = 0, executed = 0;
    Reject lastReject = Reject::None;
    int lastGesture = -1;

private:
    GestureSet set_;
    unsigned enabled_ = 0, viable_ = 0, index_ = 0;
    RecognizerState state_ = RecognizerState::WaitNeutral;
    Rates filtered_{};
    bool neutralRun_ = false, slow_ = false, inStroke_ = false;
    uint32_t neutralSince_ = 0, slowSince_ = 0, strokeStart_ = 0, lastEnd_ = 0, candidateStart_ = 0;
    unsigned axis_ = 0;
    int direction_ = 0;
    float peak_ = 0, cross_ = 0;
    void toWait(uint32_t now);
    void reject(Reject reason, uint32_t now);
    void startCandidate(unsigned dominant, int direction, float top, uint32_t now);
    GestureEvent advance(unsigned dominant, int direction, float top, uint32_t now);
    GestureEvent finishStroke(uint32_t now);
};

// One stroke found while capturing a training example.
struct Stroke {
    uint8_t axis = 0;
    int8_t sign = 0;
    float peak = 0, cross = 0;
    uint32_t startMs = 0, durationMs = 0;
};
enum class TrainPhase { Idle, Rest, Example, Analyze, Validate, Ready, Failed };
const char* name(TrainPhase phase);
class GestureTrainer {
public:
    // `other` is the already configured other gesture (may be unconfigured).
    void begin(GestureId id, uint32_t now, const GestureTemplate& other);
    void cancel();
    // `rate` is the bias-corrected, axis-mapped gyro. Valid samples only.
    void tick(const Rates& rate, uint32_t now);
    void fail(const char* reason);
    TrainPhase phase = TrainPhase::Idle;
    const char* reason = "idle";
    GestureId id = GestureId::PauseResume;
    unsigned accepted = 0, rejects = 0;
    bool validated = false;
    GestureTemplate candidate;
    float neutralRate = 0;

private:
    struct Example {
        unsigned count = 0;
        std::array<Stroke, maxStrokes> strokes{};
    };
    GestureTemplate other_;
    std::array<Example, start::trainExamples> examples_{};
    GestureRecognizer validator_;
    Rates filtered_{};
    uint32_t phaseStart_ = 0, captureStart_ = 0, quietSince_ = 0;
    // rest capture
    unsigned restCount_ = 0;
    std::array<double, 3> restMean_{}, restM2_{};
    // example capture
    bool capturing_ = false, quiet_ = false, needNeutral_ = true, active_ = false;
    unsigned segCount_ = 0, segAxis_ = 0;
    int segSign_ = 0;
    float segPeak_ = 0, segCross_ = 0, enter_ = 0;
    uint32_t segStart_ = 0;
    std::array<Stroke, maxStrokes + 1> segments_{};
    bool overflow_ = false;
    void tickRest(const Rates& rate, uint32_t now);
    void tickExample(uint32_t now);
    void segment(uint32_t now);
    void rejectExample(const char* why, uint32_t now);
    void finishExample(uint32_t now);
    void analyze(uint32_t now);
};
} // namespace nodx
