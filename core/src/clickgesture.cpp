#include "nodx/clickgesture.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace nodx {
namespace {
Vec3 minus(const Vec3& a, const Vec3& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
float absMax(const Vec3& v, unsigned skip) {
    float m = 0;
    for (unsigned i = 0; i < 3; ++i) {
        if (i != skip) {
            m = std::max(m, std::abs(v[i]));
        }
    }
    return m;
}
float traceRms(const std::array<Vec3, clickTracePoints>& t) {
    float sum = 0;
    for (const Vec3& v : t) {
        sum += dot(v, v);
    }
    return std::sqrt(sum / float(clickTracePoints));
}
std::array<Vec3, clickTracePoints> meanTrace(
    const std::vector<std::array<Vec3, clickTracePoints>>& items, int skip = -1) {
    std::array<Vec3, clickTracePoints> mean{};
    unsigned n = 0;
    for (size_t i = 0; i < items.size(); ++i) {
        if (int(i) == skip) {
            continue;
        }
        ++n;
        for (unsigned k = 0; k < clickTracePoints; ++k) {
            for (unsigned a = 0; a < 3; ++a) {
                mean[k][a] += items[i][k][a];
            }
        }
    }
    for (auto& v : mean) {
        for (float& x : v) {
            x = n ? x / float(n) : 0.f;
        }
    }
    return mean;
}
} // namespace

const char* name(ClickPhase p) {
    switch (p) {
    case ClickPhase::Idle:
        return "IDLE";
    case ClickPhase::Rest:
        return "REST";
    case ClickPhase::Example:
        return "EXAMPLE";
    case ClickPhase::Confusion:
        return "CONFUSION_CHECK";
    case ClickPhase::Ready:
        return "READY";
    case ClickPhase::Done:
        return "DONE";
    case ClickPhase::Failed:
        return "FAILED";
    }
    return "FAILED";
}
const char* name(ClickCue c) {
    switch (c) {
    case ClickCue::None:
        return "NONE";
    case ClickCue::HoldStill:
        return "HOLD_STILL";
    case ClickCue::Countdown:
        return "COUNTDOWN";
    case ClickCue::Go:
        return "GO";
    case ClickCue::Recording:
        return "RECORDING";
    case ClickCue::ReturnToCentre:
        return "RETURN_TO_CENTRE";
    case ClickCue::PointNormally:
        return "POINT_NORMALLY";
    case ClickCue::Ready:
        return "READY";
    }
    return "NONE";
}

bool ClickTemplate::valid() const {
    if (!std::isfinite(sigma) || sigma <= 0 || !std::isfinite(peak) || peak < start::clickMinPeak ||
        !std::isfinite(threshold) || threshold < start::clickThresholdMin ||
        threshold > start::clickThresholdMax || !(meanMs >= float(start::clickMinMs)) ||
        meanMs > float(start::clickMaxMs) || axis > 2 || !(std::abs(firstSign) == 1.f)) {
        return false;
    }
    for (const Vec3& v : trace) {
        for (float x : v) {
            if (!std::isfinite(x)) {
                return false;
            }
        }
    }
    return true;
}

std::array<Vec3, clickTracePoints> resampleTrace(const std::vector<Vec3>& samples,
                                                 const std::vector<uint32_t>& times) {
    std::array<Vec3, clickTracePoints> out{};
    if (samples.size() < 2 || samples.size() != times.size()) {
        return out;
    }
    const float t0 = float(times.front());
    const float span = float(times.back()) - t0;
    if (!(span > 0)) {
        return out;
    }
    size_t seg = 0;
    for (unsigned k = 0; k < clickTracePoints; ++k) {
        const float t = t0 + span * float(k) / float(clickTracePoints - 1);
        while (seg + 2 < samples.size() && float(times[seg + 1]) < t) {
            ++seg;
        }
        const float a = float(times[seg]), b = float(times[seg + 1]);
        const float w = b > a ? std::clamp((t - a) / (b - a), 0.f, 1.f) : 0.f;
        for (unsigned axis = 0; axis < 3; ++axis) {
            out[k][axis] = samples[seg][axis] * (1.f - w) + samples[seg + 1][axis] * w;
        }
    }
    return out;
}
float relativeDistance(const std::array<Vec3, clickTracePoints>& a,
                       const std::array<Vec3, clickTracePoints>& b) {
    float sum = 0;
    for (unsigned k = 0; k < clickTracePoints; ++k) {
        const Vec3 d = minus(a[k], b[k]);
        sum += dot(d, d);
    }
    return std::sqrt(sum / float(clickTracePoints)) / std::max(traceRms(b), 10.f);
}

// ---------------------------------------------------------------- recognizer
float ClickRecognizer::enterRate() const {
    return std::max(start::clickEnterFloor, 8.f * tmpl_.sigma);
}
float ClickRecognizer::exitRate() const {
    return std::max(start::clickExitFloor, 4.f * tmpl_.sigma);
}
void ClickRecognizer::configure(const ClickTemplate& tmpl) {
    tmpl_ = tmpl;
    accepted = rejected = candidates = 0;
    lastReject = "NONE";
    reset(0);
}
void ClickRecognizer::reset(uint32_t now) {
    state_ = State::NeutralWait; // after any reset a neutral stretch is required before arming
    neutralSince_ = 0;
    calmSince_ = 0;
    lastMs_ = now;
    samples_.clear();
    times_.clear();
}
bool ClickRecognizer::isCandidateStart(const Vec3& rates) const {
    const float p = rates[tmpl_.axis];
    return p * tmpl_.firstSign > enterRate() &&
           std::abs(p) >= start::clickDominance * absMax(rates, tmpl_.axis);
}
bool ClickRecognizer::finish(uint32_t) {
    // drop the trailing calm samples; they are not part of the gesture
    while (!times_.empty() && times_.back() >= calmSince_ && calmSince_ != 0) {
        times_.pop_back();
        samples_.pop_back();
    }
    if (samples_.size() < 5) {
        lastReject = "TOO_SHORT";
        return false;
    }
    const float duration = float(times_.back() - times_.front());
    if (duration < float(start::clickMinMs) || duration > float(start::clickMaxMs)) {
        lastReject = duration < float(start::clickMinMs) ? "TOO_SHORT" : "TOO_LONG";
        return false;
    }
    if (duration < .5f * tmpl_.meanMs || duration > 2.f * tmpl_.meanMs) {
        lastReject = "WRONG_SPEED";
        return false;
    }
    if (relativeDistance(resampleTrace(samples_, times_), tmpl_.trace) > tmpl_.threshold) {
        lastReject = "NO_MATCH";
        return false;
    }
    return true;
}
ClickRecognizer::Event ClickRecognizer::update(const Vec3& frame, uint32_t now) {
    Event event;
    const Vec3 rates = minus(frame, tmpl_.bias);
    const float rate = norm(rates);
    lastMs_ = now;
    switch (state_) {
    case State::NeutralWait:
        if (rate < exitRate()) {
            if (neutralSince_ == 0) {
                neutralSince_ = now;
            }
            if (uint32_t(now - neutralSince_) >= start::clickNeutralMs) {
                state_ = State::Armed;
            }
        } else {
            neutralSince_ = 0;
        }
        break;
    case State::Armed:
        if (isCandidateStart(rates)) {
            state_ = State::Candidate;
            startMs_ = now;
            calmSince_ = 0;
            samples_.assign(1, rates);
            times_.assign(1, now);
            ++candidates;
        }
        break;
    case State::Candidate:
        samples_.push_back(rates);
        times_.push_back(now);
        if (rate < exitRate()) {
            if (calmSince_ == 0) {
                calmSince_ = now;
            }
        } else {
            calmSince_ = 0;
        }
        const bool ended = calmSince_ != 0 && uint32_t(now - calmSince_) >= start::clickCalmMs;
        const bool tooLong = uint32_t(now - startMs_) > start::clickMaxMs + start::clickCalmMs;
        if (ended || tooLong) {
            if (tooLong && !ended) {
                lastReject = "TOO_LONG";
                ++rejected;
            } else if (finish(now)) {
                ++accepted;
                event.accepted = true;
            } else {
                ++rejected; // movement collected for a command candidate is discarded, never replayed
            }
            samples_.clear();
            times_.clear();
            state_ = State::NeutralWait;
            neutralSince_ = 0;
        }
        break;
    }
    return event;
}

// ---------------------------------------------------------------- trainer
float ClickTrainer::enterRate() const {
    return std::max(start::clickEnterFloor, 8.f * sigma_);
}
float ClickTrainer::exitRate() const {
    return std::max(start::clickExitFloor, 4.f * sigma_);
}
void ClickTrainer::begin(uint32_t now) {
    *this = ClickTrainer{};
    phase_ = ClickPhase::Rest;
    phaseStart_ = now;
    lastMs_ = now;
    reason_ = "hold the assembly completely still";
}
void ClickTrainer::cancel() {
    if (active()) {
        phase_ = ClickPhase::Failed;
        reason_ = "cancelled";
    }
}
void ClickTrainer::fail(const char* reason) {
    phase_ = ClickPhase::Failed;
    reason_ = reason;
}
bool ClickTrainer::accept() {
    if (phase_ != ClickPhase::Ready) {
        return false;
    }
    phase_ = ClickPhase::Done;
    reason_ = "accepted";
    return true;
}
void ClickTrainer::tick(const Vec3& frame, uint32_t now) {
    if (!active()) {
        return;
    }
    if (phase_ == ClickPhase::Rest) {
        tickRest(frame, now);
    } else if (phase_ == ClickPhase::Example) {
        tickExample(frame, now);
    } else if (phase_ == ClickPhase::Confusion) {
        tickConfusion(frame, now);
    }
    lastMs_ = now;
}
void ClickTrainer::tickRest(const Vec3& g, uint32_t now) {
    if (uint32_t(now - phaseStart_) > start::clickRestWindowMs) {
        fail("could not hold still for 1.5 seconds within 6 seconds");
        return;
    }
    auto seed = [&] {
        runStart_ = now;
        runCount_ = 1;
        runMean_ = g;
        runM2_ = {};
    };
    if (runCount_ == 0) {
        seed();
        return;
    }
    bool moved = false;
    for (unsigned i = 0; i < 3; ++i) {
        moved = moved || std::abs(g[i] - runMean_[i]) > start::mapStillGyroDeviation;
    }
    if (moved) {
        reason_ = "movement detected: hold the assembly completely still";
        seed();
        return;
    }
    ++runCount_;
    for (unsigned i = 0; i < 3; ++i) {
        const float delta = g[i] - runMean_[i];
        runMean_[i] += delta / float(runCount_);
        runM2_[i] += delta * (g[i] - runMean_[i]);
    }
    if (uint32_t(now - runStart_) < start::clickRestMs || runCount_ < 100) {
        return;
    }
    bias_ = runMean_;
    sigma_ = .05f;
    for (unsigned i = 0; i < 3; ++i) {
        sigma_ = std::max(sigma_, std::sqrt(runM2_[i] / float(runCount_ - 1)));
    }
    if (sigma_ > 4.f) {
        fail("the sensor is too noisy at rest");
        return;
    }
    phase_ = ClickPhase::Example;
    step_ = 0;
    retries_ = 0;
    reason_ = "stillness measured";
    beginStep(now);
}
void ClickTrainer::beginStep(uint32_t now) {
    sub_ = Sub::Countdown;
    subStart_ = now;
    onsetCount_ = 0;
    samples_.clear();
    times_.clear();
    calmSince_ = 0;
}
void ClickTrainer::reject(const char* reason, uint32_t now) {
    reason_ = reason;
    pendingAdvance_ = false;
    if (++retries_ > start::clickMaxRetries) {
        fail("too many failed attempts at one example; start the training again");
        return;
    }
    sub_ = Sub::Settle;
    subStart_ = now;
}
void ClickTrainer::finishMovement(uint32_t now) {
    while (!times_.empty() && calmSince_ != 0 && times_.back() >= calmSince_) {
        times_.pop_back();
        samples_.pop_back();
    }
    if (samples_.size() < 5) {
        reject("movement too short: make the whole gesture", now);
        return;
    }
    const float duration = float(times_.back() - times_.front());
    float peak = 0;
    for (const Vec3& v : samples_) {
        peak = std::max(peak, std::max({std::abs(v[0]), std::abs(v[1]), std::abs(v[2])}));
    }
    if (duration < float(start::clickMinMs)) {
        reject("movement too short: make the whole gesture", now);
        return;
    }
    if (peak < start::clickMinPeak) {
        reject("movement too weak: make it clearer", now);
        return;
    }
    const auto trace = resampleTrace(samples_, times_);
    const bool validation = step_ >= start::clickExamples;
    if (validation) {
        if (relativeDistance(trace, result_.trace) > result_.threshold) {
            reject("that did not match what you taught; try the same gesture again", now);
            return;
        }
    } else if (!examples_.empty() &&
               relativeDistance(trace, meanTrace(examples_)) > start::clickConsistency) {
        reject("that gesture differed from your earlier ones; repeat the same one", now);
        return;
    }
    if (!validation) {
        examples_.push_back(trace);
        durations_.push_back(duration);
    }
    retries_ = 0;
    pendingAdvance_ = true;
    reason_ = "example accepted";
    sub_ = Sub::Settle;
    subStart_ = now;
}
bool ClickTrainer::buildTemplate() {
    result_ = ClickTemplate{};
    result_.trace = meanTrace(examples_);
    result_.bias = bias_;
    result_.sigma = sigma_;
    float energy[3] = {0, 0, 0}, peak = 0;
    for (const Vec3& v : result_.trace) {
        for (unsigned a = 0; a < 3; ++a) {
            energy[a] += v[a] * v[a];
            peak = std::max(peak, std::abs(v[a]));
        }
    }
    const float total = energy[0] + energy[1] + energy[2];
    result_.axis = unsigned(std::max_element(energy, energy + 3) - energy);
    if (!(total > 0) || energy[result_.axis] / total < start::clickPrincipalShare) {
        fail("that pattern is ambiguous: use one clear movement along a single direction");
        return false;
    }
    result_.peak = peak;
    float axisPeak = 0;
    for (const Vec3& v : result_.trace) {
        axisPeak = std::max(axisPeak, std::abs(v[result_.axis]));
    }
    result_.firstSign = 1.f;
    for (const Vec3& v : result_.trace) {
        if (std::abs(v[result_.axis]) >= .3f * axisPeak) {
            result_.firstSign = v[result_.axis] < 0 ? -1.f : 1.f;
            break;
        }
    }
    float worst = 0, meanMs = 0;
    for (size_t i = 0; i < examples_.size(); ++i) {
        worst = std::max(worst, relativeDistance(examples_[i], meanTrace(examples_, int(i))));
        meanMs += durations_[i] / float(durations_.size());
    }
    if (worst > start::clickMaxExampleError) {
        fail("your examples were too different from each other; teach one steady gesture");
        return false;
    }
    result_.threshold = std::clamp(start::clickThresholdMargin * worst, start::clickThresholdMin,
                                   start::clickThresholdMax);
    result_.meanMs = meanMs;
    if (!result_.valid()) {
        fail("the gesture was too weak or too short to use");
        return false;
    }
    return true;
}
void ClickTrainer::tickExample(const Vec3& frame, uint32_t now) {
    const Vec3 g = minus(frame, bias_);
    const float rate = norm(g);
    switch (sub_) {
    case Sub::Countdown:
        if (uint32_t(now - subStart_) >= start::mapCountdownMs) {
            sub_ = Sub::WaitOnset;
            subStart_ = now;
            onsetCount_ = 0;
            samples_.clear();
            times_.clear();
        }
        break;
    case Sub::WaitOnset:
        samples_.push_back(g);
        times_.push_back(now);
        if (samples_.size() > 3) {
            samples_.erase(samples_.begin());
            times_.erase(times_.begin());
        }
        onsetCount_ = rate > enterRate() ? onsetCount_ + 1 : 0;
        if (onsetCount_ >= 3) {
            sub_ = Sub::Moving;
            moveStart_ = now;
            calmSince_ = 0;
        } else if (uint32_t(now - subStart_) >= start::mapOnsetWindowMs) {
            reject("no movement detected after the cue", now);
        }
        break;
    case Sub::Moving:
        samples_.push_back(g);
        times_.push_back(now);
        if (rate < exitRate()) {
            if (calmSince_ == 0) {
                calmSince_ = now;
            }
            if (uint32_t(now - calmSince_) >= start::clickCalmMs) {
                finishMovement(now);
                return;
            }
        } else {
            calmSince_ = 0;
        }
        if (uint32_t(now - moveStart_) > start::clickMaxMs + start::clickCalmMs) {
            reject("the gesture was too long: do it in about a second", now);
        }
        break;
    case Sub::Settle:
        if (uint32_t(now - subStart_) < start::mapSettleMs) {
            break;
        }
        if (pendingAdvance_) {
            pendingAdvance_ = false;
            ++step_;
            if (step_ == start::clickExamples && !buildTemplate()) {
                return;
            }
            if (step_ >= start::clickExamples + start::clickValidations) {
                phase_ = ClickPhase::Confusion;
                phaseStart_ = now;
                activityMs_ = 0;
                confusion_.configure(result_);
                reason_ = "now move the pointer as you normally would";
                return;
            }
        }
        beginStep(now);
        break;
    }
}
void ClickTrainer::tickConfusion(const Vec3& frame, uint32_t now) {
    const uint32_t dt = std::min<uint32_t>(50, uint32_t(now - lastMs_));
    if (norm(minus(frame, bias_)) > 8.f) {
        activityMs_ += dt;
    }
    confusion_.update(frame, now);
    if (confusion_.accepted > 0) {
        fail("ordinary pointing triggered this gesture; choose a different movement");
        return;
    }
    if (activityMs_ >= start::clickConfusionActivityMs) {
        phase_ = ClickPhase::Ready;
        reason_ = "ready: ordinary pointing did not trigger it; accept to use it";
        return;
    }
    if (uint32_t(now - phaseStart_) > start::clickConfusionWindowMs) {
        fail("not enough ordinary pointing to check: keep moving as you normally point");
    }
}
ClickTrainStatus ClickTrainer::status(uint32_t now) const {
    ClickTrainStatus st;
    st.phase = phase_;
    st.steps = start::clickExamples + start::clickValidations;
    st.step = step_;
    st.retries = retries_;
    st.reason = reason_;
    st.activityMs = activityMs_;
    auto left = [&](uint32_t total) {
        const uint32_t spent = uint32_t(now - subStart_);
        return spent >= total ? 0u : total - spent;
    };
    if (phase_ == ClickPhase::Rest) {
        st.cue = ClickCue::HoldStill;
        st.progress = .1f;
    } else if (phase_ == ClickPhase::Example) {
        st.validation = step_ >= start::clickExamples;
        switch (sub_) {
        case Sub::Countdown:
            st.cue = ClickCue::Countdown;
            st.cueMs = left(start::mapCountdownMs);
            break;
        case Sub::WaitOnset:
            st.cue = ClickCue::Go;
            st.cueMs = left(start::mapOnsetWindowMs);
            break;
        case Sub::Moving:
            st.cue = ClickCue::Recording;
            break;
        case Sub::Settle:
            st.cue = ClickCue::ReturnToCentre;
            st.cueMs = left(start::mapSettleMs);
            break;
        }
        st.progress = .1f + .7f * float(step_) / float(st.steps);
    } else if (phase_ == ClickPhase::Confusion) {
        st.cue = ClickCue::PointNormally;
        st.progress = .8f + .2f * std::min(1.f, float(activityMs_) /
                                                    float(start::clickConfusionActivityMs));
    } else if (phase_ == ClickPhase::Ready || phase_ == ClickPhase::Done) {
        st.cue = ClickCue::Ready;
        st.progress = 1.f;
    }
    return st;
}
size_t clickJson(char* out, size_t capacity, const ClickStatus& s) {
    const int written = std::snprintf(
        out, capacity,
        "{\"phase\":\"%s\",\"cue\":\"%s\",\"step\":%u,\"steps\":%u,\"validation\":%s,"
        "\"retries\":%u,\"cueMs\":%lu,\"progress\":%.3f,\"reason\":\"%s\",\"activityMs\":%lu,"
        "\"ready\":%s,\"enabled\":%s,\"frame\":\"%s\",\"state\":\"%s\",\"suppressing\":%s,"
        "\"accepted\":%lu,\"rejected\":%lu,\"candidates\":%lu,\"clicks\":%lu,"
        "\"lastReject\":\"%s\",\"blocked\":\"%s\"}",
        name(s.train.phase), name(s.train.cue), s.train.step, s.train.steps,
        s.train.validation ? "true" : "false", s.train.retries,
        static_cast<unsigned long>(s.train.cueMs), s.train.progress, s.train.reason,
        static_cast<unsigned long>(s.train.activityMs), s.ready ? "true" : "false",
        s.enabled ? "true" : "false", s.configuredFrame ? "CONFIGURED" : "FALLBACK", s.state,
        s.suppressing ? "true" : "false", static_cast<unsigned long>(s.accepted),
        static_cast<unsigned long>(s.rejected), static_cast<unsigned long>(s.candidates),
        static_cast<unsigned long>(s.clicks), s.lastReject, s.blocked);
    return written > 0 && size_t(written) < capacity ? size_t(written) : 0;
}
} // namespace nodx
