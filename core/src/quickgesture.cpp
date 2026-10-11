#include "nodx/quickgesture.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace nodx {
namespace {
bool between(float v, float lo, float hi) {
    return std::isfinite(v) && v >= lo && v <= hi;
}
Vec3 minus(const Vec3& a, const Vec3& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
Vec3 scaled(const Vec3& a, float k) {
    return {a[0] * k, a[1] * k, a[2] * k};
}
} // namespace

bool QuickSettings::valid() const {
    return between(sensitivity, .5f, 2.f) && between(returnTolerance, .15f, .6f) &&
           between(directionToleranceDeg, 10.f, 60.f);
}
bool QuickProfile::valid() const {
    return between(norm(direction), .99f, 1.01f) && between(sigma, .01f, 10.f) &&
           between(practiceDeg, start::quickPracticeMinDeg, 120.f) &&
           between(practicePeak, 10.f, 240.f) && between(norm(bias), 0.f, 30.f);
}
QuickThresholds deriveThresholds(const QuickProfile& p, const QuickSettings& s) {
    QuickThresholds t{};
    const float base = std::max({start::quickEnterFloor, 8.f * p.sigma, .25f * p.practicePeak});
    t.enterRate = std::clamp(base / s.sensitivity, 10.f, start::quickEnterCap);
    t.exitRate = std::max(start::quickExitFloor, 4.f * p.sigma);
    t.returnRate = std::max(t.exitRate + 2.f, .25f * t.enterRate);
    t.minExcursionDeg = std::clamp(.5f * p.practiceDeg / s.sensitivity, 5.f, 40.f);
    return t;
}
const char* name(QuickReject r) {
    switch (r) {
    case QuickReject::None:
        return "NONE";
    case QuickReject::TooSmall:
        return "TOO_SMALL";
    case QuickReject::CrossAxis:
        return "CROSS_AXIS";
    case QuickReject::NoReturn:
        return "NO_RETURN";
    case QuickReject::Residual:
        return "NOT_BACK_TO_START";
    case QuickReject::Timeout:
        return "TIMEOUT";
    case QuickReject::InvalidSample:
        return "INVALID_SAMPLE";
    }
    return "NONE";
}

// ---------------------------------------------------------------- recognizer
void QuickRecognizer::configure(const QuickProfile& profile, const QuickSettings& settings) {
    profile_ = profile;
    settings_ = settings;
    th_ = deriveThresholds(profile, settings);
    const float tolerance = std::clamp(settings.directionToleranceDeg, 10.f, 60.f) * 3.14159265f / 180.f;
    cosTolerance_ = std::cos(tolerance);
    crossRatio_ = std::tan(tolerance);
    accepted = rejected = candidates = suppressedMs = 0;
    lastReject = QuickReject::None;
    lastExcursionDeg = lastResidualDeg = lastDurationMs = lastCrossDeg = 0;
    reset(0);
}
void QuickRecognizer::reset(uint32_t now) {
    state_ = State::Neutral; // a fresh neutral period is always required
    calmSince_ = 0;
    lastMs_ = now;
    theta_ = {};
    maxAlong_ = maxCross_ = 0;
    retStarted_ = false;
}
void QuickRecognizer::finishCandidate(Event& event, bool accept, QuickReject reason, uint32_t now) {
    lastDurationMs = float(uint32_t(now - onsetMs_));
    lastExcursionDeg = maxAlong_;
    lastResidualDeg = norm(theta_);
    lastCrossDeg = maxCross_;
    if (accept) {
        ++accepted;
        event.accepted = true;
        lastReject = QuickReject::None;
        nextAllowedMs_ = now + start::quickMinIntervalMs; // minimum interval after a click
    } else {
        ++rejected;
        event.rejected = true;
        lastReject = reason;
    }
    // the frozen movement is discarded by the caller; here the candidate is simply closed
    theta_ = {};
    maxAlong_ = maxCross_ = 0;
    retStarted_ = false;
    state_ = State::Neutral;
    calmSince_ = 0;
}
QuickRecognizer::Event QuickRecognizer::update(const Vec3& frame, uint32_t now) {
    Event event;
    const Vec3 r = minus(frame, profile_.bias);
    const float rate = norm(r);
    const float along = dot(r, profile_.direction);
    const uint32_t gap = uint32_t(now - lastMs_);
    lastMs_ = now;
    const float dt = float(gap) / 1000.f;
    const bool candidateOpen = state_ == State::Outward || state_ == State::Return ||
                               state_ == State::Settling;
    if (!std::isfinite(rate) || (candidateOpen && (gap == 0 || gap > 50))) {
        if (candidateOpen) {
            finishCandidate(event, false, QuickReject::InvalidSample, now);
        }
        return event;
    }
    if (candidateOpen) {
        suppressedMs += gap;
    }
    switch (state_) {
    case State::Neutral:
        if (rate < th_.exitRate) {
            if (calmSince_ == 0) {
                calmSince_ = now;
            }
            if (uint32_t(now - calmSince_) >= start::quickNeutralMs &&
                int32_t(now - nextAllowedMs_) >= 0) {
                state_ = State::Armed;
            }
        } else {
            calmSince_ = 0;
        }
        break;
    case State::Armed:
        // Only the designated direction, with the right sign, within the angular tolerance. The
        // opposite direction and perpendicular movements never open a candidate.
        if (along > th_.enterRate && along >= cosTolerance_ * rate) {
            state_ = State::Outward; // candidate detected: pointer output is frozen from here
            onsetMs_ = now;
            theta_ = {};
            maxAlong_ = maxCross_ = 0;
            retStarted_ = false;
            calmSince_ = 0;
            ++candidates;
            // the detecting sample itself belongs to the stroke
            theta_ = scaled(r, dt);
            maxAlong_ = std::max(0.f, dot(theta_, profile_.direction));
        } else if (rate >= th_.exitRate && !(along > 0)) {
            // ordinary movement away from the gesture direction: not armed for a moment
            state_ = State::Neutral;
            calmSince_ = 0;
        }
        break;
    case State::Outward:
    case State::Return:
    case State::Settling: {
        for (unsigned i = 0; i < 3; ++i) {
            theta_[i] += r[i] * dt; // real sample intervals
        }
        const float a = dot(theta_, profile_.direction);
        const float cross = norm(minus(theta_, scaled(profile_.direction, a)));
        maxAlong_ = std::max(maxAlong_, a);
        maxCross_ = std::max(maxCross_, cross);
        if (state_ == State::Outward && maxAlong_ >= th_.minExcursionDeg &&
            along < -th_.returnRate) {
            state_ = State::Return; // an opposite stroke after a clear outward excursion
            retStarted_ = true;
        }
        const uint32_t elapsed = uint32_t(now - onsetMs_);
        if (state_ != State::Settling && calmSince_ == 0 && elapsed > start::quickMaxMs) {
            finishCandidate(event, false,
                            retStarted_ ? QuickReject::Timeout : QuickReject::NoReturn, now);
            break;
        }
        if (state_ == State::Return || state_ == State::Settling) {
            if (rate < th_.exitRate) {
                if (calmSince_ == 0) {
                    calmSince_ = now;
                    if (uint32_t(calmSince_ - onsetMs_) > start::quickMaxMs) {
                        finishCandidate(event, false, QuickReject::Timeout, now);
                        break;
                    }
                }
                state_ = State::Settling;
                if (uint32_t(now - calmSince_) >= start::quickSettleMs) {
                    const float residual = norm(theta_);
                    if (maxAlong_ < th_.minExcursionDeg) {
                        finishCandidate(event, false, QuickReject::TooSmall, now);
                    } else if (maxCross_ > crossRatio_ * maxAlong_) {
                        finishCandidate(event, false, QuickReject::CrossAxis, now);
                    } else if (residual > settings_.returnTolerance * maxAlong_) {
                        finishCandidate(event, false, QuickReject::Residual, now);
                    } else {
                        finishCandidate(event, true, QuickReject::None, now);
                    }
                }
            } else {
                calmSince_ = 0;
                state_ = State::Return;
            }
        }
        // Excessive deviation cancels the candidate at once (outward or return stroke): no click,
        // and the frozen movement is discarded, never replayed.
        if (state_ != State::Neutral && state_ != State::Settling &&
            maxCross_ > crossRatio_ * std::max(maxAlong_, th_.minExcursionDeg) &&
            maxCross_ > .5f * th_.minExcursionDeg) {
            finishCandidate(event, false, QuickReject::CrossAxis, now);
        }
        break;
    }
    }
    return event;
}

// ---------------------------------------------------------------- practice
const char* name(QuickPhase p) {
    switch (p) {
    case QuickPhase::Idle:
        return "IDLE";
    case QuickPhase::Rest:
        return "REST";
    case QuickPhase::Tilt:
        return "TILT";
    case QuickPhase::Pointing:
        return "POINTING";
    case QuickPhase::Preview:
        return "PREVIEW";
    case QuickPhase::Done:
        return "DONE";
    case QuickPhase::Failed:
        return "FAILED";
    }
    return "FAILED";
}
const char* name(QuickCue c) {
    switch (c) {
    case QuickCue::None:
        return "NONE";
    case QuickCue::HoldStill:
        return "HOLD_STILL";
    case QuickCue::Countdown:
        return "COUNTDOWN";
    case QuickCue::Go:
        return "GO";
    case QuickCue::Recording:
        return "RECORDING";
    case QuickCue::ReturnToCentre:
        return "RETURN_TO_CENTRE";
    case QuickCue::PointNormally:
        return "POINT_NORMALLY";
    case QuickCue::Preview:
        return "PREVIEW";
    }
    return "NONE";
}
void QuickPractice::begin(uint32_t now, const QuickSettings& settings) {
    *this = QuickPractice{};
    settings_ = settings;
    phase_ = QuickPhase::Rest;
    phaseStart_ = now;
    lastMs_ = now;
    reason_ = "hold the assembly completely still";
}
void QuickPractice::cancel() {
    if (active()) {
        phase_ = QuickPhase::Failed;
        reason_ = "cancelled";
    }
}
void QuickPractice::fail(const char* reason) {
    phase_ = QuickPhase::Failed;
    reason_ = reason;
}
bool QuickPractice::accept() {
    if (phase_ != QuickPhase::Preview) {
        return false;
    }
    phase_ = QuickPhase::Done;
    reason_ = "accepted";
    return true;
}
void QuickPractice::retry(uint32_t now) {
    if (phase_ == QuickPhase::Preview) {
        phase_ = QuickPhase::Tilt;
        beginTilt(now);
        reason_ = "try another movement in your chosen direction, then back to the start";
    }
}
void QuickPractice::beginTilt(uint32_t now) {
    sub_ = Sub::Countdown;
    subStart_ = now;
    onsetCount_ = 0;
    calmSince_ = 0;
    rates_.clear();
    dts_.clear();
}
void QuickPractice::tick(const Vec3& frame, uint32_t now) {
    if (!active()) {
        return;
    }
    if (phase_ == QuickPhase::Rest) {
        tickRest(frame, now);
    } else if (phase_ == QuickPhase::Tilt) {
        tickTilt(frame, now);
    } else if (phase_ == QuickPhase::Pointing) {
        tickPointing(frame, now);
    } else if (phase_ == QuickPhase::Preview) {
        if (uint32_t(now - phaseStart_) > start::quickPreviewTimeoutMs) {
            fail("the preview timed out; start the practice again");
        } else {
            preview_.update(frame, now);
        }
    }
    lastMs_ = now;
}
void QuickPractice::tickRest(const Vec3& g, uint32_t now) {
    if (uint32_t(now - phaseStart_) > start::quickRestWindowMs) {
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
    if (uint32_t(now - runStart_) < start::quickRestMs || runCount_ < 100) {
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
    phase_ = QuickPhase::Tilt;
    reason_ = "stillness measured";
    beginTilt(now);
}
void QuickPractice::tickTilt(const Vec3& frame, uint32_t now) {
    const Vec3 r = minus(frame, bias_);
    const float rate = norm(r);
    const float dt = std::min(.05f, float(uint32_t(now - lastMs_)) / 1000.f);
    const float enter = std::max(start::quickEnterFloor, 8.f * sigma_);
    const float exit = std::max(start::quickExitFloor, 4.f * sigma_);
    auto redo = [&](const char* why) {
        reason_ = why;
        beginTilt(now); // retry the same step with a fresh countdown
    };
    switch (sub_) {
    case Sub::Countdown:
        if (uint32_t(now - subStart_) >= start::mapCountdownMs) {
            sub_ = Sub::WaitOnset;
            subStart_ = now;
            onsetCount_ = 0;
            rates_.clear();
            dts_.clear();
        }
        break;
    case Sub::WaitOnset:
        rates_.push_back(r);
        dts_.push_back(dt);
        if (rates_.size() > 3) {
            rates_.erase(rates_.begin());
            dts_.erase(dts_.begin());
        }
        onsetCount_ = rate > enter ? onsetCount_ + 1 : 0;
        if (onsetCount_ >= 3) {
            sub_ = Sub::Moving;
            moveStart_ = now;
            calmSince_ = 0;
        } else if (uint32_t(now - subStart_) >= start::mapOnsetWindowMs) {
            redo("no movement detected after the cue: try again");
        }
        break;
    case Sub::Moving:
        rates_.push_back(r);
        dts_.push_back(dt);
        if (rate < exit) {
            if (calmSince_ == 0) {
                calmSince_ = now;
            }
            if (uint32_t(now - calmSince_) >= start::quickSettleMs) {
                if (!analyze() && phase_ == QuickPhase::Tilt) {
                    beginTilt(now);
                }
                return;
            }
        } else {
            calmSince_ = 0;
        }
        if (uint32_t(now - moveStart_) > start::quickPracticeMaxMs) {
            redo("that took too long: make one quick tilt and return, about a second");
        }
        break;
    }
}
bool QuickPractice::analyze() {
    // theta(t): bias-corrected angle integrated over the real sample intervals
    Vec3 theta{};
    float best = 0, peakRate = 0;
    Vec3 bestTheta{};
    std::vector<Vec3> path;
    for (size_t i = 0; i < rates_.size(); ++i) {
        for (unsigned a = 0; a < 3; ++a) {
            theta[a] += rates_[i][a] * dts_[i];
        }
        path.push_back(theta);
        if (norm(theta) > best) {
            best = norm(theta);
            bestTheta = theta;
        }
        peakRate = std::max(peakRate, norm(rates_[i]));
    }
    auto again = [&](const char* why) {
        reason_ = why;
        return false;
    };
    if (best < start::quickPracticeMinDeg) {
        return again("that tilt was too small: tilt further, then come back");
    }
    const Vec3 d = scaled(bestTheta, 1.f / best);
    const float duration = float(uint32_t(calmSince_ - moveStart_));
    if (duration > float(start::quickMaxMs)) {
        return again("that was too slow: make it one quick tilt and return, under a second");
    }
    float cross = 0;
    for (const Vec3& p : path) {
        cross = std::max(cross, norm(minus(p, scaled(d, dot(p, d)))));
    }
    const float residual = norm(theta);
    crossDeg_ = cross;
    residualDeg_ = residual;
    if (cross > start::quickPracticeCrossRatio * best) {
        return again("that movement wandered off its line: make one clean movement in one direction and return");
    }
    if (residual > start::quickPracticeResidualRatio * best) {
        return again("you did not come back to the start: tilt, then return fully to centre");
    }
    profile_ = QuickProfile{};
    profile_.direction = d;
    profile_.bias = bias_;
    profile_.sigma = sigma_;
    profile_.practiceDeg = best;
    profile_.practicePeak = std::clamp(peakRate, 10.f, 240.f);
    if (!profile_.valid()) {
        return again("that practice could not be used: try another movement in your chosen direction");
    }
    if (!pointCaptured_) {
        // Next: a short sample of the user's ORDINARY pointing, measured, not assumed.
        phase_ = QuickPhase::Pointing;
        pointStart_ = lastMs_;
        activityMs_ = 0;
        activityTarget_ = start::quickPointingActivityMs;
        pointFrames_.clear();
        pointTimes_.clear();
        covCount_ = 0;
        for (double& c : cov_) {
            c = 0;
        }
        reason_ = "now move the pointer around in different directions, as you normally would";
        return true;
    }
    return evaluatePointing(lastMs_);
}
namespace {
// Eigenvalues of a symmetric 3x3 matrix (Jacobi rotations), sorted descending; also the Rayleigh
// quotient helper below uses the matrix itself.
void eigenvalues(const double c[6], double out[3]) {
    double a[3][3] = {{c[0], c[1], c[2]}, {c[1], c[3], c[4]}, {c[2], c[4], c[5]}};
    for (int sweep = 0; sweep < 20; ++sweep) {
        for (int p = 0; p < 2; ++p) {
            for (int q = p + 1; q < 3; ++q) {
                if (std::abs(a[p][q]) < 1e-12) {
                    continue;
                }
                const double theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
                const double t = (theta >= 0 ? 1.0 : -1.0) / (std::abs(theta) + std::sqrt(theta * theta + 1.0));
                const double cs = 1.0 / std::sqrt(t * t + 1.0), sn = t * cs;
                for (int k = 0; k < 3; ++k) {
                    const double akp = a[k][p], akq = a[k][q];
                    a[k][p] = cs * akp - sn * akq;
                    a[k][q] = sn * akp + cs * akq;
                }
                for (int k = 0; k < 3; ++k) {
                    const double apk = a[p][k], aqk = a[q][k];
                    a[p][k] = cs * apk - sn * aqk;
                    a[q][k] = sn * apk + cs * aqk;
                }
            }
        }
    }
    out[0] = a[0][0];
    out[1] = a[1][1];
    out[2] = a[2][2];
    std::sort(out, out + 3, [](double x, double y) { return x > y; });
}
} // namespace
void QuickPractice::tickPointing(const Vec3& frame, uint32_t now) {
    if (uint32_t(now - pointStart_) > start::quickPointingWindowMs) {
        fail("not enough varied ordinary pointing to check the gesture against: start the practice again");
        return;
    }
    const Vec3 r = minus(frame, bias_);
    const uint32_t gap = std::min<uint32_t>(50, uint32_t(now - lastMs_));
    if (pointFrames_.size() < 2000) {
        pointFrames_.push_back(frame);
        pointTimes_.push_back(now);
    }
    if (norm(r) > start::quickPointingActiveRate) {
        activityMs_ += gap;
        ++covCount_;
        cov_[0] += double(r[0]) * r[0];
        cov_[1] += double(r[0]) * r[1];
        cov_[2] += double(r[0]) * r[2];
        cov_[3] += double(r[1]) * r[1];
        cov_[4] += double(r[1]) * r[2];
        cov_[5] += double(r[2]) * r[2];
    }
    if (activityMs_ >= activityTarget_) {
        if (evaluatePointing(now) && phase_ == QuickPhase::Pointing) {
            phase_ = QuickPhase::Preview;
        }
    }
}
bool QuickPractice::evaluatePointing(uint32_t now) {
    pointCaptured_ = true;
    auto reject = [&](const char* why) {
        // keep the measured pointing; ask for another tilt direction
        reason_ = why;
        phase_ = QuickPhase::Tilt;
        beginTilt(now);
        return false;
    };
    if (covCount_ < 20) {
        pointCaptured_ = false;
        activityTarget_ += 1500;
        reason_ = "keep moving the pointer around, as you normally would";
        phase_ = QuickPhase::Pointing;
        return false;
    }
    double c[6];
    double trace = 0;
    for (int i = 0; i < 6; ++i) {
        c[i] = cov_[i] / double(covCount_);
    }
    trace = c[0] + c[3] + c[5];
    double ev[3];
    eigenvalues(c, ev);
    if (!(trace > 1e-6) || ev[1] < start::quickPointingCoverageMin * ev[0]) {
        // pointing in only one direction proves nothing about the others: ask for more variety
        pointCaptured_ = false;
        activityTarget_ += 2000;
        reason_ = "move the pointer in more different directions (not only one way)";
        phase_ = QuickPhase::Pointing;
        return false;
    }
    const Vec3& d = profile_.direction;
    const double along = d[0] * (c[0] * d[0] + c[1] * d[1] + c[2] * d[2]) +
                         d[1] * (c[1] * d[0] + c[3] * d[1] + c[4] * d[2]) +
                         d[2] * (c[2] * d[0] + c[4] * d[1] + c[5] * d[2]);
    pointingShare_ = float(std::clamp(along / trace, 0.0, 1.0));
    if (pointingShare_ > start::quickPointingShareMax) {
        std::snprintf(reasonBuf_, sizeof reasonBuf_,
                      "that direction is %d%% of your ordinary pointing: choose a direction you do not point in",
                      int(std::lround(pointingShare_ * 100.f)));
        return reject(reasonBuf_);
    }
    // Replay the measured pointing through the recognizer this practice would create.
    QuickRecognizer replay;
    replay.configure(profile_, settings_);
    for (size_t i = 0; i < pointFrames_.size(); ++i) {
        replay.update(pointFrames_[i], pointTimes_[i]);
    }
    const float duration = pointTimes_.size() > 1
                               ? float(pointTimes_.back() - pointTimes_.front())
                               : 1.f;
    if (replay.accepted > 0 || float(replay.suppressedMs) > start::quickPointingSuppressedMax * duration) {
        std::snprintf(reasonBuf_, sizeof reasonBuf_,
                      "your ordinary pointing triggered this gesture (%u clicks, pointer paused %d%% of the time): choose another direction",
                      unsigned(replay.accepted),
                      int(std::lround(100.f * float(replay.suppressedMs) / duration)));
        return reject(reasonBuf_);
    }
    preview_.configure(profile_, settings_);
    phase_ = QuickPhase::Preview;
    phaseStart_ = now;
    reason_ = "checked against your ordinary pointing: try it in the preview, then accept or retry";
    return true;
}
QuickStatus QuickPractice::status(uint32_t now) const {
    QuickStatus st;
    st.phase = phase_;
    st.reason = reason_;
    st.sensitivity = settings_.sensitivity;
    st.returnTolerance = settings_.returnTolerance;
    st.directionToleranceDeg = settings_.directionToleranceDeg;
    auto left = [&](uint32_t total) {
        const uint32_t spent = uint32_t(now - subStart_);
        return spent >= total ? 0u : total - spent;
    };
    if (phase_ == QuickPhase::Rest) {
        st.cue = QuickCue::HoldStill;
        st.progress = .1f;
    } else if (phase_ == QuickPhase::Tilt) {
        switch (sub_) {
        case Sub::Countdown:
            st.cue = QuickCue::Countdown;
            st.cueMs = left(start::mapCountdownMs);
            break;
        case Sub::WaitOnset:
            st.cue = QuickCue::Go;
            st.cueMs = left(start::mapOnsetWindowMs);
            break;
        case Sub::Moving:
            st.cue = QuickCue::Recording;
            break;
        }
        st.progress = .4f;
    } else if (phase_ == QuickPhase::Pointing) {
        st.cue = QuickCue::PointNormally;
        st.pointingMs = activityMs_;
        st.progress = .5f + .4f * std::min(1.f, float(activityMs_) / float(std::max<uint32_t>(1, activityTarget_)));
    } else if (phase_ == QuickPhase::Preview || phase_ == QuickPhase::Done) {
        st.cue = QuickCue::Preview;
        st.progress = 1.f;
        st.practiceDeg = profile_.practiceDeg;
        st.direction = profile_.direction;
        st.designated = true;
        st.practiceResidualDeg = residualDeg_;
        st.practiceCrossDeg = crossDeg_;
        st.pointingShare = pointingShare_;
        st.pointingMs = activityMs_;
        st.accepted = preview_.accepted;
        st.rejected = preview_.rejected;
        st.candidates = preview_.candidates;
        st.lastExcursionDeg = preview_.lastExcursionDeg;
        st.lastResidualDeg = preview_.lastResidualDeg;
        st.lastDurationMs = preview_.lastDurationMs;
        st.lastReject = name(preview_.lastReject);
        st.suppressedMs = preview_.suppressedMs;
        const auto state = preview_.state();
        st.state = state == QuickRecognizer::State::Armed        ? "READY"
                   : state == QuickRecognizer::State::Outward    ? "OUTWARD"
                   : state == QuickRecognizer::State::Return     ? "RETURN"
                   : state == QuickRecognizer::State::Settling   ? "SETTLING"
                                                                  : "NEUTRAL";
    }
    return st;
}

size_t quickJson(char* out, size_t capacity, const QuickStatus& s) {
    const int written = std::snprintf(
        out, capacity,
        "{\"phase\":\"%s\",\"cue\":\"%s\",\"cueMs\":%lu,\"progress\":%.3f,\"reason\":\"%s\","
        "\"practice\":{\"excursion\":%.1f,\"residual\":%.1f,\"cross\":%.1f,\"pointingShare\":%.2f,\"pointingMs\":%lu},"
        "\"ready\":%s,\"enabled\":%s,\"frame\":\"%s\",\"state\":\"%s\",\"suppressing\":%s,"
        "\"accepted\":%lu,\"rejected\":%lu,\"candidates\":%lu,\"clicks\":%lu,"
        "\"suppressedMs\":%lu,\"lastReject\":\"%s\",\"last\":{\"excursion\":%.1f,\"residual\":%.1f,"
        "\"durationMs\":%.0f},\"sensitivity\":%.2f,\"returnTolerance\":%.2f,"
        "\"directionTolerance\":%.0f,\"designated\":%s,\"direction\":[%.3f,%.3f,%.3f],\"blocked\":\"%s\"}",
        name(s.phase), name(s.cue), static_cast<unsigned long>(s.cueMs), s.progress, s.reason,
        s.practiceDeg, s.practiceResidualDeg, s.practiceCrossDeg, s.pointingShare,
        static_cast<unsigned long>(s.pointingMs),
        s.ready ? "true" : "false", s.enabled ? "true" : "false",
        s.configuredFrame ? "CONFIGURED" : "FALLBACK", s.state, s.suppressing ? "true" : "false",
        static_cast<unsigned long>(s.accepted), static_cast<unsigned long>(s.rejected),
        static_cast<unsigned long>(s.candidates), static_cast<unsigned long>(s.clicks),
        static_cast<unsigned long>(s.suppressedMs), s.lastReject, s.lastExcursionDeg,
        s.lastResidualDeg, s.lastDurationMs, s.sensitivity, s.returnTolerance,
        s.directionToleranceDeg, s.designated ? "true" : "false", s.direction[0], s.direction[1],
        s.direction[2], s.blocked);
    return written > 0 && size_t(written) < capacity ? size_t(written) : 0;
}
} // namespace nodx
