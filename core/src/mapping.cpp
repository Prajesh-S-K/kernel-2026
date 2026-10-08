#include "nodx/mapping.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace nodx {
namespace {
bool between(float v, float low, float high) {
    return std::isfinite(v) && v >= low && v <= high;
}
void put(std::vector<uint8_t>& b, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) {
        b.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
}
uint32_t get(const std::vector<uint8_t>& b, size_t& at) {
    uint32_t v = 0;
    for (unsigned i = 0; i < 4; ++i) {
        v |= uint32_t(b.at(at++)) << (8 * i);
    }
    return v;
}
void putFloat(std::vector<uint8_t>& b, float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    put(b, bits);
}
float getFloat(const std::vector<uint8_t>& b, size_t& at) {
    uint32_t bits = get(b, at);
    float v;
    std::memcpy(&v, &bits, 4);
    return v;
}
Vec3 sub(const Vec3& a, const Vec3& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
Vec3 scale(const Vec3& a, float k) {
    return {a[0] * k, a[1] * k, a[2] * k};
}
Vec3 unit(const Vec3& a) {
    const float n = norm(a);
    return n > 1e-6f ? scale(a, 1.f / n) : Vec3{0, 0, 0};
}
float cosine(const Vec3& a, const Vec3& b) {
    const float d = norm(a) * norm(b);
    return d > 1e-6f ? dot(a, b) / d : 0.f;
}
Vec3 meanOf(const std::vector<Vec3>& items) {
    Vec3 sum{};
    for (const Vec3& v : items) {
        for (unsigned i = 0; i < 3; ++i) {
            sum[i] += v[i];
        }
    }
    return items.empty() ? sum : scale(sum, 1.f / float(items.size()));
}
float alphaFor(float cutoffHz, float dt) {
    const float tau = 1.f / (2.f * 3.14159265f * cutoffHz);
    return 1.f / (1.f + tau / dt);
}
} // namespace

float dot(const Vec3& a, const Vec3& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
float norm(const Vec3& a) {
    return std::sqrt(dot(a, a));
}

// ---------------------------------------------------------------- One Euro
bool OneEuroParams::valid() const {
    return between(minCutoffHz, .05f, 20.f) && between(beta, 0.f, 1.f) &&
           between(derivativeCutoffHz, .1f, 20.f);
}
void OneEuroFilter::reset() {
    primed_ = false;
    x_ = dx_ = 0;
}
float OneEuroFilter::filter(float x, float dt, const OneEuroParams& p) {
    if (!std::isfinite(x)) {
        reset();
        return 0.f;
    }
    if (!primed_ || !(dt > 0.f) || dt > .05f) {
        primed_ = true; // an unusable interval restarts the filter instead of guessing
        x_ = x;
        dx_ = 0;
        prevRaw_ = x;
        return x;
    }
    const float rawDx = (x - prevRaw_) / dt;
    prevRaw_ = x;
    dx_ += alphaFor(p.derivativeCutoffHz, dt) * (rawDx - dx_);
    const float cutoff = p.minCutoffHz + p.beta * std::abs(dx_);
    x_ += alphaFor(cutoff, dt) * (x - x_);
    return x_;
}

// ---------------------------------------------------------------- learned record
bool LearnedControl::valid() const {
    for (const Vec3* v : {&horizontal, &vertical, &gravity}) {
        if (!between(norm(*v), .99f, 1.01f)) {
            return false;
        }
    }
    if (!between(dot(horizontal, vertical), -.1f, .1f)) {
        return false; // the rows are orthogonal by construction
    }
    for (unsigned i = 0; i < 3; ++i) {
        if (!between(bias[i], -30.f, 30.f) || !between(noise[i], .01f, 10.f)) {
            return false;
        }
    }
    for (float g : gain) {
        if (!between(g, 1.f, 120.f)) {
            return false;
        }
    }
    for (unsigned i = 0; i < 2; ++i) {
        if (!between(deadzoneEnter[i], .1f, 15.f) || !between(deadzoneExit[i], .05f, deadzoneEnter[i])) {
            return false;
        }
    }
    return filter.valid();
}
Vec3 LearnedControl::rollRow() const {
    return {horizontal[1] * vertical[2] - horizontal[2] * vertical[1],
            horizontal[2] * vertical[0] - horizontal[0] * vertical[2],
            horizontal[0] * vertical[1] - horizontal[1] * vertical[0]};
}
std::vector<uint8_t> encode(const LearnedControl& c, uint32_t generation) {
    std::vector<uint8_t> b;
    put(b, LearnedControl::magic);
    put(b, LearnedControl::schema);
    put(b, generation);
    for (const Vec3* v : {&c.horizontal, &c.vertical, &c.bias, &c.noise, &c.gravity}) {
        for (float x : *v) {
            putFloat(b, x);
        }
    }
    for (float g : c.gain) {
        putFloat(b, g);
    }
    for (float f : {c.filter.minCutoffHz, c.filter.beta, c.filter.derivativeCutoffHz}) {
        putFloat(b, f);
    }
    for (float d : {c.deadzoneEnter[0], c.deadzoneEnter[1], c.deadzoneExit[0], c.deadzoneExit[1]}) {
        putFloat(b, d);
    }
    put(b, checksum(b));
    return b;
}
bool decode(const std::vector<uint8_t>& b, LearnedControl& out, uint32_t& generation) {
    if (b.size() != LearnedControl::recordBytes) {
        return false;
    }
    size_t tail = b.size() - 4;
    if (checksum(std::vector<uint8_t>(b.begin(), b.begin() + tail)) != get(b, tail)) {
        return false;
    }
    size_t at = 0;
    if (get(b, at) != LearnedControl::magic || get(b, at) != LearnedControl::schema) {
        return false;
    }
    const uint32_t g = get(b, at);
    LearnedControl c;
    for (Vec3* v : {&c.horizontal, &c.vertical, &c.bias, &c.noise, &c.gravity}) {
        for (float& x : *v) {
            x = getFloat(b, at);
        }
    }
    for (float& x : c.gain) {
        x = getFloat(b, at);
    }
    c.filter.minCutoffHz = getFloat(b, at);
    c.filter.beta = getFloat(b, at);
    c.filter.derivativeCutoffHz = getFloat(b, at);
    c.deadzoneEnter[0] = getFloat(b, at);
    c.deadzoneEnter[1] = getFloat(b, at);
    c.deadzoneExit[0] = getFloat(b, at);
    c.deadzoneExit[1] = getFloat(b, at);
    if (!c.valid()) {
        return false;
    }
    out = c;
    generation = g;
    return true;
}
const char* name(ControlRecordState state) {
    switch (state) {
    case ControlRecordState::Missing:
        return "MISSING";
    case ControlRecordState::Valid:
        return "VALID";
    case ControlRecordState::Corrupt:
        return "CORRUPT";
    }
    return "CORRUPT";
}
bool ControlRepository::load(LearnedControl& c) {
    LearnedControl a, b;
    uint32_t ga = 0, gb = 0;
    const auto s0 = storage_.read(0), s1 = storage_.read(1);
    const bool va = decode(s0, a, ga), vb = decode(s1, b, gb);
    if (!va && !vb) {
        state_ = s0.empty() && s1.empty() ? ControlRecordState::Missing : ControlRecordState::Corrupt;
        return false;
    }
    state_ = ControlRecordState::Valid;
    c = (!va || (vb && gb > ga)) ? b : a;
    return true;
}
bool ControlRepository::save(const LearnedControl& c) {
    if (!c.valid()) {
        return false;
    }
    LearnedControl a, b;
    uint32_t ga = 0, gb = 0;
    const bool va = decode(storage_.read(0), a, ga), vb = decode(storage_.read(1), b, gb);
    const unsigned slot = (!va || (vb && ga <= gb)) ? 0 : 1; // never the newest valid slot
    const uint32_t g = std::max(va ? ga : 0, vb ? gb : 0);
    if (g == UINT32_MAX) {
        return false;
    }
    const auto bytes = encode(c, g + 1);
    if (!storage_.write(slot, bytes) || storage_.read(slot) != bytes) {
        return false;
    }
    state_ = ControlRecordState::Valid;
    return true;
}

// ---------------------------------------------------------------- processor
void ControlProcessor::reset() {
    h_.reset();
    v_.reset();
    activeH_ = activeV_ = primed_ = false;
    rates_ = {};
}
Motion ControlProcessor::process(const MotionSample& s, const LearnedControl& c) {
    const float dt = primed_ ? float(uint32_t(s.timestampMs - lastMs_)) / 1000.f : 0.f;
    primed_ = true;
    lastMs_ = s.timestampMs;
    const Vec3 g = sub(Vec3{s.gyro[0], s.gyro[1], s.gyro[2]}, c.bias);
    rates_ = {dot(c.horizontal, g), dot(c.vertical, g), dot(c.rollRow(), g)};
    const float fh = h_.filter(rates_[0], dt, c.filter);
    const float fv = v_.filter(rates_[1], dt, c.filter);
    auto zone = [](float value, float enter, float exit, bool& active) {
        if (!active && std::abs(value) > enter) {
            active = true;
        } else if (active && std::abs(value) < exit) {
            active = false;
        }
        return active ? std::copysign(std::max(0.f, std::abs(value) - exit), value) : 0.f;
    };
    const float x = zone(fh, c.deadzoneEnter[0], c.deadzoneExit[0], activeH_);
    const float y = zone(fv, c.deadzoneEnter[1], c.deadzoneExit[1], activeV_);
    return {x, y, 0.f, 1.f / (1.f + std::hypot(fh, fv))};
}

// ---------------------------------------------------------------- teacher
const char* name(MapPhase p) {
    switch (p) {
    case MapPhase::Idle:
        return "IDLE";
    case MapPhase::Still:
        return "STILL";
    case MapPhase::Example:
        return "EXAMPLE";
    case MapPhase::Analyze:
        return "ANALYZE";
    case MapPhase::Preview:
        return "PREVIEW";
    case MapPhase::Done:
        return "DONE";
    case MapPhase::Failed:
        return "FAILED";
    }
    return "FAILED";
}
const char* name(MapCue c) {
    switch (c) {
    case MapCue::None:
        return "NONE";
    case MapCue::HoldStill:
        return "HOLD_STILL";
    case MapCue::Countdown:
        return "COUNTDOWN";
    case MapCue::Go:
        return "GO";
    case MapCue::Recording:
        return "RECORDING";
    case MapCue::ReturnToCentre:
        return "RETURN_TO_CENTRE";
    case MapCue::Preview:
        return "PREVIEW";
    }
    return "NONE";
}
const char* directionName(unsigned d) {
    static const char* names[] = {"RIGHT", "LEFT", "UP", "DOWN"};
    return d < 4 ? names[d] : "NONE";
}

namespace {
constexpr unsigned trainSteps = 4 * start::mapExamples;
constexpr unsigned allSteps = trainSteps + 4;
unsigned directionOf(unsigned step) {
    return step < trainSteps ? step / start::mapExamples : step - trainSteps;
}
} // namespace

float MappingTeacher::onsetRate() const {
    return std::max(start::mapOnsetFloor, 8.f * std::max({noise_[0], noise_[1], noise_[2]}));
}
float MappingTeacher::exitRate() const {
    return std::max(start::mapExitFloor, 4.f * std::max({noise_[0], noise_[1], noise_[2]}));
}
void MappingTeacher::begin(uint32_t now) {
    *this = MappingTeacher{};
    phase_ = MapPhase::Still;
    phaseStart_ = now;
    lastMs_ = now;
    reason_ = "hold the assembly completely still";
}
void MappingTeacher::cancel() {
    if (active()) {
        phase_ = MapPhase::Failed;
        reason_ = "cancelled";
    }
}
void MappingTeacher::fail(const char* reason) {
    phase_ = MapPhase::Failed;
    reason_ = reason;
}
bool MappingTeacher::accept() {
    if (phase_ != MapPhase::Preview) {
        return false;
    }
    phase_ = MapPhase::Done;
    reason_ = "accepted";
    return true;
}
void MappingTeacher::tick(const MotionSample& s, uint32_t now) {
    if (!active()) {
        return;
    }
    if (phase_ == MapPhase::Still) {
        tickStill(s, now);
    } else if (phase_ == MapPhase::Example) {
        tickExample(s, now);
    } else if (phase_ == MapPhase::Preview) {
        tickPreview(s, now);
    }
    lastMs_ = now;
}

void MappingTeacher::tickStill(const MotionSample& s, uint32_t now) {
    if (uint32_t(now - phaseStart_) > start::mapStillWindowMs) {
        fail("could not hold still for 2 seconds within 10 seconds");
        return;
    }
    const Vec3 g{s.gyro[0], s.gyro[1], s.gyro[2]}, a{s.accel[0], s.accel[1], s.accel[2]};
    auto seed = [&] {
        runStart_ = now;
        runCount_ = 1;
        runMean_ = g;
        runM2_ = {};
        accelMean_ = a;
    };
    if (runCount_ == 0) {
        seed();
        return;
    }
    bool movedGyro = false, movedAccel = false;
    for (unsigned i = 0; i < 3; ++i) {
        movedGyro = movedGyro || std::abs(g[i] - runMean_[i]) > start::mapStillGyroDeviation;
        movedAccel = movedAccel || std::abs(a[i] - accelMean_[i]) > start::mapStillAccelDeviation;
    }
    const float an = norm(a);
    movedAccel = movedAccel || an < .8f || an > 1.2f;
    if (movedGyro || movedAccel) {
        ++interruptions_;
        reason_ = movedGyro ? "movement detected: hold the assembly completely still"
                            : "tilt detected: keep the assembly from tilting";
        seed(); // a qualified stretch must be contiguous
        return;
    }
    ++runCount_;
    for (unsigned i = 0; i < 3; ++i) {
        const float delta = g[i] - runMean_[i];
        runMean_[i] += delta / float(runCount_);
        runM2_[i] += delta * (g[i] - runMean_[i]);
        accelMean_[i] += (a[i] - accelMean_[i]) / float(runCount_);
    }
    if (uint32_t(now - runStart_) < start::mapStillMs || runCount_ < 150) {
        if (interruptions_ == 0) {
            reason_ = "keep holding still"; // after an interruption the explanation stays visible
        }
        return;
    }
    bias_ = runMean_;
    for (unsigned i = 0; i < 3; ++i) {
        noise_[i] = std::max(.05f, std::sqrt(runM2_[i] / float(runCount_ - 1)));
    }
    if (norm(bias_) > 20.f || std::max({noise_[0], noise_[1], noise_[2]}) > 4.f) {
        fail("the sensor is too noisy or its offset too large at rest");
        return;
    }
    gravity_ = unit(accelMean_);
    phase_ = MapPhase::Example;
    step_ = 0;
    retries_ = 0;
    reason_ = "stillness measured";
    beginStep(now);
}

void MappingTeacher::beginStep(uint32_t now) {
    sub_ = Sub::Countdown;
    subStart_ = now;
    onsetCount_ = 0;
    rotation_ = {};
    peak_ = path_ = 0;
    calmSince_ = 0;
    pre_.clear();
}

void MappingTeacher::rejectExample(const char* reason, uint32_t now) {
    reason_ = reason;
    ++retries_;
    pendingAdvance_ = false;
    if (retries_ > start::mapMaxRetries) {
        fail("too many failed attempts at one example; start the teaching again");
        return;
    }
    sub_ = Sub::Settle;
    subStart_ = now;
}

void MappingTeacher::acceptExample(uint32_t now) {
    if (step_ < trainSteps) {
        taught_[directionOf(step_)].push_back(rotation_);
    }
    retries_ = 0;
    pendingAdvance_ = true;
    reason_ = "example accepted";
    sub_ = Sub::Settle;
    subStart_ = now;
}

bool MappingTeacher::validateExample(unsigned d, const Vec3& r, const char** why) const {
    const float sign = (d == 0 || d == 3) ? 1.f : -1.f;
    const Vec3& own = d < 2 ? candidate_.horizontal : candidate_.vertical;
    const Vec3& other = d < 2 ? candidate_.vertical : candidate_.horizontal;
    const float along = sign * dot(own, r), across = std::abs(dot(other, r));
    if (along < .5f * angle_[d] || across > .5f * along) {
        *why = "that movement did not match what you taught; try the same direction again";
        return false;
    }
    return true;
}

void MappingTeacher::finishMovement(uint32_t now) {
    const float duration = float(uint32_t(calmSince_ - moveStart_));
    const float angle = norm(rotation_);
    const unsigned d = directionOf(step_);
    if (duration < float(start::mapMinMoveMs)) {
        rejectExample("movement too short: make one steady turn", now);
    } else if (angle < start::mapMinAngle) {
        rejectExample("movement too small: turn a little further", now);
    } else if (path_ <= 0.f || angle / path_ < start::mapMinStraightness) {
        rejectExample("movement was not one steady turn: no wobble, no back-and-forth", now);
    } else if (step_ < trainSteps && !taught_[d].empty() &&
               cosine(rotation_, meanOf(taught_[d])) < start::mapConsistency) {
        rejectExample("that turn went a different way than your earlier ones", now);
    } else if (step_ >= trainSteps) {
        const char* why = nullptr;
        if (!validateExample(d, rotation_, &why)) {
            rejectExample(why, now);
            return;
        }
        acceptExample(now);
    } else {
        acceptExample(now);
    }
}

void MappingTeacher::tickExample(const MotionSample& s, uint32_t now) {
    const float dt = std::min(.05f, float(uint32_t(now - lastMs_)) / 1000.f);
    const Vec3 g = sub(Vec3{s.gyro[0], s.gyro[1], s.gyro[2]}, bias_);
    const float rate = norm(g);
    switch (sub_) {
    case Sub::Countdown:
        if (uint32_t(now - subStart_) >= start::mapCountdownMs) {
            sub_ = Sub::WaitOnset;
            subStart_ = now;
            onsetCount_ = 0;
            pre_.clear();
        }
        break;
    case Sub::WaitOnset:
        pre_.push_back({g, dt});
        if (pre_.size() > 3) {
            pre_.erase(pre_.begin());
        }
        onsetCount_ = rate > onsetRate() ? onsetCount_ + 1 : 0;
        if (onsetCount_ >= 3) {
            sub_ = Sub::Moving;
            moveStart_ = now;
            calmSince_ = 0;
            rotation_ = {};
            peak_ = path_ = 0;
            for (const auto& [v, step] : pre_) {
                for (unsigned i = 0; i < 3; ++i) {
                    rotation_[i] += v[i] * step;
                }
                path_ += norm(v) * step;
                peak_ = std::max(peak_, norm(v));
            }
        } else if (uint32_t(now - subStart_) >= start::mapOnsetWindowMs) {
            rejectExample("no movement detected after the cue", now);
        }
        break;
    case Sub::Moving:
        for (unsigned i = 0; i < 3; ++i) {
            rotation_[i] += g[i] * dt;
        }
        path_ += rate * dt;
        peak_ = std::max(peak_, rate);
        if (rate < exitRate()) {
            if (calmSince_ == 0) {
                calmSince_ = now;
            }
            if (uint32_t(now - calmSince_) >= start::mapCalmMs) {
                finishMovement(now);
                return;
            }
        } else {
            calmSince_ = 0;
        }
        if (uint32_t(now - moveStart_) > start::mapMaxMoveMs) {
            rejectExample("the movement did not finish: make one short, steady turn, then stop", now);
        }
        break;
    case Sub::Settle:
        if (uint32_t(now - subStart_) < start::mapSettleMs) {
            break;
        }
        if (pendingAdvance_) {
            pendingAdvance_ = false;
            ++step_;
            if (step_ == trainSteps && !analyze()) {
                return;
            }
            if (step_ >= allSteps) {
                phase_ = MapPhase::Preview;
                phaseStart_ = now;
                filtered_ = {};
                angleX_ = angleY_ = 0;
                reason_ = "validated: try the preview, then accept";
                return;
            }
        }
        beginStep(now);
        break;
    }
}

bool MappingTeacher::analyze() {
    const Vec3 right = meanOf(taught_[0]), left = meanOf(taught_[1]), up = meanOf(taught_[2]),
               down = meanOf(taught_[3]);
    if (cosine(right, left) > start::mapOppositeMax) {
        fail("right and left were not opposite movements; teach them again");
        return false;
    }
    if (cosine(up, down) > start::mapOppositeMax) {
        fail("up and down were not opposite movements; teach them again");
        return false;
    }
    Vec3 h = unit(sub(right, left));
    Vec3 v = unit(sub(down, up));
    if (norm(h) < .5f || norm(v) < .5f || std::abs(dot(h, v)) > start::mapSeparationMax) {
        fail("horizontal and vertical movements overlap; the directions are indistinguishable");
        return false;
    }
    v = unit(sub(v, scale(h, dot(v, h)))); // exactly orthogonal rows
    angle_ = {dot(right, h), -dot(left, h), -dot(up, v), dot(down, v)};
    for (float a : angle_) {
        if (a < start::mapMinAngle) {
            fail("one direction was too small to learn; teach it again with a larger turn");
            return false;
        }
    }
    candidate_ = LearnedControl{};
    candidate_.horizontal = h;
    candidate_.vertical = v;
    candidate_.bias = bias_;
    candidate_.noise = noise_;
    candidate_.gravity = gravity_;
    // gain order is L R U D; angle_ order is R L U D
    candidate_.gain = {
        std::clamp(start::mapTravelPixels / angle_[1], 3.f, 40.f),
        std::clamp(start::mapTravelPixels / angle_[0], 3.f, 40.f),
        std::clamp(start::mapTravelPixels / angle_[2], 3.f, 40.f),
        std::clamp(start::mapTravelPixels / angle_[3], 3.f, 40.f),
    };
    const Vec3 rows[2] = {h, v};
    for (unsigned k = 0; k < 2; ++k) {
        float variance = 0;
        for (unsigned i = 0; i < 3; ++i) {
            variance += rows[k][i] * rows[k][i] * noise_[i] * noise_[i];
        }
        const float enter = std::clamp(start::mapDeadzoneSigmas * std::sqrt(variance),
                                       start::mapDeadzoneFloor, start::mapDeadzoneCap);
        candidate_.deadzoneEnter[k] = enter;
        candidate_.deadzoneExit[k] = start::mapHysteresis * enter;
    }
    if (!candidate_.valid()) {
        fail("the learned settings were not valid");
        return false;
    }
    return true;
}

void MappingTeacher::tickPreview(const MotionSample& s, uint32_t now) {
    if (uint32_t(now - phaseStart_) > start::mapPreviewTimeoutMs) {
        fail("the preview timed out; start the teaching again");
        return;
    }
    const float dt = std::min(.05f, float(uint32_t(now - lastMs_)) / 1000.f);
    const Vec3 g = sub(Vec3{s.gyro[0], s.gyro[1], s.gyro[2]}, candidate_.bias);
    const float x = dot(candidate_.horizontal, g), y = dot(candidate_.vertical, g);
    filtered_[0] += .3f * (x - filtered_[0]);
    filtered_[1] += .3f * (y - filtered_[1]);
    angleX_ = std::clamp(angleX_ * .995f + filtered_[0] * dt, -45.f, 45.f);
    angleY_ = std::clamp(angleY_ * .995f + filtered_[1] * dt, -45.f, 45.f);
}

MappingStatus MappingTeacher::status(uint32_t now) const {
    MappingStatus st;
    st.phase = phase_;
    st.steps = allSteps;
    st.examplesPerDirection = start::mapExamples;
    st.retries = retries_;
    st.interruptions = interruptions_;
    st.reason = reason_;
    st.bias = phase_ == MapPhase::Still || phase_ == MapPhase::Idle ? Vec3{} : bias_;
    st.noise = phase_ == MapPhase::Still || phase_ == MapPhase::Idle ? Vec3{} : noise_;
    if (phase_ == MapPhase::Still) {
        st.cue = MapCue::HoldStill;
        st.stillMs = runCount_ ? uint32_t(lastMs_ - runStart_) : 0;
        st.windowMs = uint32_t(lastMs_ - phaseStart_);
        st.progress = .1f * std::min(1.f, float(st.stillMs) / float(start::mapStillMs));
    } else if (phase_ == MapPhase::Example) {
        st.step = step_;
        st.direction = int(directionOf(step_));
        st.validation = step_ >= trainSteps;
        st.example = step_ < trainSteps ? step_ % start::mapExamples : 0;
        auto left = [&](uint32_t total) {
            const uint32_t spent = uint32_t(now - subStart_);
            return spent >= total ? 0u : total - spent;
        };
        switch (sub_) {
        case Sub::Countdown:
            st.cue = MapCue::Countdown;
            st.cueMs = left(start::mapCountdownMs);
            break;
        case Sub::WaitOnset:
            st.cue = MapCue::Go;
            st.cueMs = left(start::mapOnsetWindowMs);
            break;
        case Sub::Moving:
            st.cue = MapCue::Recording;
            break;
        case Sub::Settle:
            st.cue = MapCue::ReturnToCentre;
            st.cueMs = left(start::mapSettleMs);
            break;
        }
        st.progress = .1f + .9f * float(step_) / float(allSteps);
    } else if (phase_ == MapPhase::Preview) {
        st.step = allSteps;
        st.cue = MapCue::Preview;
        st.progress = 1.f;
        st.previewX = filtered_[0];
        st.previewY = filtered_[1];
        st.previewAngleX = angleX_;
        st.previewAngleY = angleY_;
    } else if (phase_ == MapPhase::Done) {
        st.progress = 1.f;
    }
    return st;
}

size_t mappingJson(char* out, size_t capacity, const MappingStatus& s) {
    const int written = std::snprintf(
        out, capacity,
        "{\"phase\":\"%s\",\"cue\":\"%s\",\"step\":%u,\"steps\":%u,\"direction\":\"%s\","
        "\"validation\":%s,\"example\":%u,\"perDirection\":%u,\"retries\":%u,\"interruptions\":%u,"
        "\"cueMs\":%lu,\"stillMs\":%lu,\"windowMs\":%lu,\"progress\":%.3f,\"reason\":\"%s\","
        "\"preview\":{\"x\":%.2f,\"y\":%.2f,\"angleX\":%.2f,\"angleY\":%.2f},"
        "\"bias\":[%.3f,%.3f,%.3f],\"noise\":[%.3f,%.3f,%.3f],"
        "\"learnedValid\":%s,\"unsaved\":%s,\"stored\":\"%s\",\"mode\":\"%s\",\"blocked\":\"%s\","
        "\"saveResult\":\"%s\"}",
        name(s.phase), name(s.cue), s.step, s.steps, s.direction < 0 ? "NONE" : directionName(unsigned(s.direction)),
        s.validation ? "true" : "false", s.example, s.examplesPerDirection, s.retries,
        s.interruptions, static_cast<unsigned long>(s.cueMs), static_cast<unsigned long>(s.stillMs),
        static_cast<unsigned long>(s.windowMs), s.progress, s.reason, s.previewX, s.previewY,
        s.previewAngleX, s.previewAngleY, s.bias[0], s.bias[1], s.bias[2], s.noise[0], s.noise[1],
        s.noise[2], s.learnedValid ? "true" : "false", s.unsaved ? "true" : "false", s.stored,
        s.mode, s.blocked, s.saveResult);
    return written > 0 && size_t(written) < capacity ? size_t(written) : 0;
}
} // namespace nodx
