// Gesture click: trainer, recognizer, misses and false activations. SYNTHETIC recordings only (a
// modelled sensor with noise and a bias); nothing here is a hardware measurement.
#include "nodx/clickgesture.hpp"
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

struct Gesture {
    Vec3 axis{0, 0, 1}; // side tilt = frame axis 2
    float peak = 60;
    unsigned ms = 600;
    unsigned latency = 300;
    bool outAndBack = true; // tilt and return: one full sine period
};
struct Sim {
    std::mt19937 rng{777};
    std::normal_distribution<float> noise{0.f, .8f};
    uint32_t now = 0;
    Vec3 frame(const Vec3& body) {
        return {body[0] + biasTruth[0] + noise(rng), body[1] + biasTruth[1] + noise(rng),
                body[2] + biasTruth[2] + noise(rng)};
    }
    static Vec3 gestureAt(const Gesture& g, float tMs) {
        const float t = tMs / float(g.ms);
        if (t < 0 || t > 1) {
            return {0, 0, 0};
        }
        const float level = g.peak * (g.outAndBack ? std::sin(2 * kPi * t) : std::sin(kPi * t));
        return {g.axis[0] * level, g.axis[1] * level, g.axis[2] * level};
    }
};

// A pointing session: yaw/pitch sweeps with some roll leakage, flicks, pauses.
struct Pointing {
    float rollLeak = .2f; // roll component as a fraction of the main sweep
    std::mt19937 rng{4242};
    uint32_t until = 0;
    Vec3 dir{1, 0, 0};
    float peak = 40;
    uint32_t startAt = 0, ms = 800;
    bool paused = false;
    Vec3 at(uint32_t now) {
        if (now >= until) {
            std::uniform_real_distribution<float> u(0.f, 1.f);
            startAt = now;
            paused = u(rng) < .3f;
            ms = unsigned(400 + 1200 * u(rng));
            until = now + ms;
            const float a = 2 * kPi * u(rng);
            // mostly yaw/pitch; roll leakage up to ~20% of the main component, never dominant
            dir = {std::cos(a), std::sin(a), rollLeak * (u(rng) - .5f) * 2.f};
            peak = 15 + 70 * u(rng);
        }
        if (paused) {
            return {0, 0, 0};
        }
        const float level = peak * std::sin(kPi * float(now - startAt) / float(ms));
        return {dir[0] * level, dir[1] * level, dir[2] * level};
    }
};

struct Trainer {
    Sim sim;
    ClickTrainer trainer;
    ClickCue last = ClickCue::None;
    unsigned attempt = 0;
    bool moving = false;
    uint32_t moveStart = 0;
    Gesture gesture;
    std::function<Gesture(unsigned, bool, unsigned)> behavior = [](unsigned, bool, unsigned) {
        return Gesture{};
    };
    std::function<Vec3(uint32_t)> pointing = [p = Pointing{}](uint32_t now) mutable {
        return p.at(now);
    };
    void step() {
        sim.now += 10;
        const ClickTrainStatus st = trainer.status(sim.now);
        if (st.cue == ClickCue::Go && last != ClickCue::Go) {
            ++attempt;
            gesture = behavior(st.step, st.validation, attempt);
            moving = true;
            moveStart = sim.now + gesture.latency;
        }
        last = st.cue;
        Vec3 body{};
        if (st.phase == ClickPhase::Confusion) {
            body = pointing(sim.now);
        } else if (moving && sim.now >= moveStart) {
            if (sim.now - moveStart > gesture.ms) {
                moving = false;
            } else {
                body = Sim::gestureAt(gesture, float(sim.now - moveStart));
            }
        }
        trainer.tick(sim.frame(body), sim.now);
    }
    bool until(ClickPhase phase, unsigned maxMs) {
        for (unsigned i = 0; i < maxMs / 10 && trainer.phase() != phase; ++i) {
            if (!trainer.active()) {
                break;
            }
            step();
        }
        return trainer.phase() == phase;
    }
};

struct Trained {
    ClickTemplate tmpl;
};
Trained train() {
    Trainer t;
    t.trainer.begin(t.sim.now);
    require(t.until(ClickPhase::Ready, 300000), "training did not become ready");
    require(t.trainer.accept(), "accept");
    return {t.trainer.result()};
}
// Feed a frame sequence to a recognizer; returns how many clicks it produced.
struct Run {
    ClickRecognizer rec;
    Sim sim;
    unsigned clicks = 0, suppressedTicks = 0;
    void feed(const Vec3& body) {
        sim.now += 10;
        if (rec.update(sim.frame(body), sim.now).accepted) {
            ++clicks;
        }
        suppressedTicks += rec.suppressing() ? 1 : 0;
    }
    void quiet(unsigned ms) {
        for (unsigned i = 0; i < ms / 10; ++i) {
            feed({0, 0, 0});
        }
    }
    void perform(const Gesture& g, unsigned tailMs = 600) {
        for (unsigned t = 0; t <= g.ms; t += 10) {
            feed(Sim::gestureAt(g, float(t)));
        }
        quiet(tailMs);
    }
};
} // namespace

int main() {
    test("trainer: rest, five examples, two checks, an ordinary-pointing check, then ready", [] {
        Trainer t;
        t.trainer.begin(t.sim.now);
        require(t.trainer.status(t.sim.now).cue == ClickCue::HoldStill, "rest cue");
        require(t.until(ClickPhase::Ready, 300000), "never ready");
        const ClickTemplate tmpl = t.trainer.result();
        require(tmpl.valid() && tmpl.axis == 2, "the principal axis is the side tilt");
        require(tmpl.threshold >= start::clickThresholdMin && tmpl.threshold <= start::clickThresholdMax,
                "threshold bounds");
        require(std::abs(tmpl.bias[0] - biasTruth[0]) < .5f, "bias estimate");
        require(!t.trainer.accept() == false, "accept in the ready phase");
        require(t.attempt == start::clickExamples + start::clickValidations, "exactly 7 attempts");
    });
    test("trainer: a weak example is rejected and only that example is retried", [] {
        Trainer t;
        t.behavior = [](unsigned, bool, unsigned attempt) {
            Gesture g;
            if (attempt == 3) {
                g.peak = 15; // too weak
            }
            return g;
        };
        t.trainer.begin(t.sim.now);
        require(t.until(ClickPhase::Ready, 300000), "should recover");
        require(t.attempt == 8, "seven examples plus exactly one retry");
    });
    test("trainer: an inconsistent example is rejected", [] {
        Trainer t;
        t.behavior = [](unsigned, bool, unsigned attempt) {
            Gesture g;
            if (attempt == 3) {
                g.ms = 1300; // a very different speed and shape
                g.axis = {0, 1, 0};
            }
            return g;
        };
        t.trainer.begin(t.sim.now);
        require(t.until(ClickPhase::Ready, 300000), "should recover");
        require(t.attempt == 8, "one retry");
    });
    test("trainer: an ambiguous pattern (energy spread over all axes) is refused", [] {
        Trainer t;
        t.behavior = [](unsigned, bool, unsigned) {
            Gesture g;
            g.axis = {.58f, .58f, .58f};
            return g;
        };
        t.trainer.begin(t.sim.now);
        t.until(ClickPhase::Ready, 300000);
        require(t.trainer.phase() == ClickPhase::Failed &&
                    std::string(t.trainer.status(t.sim.now).reason).find("ambiguous") != std::string::npos,
                "must say the pattern is ambiguous");
    });
    test("trainer: a pattern that ordinary pointing triggers is refused", [] {
        Trainer t;
        // "Ordinary pointing" that contains the very same side tilt now and then.
        t.pointing = [g = Gesture{}](uint32_t now) {
            const uint32_t phase = now % 2000;
            return Sim::gestureAt(g, float(phase) - 200.f);
        };
        t.trainer.begin(t.sim.now);
        t.until(ClickPhase::Ready, 300000);
        require(t.trainer.phase() == ClickPhase::Failed &&
                    std::string(t.trainer.status(t.sim.now).reason).find("ordinary pointing") !=
                        std::string::npos,
                "confusion with pointing must fail the training");
    });
    test("trainer: too little ordinary pointing cannot confirm the gesture", [] {
        Trainer t;
        t.pointing = [](uint32_t) { return Vec3{0, 0, 0}; };
        t.trainer.begin(t.sim.now);
        t.until(ClickPhase::Ready, 300000);
        require(t.trainer.phase() == ClickPhase::Failed &&
                    std::string(t.trainer.status(t.sim.now).reason).find("not enough") != std::string::npos,
                "no activity: no confirmation");
    });
    test("trainer: never holding still, giving up and cancelling are handled", [] {
        Trainer t;
        t.trainer.begin(t.sim.now);
        for (unsigned i = 0; i < 700 && t.trainer.active(); ++i) {
            t.sim.now += 10;
            t.trainer.tick(t.sim.frame({(i / 40) % 2 ? 40.f : -40.f, 0, 0}), t.sim.now);
        }
        require(t.trainer.phase() == ClickPhase::Failed, "rest never qualified");
        Trainer c;
        c.trainer.begin(c.sim.now);
        c.trainer.cancel();
        require(c.trainer.phase() == ClickPhase::Failed && !c.trainer.accept(), "cancel");
        Trainer m;
        m.behavior = [](unsigned, bool, unsigned) {
            Gesture g;
            g.peak = 12;
            return g;
        };
        m.trainer.begin(m.sim.now);
        m.until(ClickPhase::Ready, 400000);
        require(m.trainer.phase() == ClickPhase::Failed, "too many weak attempts must fail");
    });

    test("recognizer: a deliberate gesture clicks exactly once; variation is tolerated", [] {
        const Trained trained = train();
        Run run;
        run.rec.configure(trained.tmpl);
        run.quiet(1000);
        unsigned attempts = 0;
        for (float amp : {.8f, .9f, 1.f, 1.1f, 1.2f}) {
            for (float speed : {.8f, 1.f, 1.25f}) {
                Gesture g;
                g.peak = 60 * amp;
                g.ms = unsigned(600 * speed);
                run.perform(g, 700);
                ++attempts;
            }
        }
        std::printf("INFO synthetic: %u of %u varied gestures accepted (candidates %u, rejects %u)\n",
                    run.clicks, attempts, run.rec.candidates, run.rec.rejected);
        require(run.clicks == attempts, "a deliberate gesture was missed");
        require(run.rec.candidates == attempts, "one candidate per gesture, none extra");
    });
    test("recognizer: a still sensor never clicks or opens a candidate", [] {
        const Trained trained = train();
        Run run;
        run.rec.configure(trained.tmpl);
        run.quiet(120000);
        require(run.clicks == 0 && run.rec.candidates == 0 && run.suppressedTicks == 0, "false activity");
    });
    test("recognizer: 10 minutes of ordinary pointing gives no false clicks", [] {
        const Trained trained = train();
        Run run;
        run.rec.configure(trained.tmpl);
        Pointing p;
        for (unsigned i = 0; i < 60000; ++i) {
            run.feed(p.at(run.sim.now + 10));
        }
        std::printf("INFO synthetic: 600 s of pointing: %u false clicks, %u candidates, %u suppressed ticks\n",
                    run.clicks, run.rec.candidates, run.suppressedTicks);
        require(run.clicks == 0, "false activations while pointing");
    });
    test("recognizer: pointing with heavy wrist roll opens candidates but still gives no false click", [] {
        const Trained trained = train();
        Run run;
        run.rec.configure(trained.tmpl);
        Pointing p;
        p.rollLeak = 2.5f; // roll comparable to or larger than the sweep: a hard case
        for (unsigned i = 0; i < 60000; ++i) {
            run.feed(p.at(run.sim.now + 10));
        }
        std::printf("INFO synthetic: 600 s of hard pointing: %u false clicks, %u candidates (%u rejected), %u suppressed ticks\n",
                    run.clicks, run.rec.candidates, run.rec.rejected, run.suppressedTicks);
        require(run.rec.candidates > 0, "the hard case should exercise the matcher");
        require(run.clicks == 0, "false activations with heavy roll");
    });
    test("recognizer: wrong, weak, reversed and wrong-speed movements are not accepted", [] {
        const Trained trained = train();
        auto clicksFor = [&](const Gesture& g) {
            Run run;
            run.rec.configure(trained.tmpl);
            run.quiet(1000);
            run.perform(g, 800);
            return run.clicks;
        };
        Gesture weak;
        weak.peak = 18;
        Gesture reversed;
        reversed.peak = -60; // opposite first lobe
        Gesture yaw;
        yaw.axis = {1, 0, 0};
        Gesture slow;
        slow.ms = 1450;
        slow.peak = 25;
        Gesture quick;
        quick.ms = 260;
        Gesture oneWay;
        oneWay.outAndBack = false; // tilt without returning
        require(clicksFor(weak) == 0, "weak");
        require(clicksFor(reversed) == 0, "reversed");
        require(clicksFor(yaw) == 0, "yaw");
        require(clicksFor(slow) == 0, "slow");
        require(clicksFor(quick) == 0, "quick");
        require(clicksFor(oneWay) == 0, "tilt without return");
        require(clicksFor(Gesture{}) == 1, "the real gesture still works");
    });
    test("recognizer: staying still after a click never repeats it; a second one waits for neutral", [] {
        const Trained trained = train();
        Run run;
        run.rec.configure(trained.tmpl);
        run.quiet(1000);
        run.perform(Gesture{}, 250); // calm for 150 ms accepts it; neutral wait has run 100 ms
        require(run.clicks == 1, "first click");
        // a second gesture before the neutral return is complete is ignored (rearming lockout)
        run.perform(Gesture{}, 100);
        require(run.clicks == 1, "gesture accepted before neutral return");
        run.quiet(20000);
        require(run.clicks == 1, "repeated clicks while stationary");
        run.perform(Gesture{}, 700);
        require(run.clicks == 2, "rearmed after a neutral stretch");
    });
    test("recognizer: pointer output is suppressed only while a candidate is open", [] {
        const Trained trained = train();
        Run run;
        run.rec.configure(trained.tmpl);
        run.quiet(1000);
        require(!run.rec.suppressing(), "armed state must not suppress");
        bool saw = false;
        for (unsigned t = 0; t <= 600; t += 10) {
            run.feed(Sim::gestureAt(Gesture{}, float(t)));
            saw = saw || run.rec.suppressing();
        }
        run.quiet(600);
        require(saw && !run.rec.suppressing(), "suppression must cover the candidate only");
    });
    test("recognizer: reset (a stop, fault or reconnect) closes a candidate and needs neutral again", [] {
        const Trained trained = train();
        Run run;
        run.rec.configure(trained.tmpl);
        run.quiet(1000);
        for (unsigned t = 0; t <= 250; t += 10) {
            run.feed(Sim::gestureAt(Gesture{}, float(t)));
        }
        require(run.rec.suppressing(), "candidate open");
        run.rec.reset(run.sim.now);
        require(!run.rec.suppressing(), "reset must close the candidate");
        run.feed({0, 0, 0});
        require(run.clicks == 0, "nothing accepted from a cancelled candidate");
        for (unsigned t = 0; t <= 600; t += 10) { // an immediate gesture is not armed yet
            run.feed(Sim::gestureAt(Gesture{}, float(t)));
        }
        run.quiet(700);
        require(run.clicks == 0, "armed immediately after a reset");
        run.perform(Gesture{}, 700);
        require(run.clicks == 1, "armed after a neutral stretch");
    });
    test("template: invalid templates are rejected", [] {
        ClickTemplate t = train().tmpl;
        require(t.valid(), "precondition");
        ClickTemplate bad = t;
        bad.trace[3][1] = NAN;
        require(!bad.valid(), "NaN");
        bad = t;
        bad.threshold = .9f;
        require(!bad.valid(), "threshold");
        bad = t;
        bad.axis = 5;
        require(!bad.valid(), "axis");
        bad = t;
        bad.peak = 5;
        require(!bad.valid(), "weak");
    });
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
