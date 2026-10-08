#include "nodx/gesture.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace nodx {
namespace {
bool between(float value, float low, float high) {
    return std::isfinite(value) && value >= low && value <= high;
}
constexpr unsigned bit(unsigned index) {
    return 1u << index;
}
unsigned lowestBit(unsigned mask) {
    unsigned index = 0;
    while (!(mask & bit(index))) {
        ++index;
    }
    return index;
}
unsigned popCount(unsigned mask) {
    unsigned count = 0;
    for (; mask; mask &= mask - 1) {
        ++count;
    }
    return count;
}
bool finite(const Rates& rate) {
    return std::isfinite(rate[0]) && std::isfinite(rate[1]) && std::isfinite(rate[2]);
}
// Dominant axis and magnitude of a rate vector.
float dominantOf(const Rates& rate, unsigned& axis) {
    float top = 0;
    axis = 0;
    for (unsigned i = 0; i < 3; ++i) {
        if (std::abs(rate[i]) > top) {
            top = std::abs(rate[i]);
            axis = i;
        }
    }
    return top;
}
float othersPeak(const Rates& rate, unsigned axis) {
    float peak = 0;
    for (unsigned i = 0; i < 3; ++i) {
        if (i != axis) {
            peak = std::max(peak, std::abs(rate[i]));
        }
    }
    return peak;
}
uint16_t clampMs(double value, double low, double high) {
    return static_cast<uint16_t>(std::clamp(value, low, high));
}
} // namespace

const char* name(GestureId id) {
    return id == GestureId::PauseResume ? "PAUSE_RESUME" : "DRAG";
}
const char* name(RecognizerState state) {
    switch (state) {
    case RecognizerState::WaitNeutral:
        return "WAIT_NEUTRAL";
    case RecognizerState::Armed:
        return "ARMED";
    case RecognizerState::Candidate:
        return "CANDIDATE";
    }
    return "WAIT_NEUTRAL";
}
const char* name(Reject reason) {
    switch (reason) {
    case Reject::None:
        return "NONE";
    case Reject::WrongOrder:
        return "WRONG_ORDER";
    case Reject::TooSlow:
        return "TOO_SLOW";
    case Reject::TooFast:
        return "TOO_FAST";
    case Reject::TooWeak:
        return "TOO_WEAK";
    case Reject::TooStrong:
        return "TOO_STRONG";
    case Reject::NotSingleAxis:
        return "NOT_SINGLE_AXIS";
    case Reject::GapTimeout:
        return "GAP_TIMEOUT";
    case Reject::TotalTimeout:
        return "TOTAL_TIMEOUT";
    case Reject::Ambiguous:
        return "AMBIGUOUS";
    }
    return "NONE";
}
const char* name(TrainPhase phase) {
    switch (phase) {
    case TrainPhase::Idle:
        return "IDLE";
    case TrainPhase::Rest:
        return "REST";
    case TrainPhase::Example:
        return "EXAMPLE";
    case TrainPhase::Analyze:
        return "ANALYZE";
    case TrainPhase::Validate:
        return "VALIDATE";
    case TrainPhase::Ready:
        return "READY";
    case TrainPhase::Failed:
        return "FAILED";
    }
    return "IDLE";
}

bool GestureTemplate::valid() const {
    if (strokes < 2 || strokes > maxStrokes) {
        return false;
    }
    for (unsigned i = 0; i < strokes; ++i) {
        if (axis[i] > 2 || (sign[i] != 1 && sign[i] != -1)) {
            return false;
        }
    }
    return between(enterRate, 8.f, 100.f) && between(peakMin, enterRate, 150.f) &&
           between(peakMax, peakMin, 240.f) && peakMax > peakMin && strokeMinMs >= 20 &&
           strokeMinMs <= 300 && strokeMaxMs >= strokeMinMs + 20 && strokeMaxMs <= 1000 &&
           gapMaxMs >= 20 && gapMaxMs <= 1000 && totalMaxMs >= 200 && totalMaxMs <= 4000;
}
bool distinct(const GestureTemplate& a, const GestureTemplate& b) {
    if (!a.configured() || !b.configured()) {
        return true;
    }
    const unsigned common = std::min(a.strokes, b.strokes);
    for (unsigned i = 0; i < common; ++i) {
        if (a.axis[i] != b.axis[i] || a.sign[i] != b.sign[i]) {
            return true;
        }
    }
    return false; // equal, or one is a prefix of the other
}
bool GestureSet::valid(bool requireBoth) const {
    bool any = false;
    for (const auto& item : templates) {
        if (!item.configured()) {
            if (requireBoth) {
                return false;
            }
            continue;
        }
        any = true;
        if (!item.valid()) {
            return false;
        }
    }
    if (!distinct(templates[0], templates[1])) {
        return false;
    }
    return any ? between(neutralRate, 1.f, 10.f) : std::isfinite(neutralRate);
}

// ---------------------------------------------------------------- recognizer
void GestureRecognizer::configure(const GestureSet& set, unsigned enabledMask) {
    set_ = set;
    enabled_ = 0;
    for (unsigned i = 0; i < gestureCount; ++i) {
        if ((enabledMask & bit(i)) && set.templates[i].configured()) {
            enabled_ |= bit(i);
        }
    }
}
void GestureRecognizer::toWait(uint32_t) {
    state_ = RecognizerState::WaitNeutral;
    neutralRun_ = slow_ = inStroke_ = false;
    viable_ = 0;
}
void GestureRecognizer::reset(uint32_t now) {
    toWait(now);
    filtered_ = {};
}
void GestureRecognizer::reject(Reject reason, uint32_t now) {
    lastReject = reason;
    ++rejected;
    toWait(now);
}
void GestureRecognizer::startCandidate(unsigned dominant, int direction, float top, uint32_t now) {
    unsigned mask = 0;
    for (unsigned t = 0; t < gestureCount; ++t) {
        const auto& item = set_.templates[t];
        if ((enabled_ & bit(t)) && item.axis[0] == dominant && item.sign[0] == direction &&
            top >= item.enterRate) {
            mask |= bit(t);
        }
    }
    if (!mask) {
        toWait(now); // motion that is not the start of a pattern disarms until neutral
        return;
    }
    state_ = RecognizerState::Candidate;
    viable_ = mask;
    index_ = 0;
    inStroke_ = true;
    axis_ = dominant;
    direction_ = direction;
    strokeStart_ = candidateStart_ = now;
    peak_ = top;
    cross_ = othersPeak(filtered_, dominant);
    ++candidates;
}
GestureEvent GestureRecognizer::update(const Rates& rate, uint32_t now) {
    if (!finite(rate)) {
        reset(now);
        return {};
    }
    for (unsigned i = 0; i < 3; ++i) {
        filtered_[i] += start::gestureAlpha * (rate[i] - filtered_[i]);
    }
    unsigned dominant = 0;
    const float top = dominantOf(filtered_, dominant);
    const int direction = filtered_[dominant] < 0 ? -1 : 1;
    const bool neutral = top < set_.neutralRate;
    switch (state_) {
    case RecognizerState::WaitNeutral:
        if (!neutral) {
            neutralRun_ = false;
            return {};
        }
        if (!neutralRun_) {
            neutralRun_ = true;
            neutralSince_ = now;
        }
        if (uint32_t(now - neutralSince_) >= start::gestureNeutralHoldMs) {
            state_ = RecognizerState::Armed;
            slow_ = false;
        }
        return {};
    case RecognizerState::Armed: {
        float minEnter = std::numeric_limits<float>::max();
        for (unsigned t = 0; t < gestureCount; ++t) {
            if (enabled_ & bit(t)) {
                minEnter = std::min(minEnter, set_.templates[t].enterRate);
            }
        }
        if (!enabled_) {
            return {};
        }
        if (top >= minEnter) {
            startCandidate(dominant, direction, top, now);
        } else if (top >= start::gestureExitRatio * minEnter) {
            // Slow movement that lingers is pointing, not the start of a pattern.
            if (!slow_) {
                slow_ = true;
                slowSince_ = now;
            } else if (uint32_t(now - slowSince_) > start::gestureRampMs) {
                toWait(now);
            }
        } else {
            slow_ = false;
        }
        return {};
    }
    case RecognizerState::Candidate:
        return advance(dominant, direction, top, now);
    }
    return {};
}
GestureEvent GestureRecognizer::advance(unsigned dominant, int direction, float top, uint32_t now) {
    if (!inStroke_) {
        unsigned keep = viable_;
        bool total = false;
        for (unsigned t = 0; t < gestureCount; ++t) {
            if (!(viable_ & bit(t))) {
                continue;
            }
            const auto& item = set_.templates[t];
            if (uint32_t(now - candidateStart_) > item.totalMaxMs) {
                keep &= ~bit(t);
                total = true;
            } else if (uint32_t(now - lastEnd_) > item.gapMaxMs) {
                keep &= ~bit(t);
            }
        }
        if (!keep) {
            reject(total ? Reject::TotalTimeout : Reject::GapTimeout, now);
            return {};
        }
        viable_ = keep;
        float minEnter = std::numeric_limits<float>::max();
        for (unsigned t = 0; t < gestureCount; ++t) {
            if (viable_ & bit(t)) {
                minEnter = std::min(minEnter, set_.templates[t].enterRate);
            }
        }
        if (top < minEnter) {
            return {};
        }
        unsigned mask = 0;
        for (unsigned t = 0; t < gestureCount; ++t) {
            const auto& item = set_.templates[t];
            if ((viable_ & bit(t)) && item.axis[index_] == dominant &&
                item.sign[index_] == direction && top >= item.enterRate) {
                mask |= bit(t);
            }
        }
        if (!mask) {
            reject(Reject::WrongOrder, now);
            return {};
        }
        viable_ = mask;
        inStroke_ = true;
        axis_ = dominant;
        direction_ = direction;
        strokeStart_ = now;
        peak_ = top;
        cross_ = othersPeak(filtered_, dominant);
        return {};
    }
    peak_ = std::max(peak_, float(direction_) * filtered_[axis_]);
    cross_ = std::max(cross_, othersPeak(filtered_, axis_));
    const uint32_t duration = uint32_t(now - strokeStart_);
    unsigned keep = viable_;
    float minEnter = std::numeric_limits<float>::max();
    for (unsigned t = 0; t < gestureCount; ++t) {
        if (!(viable_ & bit(t))) {
            continue;
        }
        if (duration > set_.templates[t].strokeMaxMs) {
            keep &= ~bit(t); // bounds how long pointer output can stay suppressed
        } else {
            minEnter = std::min(minEnter, set_.templates[t].enterRate);
        }
    }
    if (!keep) {
        reject(Reject::TooSlow, now);
        return {};
    }
    viable_ = keep;
    if (float(direction_) * filtered_[axis_] >= start::gestureExitRatio * minEnter) {
        return {};
    }
    return finishStroke(now);
}
GestureEvent GestureRecognizer::finishStroke(uint32_t now) {
    const uint32_t duration = uint32_t(now - strokeStart_);
    unsigned pass = 0;
    Reject first = Reject::None;
    for (unsigned t = 0; t < gestureCount; ++t) {
        if (!(viable_ & bit(t))) {
            continue;
        }
        const auto& item = set_.templates[t];
        Reject why = Reject::None;
        if (duration < item.strokeMinMs) {
            why = Reject::TooFast;
        } else if (duration > item.strokeMaxMs) {
            why = Reject::TooSlow;
        } else if (peak_ < item.peakMin) {
            why = Reject::TooWeak;
        } else if (peak_ > item.peakMax) {
            why = Reject::TooStrong;
        } else if (cross_ > start::gestureCrossRatio * peak_) {
            why = Reject::NotSingleAxis;
        } else if (uint32_t(now - candidateStart_) > item.totalMaxMs) {
            why = Reject::TotalTimeout;
        }
        if (why == Reject::None) {
            pass |= bit(t);
        } else if (first == Reject::None) {
            first = why;
        }
    }
    if (!pass) {
        reject(first, now);
        return {};
    }
    ++index_;
    unsigned complete = 0, continuing = 0;
    for (unsigned t = 0; t < gestureCount; ++t) {
        if (!(pass & bit(t))) {
            continue;
        }
        (set_.templates[t].strokes == index_ ? complete : continuing) |= bit(t);
    }
    if (complete) {
        if (popCount(complete) != 1 || continuing) {
            reject(Reject::Ambiguous, now); // never choose between candidates
            return {};
        }
        const unsigned id = lowestBit(complete);
        ++executed;
        lastGesture = int(id);
        toWait(now);
        return {true, static_cast<GestureId>(id)};
    }
    viable_ = continuing;
    inStroke_ = false;
    lastEnd_ = now;
    return {};
}

// ------------------------------------------------------------------- trainer
void GestureTrainer::begin(GestureId gesture, uint32_t now, const GestureTemplate& other) {
    *this = GestureTrainer{};
    id = gesture;
    other_ = other;
    phase = TrainPhase::Rest;
    reason = "rest capture: sit comfortably still";
    phaseStart_ = now;
}
void GestureTrainer::cancel() {
    if (phase == TrainPhase::Idle) {
        return;
    }
    phase = TrainPhase::Idle;
    reason = "training cancelled; prior configuration preserved";
    candidate = {};
    validated = false;
}
void GestureTrainer::fail(const char* why) {
    phase = TrainPhase::Failed;
    reason = why;
    validated = false;
}
void GestureTrainer::tick(const Rates& rate, uint32_t now) {
    if (phase == TrainPhase::Idle || phase == TrainPhase::Ready || phase == TrainPhase::Failed) {
        return;
    }
    if (!finite(rate)) {
        fail("invalid sensor data during training");
        return;
    }
    for (unsigned i = 0; i < 3; ++i) {
        filtered_[i] += start::gestureAlpha * (rate[i] - filtered_[i]);
    }
    if (phase == TrainPhase::Rest) {
        tickRest(rate, now);
    } else if (phase == TrainPhase::Example) {
        tickExample(now);
    } else if (phase == TrainPhase::Validate) {
        if (validator_.update(rate, now).executed) {
            validated = true;
            phase = TrainPhase::Ready;
            reason = "gesture recognised; accept it to stage the pattern";
        } else if (uint32_t(now - phaseStart_) > start::trainValidateMs) {
            fail("validation gesture not performed in time");
        }
    }
}
void GestureTrainer::tickRest(const Rates& rate, uint32_t now) {
    ++restCount_;
    for (unsigned i = 0; i < 3; ++i) {
        const double delta = rate[i] - restMean_[i];
        restMean_[i] += delta / restCount_;
        restM2_[i] += delta * (rate[i] - restMean_[i]);
    }
    if (uint32_t(now - phaseStart_) < start::trainRestMs) {
        return;
    }
    if (restCount_ < start::trainRestSamples) {
        fail("too few rest samples; sensor is not delivering data");
        return;
    }
    float sigma = 0;
    for (unsigned i = 0; i < 3; ++i) {
        if (std::abs(restMean_[i]) > start::maxRestBias) {
            fail("not at rest; stay still and try again");
            return;
        }
        sigma = std::max(sigma, float(std::sqrt(restM2_[i] / (restCount_ - 1))));
    }
    if (!std::isfinite(sigma) || sigma > start::maxRestSigma) {
        fail("too much motion or noise at rest");
        return;
    }
    neutralRate = std::clamp(3.f * sigma + 1.5f, 2.f, 8.f);
    phase = TrainPhase::Example;
    needNeutral_ = true;
    reason = "perform the pattern (example 1)";
}
void GestureTrainer::tickExample(uint32_t now) {
    unsigned dominant = 0;
    const float top = dominantOf(filtered_, dominant);
    const bool neutral = top < neutralRate;
    if (!capturing_) {
        if (needNeutral_) {
            if (!neutral) {
                quiet_ = false;
            } else if (!quiet_) {
                quiet_ = true;
                quietSince_ = now;
            } else if (uint32_t(now - quietSince_) >= start::gestureNeutralHoldMs) {
                needNeutral_ = quiet_ = false;
                phaseStart_ = now;
            }
            return;
        }
        if (uint32_t(now - phaseStart_) > start::trainWaitMs) {
            rejectExample("no movement detected; try again", now);
            return;
        }
        enter_ = std::max(3.f * neutralRate, 15.f);
        if (top >= enter_) {
            capturing_ = true;
            captureStart_ = now;
            segCount_ = 0;
            active_ = overflow_ = quiet_ = false;
            segment(now);
        }
        return;
    }
    segment(now);
    if (overflow_) {
        rejectExample("too many strokes; use the shorter pattern", now);
    } else if (uint32_t(now - captureStart_) > start::trainCaptureMs) {
        rejectExample("movement lasted too long; perform it faster", now);
    } else if (neutral && !active_) {
        if (!quiet_) {
            quiet_ = true;
            quietSince_ = now;
        } else if (uint32_t(now - quietSince_) >= start::gestureNeutralHoldMs) {
            finishExample(now);
        }
    } else {
        quiet_ = false;
    }
}
void GestureTrainer::segment(uint32_t now) {
    unsigned dominant = 0;
    const float top = dominantOf(filtered_, dominant);
    if (!active_) {
        if (top >= enter_) {
            active_ = true;
            segAxis_ = dominant;
            segSign_ = filtered_[dominant] < 0 ? -1 : 1;
            segStart_ = now;
            segPeak_ = top;
            segCross_ = othersPeak(filtered_, dominant);
        }
        return;
    }
    segPeak_ = std::max(segPeak_, float(segSign_) * filtered_[segAxis_]);
    segCross_ = std::max(segCross_, othersPeak(filtered_, segAxis_));
    if (float(segSign_) * filtered_[segAxis_] >= start::gestureExitRatio * enter_) {
        return;
    }
    active_ = false;
    if (segCount_ >= maxStrokes) {
        overflow_ = true;
        return;
    }
    segments_[segCount_++] = {uint8_t(segAxis_), int8_t(segSign_), segPeak_,
                              segCross_,         segStart_,        uint32_t(now - segStart_)};
}
void GestureTrainer::rejectExample(const char* why, uint32_t) {
    reason = why;
    capturing_ = quiet_ = active_ = false;
    needNeutral_ = true;
    if (++rejects > start::trainMaxRejects) {
        fail("too many rejected attempts; restart training");
    }
}
void GestureTrainer::finishExample(uint32_t now) {
    if (segCount_ < 2) {
        rejectExample("need at least two strokes; repeat the whole pattern", now);
        return;
    }
    for (unsigned i = 0; i < segCount_; ++i) {
        if (segments_[i].peak < start::gestureMinPeak) {
            rejectExample("movement too small; make it a little larger", now);
            return;
        }
        if (segments_[i].cross > start::gestureCrossRatio * segments_[i].peak) {
            rejectExample("keep each movement on a single axis", now);
            return;
        }
    }
    if (accepted > 0) {
        const Example& first = examples_[0];
        bool same = first.count == segCount_;
        for (unsigned i = 0; same && i < segCount_; ++i) {
            same = first.strokes[i].axis == segments_[i].axis &&
                   first.strokes[i].sign == segments_[i].sign;
        }
        if (!same) {
            rejectExample("pattern differs from the earlier examples; repeat the same one", now);
            return;
        }
    }
    Example& stored = examples_[accepted++];
    stored.count = segCount_;
    for (unsigned i = 0; i < segCount_; ++i) {
        stored.strokes[i] = segments_[i];
    }
    capturing_ = quiet_ = active_ = false;
    needNeutral_ = true;
    if (accepted == start::trainExamples) {
        phase = TrainPhase::Analyze;
        analyze(now);
        return;
    }
    static const char* const messages[] = {"example 1 accepted; perform it again",
                                           "example 2 accepted; perform it again",
                                           "example 3 accepted; perform it again"};
    reason = messages[accepted - 1];
}
void GestureTrainer::analyze(uint32_t now) {
    double minPeak = 1e9, maxPeak = 0, minDuration = 1e9, maxDuration = 0, maxGap = 0, maxTotal = 0;
    const Example& first = examples_[0];
    for (const Example& example : examples_) {
        if (example.count != first.count) {
            fail("examples are inconsistent; restart training");
            return;
        }
        for (unsigned i = 0; i < example.count; ++i) {
            const Stroke& stroke = example.strokes[i];
            if (stroke.axis != first.strokes[i].axis || stroke.sign != first.strokes[i].sign) {
                fail("examples are inconsistent; restart training");
                return;
            }
            minPeak = std::min<double>(minPeak, stroke.peak);
            maxPeak = std::max<double>(maxPeak, stroke.peak);
            minDuration = std::min<double>(minDuration, stroke.durationMs);
            maxDuration = std::max<double>(maxDuration, stroke.durationMs);
            if (i > 0) {
                const Stroke& before = example.strokes[i - 1];
                maxGap = std::max<double>(
                    maxGap,
                    double(uint32_t(stroke.startMs - (before.startMs + before.durationMs))));
            }
        }
        const Stroke& last = example.strokes[example.count - 1];
        maxTotal = std::max<double>(maxTotal, double(uint32_t(last.startMs + last.durationMs -
                                                              example.strokes[0].startMs)));
    }
    GestureTemplate result;
    result.strokes = uint8_t(first.count);
    for (unsigned i = 0; i < first.count; ++i) {
        result.axis[i] = first.strokes[i].axis;
        result.sign[i] = first.strokes[i].sign;
    }
    result.peakMin = float(.5 * minPeak);
    result.peakMax = float(std::min(240., 1.5 * maxPeak));
    result.enterRate = std::clamp(.5f * result.peakMin, 8.f, 100.f);
    result.strokeMinMs = clampMs(.5 * minDuration, 20, 300);
    result.strokeMaxMs = clampMs(2 * maxDuration, result.strokeMinMs + 20, 1000);
    result.gapMaxMs = clampMs(std::max(2 * maxGap, 150.), 20, 1000);
    result.totalMaxMs = clampMs(1.5 * maxTotal, 200, 4000);
    if (!result.valid()) {
        fail("learned pattern is outside allowed bounds; restart training");
        return;
    }
    if (!distinct(result, other_)) {
        fail("too similar to the other gesture; choose a different pattern");
        return;
    }
    candidate = result;
    GestureSet set;
    set.neutralRate = neutralRate;
    set.templates[static_cast<unsigned>(id)] = candidate;
    validator_.configure(set, bit(static_cast<unsigned>(id)));
    validator_.reset(now);
    phase = TrainPhase::Validate;
    phaseStart_ = now;
    reason = "validation: perform the pattern once more";
}
} // namespace nodx
