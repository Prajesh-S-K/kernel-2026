// Quick tilt-and-return click: practice, recognizer, misses and false activations. SYNTHETIC
// recordings only (modelled sensor noise and bias); nothing here is a real wearable recording.
#include "nodx/quickgesture.hpp"
#include <cmath>
#include <cstdio>
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
        require(p.saw("wandered"), "diagonal movement not explained");
        require(p.saw("too slow"), "slow tilt not explained");
        require(p.saw("ordinary pointing"), "pointing-like tilt not explained");
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
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
