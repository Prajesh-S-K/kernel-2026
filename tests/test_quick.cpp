// Quick tilt-and-return click: practice, recognizer, misses and false activations. SYNTHETIC
// recordings only (modelled sensor noise and bias); nothing here is a real wearable recording.
#include "nodx/quickgesture.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <random>
#include <stdexcept>
#include <string>

using namespace nodx;
namespace {
int passed = 0, failed = 0;
void require(bool ok, const char* message) {
    if (!ok) {
        throw std::runtime_error(message);
    }
}
void test(const char* name, const std::function<void()>& body) {
    try {
        body();
        ++passed;
        std::printf("PASS %s\n", name);
    } catch (const std::exception& error) {
        ++failed;
        std::printf("FAIL %s: %s\n", name, error.what());
    }
}
constexpr float kPi = 3.14159265f;
const Vec3 biasTruth{-1.2f, -.9f, .1f};
Vec3 unitOf(Vec3 v) {
    const float n = norm(v);
    return {v[0] / n, v[1] / n, v[2] / n};
}
// Control frame: axes 0 and 1 are the pointing plane; a side tilt lives mostly on axis 2 but is
// deliberately given other orientations in the tests (any 3-D direction, not a raw axis).
const Vec3 tiltDirections[] = {unitOf({0, 0, 1}), unitOf({.3f, 0, .95f}), unitOf({.5f, .4f, .77f}),
                               unitOf({0, .5f, .87f}), unitOf({-.4f, .3f, -.86f})};

struct Shape {
    Vec3 dir{0, 0, 1};
    float excursionDeg = 13.f;
    unsigned ms = 600;      // onset to the return completing
    float returnFrac = 1.f; // fraction of the outward angle that is brought back
    Vec3 drift{0, 0, 0};    // constant extra rate that starts 100 ms in (a drifting, diagonal tilt)
    bool lShape = false;    // out along dir, then out along axis 0, then both back: a wandering path
    unsigned latency = 300;
};
// outward half sine then (partial) return half sine, rates in the body frame
Vec3 rateAt(const Shape& s, float tMs) {
    if (tMs < 0 || tMs > float(s.ms)) {
        return {0, 0, 0};
    }
    auto halfSine = [](float excursion, float spanMs, float t) {
        const float amp = excursion * kPi / (spanMs / 1000.f) / 2.f;
        return amp * std::sin(kPi * t / spanMs);
    };
    if (s.lShape) {
        const float q = float(s.ms) / 4.f;
        Vec3 out{0, 0, 0};
        if (tMs < q) {
            const float l = halfSine(s.excursionDeg, q, tMs);
            out = {s.dir[0] * l, s.dir[1] * l, s.dir[2] * l};
        } else if (tMs < 2 * q) {
            out = {halfSine(s.excursionDeg, q, tMs - q), 0, 0};
        } else {
            const float l = -halfSine(s.excursionDeg, 2 * q, tMs - 2 * q);
            out = {s.dir[0] * l + l, s.dir[1] * l, s.dir[2] * l};
        }
        return out;
    }
    const float half = float(s.ms) / 2.f;
    const float level = tMs < half ? halfSine(s.excursionDeg, half, tMs)
                                   : -s.returnFrac * halfSine(s.excursionDeg, half, tMs - half);
    Vec3 out{s.dir[0] * level, s.dir[1] * level, s.dir[2] * level};
    if (tMs >= 100.f) {
        for (unsigned i = 0; i < 3; ++i) {
            out[i] += s.drift[i];
        }
    }
    return out;
}

struct Sim {
    std::mt19937 rng{777};
    std::normal_distribution<float> noise{0.f, .8f};
    uint32_t now = 0;
    Vec3 frame(const Vec3& body) {
        return {body[0] + biasTruth[0] + noise(rng), body[1] + biasTruth[1] + noise(rng),
                body[2] + biasTruth[2] + noise(rng)};
    }
};

struct Practice {
    Sim sim;
    QuickPractice practice;
    std::function<Shape(unsigned)> behavior;
    QuickCue last = QuickCue::None;
    unsigned attempt = 0;
    bool moving = false;
    uint32_t moveStart = 0;
    Shape shape;
    std::vector<std::string> reasons;
    std::function<Vec3(uint32_t)> preview = [](uint32_t) { return Vec3{0, 0, 0}; };
    // Ordinary pointing shown during the pointing sample: yaw/pitch sweeps in random directions, in
    // the frame's axes 0 and 1 unless a test overrides it (e.g. a different, rotated plane).
    std::function<Vec3(uint32_t)> pointing = [rng = std::mt19937(2026), until = uint32_t(0),
                                              from = uint32_t(0), dir = Vec3{1, 0, 0},
                                              peak = 40.f](uint32_t now) mutable {
        std::uniform_real_distribution<float> u(0.f, 1.f);
        if (now >= until) {
            from = now;
            until = now + 300 + unsigned(900 * u(rng));
            const float a = 2 * kPi * u(rng);
            dir = {std::cos(a), std::sin(a), 0.f};
            peak = 25 + 50 * u(rng);
        }
        const float level = peak * std::sin(kPi * float(now - from) / float(until - from));
        return Vec3{dir[0] * level, dir[1] * level, dir[2] * level};
    };
    explicit Practice(std::function<Shape(unsigned)> b = [](unsigned) { return Shape{}; })
        : behavior(std::move(b)) {}
    void step() {
        sim.now += 10;
        const QuickStatus st = practice.status(sim.now);
        if (st.cue == QuickCue::Go && last != QuickCue::Go) {
            ++attempt;
            shape = behavior(attempt);
            moving = true;
            moveStart = sim.now + shape.latency;
        }
        last = st.cue;
        Vec3 body{};
        if (st.phase == QuickPhase::Preview) {
            body = preview(sim.now);
        } else if (st.phase == QuickPhase::Pointing) {
            body = pointing(sim.now);
        } else if (moving && sim.now >= moveStart) {
            if (sim.now - moveStart > shape.ms) {
                moving = false;
            } else {
                body = rateAt(shape, float(sim.now - moveStart));
            }
        }
        practice.tick(sim.frame(body), sim.now);
        const std::string reason = practice.status(sim.now).reason;
        if (reasons.empty() || reasons.back() != reason) {
            reasons.push_back(reason);
        }
    }
    bool until(QuickPhase phase, unsigned maxMs) {
        for (unsigned i = 0; i < maxMs / 10 && practice.phase() != phase; ++i) {
            if (!practice.active()) {
                break;
            }
            step();
        }
        return practice.phase() == phase;
    }
    bool saw(const char* needle) const {
        for (const auto& r : reasons) {
            if (r.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
};
QuickProfile practiceFor(const Vec3& dir, float excursion = 13.f) {
    Practice p([=](unsigned) {
        Shape s;
        s.dir = dir;
        s.excursionDeg = excursion;
        return s;
    });
    p.practice.begin(p.sim.now);
    require(p.until(QuickPhase::Preview, 120000), "practice did not reach the preview");
    require(p.practice.accept(), "accept");
    return p.practice.profile();
}

struct Run {
    QuickRecognizer rec;
    Sim sim;
    unsigned clicks = 0;
    std::vector<uint32_t> clickTimes;
    unsigned suppressedTicksWithOutput = 0;
    Run(const QuickProfile& p, const QuickSettings& s = QuickSettings{}) {
        rec.configure(p, s);
    }
    void feed(const Vec3& body, unsigned dtMs = 10) {
        sim.now += dtMs;
        if (rec.update(sim.frame(body), sim.now).accepted) {
            ++clicks;
            clickTimes.push_back(sim.now);
        }
    }
    void quiet(unsigned ms) {
        for (unsigned i = 0; i < ms / 10; ++i) {
            feed({0, 0, 0});
        }
    }
    void perform(const Shape& s, unsigned tailMs) {
        for (unsigned t = 0; t <= s.ms; t += 10) {
            feed(rateAt(s, float(t)));
        }
        quiet(tailMs);
    }
};
} // namespace

int main() {
    test("settings: sensitivity and return tolerance are validated", [] {
        require(QuickSettings{}.valid(), "defaults");
        require(QuickSettings{.5f, .15f}.valid() && QuickSettings{2.f, .6f}.valid(), "bounds inclusive");
        for (QuickSettings bad : {QuickSettings{.4f, .35f}, QuickSettings{2.1f, .35f},
                                  QuickSettings{1.f, .1f}, QuickSettings{1.f, .7f},
                                  QuickSettings{NAN, .35f}, QuickSettings{1.f, NAN}}) {
            require(!bad.valid(), "an out-of-range setting was accepted");
        }
        QuickProfile p = practiceFor(tiltDirections[0]);
        const QuickThresholds strict = deriveThresholds(p, {.5f, .35f});
        const QuickThresholds loose = deriveThresholds(p, {2.f, .35f});
        require(loose.minExcursionDeg <= strict.minExcursionDeg && loose.enterRate <= strict.enterRate,
                "more sensitivity must not raise the thresholds");
    });
    test("practice: stillness, one tilt, a preview; direction found for any orientation", [] {
        for (const Vec3& dir : tiltDirections) {
            Practice p([&](unsigned) {
                Shape s;
                s.dir = dir;
                return s;
            });
            p.practice.begin(p.sim.now);
            require(p.practice.status(p.sim.now).cue == QuickCue::HoldStill, "rest cue");
            require(p.until(QuickPhase::Preview, 120000), "no preview");
            const QuickProfile q = p.practice.profile();
            require(q.valid(), "profile");
            require(std::abs(dot(q.direction, dir)) > .96f, "direction not learned (3-D)");
            require(q.practiceDeg > 9.f && q.practiceDeg < 18.f, "amplitude");
            require(std::abs(q.bias[0] - biasTruth[0]) < .5f, "bias");
            require(p.attempt == 1, "one tilt only");
        }
    });
    test("practice: ambiguous attempts are explained and another movement is requested", [] {
        Practice p([](unsigned attempt) {
            Shape s;
            if (attempt == 1) {
                s.excursionDeg = 3; // too small
            } else if (attempt == 2) {
                s.returnFrac = 0; // never comes back (the half sine stops at the top)
            } else if (attempt == 3) {
                s.lShape = true; // out, then out sideways, then back: a wandering path
            } else if (attempt == 4) {
                s.ms = 1400; // too slow
            } else if (attempt == 5) {
                s.dir = {1, 0, 0}; // a pointing-plane movement: indistinguishable from pointing
            }
            return s;
        });
        p.practice.begin(p.sim.now);
        require(p.until(QuickPhase::Preview, 400000), "should recover with the sixth, good tilt");
        require(p.saw("too small"), "tiny tilt not explained");
        require(p.saw("did not come back") || p.saw("not come back"), "missing return not explained");
        require(p.saw("wandered"), "wandering movement not explained");
        require(p.saw("too slow"), "slow tilt not explained");
        require(p.saw("ordinary pointing"), "pointing-like tilt not explained (measured, not assumed)");
        require(p.attempt == 6, "five rejected attempts then one good");
        require(p.practice.profile().valid(), "profile after the retries");
    });
    test("practice: silence never enables it; cancel and failure stay failed", [] {
        Practice p([](unsigned) {
            Shape s;
            s.excursionDeg = 0;
            return s;
        });
        p.practice.begin(p.sim.now);
        for (unsigned i = 0; i < 4000 && p.practice.active() && p.practice.phase() != QuickPhase::Preview;
             ++i) {
            p.step();
            if (p.sim.now > 60000) {
                break;
            }
        }
        require(p.practice.phase() != QuickPhase::Preview && !p.practice.accept(), "silently enabled");
        Practice c;
        c.practice.begin(c.sim.now);
        c.practice.cancel();
        require(c.practice.phase() == QuickPhase::Failed && !c.practice.accept(), "cancel");
        Practice s;
        s.practice.begin(s.sim.now);
        for (unsigned i = 0; i < 800 && s.practice.active(); ++i) {
            s.sim.now += 10;
            s.practice.tick(s.sim.frame({(i / 30) % 2 ? 40.f : -40.f, 0, 0}), s.sim.now);
        }
        require(s.practice.phase() == QuickPhase::Failed, "never still");
    });
    test("practice: the preview shows states and a retry captures another tilt", [] {
        Practice p;
        p.practice.begin(p.sim.now);
        require(p.until(QuickPhase::Preview, 120000), "preview");
        const Shape tilt;
        uint32_t t0 = p.sim.now + 800;
        p.preview = [&](uint32_t now) {
            return now >= t0 ? rateAt(tilt, float(now - t0)) : Vec3{0, 0, 0};
        };
        for (unsigned i = 0; i < 300; ++i) {
            p.step();
        }
        const QuickStatus st = p.practice.status(p.sim.now);
        require(st.phase == QuickPhase::Preview && st.accepted == 1 && st.rejected == 0,
                "the preview recognizer should accept the practice-like tilt (no click is sent)");
        require(st.lastExcursionDeg > 0, "excursion not reported");
        p.preview = [](uint32_t) { return Vec3{0, 0, 0}; };
        p.practice.retry(p.sim.now);
        require(p.practice.phase() == QuickPhase::Tilt, "retry");
        require(p.until(QuickPhase::Preview, 60000), "second preview");
    });

    test("recognizer: valid tilts at different comfortable speeds and orientations click once", [] {
        unsigned attempts = 0, clicked = 0, extra = 0;
        for (const Vec3& dir : tiltDirections) {
            const QuickProfile profile = practiceFor(dir);
            for (unsigned ms : {400u, 500u, 600u, 750u, 900u}) {
                for (float amp : {.85f, 1.f, 1.15f}) {
                    Run run(profile);
                    run.quiet(800);
                    Shape s;
                    s.dir = dir;
                    s.ms = ms;
                    s.excursionDeg = 13.f * amp;
                    run.perform(s, 700);
                    ++attempts;
                    clicked += run.clicks >= 1;
                    extra += run.clicks > 1;
                    require(run.clicks <= 1, "more than one click for one gesture");
                }
            }
        }
        std::printf("INFO synthetic: %u of %u comfortable tilts accepted, %u double clicks\n", clicked,
                    attempts, extra);
        require(clicked == attempts, "a comfortable tilt was missed");
    });
    test("recognizer: the click comes only after the settled confirmation", [] {
        const QuickProfile profile = practiceFor(tiltDirections[0]);
        Run run(profile);
        run.quiet(800);
        Shape s;
        const uint32_t start = run.sim.now;
        run.perform(s, 400);
        require(run.clicks == 1, "click");
        const uint32_t gestureEnd = start + s.ms;
        require(run.clickTimes[0] >= gestureEnd + start::quickSettleMs - 20,
                "clicked before the settled confirmation");
        require(run.rec.lastDurationMs <= float(s.ms + start::quickSettleMs + 60), "duration reported");
    });
    test("recognizer: neutral, one click, rearming and the minimum interval", [] {
        const QuickProfile profile = practiceFor(tiltDirections[1]);
        Run run(profile);
        Shape s;
        s.dir = tiltDirections[1];
        // not armed right after configuration: needs 250 ms of neutral first
        run.perform(s, 50);
        require(run.clicks == 0, "armed before the neutral period");
        run.quiet(2000);
        run.perform(s, 250);
        require(run.clicks == 1, "first click");
        // an immediate repeat is ignored (neutral + 500 ms interval needed)
        run.perform(s, 50);
        require(run.clicks == 1, "second click without a fresh neutral period");
        run.quiet(20000);
        require(run.clicks == 1, "repeated clicks while stationary");
        run.perform(s, 700);
        require(run.clicks == 2, "rearmed after a fresh neutral period");
    });
    test("recognizer: ordinary pointing, fast reversals, diagonals and tremor do not click", [] {
        const QuickProfile profile = practiceFor(tiltDirections[0]);
        Run run(profile);
        std::mt19937 rng(31);
        std::uniform_real_distribution<float> u(0.f, 1.f);
        uint32_t until = 0, from = 0;
        Vec3 dir{1, 0, 0};
        float peak = 30;
        bool reversal = false;
        for (unsigned i = 0; i < 60000; ++i) { // 600 s
            if (run.sim.now >= until) {
                from = run.sim.now;
                until = from + 250 + unsigned(1200 * u(rng));
                const float a = 2 * kPi * u(rng);
                dir = {std::cos(a), std::sin(a), .25f * (u(rng) - .5f) * 2.f};
                peak = 15 + 90 * u(rng);
                reversal = u(rng) < .3f; // a fast reversal: out and straight back
            }
            const float x = float(run.sim.now - from) / float(until - from);
            float level = peak * std::sin(kPi * x);
            if (reversal) {
                level = peak * std::sin(2 * kPi * x);
            }
            // plus tremor: 9 Hz, 4 deg/s, on every axis
            const float tremor = 4.f * std::sin(2 * kPi * 9.f * float(run.sim.now) / 1000.f);
            run.feed({dir[0] * level + tremor, dir[1] * level + tremor, dir[2] * level + tremor});
        }
        std::printf("INFO synthetic: 600 s of pointing with reversals and tremor: %u false clicks, %u candidates (%u rejected), %u ms suppressed\n",
                    run.clicks, run.rec.candidates, run.rec.rejected, run.rec.suppressedMs);
        require(run.clicks == 0, "false clicks while pointing");
    });
    test("recognizer: tiny movements and pure tremor never open a candidate", [] {
        const QuickProfile profile = practiceFor(tiltDirections[0]);
        Run run(profile);
        run.quiet(800);
        for (unsigned i = 0; i < 12000; ++i) {
            const float tremor = 6.f * std::sin(2 * kPi * 10.f * float(run.sim.now) / 1000.f);
            run.feed({tremor * .2f, tremor * .3f, tremor});
        }
        require(run.rec.candidates == 0 && run.clicks == 0, "tremor opened a candidate");
        Run tiny(profile);
        tiny.quiet(800);
        Shape s;
        s.excursionDeg = 3.f;
        tiny.perform(s, 600);
        require(tiny.clicks == 0, "a tiny tilt clicked");
    });
    test("recognizer: incomplete returns, diagonal and timeout cases are rejected with reasons", [] {
        const QuickProfile profile = practiceFor(tiltDirections[0]);
        auto runShape = [&](const Shape& s) {
            Run run(profile);
            run.quiet(800);
            run.perform(s, 1500);
            return std::pair<unsigned, QuickReject>{run.clicks, run.rec.lastReject};
        };
        Shape half;
        half.returnFrac = .4f; // returns only 40 percent
        auto r = runShape(half);
        require(r.first == 0 && r.second == QuickReject::Residual, "partial return not rejected");
        Shape none;
        none.returnFrac = 0.f; // outward only, then holds
        r = runShape(none);
        require(r.first == 0 && (r.second == QuickReject::NoReturn || r.second == QuickReject::Timeout),
                "no return not rejected");
        Shape diagonal;
        diagonal.drift = {0, 25, 0}; // drifts off the practiced direction after the tilt has begun
        r = runShape(diagonal);
        require(r.first == 0 && r.second == QuickReject::CrossAxis, "diagonal movement not rejected");
        Shape wander;
        wander.lShape = true;
        r = runShape(wander);
        require(r.first == 0, "a wandering path clicked");
        Shape small;
        small.excursionDeg = 6.5f; // above noise, below the clear-excursion bar
        r = runShape(small);
        require(r.first == 0, "a barely-there tilt clicked");
        // timeout boundaries: the settled return must START within 1000 ms of the onset
        Shape inside;
        inside.ms = 900;
        require(runShape(inside).first == 1, "a 900 ms tilt (inside the limit) was missed");
        Shape outside;
        outside.ms = 1250;
        r = runShape(outside);
        require(r.first == 0 && r.second == QuickReject::Timeout, "a 1250 ms tilt must time out");
    });
    test("recognizer: invalid or irregular samples cancel a candidate", [] {
        const QuickProfile profile = practiceFor(tiltDirections[0]);
        Run run(profile);
        run.quiet(800);
        Shape s;
        for (unsigned t = 0; t <= 250; t += 10) {
            run.feed(rateAt(s, float(t)));
        }
        require(run.rec.suppressing(), "candidate not open");
        run.feed({0, 0, 0}, 200); // a 200 ms gap in the samples
        require(!run.rec.suppressing() && run.rec.lastReject == QuickReject::InvalidSample,
                "an irregular interval must cancel the candidate");
        run.quiet(2000);
        require(run.clicks == 0, "clicked from a broken candidate");
        Run nan(profile);
        nan.quiet(800);
        for (unsigned t = 0; t <= 250; t += 10) {
            nan.feed(rateAt(s, float(t)));
        }
        nan.sim.now += 10;
        nan.rec.update({NAN, 0, 0}, nan.sim.now);
        require(!nan.rec.suppressing(), "NaN must cancel the candidate");
    });
    test("recognizer: the pointer is frozen only while a candidate is open; suppression is counted", [] {
        const QuickProfile profile = practiceFor(tiltDirections[0]);
        Run run(profile);
        run.quiet(800);
        require(!run.rec.suppressing(), "armed must not suppress");
        Shape s;
        bool sawOpen = false;
        unsigned suppressedTicks = 0;
        for (unsigned t = 0; t <= s.ms + 300; t += 10) {
            run.feed(rateAt(s, float(t)));
            if (run.rec.suppressing()) {
                sawOpen = true;
                ++suppressedTicks;
            }
        }
        require(sawOpen && !run.rec.suppressing(), "suppression must end at acceptance");
        require(run.rec.suppressedMs > 300 && run.rec.suppressedMs < 1300, "suppressed time reported");
        require(std::abs(float(run.rec.suppressedMs) - float(suppressedTicks) * 10.f) < 30.f,
                "suppressed time matches the open candidate");
    });
    test("recognizer: reset (stop, fault, reconnect, mode change) closes a candidate; neutral again", [] {
        const QuickProfile profile = practiceFor(tiltDirections[0]);
        Run run(profile);
        run.quiet(800);
        Shape s;
        for (unsigned t = 0; t <= 250; t += 10) {
            run.feed(rateAt(s, float(t)));
        }
        require(run.rec.suppressing(), "open");
        run.rec.reset(run.sim.now);
        require(!run.rec.suppressing(), "reset must close it");
        run.perform(s, 60); // an immediate gesture: not armed yet
        require(run.clicks == 0, "armed immediately after a reset");
        run.quiet(2000);
        run.perform(s, 700);
        require(run.clicks == 1, "armed after a neutral period");
    });
    test("recognizer: sensitivity trades misses against sensitivity to small tilts", [] {
        const QuickProfile profile = practiceFor(tiltDirections[0]);
        Shape small;
        small.excursionDeg = 5.5f;
        auto clicksAt = [&](float sens) {
            Run run(profile, QuickSettings{sens, .35f});
            run.quiet(800);
            run.perform(small, 800);
            return run.clicks;
        };
        require(clicksAt(1.f) == 0, "default sensitivity should not take a 5.5 degree tilt");
        std::printf("INFO synthetic: 5.5 degree tilt: %u clicks at sensitivity 2.0, %u at 0.5\n",
                    clicksAt(2.f), clicksAt(.5f));
        require(clicksAt(.5f) == 0, "strict sensitivity must not take it");
    });

    // ------------------------------------------------ the designated direction (signed, 3-D)
    // d is the practised outward direction; rotated(phi) tilts it by phi degrees toward a perpendicular.
    auto perpendicularTo = [](const Vec3& d) {
        Vec3 helper = std::abs(d[0]) < .8f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
        Vec3 p{helper[0] - dot(helper, d) * d[0], helper[1] - dot(helper, d) * d[1],
               helper[2] - dot(helper, d) * d[2]};
        return unitOf(p);
    };
    auto rotated = [&](const Vec3& d, float phiDeg) {
        const Vec3 p = perpendicularTo(d);
        const float c = std::cos(phiDeg * kPi / 180.f), sn = std::sin(phiDeg * kPi / 180.f);
        return Vec3{d[0] * c + p[0] * sn, d[1] * c + p[1] * sn, d[2] * c + p[2] * sn};
    };
    test("direction: the practice learns a SIGNED direction; the opposite sign is a different gesture", [&] {
        const Vec3 d = unitOf({.2f, .1f, 1.f});
        const Vec3 neg{-d[0], -d[1], -d[2]};
        const QuickProfile positive = practiceFor(d), negative = practiceFor(neg);
        require(dot(positive.direction, d) > .97f, "positive direction not learned with its sign");
        require(dot(negative.direction, neg) > .97f && dot(negative.direction, d) < -.97f,
                "the opposite practice must learn the opposite sign");
        for (const QuickProfile* profile : {&positive, &negative}) {
            Run run(*profile);
            run.quiet(800);
            Shape designated;
            designated.dir = profile->direction;
            run.perform(designated, 700);
            require(run.clicks == 1, "the designated direction did not click");
            // the OPPOSITE direction, even larger and faster, must not open a candidate at all
            Run other(*profile);
            other.quiet(800);
            Shape opposite;
            opposite.dir = {-profile->direction[0], -profile->direction[1], -profile->direction[2]};
            opposite.excursionDeg = 25.f;
            opposite.ms = 400;
            other.perform(opposite, 800);
            require(other.clicks == 0 && other.rec.candidates == 0 && other.rec.suppressedMs == 0,
                    "the opposite direction opened a candidate");
        }
    });
    test("direction: perpendicular movements never open a candidate", [&] {
        const Vec3 d = unitOf({0, .3f, 1.f});
        const QuickProfile profile = practiceFor(d);
        const Vec3 p1 = perpendicularTo(d);
        const Vec3 p2 = unitOf({d[1] * p1[2] - d[2] * p1[1], d[2] * p1[0] - d[0] * p1[2],
                                d[0] * p1[1] - d[1] * p1[0]});
        for (const Vec3& dir : {p1, p2, Vec3{-p1[0], -p1[1], -p1[2]}, Vec3{-p2[0], -p2[1], -p2[2]}}) {
            Run run(profile);
            run.quiet(800);
            Shape s;
            s.dir = dir;
            s.excursionDeg = 22.f;
            run.perform(s, 800);
            require(run.clicks == 0 && run.rec.candidates == 0, "a perpendicular movement opened a candidate");
            require(run.rec.suppressedMs == 0, "pointing must not be suppressed for a perpendicular move");
        }
    });
    test("direction: angular boundary cases follow the validated tolerance", [&] {
        const Vec3 d = unitOf({.3f, .1f, 1.f});
        const QuickProfile profile = practiceFor(d);
        auto clicksAt = [&](float phi, float toleranceDeg) {
            QuickSettings settings;
            settings.directionToleranceDeg = toleranceDeg;
            Run run(profile, settings);
            run.quiet(800);
            Shape s;
            s.dir = rotated(profile.direction, phi);
            run.perform(s, 800);
            return std::pair<unsigned, unsigned>{run.clicks, run.rec.candidates};
        };
        // default tolerance 30 degrees
        for (float phi : {0.f, 10.f, 20.f, 25.f}) {
            require(clicksAt(phi, 30.f).first == 1, "a gesture inside the tolerance was missed");
        }
        for (float phi : {38.f, 45.f, 70.f, 90.f, 135.f}) {
            const auto r = clicksAt(phi, 30.f);
            require(r.first == 0 && r.second == 0, "a gesture beyond the tolerance opened a candidate");
        }
        // the boundary moves with the setting, within its bounds
        require(clicksAt(22.f, 15.f).second == 0, "22 degrees must be outside a 15 degree tolerance");
        require(clicksAt(22.f, 45.f).first == 1, "22 degrees must be inside a 45 degree tolerance");
        require(clicksAt(50.f, 60.f).second >= 1, "50 degrees must open a candidate at the 60 degree bound");
        require(QuickSettings{1.f, .35f, 10.f}.valid() && QuickSettings{1.f, .35f, 60.f}.valid(),
                "bounds are inclusive");
        require(!QuickSettings{1.f, .35f, 9.f}.valid() && !QuickSettings{1.f, .35f, 61.f}.valid() &&
                    !QuickSettings{1.f, .35f, NAN}.valid(),
                "an out-of-range angle was accepted");
    });
    test("direction: deviation after the candidate began cancels it without a click", [&] {
        const QuickProfile profile = practiceFor(unitOf({.1f, 0, 1.f}));
        auto runWith = [&](const Vec3& drift) {
            Run run(profile);
            run.quiet(800);
            Shape s;
            s.dir = profile.direction;
            s.drift = drift;
            run.perform(s, 1500);
            return std::pair<unsigned, QuickReject>{run.clicks, run.rec.lastReject};
        };
        const Vec3 p = perpendicularTo(profile.direction);
        const auto r = runWith({p[0] * 25.f, p[1] * 25.f, p[2] * 25.f});
        require(r.first == 0 && r.second == QuickReject::CrossAxis, "a drifting gesture must be cancelled");
        // the candidate is closed at once: the recognizer is not suppressing any more
        Run run(profile);
        run.quiet(800);
        Shape s;
        s.dir = profile.direction;
        s.drift = {p[0] * 40.f, p[1] * 40.f, p[2] * 40.f};
        bool sawOpen = false;
        uint32_t cancelledAt = 0;
        for (unsigned t = 0; t <= s.ms; t += 10) {
            run.feed(rateAt(s, float(t)));
            sawOpen = sawOpen || run.rec.suppressing();
            if (sawOpen && !run.rec.suppressing() && cancelledAt == 0) {
                cancelledAt = run.sim.now;
            }
        }
        require(sawOpen && cancelledAt != 0, "the candidate should have opened and then been cancelled early");
        require(cancelledAt < run.sim.now - 100, "cancelled only at the end instead of at the deviation");
        require(run.clicks == 0, "clicked after excessive deviation");
    });
    test("direction: the status reports the designated direction and tolerance", [&] {
        const Vec3 d = unitOf({0, .2f, 1.f});
        Practice p([&](unsigned) {
            Shape s;
            s.dir = d;
            return s;
        });
        p.practice.begin(p.sim.now);
        require(p.until(QuickPhase::Preview, 120000), "preview");
        const QuickStatus st = p.practice.status(p.sim.now);
        require(st.designated && dot(st.direction, d) > .97f, "direction not reported in the preview");
        require(std::abs(st.directionToleranceDeg - 30.f) < .01f, "tolerance not reported");
        char buffer[quickJsonCapacity];
        const size_t n = quickJson(buffer, sizeof buffer, st);
        require(n > 100 && n < quickJsonCapacity * 3 / 4, "length headroom");
        require(std::strstr(buffer, "\"designated\":true") && std::strstr(buffer, "\"direction\":[") &&
                    std::strstr(buffer, "\"directionTolerance\":30"),
                "direction fields missing from the JSON");
        require(!std::strstr(buffer, "nan") && !std::strstr(buffer, "inf"), "non-finite token");
        QuickStatus none;
        quickJson(buffer, sizeof buffer, none);
        require(std::strstr(buffer, "\"designated\":false"), "no direction before a practice");
    });

    // ------------------------------------------------ the pointing check is MEASURED
    // A pointing sample in an arbitrary plane. `a` and `b` are orthonormal in the control frame.
    auto planarPointing = [](Vec3 a, Vec3 b) {
        return [=, rng = std::mt19937(7), until = uint32_t(0), from = uint32_t(0), angle = 0.f,
                peak = 40.f](uint32_t now) mutable {
            std::uniform_real_distribution<float> u(0.f, 1.f);
            if (now >= until) {
                from = now;
                until = now + 300 + unsigned(900 * u(rng));
                angle = 2 * kPi * u(rng);
                peak = 25 + 50 * u(rng);
            }
            const float level = peak * std::sin(kPi * float(now - from) / float(until - from));
            const float c = std::cos(angle) * level, sn = std::sin(angle) * level;
            return Vec3{a[0] * c + b[0] * sn, a[1] * c + b[1] * sn, a[2] * c + b[2] * sn};
        };
    };
    auto practiceWith = [&](const Vec3& dir, std::function<Vec3(uint32_t)> pointing) {
        auto p = std::make_unique<Practice>([=](unsigned) {
            Shape s;
            s.dir = dir;
            return s;
        });
        p->pointing = std::move(pointing);
        p->practice.begin(p->sim.now);
        p->until(QuickPhase::Preview, 150000);
        return p;
    };
    test("pointing check: independent of the default pointing plane (axes 0 and 1)", [&] {
        // Ordinary pointing happens in the plane of axes 0 and 2 here (a different mounting), so a
        // direction along axis 2 IS the user's pointing, and a direction along axis 1 is not.
        const auto plane = planarPointing({1, 0, 0}, {0, 0, 1});
        auto along2 = practiceWith({0, 0, 1}, plane);
        require(along2->practice.phase() != QuickPhase::Preview,
                "a direction inside the user's measured pointing was accepted");
        require(along2->saw("ordinary pointing"), "the rejection must name ordinary pointing");
        auto along1 = practiceWith({0, 1, 0}, plane);
        require(along1->practice.phase() == QuickPhase::Preview,
                "a direction outside the measured pointing was refused (the old default-plane rule)");
        require(along1->practice.status(along1->sim.now).pointingShare < .1f,
                "the measured share of pointing along the direction should be small");
    });
    test("pointing check: the designated direction is compared with what the user actually does", [&] {
        const auto defaultPlane = planarPointing({1, 0, 0}, {0, 1, 0});
        auto ok = practiceWith(unitOf({.3f, 0, 1.f}), defaultPlane);
        require(ok->practice.phase() == QuickPhase::Preview, "a clearly separate direction was refused");
        const QuickStatus st = ok->practice.status(ok->sim.now);
        require(st.pointingShare < start::quickPointingShareMax && st.pointingMs >= 5000,
                "the pointing share and the observed pointing time must be reported");
        auto in = practiceWith(unitOf({1, .1f, 0}), defaultPlane);
        require(in->practice.phase() != QuickPhase::Preview, "a pointing direction was accepted");
        require(in->saw("% of your ordinary pointing"), "the share of pointing must be reported");
    });
    test("pointing check: too little or one-directional pointing cannot confirm a direction", [&] {
        auto none = practiceWith({0, 0, 1}, [](uint32_t) { return Vec3{0, 0, 0}; });
        require(none->practice.phase() == QuickPhase::Failed, "still sensor must not pass the check");
        require(none->saw("not enough varied ordinary pointing"), "reason");
        // pointing in one direction only proves nothing about the others
        auto oneWay = practiceWith({0, 0, 1}, [t0 = uint32_t(0)](uint32_t now) mutable {
            const float level = 45.f * std::sin(2 * kPi * float(now) / 700.f);
            return Vec3{level, 0, 0};
        });
        require(oneWay->saw("more different directions"), "one-way pointing must ask for variety");
    });
    test("pointing check: a gesture the measured pointing triggers is refused with the pointer-pause cost", [&] {
        // ordinary pointing that now and then contains the designated tilt-and-return itself
        const Vec3 d = unitOf({.1f, 0, 1.f});
        auto base = planarPointing({1, 0, 0}, {0, 1, 0});
        auto p = practiceWith(d, [=](uint32_t now) mutable {
            Shape s;
            s.dir = d;
            // a designated tilt-and-return every 2.5 s, on top of NORMAL pointing in the other axes
            const uint32_t phase = now % 2500;
            const Vec3 tilt = phase < 700 ? rateAt(s, float(phase)) : Vec3{0, 0, 0};
            const Vec3 point = phase < 1100 ? Vec3{0, 0, 0} : base(now); // pause pointing around it
            return Vec3{tilt[0] + point[0], tilt[1] + point[1], tilt[2] + point[2]};
        });
        require(p->practice.phase() != QuickPhase::Preview, "a gesture present in the pointing was accepted");
        require(p->saw("triggered this gesture") || p->saw("% of your ordinary pointing"),
                "the confusion must be explained");
    });
    test("pointing check: the replay catches a rare embedded gesture even when its variance share is small", [&] {
        // Strong ordinary pointing with ONE designated tilt-and-return every 4 s: the tilt is only a few
        // percent of the pointing variance, so only replaying the sample through the recognizer finds it.
        const Vec3 d = unitOf({.1f, 0, 1.f});
        auto base = planarPointing({1, 0, 0}, {0, 1, 0});
        auto p = practiceWith(d, [=](uint32_t now) mutable {
            Shape s;
            s.dir = d;
            const uint32_t phase = now % 4000;
            const Vec3 tilt = phase < 700 ? rateAt(s, float(phase)) : Vec3{0, 0, 0};
            // pointing in [1200, 3400) ms of each cycle: a calm gap precedes the tilt (armed again)
            const Vec3 point = (phase < 1200 || phase >= 3400) ? Vec3{0, 0, 0} : base(now);
            return Vec3{tilt[0] + 1.8f * point[0], tilt[1] + 1.8f * point[1], tilt[2] + 1.8f * point[2]};
        });
        require(p->practice.phase() != QuickPhase::Preview, "an embedded gesture was accepted");
        require(p->saw("triggered this gesture"), "the replay must be what rejects it");
        require(p->saw("pointer paused"), "the pointer-pause cost must be reported");
    });
    test("pointing check: a retried tilt reuses the measured pointing and is checked again", [&] {
        auto p = std::make_unique<Practice>([](unsigned attempt) {
            Shape s;
            s.dir = attempt == 1 ? Vec3{1, 0, 0} : unitOf({.2f, 0, 1.f}); // first a pointing direction
            return s;
        });
        p->practice.begin(p->sim.now);
        require(p->until(QuickPhase::Preview, 200000), "second tilt should pass");
        require(p->attempt == 2, "exactly one retried tilt");
        require(p->saw("ordinary pointing"), "the first tilt was rejected against measured pointing");
    });

    // ------------------------------------------------ opposite-direction return and near-zero settling
    test("return: overshoot, slow return, wrong order and detours are not accepted", [&] {
        const Vec3 d = unitOf({.2f, 0, 1.f});
        const QuickProfile profile = practiceFor(d);
        auto outcome = [&](const Shape& s, unsigned tail = 1500) {
            Run run(profile);
            run.quiet(800);
            run.perform(s, tail);
            return std::pair<unsigned, QuickReject>{run.clicks, run.rec.lastReject};
        };
        Shape good;
        good.dir = profile.direction;
        require(outcome(good, 400).first == 1, "the designated gesture must click");
        Shape overshoot = good;
        overshoot.returnFrac = 1.7f; // goes past the start, into the opposite side
        auto r = outcome(overshoot);
        require(r.first == 0 && r.second == QuickReject::Residual, "an overshooting return was accepted");
        Shape slow = good;
        slow.returnFrac = 0.f; // outward only...
        slow.drift = {0, 0, 0};
        r = outcome(slow);
        require(r.first == 0, "no return clicked");
        // a creeping, sub-threshold return (about 3 deg/s) is not a return stroke
        Run creep(profile);
        creep.quiet(800);
        for (unsigned t = 0; t <= 400; t += 10) {
            Shape slowOut;
            slowOut.dir = profile.direction;
            slowOut.ms = 800;
            slowOut.returnFrac = 0.f;
            creep.feed(rateAt(slowOut, float(t)));
        }
        for (unsigned t = 0; t < 2500; t += 10) {
            creep.feed({-3.f * profile.direction[0], -3.f * profile.direction[1], -3.f * profile.direction[2]});
        }
        require(creep.clicks == 0 && creep.rec.rejected >= 1, "a creeping return was accepted");
        // the opposite stroke FIRST, then the designated one straight after: wrong order, no candidate
        Run wrong(profile);
        wrong.quiet(800);
        for (unsigned t = 0; t <= 600; t += 10) {
            const Vec3 v = rateAt(good, float(t));
            wrong.feed({-v[0], -v[1], -v[2]}); // out the opposite way, back the designated way
        }
        wrong.quiet(1500);
        require(wrong.clicks == 0 && wrong.rec.candidates == 0, "opposite-first order opened a candidate");
        // out along the direction, back along a DIFFERENT path (a detour): cross-axis rejection
        Shape loop = good;
        loop.lShape = true;
        r = outcome(loop);
        require(r.first == 0, "a detouring return was accepted");
    });
    test("settling: the final residual boundary follows the return tolerance", [&] {
        const QuickProfile profile = practiceFor(unitOf({0, .1f, 1.f}));
        auto clicksFor = [&](float returnFrac, float tolerance) {
            Run run(profile, QuickSettings{1.f, tolerance});
            run.quiet(800);
            Shape s;
            s.dir = profile.direction;
            s.returnFrac = returnFrac;
            run.perform(s, 800);
            return run.clicks;
        };
        // residual = (1 - returnFrac) of the excursion; default tolerance 0.35
        require(clicksFor(1.0f, .35f) == 1 && clicksFor(.85f, .35f) == 1 && clicksFor(.75f, .35f) == 1,
                "residuals up to 25 percent must settle");
        require(clicksFor(.6f, .35f) == 0 && clicksFor(.5f, .35f) == 0,
                "residuals of 40 percent and more must not");
        require(clicksFor(.6f, .5f) == 1, "the boundary must move with the tolerance setting");
        require(clicksFor(.85f, .15f) == 1 && clicksFor(.7f, .15f) == 0, "tight tolerance");
    });
    test("settling: the 150 ms calm must be unbroken and genuinely quiet", [&] {
        const QuickProfile profile = practiceFor(unitOf({0, 0, 1.f}));
        Shape s;
        s.dir = profile.direction;
        auto withBump = [&](unsigned bumpAfterMs, float bumpRate) {
            Run run(profile);
            run.quiet(800);
            for (unsigned t = 0; t <= s.ms; t += 10) {
                run.feed(rateAt(s, float(t)));
            }
            for (unsigned t = 0; t < 1500; t += 10) {
                // after the return a short disturbance, then calm again
                const bool bump = t >= bumpAfterMs && t < bumpAfterMs + 30;
                run.feed(bump ? Vec3{bumpRate, 0, 0} : Vec3{0, 0, 0});
            }
            return run.clicks;
        };
        std::printf("INFO synthetic: calm broken at 100 ms by a 30 deg/s bump: %u clicks (the calm restarts); at 400 ms: %u click(s)\n",
                    withBump(100, 30.f), withBump(400, 30.f));
        // a bump inside the confirmation window restarts the calm: the click is delayed, not lost
        require(withBump(100, 30.f) <= 1, "more than one click");
        // a bump AFTER the click does not undo it
        require(withBump(400, 30.f) == 1, "a late bump must not remove an accepted gesture");
        // creeping around the exit threshold never settles: no click, a timeout/no-return rejection
        Run creeping(profile);
        creeping.quiet(800);
        for (unsigned t = 0; t <= s.ms; t += 10) {
            creeping.feed(rateAt(s, float(t)));
        }
        for (unsigned t = 0; t < 2500; t += 10) {
            creeping.feed({0, 0, 11.f * std::sin(2 * kPi * float(t) / 400.f)}); // 11 deg/s, above exit
        }
        require(creeping.clicks == 0, "a never-settling return clicked");
        // genuinely quiet (below the exit threshold) after the return settles in 150 ms
        Run quiet(profile);
        quiet.quiet(800);
        const uint32_t startAt = quiet.sim.now;
        for (unsigned t = 0; t <= s.ms; t += 10) {
            quiet.feed(rateAt(s, float(t)));
        }
        quiet.quiet(400);
        require(quiet.clicks == 1 && quiet.clickTimes[0] - startAt <= s.ms + 300,
                "the settled return should confirm about 150 ms after it ends");
    });
    test("settling: bias estimate error drifts the integrated residual; the documented limit", [&] {
        const QuickProfile base = practiceFor(unitOf({0, 0, 1.f}));
        auto clicksWithBiasError = [&](float errorDps) {
            QuickProfile p = base;
            for (float& b : p.bias) {
                b += errorDps; // the stored bias is wrong by this much on every axis
            }
            Run run(p);
            run.quiet(800);
            Shape s;
            s.dir = base.direction;
            run.perform(s, 500);
            return run.clicks;
        };
        require(clicksWithBiasError(0.f) == 1 && clicksWithBiasError(1.f) == 1,
                "a 1 deg/s bias error must still settle");
        std::printf("INFO synthetic: bias error 2 deg/s: %u click(s); 3: %u; 6: %u\n",
                    clicksWithBiasError(2.f), clicksWithBiasError(3.f), clicksWithBiasError(6.f));
        require(clicksWithBiasError(6.f) == 0,
                "a large bias error must not be silently accepted as a settled return");
    });
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
