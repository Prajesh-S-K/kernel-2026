// Hands-free revision regression tests. Deterministic, synthetic input only: they show that the
// logic behaves as specified, not accidental-trigger rates, comfort or suitability for any user.
#include "hf_support.hpp"
#include <random>
#include <cstring>

namespace {
HandsFreeConfig sampleConfig() {
    HandsFreeConfig config;
    config.enabled = true;
    config.gestures = makeSet();
    return config;
}
// Recompute the CRC after a deliberate mutation so only the intended defect remains.
std::vector<uint8_t> reseal(std::vector<uint8_t> bytes) {
    bytes.resize(bytes.size() - 4);
    const uint32_t crc = checksum(bytes);
    for (unsigned i = 0; i < 4; ++i) {
        bytes.push_back(uint8_t(crc >> (8 * i)));
    }
    return bytes;
}
void setWord(std::vector<uint8_t>& bytes, size_t word, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) {
        bytes.at(word * 4 + i) = uint8_t(value >> (8 * i));
    }
}
uint32_t bitsOf(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    return bits;
}
bool contains(const char* text, const char* part) {
    return std::strstr(text, part) != nullptr;
}
std::vector<Rates> join(std::vector<Rates> a, const std::vector<Rates>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}
std::vector<Rates> hold(unsigned axis, float rate, unsigned ms) {
    std::vector<Rates> out(ms / 10, Rates{});
    for (auto& item : out) {
        item[axis] = rate;
    }
    return out;
}
// Words 12 and 26 hold the first and second template's enter rate in the wire format.
constexpr size_t enterWord[2] = {12, 26};
constexpr size_t strokesWord[2] = {5, 19};

bool anyMovement(const Report& report) {
    return report.dx != 0 || report.dy != 0 || report.wheel != 0;
}
// Everything after `from` released and stationary (a pulse sends press then release).
bool quietSince(const HF& h, size_t from) {
    for (size_t i = from; i < h.transport.reports.size(); ++i) {
        const Report& r = h.transport.reports[i];
        if (r.down || anyMovement(r)) {
            return false;
        }
    }
    return true;
}
unsigned clicksSince(const HF& h, size_t from) {
    unsigned clicks = 0;
    // Compare each report with the one before it; an idle link carries no repeated zero reports.
    for (size_t i = from; i < h.transport.reports.size(); ++i) {
        const bool before = i > 0 && h.transport.reports[i - 1].down;
        if (h.transport.reports[i].down && !before) {
            ++clicks;
        }
    }
    return clicks;
}
// Nudge the pointer so a post-resume dwell lockout is cleared.
void moveAway(HF& h) {
    h.run(hold(0, 30, 300));
    h.quiet(100);
}
// A deliberate gesture: neutral first, the pattern, then neutral again so the system can rearm.
void perform(HF& h, const char* pattern) {
    h.quiet(400);
    h.run(script(pattern));
    h.quiet(400);
}
void beginDrag(HF& h) {
    perform(h, "tilt2");
    require(h.s().dragging && h.last().down, "drag not started");
}
} // namespace

// ---- ordinary pointing versus recognition candidates (observed in the Lab block) ----------------
// What one run of synthetic pointing cost, measured against the same input on a legacy-mode system
// (same default profile, plain resume) that has no recognizer to suppress anything.
struct Pointing {
    unsigned candidates = 0, rejected = 0, executed = 0, suppressedMs = 0, longestMs = 0;
    float dx = 0, dy = 0, wheel = 0;
    bool dragSeen = false, leftActive = false;
    Reject lastReject = Reject::None;
};
Pointing pointing(HF& h, const std::vector<Rates>& input) {
    Pointing result;
    const auto before = h.s().recognizer;
    const size_t mark = h.transport.reports.size();
    unsigned run = 0;
    for (const auto& rate : input) {
        h.tick(rate);
        if (h.s().recognizer.suppressing()) {
            result.suppressedMs += 10;
            run += 10;
            result.longestMs = std::max(result.longestMs, run);
        } else {
            run = 0;
        }
        result.dragSeen = result.dragSeen || h.s().dragging;
        result.leftActive = result.leftActive || h.s().state != SystemState::Active;
    }
    result.candidates = h.s().recognizer.candidates - before.candidates;
    result.rejected = h.s().recognizer.rejected - before.rejected;
    result.executed = h.s().recognizer.executed - before.executed;
    result.lastReject = h.s().recognizer.lastReject;
    for (size_t i = mark; i < h.transport.reports.size(); ++i) {
        result.dx += h.transport.reports[i].dx;
        result.dy += h.transport.reports[i].dy;
        result.wheel += h.transport.reports[i].wheel;
    }
    return result;
}
std::unique_ptr<HF> legacyActive() {
    auto h = std::make_unique<HF>();
    h->sw = false; // the enable switch is irrelevant in legacy mode
    h->quiet(500);
    require(h->s().resume(), "legacy resume");
    h->quiet(400);
    return h;
}
// Rest, a held movement on one axis, rest. A held key in the companion produces exactly this.
std::vector<Rates> held(unsigned axis, float rate, unsigned holdMs, unsigned restBefore = 600,
                        unsigned restAfter = 600) {
    std::vector<Rates> out;
    sim::neutral(out, restBefore);
    Rates value{};
    value[axis] = rate;
    out.insert(out.end(), holdMs / 10, value);
    sim::neutral(out, restAfter);
    return out;
}
// ---- the momentary enable push button --------------------------------------------------------
// Drives the gate alone at 1 ms resolution and counts permission changes ("toggles").
struct GateRig {
    EnableGate gate;
    uint32_t t = 1000;
    unsigned toggles = 0;
    bool last = false;
    explicit GateRig(EnableKind kind = EnableKind::Momentary) {
        gate.configure(true, false, kind);
    }
    void raw(bool active, unsigned ms) {
        for (unsigned i = 0; i < ms; ++i) {
            ++t;
            gate.update(active, t);
            if (gate.permitted() != last) {
                last = gate.permitted();
                ++toggles;
            }
        }
    }
    // A press that bounces for `bounceMs` (2 ms down / 2 ms up) before it settles pressed.
    void bouncyPress(unsigned bounceMs, unsigned settledMs) {
        for (unsigned i = 0; i < bounceMs; i += 4) {
            raw(true, 2);
            raw(false, 2);
        }
        raw(true, settledMs);
    }
    void bouncyRelease(unsigned bounceMs, unsigned settledMs) {
        for (unsigned i = 0; i < bounceMs; i += 4) {
            raw(false, 2);
            raw(true, 2);
        }
        raw(false, settledMs);
    }
};
HF buttonRig() {
    return HF(true, EnableKind::Momentary);
}

// Candidate suppression stops at the learned stroke limit (300 ms here) plus one sample.
constexpr unsigned maxSuppressionMs = 320;

int main() {
    unsigned passed = 0, failed = 0;
    auto test = [&](const std::string& label, const std::function<void()>& body) {
        try {
            body();
            ++passed;
            std::cout << "PASS " << label << '\n';
        } catch (const std::exception& e) {
            ++failed;
            std::cout << "FAIL " << label << ": " << e.what() << '\n';
        }
    };

    // ------------------------------------------------------------ C: recognition
    test("recognizer executes the nod pattern exactly once", [] {
        Recog r;
        r.quiet(400);
        r.run(script("nod2"));
        r.quiet(300);
        require(r.events == 1 && r.lastId == 0, "event count");
        r.quiet(2000);
        require(r.events == 1, "repeated execution");
    });
    test("recognizer tells the two patterns apart", [] {
        Recog r;
        r.quiet(400);
        r.run(script("nod2"));
        r.quiet(400);
        require(r.events == 1 && r.lastId == 0, "nod");
        r.run(script("tilt2"));
        r.quiet(400);
        require(r.events == 2 && r.lastId == 1, "tilt");
    });
    test("partial pattern is rejected and executes nothing", [] {
        Recog r;
        r.quiet(400);
        r.run(script("nod1"));
        r.quiet(600);
        require(r.events == 0 && r.r.rejected == 1, "partial executed");
        require(r.r.lastReject == Reject::GapTimeout, "reason");
    });
    test("wrong-order pattern is rejected", [] {
        Recog r;
        r.quiet(400);
        std::vector<Rates> mixed;
        sim::stroke(mixed, 1, 1, 70, 160);
        sim::neutral(mixed, 20);
        sim::stroke(mixed, 2, -1, 70, 160); // roll where a pitch stroke is expected
        r.run(mixed);
        r.quiet(400);
        require(r.events == 0 && r.r.lastReject == Reject::WrongOrder, "wrong order");
    });
    test("pattern whose first stroke has the wrong sign is not a candidate", [] {
        Recog r;
        r.quiet(400);
        std::vector<Rates> backwards;
        for (int sign : {-1, 1, -1, 1}) {
            sim::stroke(backwards, 1, sign, 70, 160);
            sim::neutral(backwards, 20);
        }
        r.run(backwards);
        r.quiet(400);
        require(r.events == 0, "reversed pattern executed");
    });
    test("too-fast pattern is rejected", [] {
        Recog r;
        r.quiet(400);
        r.run(sim::cycles(1, 2, 120, 30, 10));
        r.quiet(400);
        require(r.events == 0 && r.r.lastReject == Reject::TooFast, "too fast");
    });
    test("too-slow pattern is rejected before it can suppress for long", [] {
        Recog r;
        r.quiet(400);
        const auto slow = sim::cycles(1, 2, 70, 700, 20);
        unsigned suppressed = 0;
        for (const auto& rate : slow) {
            r.tick(rate);
            suppressed += r.r.suppressing() ? 1 : 0;
        }
        require(r.events == 0 && r.r.lastReject == Reject::TooSlow, "too slow");
        require(suppressed * 10 <= 500, "suppression not bounded by the stroke limit");
    });
    test("weak motion never starts a candidate; a sub-minimum peak is rejected", [] {
        Recog weak;
        weak.quiet(400);
        weak.run(sim::cycles(1, 2, 12, 160, 20));
        weak.quiet(400);
        require(weak.r.candidates == 0 && weak.events == 0, "weak motion became a candidate");
        Recog low;
        low.quiet(400);
        low.run(sim::cycles(1, 2, 26, 160, 20));
        low.quiet(400);
        require(low.events == 0 && low.r.lastReject == Reject::TooWeak, "peak below minimum");
    });
    test("excessive peak is rejected", [] {
        Recog r;
        r.quiet(400);
        r.run(sim::cycles(1, 2, 300, 160, 20));
        r.quiet(400);
        require(r.events == 0 && r.r.lastReject == Reject::TooStrong, "too strong");
    });
    test("diagonal movement is rejected as not single axis", [] {
        Recog r;
        r.quiet(400);
        std::vector<Rates> diagonal;
        for (int sign : {1, -1, 1, -1}) {
            std::vector<Rates> pitch, yaw;
            sim::stroke(pitch, 1, sign, 70, 160);
            sim::stroke(yaw, 0, sign, 60, 160);
            for (size_t i = 0; i < pitch.size(); ++i) {
                diagonal.push_back({yaw[i][0], pitch[i][1], 0});
            }
            sim::neutral(diagonal, 20);
        }
        r.run(diagonal);
        r.quiet(400);
        require(r.events == 0 && r.r.lastReject == Reject::NotSingleAxis, "diagonal");
    });
    test("gap and total time limits reject", [] {
        Recog gap;
        gap.quiet(400);
        std::vector<Rates> slowGap;
        sim::stroke(slowGap, 1, 1, 70, 160);
        sim::neutral(slowGap, 400);
        sim::stroke(slowGap, 1, -1, 70, 160);
        gap.run(slowGap);
        gap.quiet(400);
        require(gap.events == 0 && gap.r.lastReject == Reject::GapTimeout, "gap");
        GestureSet set = makeSet();
        set.templates[0].totalMaxMs = 600;
        Recog total(set);
        total.quiet(400);
        total.run(script("nod2"));
        total.quiet(400);
        require(total.events == 0 && total.r.lastReject == Reject::TotalTimeout, "total");
    });
    test("neutral return is required before another command", [] {
        Recog r;
        r.quiet(400);
        r.run(script("nod2"));
        r.quiet(100);
        require(r.events == 1, "first");
        r.run(script("nod2")); // without enough neutral in between
        r.quiet(100);
        require(r.events == 1, "second executed without neutral");
        r.quiet(400);
        r.run(script("nod2"));
        r.quiet(300);
        require(r.events == 2, "no execution after neutral");
    });
    test("held posture and repeated samples cannot retrigger indefinitely", [] {
        Recog held;
        held.quiet(400);
        held.run(hold(1, 60, 5000)); // a posture held at a steady rate
        require(held.events == 0 && held.r.candidates == 1, "held rate");
        held.quiet(400);
        held.run(script("nod2"));
        held.quiet(10000); // end posture held for ten seconds
        require(held.events == 1, "retriggered after completion");
        Recog repeated;
        repeated.quiet(400);
        repeated.run(hold(0, 10, 10000)); // the same sample repeated
        require(repeated.events == 0, "repeated sample");
        Recog midway;
        midway.quiet(400);
        midway.run(join(script("nod1"), hold(1, 60, 3000)));
        require(midway.events == 0, "held after partial");
    });
    test("timestamp rollover does not disturb recognition", [] {
        Recog r;
        r.now = UINT32_MAX - 550;
        r.r.reset(r.now);
        r.quiet(400);
        const uint32_t before = r.now;
        r.run(script("nod2"));
        r.quiet(300);
        require(r.now < before, "timestamp did not wrap");
        require(r.events == 1, "execution across rollover");
    });
    test("irregular sample intervals still recognise exactly once", [] {
        Recog r;
        r.quiet(400);
        const auto nod = script("nod2");
        for (size_t i = 0; i < nod.size(); ++i) {
            r.tick(nod[i], 6 + unsigned((i * 7) % 9));
        }
        r.quiet(300);
        require(r.events == 1, "jittered pattern");
    });
    test("neutral hold is required before the first stroke", [] {
        Recog r;
        r.quiet(100);
        r.run(script("nod2"));
        r.quiet(300);
        require(r.events == 0 && r.r.candidates == 0, "no neutral hold");
        r.quiet(400);
        r.run(script("nod2"));
        r.quiet(300);
        require(r.events == 1, "after neutral");
    });
    test("lingering slow motion disarms recognition", [] {
        Recog r;
        r.quiet(400);
        r.run(hold(0, 10, 300)); // slow pointing between neutral and the stroke threshold
        r.run(script("nod2"));
        r.quiet(300);
        require(r.events == 0, "slow motion did not disarm");
        r.quiet(400);
        r.run(script("nod2"));
        r.quiet(300);
        require(r.events == 1, "rearm after neutral");
    });
    test("two identical templates are ambiguous and execute nothing", [] {
        GestureSet set = makeSet();
        set.templates[1] = set.templates[0];
        Recog r(set);
        r.quiet(400);
        r.run(script("nod2"));
        r.quiet(300);
        require(r.events == 0 && r.r.lastReject == Reject::Ambiguous && r.r.rejected == 1,
                "ambiguous completion executed");
    });
    test("nonfinite sample resets recognition without executing", [] {
        Recog r;
        r.quiet(400);
        const auto nod = script("nod2");
        for (size_t i = 0; i < nod.size(); ++i) {
            r.tick(i == 40 ? Rates{NAN, 0, 0} : nod[i]);
        }
        r.quiet(300);
        require(r.events == 0, "executed across NaN");
        require(r.r.state() != RecognizerState::Candidate, "stuck candidate");
    });
    test("a disabled template is never recognised", [] {
        Recog r(makeSet(), 1);
        r.quiet(400);
        r.run(script("tilt2"));
        r.quiet(400);
        require(r.events == 0, "disabled template executed");
        r.run(script("nod2"));
        r.quiet(300);
        require(r.events == 1 && r.lastId == 0, "enabled template");
    });
    test("recognition is deterministic for identical input", [] {
        Recog a, b;
        for (Recog* r : {&a, &b}) {
            r->quiet(400);
            r->run(script("nod2"));
            r->run(script("tilt1"));
            r->quiet(600);
            r->run(script("tilt2"));
            r->quiet(300);
        }
        require(a.events == b.events && a.r.candidates == b.r.candidates &&
                    a.r.rejected == b.r.rejected && a.r.lastReject == b.r.lastReject &&
                    a.lastId == b.lastId && a.r.state() == b.r.state(),
                "runs differ");
        require(a.events == 2, "unexpected event count");
    });
    test("normal pointing, scrolling and drift never execute a command pattern", [] {
        Recog r;
        unsigned sequences = 0;
        auto settle = [&] { r.quiet(500); };
        // 1. slow sinusoids on every axis, up to 1.2 Hz and 40 deg/s
        for (unsigned axis = 0; axis < 3; ++axis) {
            for (float hz : {.2f, .4f, .6f, .8f, 1.f, 1.2f}) {
                for (float amplitude : {10.f, 25.f, 40.f}) {
                    std::vector<Rates> motion(1200, Rates{});
                    for (size_t i = 0; i < motion.size(); ++i) {
                        motion[i][axis] = amplitude * std::sin(6.2831853f * hz * float(i) * .01f);
                    }
                    r.run(motion);
                    settle();
                    ++sequences;
                }
            }
        }
        // 2. sustained moves: ramp, hold, release (both directions, every axis)
        for (unsigned axis = 0; axis < 3; ++axis) {
            for (float sign : {1.f, -1.f}) {
                std::vector<Rates> motion;
                for (unsigned i = 0; i < 20; ++i) {
                    Rates rate{};
                    rate[axis] = sign * 30.f * float(i) / 20.f;
                    motion.push_back(rate);
                }
                for (const auto& rate : hold(axis, sign * 30.f, 1500)) {
                    motion.push_back(rate);
                }
                r.run(motion);
                settle();
                ++sequences;
            }
        }
        // 3. scroll tilts: a quick tilt then a long hold
        for (float sign : {1.f, -1.f}) {
            std::vector<Rates> tilt;
            sim::stroke(tilt, 2, int(sign), 40, 300);
            r.run(tilt);
            r.quiet(2000);
            ++sequences;
        }
        // 4. a single quick nod, a single quick turn, and a three-stroke wobble
        r.run(script("nod1"));
        settle();
        r.run(script("turn1"));
        settle();
        std::vector<Rates> three;
        for (int sign : {1, -1, 1}) {
            sim::stroke(three, 1, sign, 70, 160);
            sim::neutral(three, 20);
        }
        r.run(three);
        settle();
        sequences += 3;
        // 5. diagonal pointing
        std::vector<Rates> diagonal(100, Rates{25, 25, 0});
        r.run(diagonal);
        settle();
        ++sequences;
        // 6. smooth pseudo-random pointing (fixed LCG so the corpus is reproducible)
        uint32_t seed = 12345;
        auto next = [&] {
            seed = seed * 1664525u + 1013904223u;
            return float(int(seed >> 16) % 2001 - 1000) / 1000.f;
        };
        for (unsigned run = 0; run < 100; ++run) {
            Rates target{}, current{};
            std::vector<Rates> motion;
            for (unsigned i = 0; i < 600; ++i) {
                if (i % 12 == 0) {
                    target = {next() * 25, next() * 25, next() * 15};
                }
                for (unsigned a = 0; a < 3; ++a) {
                    current[a] += .15f * (target[a] - current[a]);
                }
                motion.push_back(current);
            }
            r.run(motion);
            settle();
            ++sequences;
        }
        require(sequences >= 150, "corpus too small");
        require(r.events == 0, "command pattern executed on normal movement");
        r.run(script("nod2"));
        r.quiet(300);
        require(r.events == 1, "recognizer no longer works after the corpus");
    });

    // ------------------------------------------------ A: configuration records
    test("distinct requires different sequences and no prefix relation", [] {
        const auto nod2 = makeTemplate(1, 4), nod1 = makeTemplate(1, 2), tilt2 = makeTemplate(2, 4);
        require(!distinct(nod2, nod2), "equal");
        require(!distinct(nod2, nod1) && !distinct(nod1, nod2), "prefix");
        require(distinct(nod2, tilt2), "different axis");
        auto reversed = nod2;
        for (unsigned i = 0; i < reversed.strokes; ++i) {
            reversed.sign[i] = int8_t(-reversed.sign[i]);
        }
        require(distinct(nod2, reversed), "different sign");
        require(distinct(nod2, GestureTemplate{}), "unconfigured");
    });
    test("template and set bounds are enforced", [] {
        require(makeTemplate(1, 4).valid() && makeSet().valid(true), "baseline");
        auto reject = [](const std::function<void(GestureTemplate&)>& mutate, const char* what) {
            auto item = makeTemplate(1, 4);
            mutate(item);
            require(!item.valid(), what);
        };
        reject([](GestureTemplate& t) { t.strokes = 1; }, "one stroke");
        reject([](GestureTemplate& t) { t.strokes = 7; }, "seven strokes");
        reject([](GestureTemplate& t) { t.axis[2] = 3; }, "axis");
        reject([](GestureTemplate& t) { t.sign[1] = 0; }, "sign");
        reject([](GestureTemplate& t) { t.enterRate = 7.9f; }, "enter low");
        reject([](GestureTemplate& t) { t.enterRate = 100.5f; }, "enter high");
        reject([](GestureTemplate& t) { t.enterRate = NAN; }, "enter nan");
        reject([](GestureTemplate& t) { t.peakMin = 10; }, "peak below enter");
        reject([](GestureTemplate& t) { t.peakMax = 30; }, "peak max not above min");
        reject([](GestureTemplate& t) { t.peakMax = 300; }, "peak max high");
        reject([](GestureTemplate& t) { t.strokeMinMs = 10; }, "stroke min low");
        reject([](GestureTemplate& t) { t.strokeMaxMs = 70; }, "stroke max too close");
        reject([](GestureTemplate& t) { t.strokeMaxMs = 1500; }, "stroke max high");
        reject([](GestureTemplate& t) { t.gapMaxMs = 10; }, "gap low");
        reject([](GestureTemplate& t) { t.totalMaxMs = 100; }, "total low");
        reject([](GestureTemplate& t) { t.totalMaxMs = 5000; }, "total high");
        GestureSet set = makeSet();
        set.neutralRate = .5f;
        require(!set.valid(true), "neutral low");
        set.neutralRate = 11;
        require(!set.valid(true), "neutral high");
        set = makeSet();
        set.templates[1] = {};
        require(!set.valid(true) && set.valid(false), "both required only when enabled");
        set = makeSet();
        set.templates[1] = set.templates[0];
        require(!set.valid(true), "indistinct pair");
    });
    test("configuration round trip keeps identity independent of generation", [] {
        const auto config = sampleConfig();
        HandsFreeConfig out;
        uint32_t generation = 0;
        const auto bytes = encode(config, 7);
        require(bytes.size() == HandsFreeConfig::wireSize, "wire size");
        require(decode(bytes, out, generation) == ConfigState::Valid && generation == 7, "decode");
        require(out.enabled && !out.switchlessQualified, "flags");
        require(configId(out) == configId(config), "identity changed by round trip");
        auto changed = config;
        changed.gestures.templates[0].peakMin = 31;
        require(configId(changed) != configId(config), "identity ignores content");
        auto qualified = config;
        qualified.switchlessQualified = true;
        require(configId(qualified) != configId(config), "identity ignores switchless flag");
        require(encode(config, 1) != encode(config, 2) && configId(config) != 0, "generation");
    });
    test("configuration decode rejects every single-byte corruption", [] {
        const auto bytes = encode(sampleConfig(), 3);
        for (size_t i = 0; i < bytes.size(); ++i) {
            auto damaged = bytes;
            damaged[i] ^= 1;
            HandsFreeConfig out;
            uint32_t generation;
            require(decode(damaged, out, generation) == ConfigState::Corrupt,
                    "corruption accepted");
        }
    });
    test("unsupported version is classified separately from corruption", [] {
        auto bytes = encode(sampleConfig(), 1);
        setWord(bytes, 1, 2);
        HandsFreeConfig out;
        uint32_t generation;
        require(decode(reseal(bytes), out, generation) == ConfigState::Unsupported, "version");
    });
    test("out-of-bounds fields are classified even with a correct checksum", [] {
        const auto good = encode(sampleConfig(), 1);
        auto mutate = [&](const std::function<void(std::vector<uint8_t>&)>& change) {
            auto bytes = good;
            change(bytes);
            HandsFreeConfig out;
            uint32_t generation;
            return decode(reseal(bytes), out, generation);
        };
        require(mutate([](auto& b) { setWord(b, enterWord[0], bitsOf(NAN)); }) ==
                    ConfigState::OutOfBounds,
                "nan");
        require(mutate([](auto& b) { setWord(b, enterWord[0], bitsOf(500)); }) ==
                    ConfigState::OutOfBounds,
                "range");
        require(mutate([](auto& b) { setWord(b, strokesWord[0], 9); }) == ConfigState::OutOfBounds,
                "stroke count");
        require(mutate([](auto& b) { setWord(b, 3, 8); }) == ConfigState::OutOfBounds, "flags");
        require(mutate([](auto& b) { setWord(b, 10, 1); }) == ConfigState::OutOfBounds,
                "unused stroke slot");
        require(mutate([](auto& b) { setWord(b, 6, 3); }) == ConfigState::OutOfBounds, "axis");
        require(mutate([](auto& b) {
                    for (size_t w = 0; w < 14; ++w) {
                        for (unsigned i = 0; i < 4; ++i) {
                            b[(19 + w) * 4 + i] = b[(5 + w) * 4 + i];
                        }
                    }
                }) == ConfigState::OutOfBounds,
                "indistinct gestures");
    });
    test("bad lengths are corrupt records", [] {
        const auto bytes = encode(sampleConfig(), 1);
        for (size_t size :
             {size_t(0), size_t(3), bytes.size() - 1, bytes.size() + 1, size_t(4096)}) {
            auto sized = bytes;
            sized.resize(size);
            HandsFreeConfig out;
            uint32_t generation;
            require(decode(sized, out, generation) == ConfigState::Corrupt, "length accepted");
        }
    });
    test("two-slot configuration: newest valid wins, torn write keeps prior, corrupt falls back",
         [] {
             MemoryConfigStorage storage;
             HandsFreeRepository repo(storage);
             HandsFreeConfig a = sampleConfig(), b = sampleConfig(), out;
             b.switchlessQualified = true;
             require(repo.load(out) == ConfigState::Missing, "missing");
             require(repo.save(a) && repo.save(b), "saves");
             require(repo.load(out) == ConfigState::Valid && out.switchlessQualified, "newest");
             storage.slots[1][20] ^= 4; // corrupt the newest slot
             require(repo.load(out) == ConfigState::Valid && !out.switchlessQualified, "fallback");
             storage.slots[1][20] ^= 4; // repair it again
             storage.tearWrite = true;
             auto c = a;
             c.gestures.neutralRate = 5;
             require(!repo.save(c), "torn write accepted");
             storage.tearWrite = false;
             require(repo.load(out) == ConfigState::Valid && out.switchlessQualified, "prior lost");
             storage.slots[1][20] ^= 4;
             require(repo.load(out) == ConfigState::Corrupt, "both corrupt");
         });
    test("unusable configurations are refused at save time", [] {
        MemoryConfigStorage storage;
        HandsFreeRepository repo(storage);
        auto bad = sampleConfig();
        bad.gestures.neutralRate = NAN;
        require(!repo.save(bad), "nan");
        bad = sampleConfig();
        bad.gestures.templates[1] = bad.gestures.templates[0];
        require(!repo.save(bad), "indistinct");
        require(storage.slots[0].empty() && storage.slots[1].empty(), "invalid record written");
        storage.slots[0] = encode(sampleConfig(), UINT32_MAX);
        require(!repo.save(sampleConfig()), "generation wrap");
    });
    test("telemetry strings are escape-free and the JSON is finite and bounded", [] {
        auto clean = [](const char* text) {
            for (const char* c = text; *c; ++c) {
                require(*c >= ' ' && *c <= '~' && *c != '"' && *c != '\\', "unsafe character");
            }
        };
        for (auto mode : {InteractionMode::Legacy, InteractionMode::HandsFree,
                          InteractionMode::ConfigInvalid}) {
            clean(name(mode));
        }
        for (auto state : {ConfigState::Missing, ConfigState::Valid, ConfigState::Corrupt,
                           ConfigState::Unsupported, ConfigState::OutOfBounds}) {
            clean(name(state));
        }
        for (auto state :
             {RecognizerState::WaitNeutral, RecognizerState::Armed, RecognizerState::Candidate}) {
            clean(name(state));
        }
        for (int reason = 0; reason <= int(Reject::Ambiguous); ++reason) {
            clean(name(static_cast<Reject>(reason)));
        }
        for (int phase = 0; phase <= int(TrainPhase::Failed); ++phase) {
            clean(name(static_cast<TrainPhase>(phase)));
        }
        for (GestureId id : {GestureId::PauseResume, GestureId::Drag}) {
            clean(name(id));
        }
        HF h;
        h.configStorage.slots[0] = encode(sampleConfig(), 1);
        h.configStorage.slots[0][40] ^= 8; // corrupt record
        h.boot();
        h.tick({NAN, INFINITY, 1e30f}); // invalid sample
        h.tick();
        char buffer[2048];
        const size_t length = handsFreeJson(buffer, sizeof buffer, h.s().handsFreeStatus());
        require(length > 100 && length < handsFreeJsonCapacity * 3 / 4,
                "the hands-free JSON needs headroom in the adapters' buffer");
        for (const char* bad : {"nan", "NaN", "inf", "Inf", "null"}) {
            require(!contains(buffer, bad), "nonfinite or null token");
        }
        require(handsFreeJson(buffer, 40, h.s().handsFreeStatus()) == 0, "overflow not detected");
        int depth = 0, quotes = 0;
        for (size_t i = 0; i < length; ++i) {
            depth += buffer[i] == '{' ? 1 : buffer[i] == '}' ? -1 : 0;
            quotes += buffer[i] == '"';
            require(depth >= 0, "brace order");
        }
        require(depth == 0 && quotes % 2 == 0, "unbalanced JSON");
    });
    test("new hands-free setup converts the profile to dwell and keeps the 84-byte format", [] {
        HF h;
        UserProfile before;
        require(h.repo.load(before) && !before.dwellEnabled, "precondition");
        h.setup();
        require(h.s().interaction == InteractionMode::HandsFree, "mode");
        require(h.s().profile.dwellEnabled, "dwell not enabled");
        UserProfile saved;
        require(h.repo.load(saved) && saved.dwellEnabled, "saved profile");
        for (unsigned slot = 0; slot < 2; ++slot) {
            const auto bytes = h.profileStorage.slots[slot];
            require(bytes.empty() || bytes.size() == 84, "profile size changed");
        }
        saved.dwellEnabled = false;
        require(encode(saved, 1) == encode(before, 1), "other profile fields changed");
        require(h.s().handsFreeStatus().config[0] == 'V', "config state");
    });
    test("an existing profile is untouched by boot, load, status, pause and unsaved training", [] {
        HF h(false);
        UserProfile custom;
        custom.gain[0] = 42;
        custom.dwellEnabled = false;
        require(h.repo.save(custom), "save");
        h.boot();
        const auto profileBytes = h.profileStorage.slots;
        const auto configBytes = h.configStorage.slots;
        h.quiet(1500);
        h.s().handsFreeStatus();
        require(h.s().setProfile(h.s().profile, false), "reload");
        h.s().pause();
        require(h.trainGesture(GestureId::PauseResume, "nod2"), "training");
        require(h.profileStorage.slots == profileBytes && h.configStorage.slots == configBytes,
                "storage changed without an explicit commit");
        require(h.s().interaction == InteractionMode::Legacy, "mode changed silently");
        h.boot();
        require(h.s().interaction == InteractionMode::Legacy, "staged data survived a reboot");
        near(h.s().profile.gain[0], 42);
    });
    test("missing configuration keeps legacy compatibility with the physical switch", [] {
        HF h;
        require(h.s().interaction == InteractionMode::Legacy, "mode");
        h.sw = false; // the enable switch is irrelevant in legacy mode
        h.quiet(500);
        require(h.s().resume(), "legacy resume");
        for (int i = 0; i < 8; ++i) {
            h.now += 10;
            h.s().tick({h.now, {0, 0, 0}, {0, 0, 1}, true}, h.now, true);
        }
        require(h.last().down, "legacy switch drag");
        h.quiet(500);
    });
    test("a corrupt configuration inhibits control until the helper acts", [] {
        HF h;
        h.configStorage.slots[0] = encode(sampleConfig(), 1);
        h.configStorage.slots[0][30] ^= 1;
        h.boot();
        require(h.s().interaction == InteractionMode::ConfigInvalid, "mode");
        h.quiet(600);
        require(!h.s().resume() && contains(h.s().activationBlocker(), "invalid"), "resumed");
        h.run(script("nod2"));
        h.quiet(300);
        require(h.s().state == SystemState::Ready && h.s().handsFreeStatus().executed == 0,
                "gesture accepted without configuration");
        require(h.s().useLegacyMode(), "helper legacy repair");
        require(h.s().interaction == InteractionMode::Legacy, "legacy");
        h.boot();
        require(h.s().interaction == InteractionMode::Legacy &&
                    std::strcmp(h.s().handsFreeStatus().config, "VALID") == 0,
                "repair not persistent");
    });
    test("unsupported and out-of-bounds configurations are inhibited and reported", [] {
        for (int kind = 0; kind < 2; ++kind) {
            HF h;
            auto bytes = encode(sampleConfig(), 1);
            if (kind == 0) {
                setWord(bytes, 1, 9);
            } else {
                setWord(bytes, enterWord[1], bitsOf(1000));
            }
            h.configStorage.slots[1] = reseal(bytes);
            h.boot();
            require(h.s().interaction == InteractionMode::ConfigInvalid, "mode");
            require(std::strcmp(h.s().handsFreeStatus().config,
                                kind == 0 ? "UNSUPPORTED" : "OUT_OF_BOUNDS") == 0,
                    "state name");
            h.quiet(600);
            require(!h.s().resume(), "resumed");
        }
    });
    test("a truncated configuration slot is corrupt, not silently ignored", [] {
        HF h;
        h.configStorage.slots[0] = encode(sampleConfig(), 1);
        h.configStorage.slots[0].resize(100);
        h.boot();
        require(h.s().interaction == InteractionMode::ConfigInvalid, "truncated record ignored");
    });
    test("explicit legacy mode survives a reboot and never silently re-enables", [] {
        HF h;
        h.setup();
        require(h.s().useLegacyMode(), "legacy");
        h.boot();
        require(h.s().interaction == InteractionMode::Legacy, "mode after reboot");
        require(h.s().handsFreeStatus().stored[0] && h.s().handsFreeStatus().stored[1],
                "trained patterns not kept");
        h.quiet(600);
        require(h.s().resume(), "legacy resume");
    });
    test("legacy constructor has no hands-free store and refuses setup commands", [] {
        MemoryStorage storage;
        ProfileRepository repo(storage);
        TestHID transport;
        System s(transport, repo);
        require(repo.save(UserProfile{}), "save");
        require(!s.trainStart(GestureId::PauseResume, 0) && !s.commitHandsFree() &&
                    !s.useLegacyMode(),
                "setup accepted without storage");
        require(s.interaction == InteractionMode::Legacy, "mode");
    });
    test("prior valid configuration survives a failed replacement and a reboot", [] {
        HF h;
        h.setup();
        const uint32_t before = h.s().handsFreeStatus().configId;
        require(before != 0, "identity");
        require(h.trainGesture(GestureId::Drag, "turn2"), "retrain");
        h.configStorage.failWrite = true;
        require(!h.s().commitHandsFree(), "failed save reported as success");
        require(h.s().diagnostics.faultCode == FaultCode::Storage, "storage fault not reported");
        h.configStorage.failWrite = false;
        require(h.s().handsFreeStatus().configId == before, "active configuration replaced");
        h.boot();
        require(h.s().handsFreeStatus().configId == before, "stored configuration replaced");
        require(h.s().interaction == InteractionMode::HandsFree, "mode lost");
    });
    test("failed profile write during conversion aborts without any change", [] {
        HF h;
        require(h.trainGesture(GestureId::PauseResume, "nod2") &&
                    h.trainGesture(GestureId::Drag, "tilt2"),
                "training");
        h.profileStorage.failWrite = true;
        require(!h.s().commitHandsFree(), "success reported");
        h.profileStorage.failWrite = false;
        require(h.configStorage.slots[0].empty() && h.configStorage.slots[1].empty(),
                "configuration written without a converted profile");
        require(h.s().interaction == InteractionMode::Legacy && !h.s().profile.dwellEnabled,
                "partial conversion");
        h.boot();
        require(h.s().interaction == InteractionMode::Legacy, "mode after reboot");
    });
    test("failed configuration write restores the prior profile", [] {
        HF h;
        require(h.trainGesture(GestureId::PauseResume, "nod2") &&
                    h.trainGesture(GestureId::Drag, "tilt2"),
                "training");
        h.configStorage.failWrite = true;
        require(!h.s().commitHandsFree(), "success reported");
        h.configStorage.failWrite = false;
        UserProfile saved;
        require(h.repo.load(saved) && !saved.dwellEnabled, "profile left converted");
        require(h.s().interaction == InteractionMode::Legacy && !h.s().profile.dwellEnabled,
                "partial enable in memory");
        h.boot();
        require(h.s().interaction == InteractionMode::Legacy, "mode after reboot");
    });
    test("interrupted multi-record setup never leaves hands-free partially enabled", [] {
        for (int writes = 0; writes <= 3; ++writes) {
            HF h;
            require(h.trainGesture(GestureId::PauseResume, "nod2") &&
                        h.trainGesture(GestureId::Drag, "tilt2"),
                    "training");
            h.budget.writes = writes; // power is lost after this many successful writes
            const bool committed = h.s().commitHandsFree();
            h.budget.writes = 1 << 30;
            h.boot(); // reboot on whatever reached storage
            const bool handsFree = h.s().interaction == InteractionMode::HandsFree;
            require(handsFree == committed, "mode disagrees with reported result");
            if (handsFree) {
                require(h.s().profile.dwellEnabled, "hands-free without dwell");
            } else {
                require(h.s().interaction == InteractionMode::Legacy, "invalid mode");
                require(h.configStorage.slots[0].empty() && h.configStorage.slots[1].empty(),
                        "orphan record");
            }
            require(committed == (writes >= 2), "commit point");
        }
    });
    test("profile reload without dwell blocks resume in hands-free mode", [] {
        HF h;
        h.setup();
        h.quiet(500);
        require(h.s().setProfile(UserProfile{}, false), "temporary generic profile");
        require(!h.s().resume() && contains(h.s().activationBlocker(), "dwell"), "resumed");
        auto saved = h.s().profile;
        saved.dwellEnabled = false;
        require(!h.s().setProfile(saved, true), "persistent dwell-off accepted in hands-free");
        UserProfile loaded;
        require(h.repo.load(loaded) && loaded.dwellEnabled, "saved profile changed");
    });

    // ------------------------------------------------------------- B: training
    test("training learns a valid template from repeated examples and validates it", [] {
        HF h;
        require(h.s().trainStart(GestureId::PauseResume, h.now), "start");
        require(h.s().state == SystemState::Training, "state");
        h.quiet(1100);
        require(h.s().trainer.phase == TrainPhase::Example, "rest");
        for (unsigned i = 0; i < start::trainExamples; ++i) {
            h.quiet(400);
            h.run(script("nod2"));
            h.quiet(500);
            require(h.s().trainer.accepted == i + 1, "example not accepted");
        }
        require(h.s().trainer.phase == TrainPhase::Validate, "analysis");
        const GestureTemplate learned = h.s().trainer.candidate;
        require(learned.valid() && learned.strokes == 4, "learned template");
        require(learned.axis[0] == 1 && learned.sign[0] == 1 && learned.sign[1] == -1, "pattern");
        require(!h.s().trainAccept(), "accepted before validation");
        h.quiet(400);
        h.run(script("nod2"));
        h.quiet(400);
        require(h.s().trainer.phase == TrainPhase::Ready && h.s().trainer.validated, "validation");
        require(h.s().trainAccept(), "accept");
        require(h.s().handsFreeStatus().staged[0] && !h.s().handsFreeStatus().stored[0],
                "accepted pattern must be staged, not stored");
        require(h.configStorage.slots[0].empty(), "persisted before commit");
    });
    test("training rejects too few strokes and allows a retry", [] {
        HF h;
        require(h.s().trainStart(GestureId::PauseResume, h.now), "start");
        h.quiet(1100);
        h.quiet(400);
        std::vector<Rates> single;
        sim::stroke(single, 1, 1, 70, 160);
        h.run(single);
        h.quiet(500);
        require(h.s().trainer.accepted == 0 && h.s().trainer.rejects == 1,
                "single stroke accepted");
        require(contains(h.s().trainer.reason, "at least two"), "reason");
        h.quiet(400);
        h.run(script("nod2"));
        h.quiet(500);
        require(h.s().trainer.accepted == 1, "retry failed");
    });
    test("training rejects excessive rest noise", [] {
        HF h;
        require(h.s().trainStart(GestureId::PauseResume, h.now), "start");
        for (int i = 0; i < 120; ++i) {
            h.tick({(i % 2) ? 9.f : -9.f, 0, 0});
        }
        require(h.s().trainer.phase == TrainPhase::Failed, "noise accepted");
        require(contains(h.s().trainer.reason, "noise"), "reason");
    });
    test("training rejects a rest period that is not at rest", [] {
        HF h;
        require(h.s().trainStart(GestureId::PauseResume, h.now), "start");
        for (int i = 0; i < 120; ++i) {
            h.tick({0, 12, 0});
        }
        require(h.s().trainer.phase == TrainPhase::Failed, "motion accepted as rest");
    });
    test("training rejects insufficient motion", [] {
        HF h;
        require(h.s().trainStart(GestureId::PauseResume, h.now), "start");
        h.quiet(1100);
        h.quiet(400);
        h.run(sim::cycles(1, 2, 18, 160, 20)); // above the capture threshold, below the minimum
        h.quiet(500);
        require(h.s().trainer.accepted == 0 && contains(h.s().trainer.reason, "too small"),
                "weak example accepted");
    });
    test("training rejects movement that is not on a single axis", [] {
        HF h;
        require(h.s().trainStart(GestureId::PauseResume, h.now), "start");
        h.quiet(1100);
        h.quiet(400);
        std::vector<Rates> diagonal;
        for (int sign : {1, -1}) {
            std::vector<Rates> a, b;
            sim::stroke(a, 1, sign, 70, 160);
            sim::stroke(b, 0, sign, 70, 160);
            for (size_t i = 0; i < a.size(); ++i) {
                diagonal.push_back({b[i][0], a[i][1], 0});
            }
        }
        h.run(diagonal);
        h.quiet(500);
        require(h.s().trainer.accepted == 0 && contains(h.s().trainer.reason, "single axis"),
                "diagonal example accepted");
    });
    test("training rejects inconsistent examples", [] {
        HF h;
        require(h.s().trainStart(GestureId::PauseResume, h.now), "start");
        h.quiet(1100);
        h.quiet(400);
        h.run(script("nod2"));
        h.quiet(500);
        require(h.s().trainer.accepted == 1, "first");
        h.quiet(400);
        h.run(script("tilt2")); // a different pattern
        h.quiet(500);
        require(h.s().trainer.accepted == 1 && contains(h.s().trainer.reason, "differs"),
                "different pattern accepted");
    });
    test("training fails after too many rejected attempts", [] {
        HF h;
        require(h.s().trainStart(GestureId::PauseResume, h.now), "start");
        h.quiet(1100);
        for (unsigned i = 0; i <= start::trainMaxRejects; ++i) {
            h.quiet(400);
            std::vector<Rates> single;
            sim::stroke(single, 1, 1, 70, 160);
            h.run(single);
            h.quiet(500);
        }
        require(h.s().trainer.phase == TrainPhase::Failed, "endless retries");
    });
    for (const char* kind : {"NaN", "infinity", "negative infinity", "extreme"}) {
        test(std::string("training cancels through the safe path on ") + kind + " data", [kind] {
            HF h;
            require(h.s().trainStart(GestureId::PauseResume, h.now), "start");
            h.quiet(300);
            float bad = std::strcmp(kind, "NaN") == 0                 ? NAN
                        : std::strcmp(kind, "infinity") == 0          ? INFINITY
                        : std::strcmp(kind, "negative infinity") == 0 ? -INFINITY
                                                                      : 10000.f;
            h.now += 10;
            h.s().tick({h.now, {bad, 0, 0}, {0, 0, 1}, true}, h.now, false);
            require(h.s().state == SystemState::SafeState, "safe state");
            require(h.s().trainer.phase == TrainPhase::Idle, "training continued");
            require(h.released(), "output not released");
            require(std::strcmp(h.s().handsFreeStatus().config, "MISSING") == 0, "config touched");
        });
    }
    test("training cancels through the safe path on a timing fault", [] {
        HF h;
        require(h.s().trainStart(GestureId::PauseResume, h.now), "start");
        h.quiet(300);
        h.now += 150; // longer than the 100 ms sensor timeout
        h.s().tick({h.now, {0, 0, 0}, {0, 0, 1}, true}, h.now, false);
        require(h.s().state == SystemState::SafeState && h.s().trainer.phase == TrainPhase::Idle,
                "timing fault ignored");
    });
    test("training with too few samples fails instead of accepting sparse data", [] {
        GestureTrainer trainer;
        trainer.begin(GestureId::PauseResume, 0, GestureTemplate{});
        for (uint32_t t = 100; t <= 1200; t += 100) {
            trainer.tick({0, 0, 0}, t);
        }
        require(trainer.phase == TrainPhase::Failed && contains(trainer.reason, "few"), "sparse");
        GestureTrainer direct;
        direct.begin(GestureId::Drag, 0, GestureTemplate{});
        direct.tick({NAN, 0, 0}, 10);
        require(direct.phase == TrainPhase::Failed, "nan");
    });
    test("training rejects patterns indistinguishable from the other gesture", [] {
        for (const char* second : {"nod2", "nod1", "nod3"}) {
            HF h;
            require(h.trainGesture(GestureId::PauseResume, "nod2"), "first pattern");
            require(h.s().trainStart(GestureId::Drag, h.now), "start");
            h.quiet(1100);
            for (unsigned i = 0; i < start::trainExamples; ++i) {
                h.quiet(400);
                h.run(script(second));
                h.quiet(500);
            }
            require(h.s().trainer.phase == TrainPhase::Failed, second);
            require(contains(h.s().trainer.reason, "similar"), "reason");
            require(!h.s().handsFreeStatus().staged[1], "staged");
        }
    });
    test("training cancel preserves the prior configuration", [] {
        HF h;
        h.setup();
        const uint32_t before = h.s().handsFreeStatus().configId;
        require(h.s().trainStart(GestureId::Drag, h.now), "start");
        h.quiet(1500);
        h.s().trainCancel();
        require(h.s().state == SystemState::Ready && h.s().trainer.phase == TrainPhase::Idle,
                "state");
        require(h.s().handsFreeStatus().configId == before, "configuration changed");
        require(!h.s().handsFreeStatus().staged[1], "partial pattern staged");
        h.boot();
        require(h.s().handsFreeStatus().configId == before, "stored configuration changed");
    });
    test("training inhibits and releases output immediately", [] {
        HF h;
        h.active();
        beginDrag(h);
        const size_t before = h.transport.reports.size();
        require(h.s().trainStart(GestureId::Drag, h.now), "start");
        require(h.transport.reports.size() > before && h.released(), "no immediate release");
        require(h.s().state == SystemState::Training && !h.s().dragging, "state");
        const size_t mark = h.transport.reports.size();
        h.run(hold(0, 60, 500));
        require(quietSince(h, mark), "output during training");
    });
    test("recalibration while hands-free keeps dwell enabled", [] {
        HF h;
        h.setup();
        h.quiet(300);
        h.s().calibrate(h.now);
        for (int i = 0; i < 1100 && h.s().calibration.phase != CalPhase::Complete &&
                        h.s().calibration.phase != CalPhase::Failed;
             ++i) {
            h.now += 10;
            Rates rate{};
            const float noise = (int((h.now / 10) % 7) - 3) * .05f;
            rate = {noise, noise, noise};
            switch (h.s().calibration.phase) {
            case CalPhase::Left:
                rate[0] -= 12;
                break;
            case CalPhase::Right:
                rate[0] += 24;
                break;
            case CalPhase::Up:
                rate[1] -= 10;
                break;
            case CalPhase::Down:
                rate[1] += 20;
                break;
            default:
                break;
            }
            h.s().setControlSwitch(true, h.now);
            h.s().tick({h.now, {rate[0], rate[1], rate[2]}, {0, 0, 1}, true}, h.now, false);
        }
        require(h.s().calibration.phase == CalPhase::Complete, "calibration");
        UserProfile saved;
        require(h.repo.load(saved) && saved.dwellEnabled, "dwell lost after recalibration");
        require(h.s().interaction == InteractionMode::HandsFree, "mode");
    });

    // ------------------------------------------------ D: interaction conflicts
    test("recognition inhibits pointing, scrolling and dwell", [] {
        // A held roll tilt keeps scrolling after the head stops moving; the pitch pattern then
        // starts while the scroll angle is still beyond the threshold.
        auto prepare = [](HF& h) {
            auto profile = h.s().profile;
            profile.scrollThreshold = 5;
            profile.scrollGain = 3;
            require(h.s().setProfile(profile), "profile");
            h.quiet(500);
            require(h.s().resume(), "resume");
            moveAway(h);
            h.run(hold(2, -60, 1000)); // slow tilt: not a pattern, builds the scroll angle
            h.quiet(400);
        };
        HF legacy;
        prepare(legacy);
        long legacyWheel = 0, legacyDy = 0;
        for (const auto& rate : script("nod2")) {
            legacy.tick(rate);
            legacyWheel += std::abs(int(legacy.last().wheel));
        }
        require(legacyWheel > 0, "precondition: the held tilt does not scroll unsuppressed");
        HF plain; // the same pattern without the tilt moves the pointer when nothing suppresses it
        plain.quiet(500);
        require(plain.s().resume(), "resume");
        plain.quiet(400);
        for (const auto& rate : script("nod2")) {
            plain.tick(rate);
            legacyDy += std::abs(int(plain.last().dy));
        }
        require(legacyDy > 0, "precondition: pattern does not move the pointer unsuppressed");
        HF h;
        h.active();
        prepare(h);
        h.quiet(0);
        bool suppressed = false;
        for (const auto& rate : script("nod2")) {
            const size_t mark = h.transport.reports.size();
            h.tick(rate);
            if (h.s().recognizer.suppressing()) {
                suppressed = true;
                for (size_t i = mark; i < h.transport.reports.size(); ++i) {
                    const Report& r = h.transport.reports[i];
                    require(!anyMovement(r) && !r.down, "output while recognizing");
                }
                require(h.s().selection.dwell == DwellState::Idle, "dwell during recognition");
            }
        }
        h.quiet(300);
        require(suppressed, "never recognizing");
    });
    test("no delayed dwell click after cancelled recognition", [] {
        HF h;
        h.active();
        moveAway(h);
        h.quiet(900); // dwell progress is well under way but not complete
        require(h.s().selection.dwell == DwellState::Progress, "precondition");
        std::vector<Rates> single;
        sim::stroke(single, 1, 1, 70, 160); // a partial pattern, rejected after the gap limit
        h.run(single);
        const size_t cancellationsBefore = h.s().selection.cancellations;
        unsigned afterReject = 0;
        const size_t mark = h.transport.reports.size();
        bool rejected = false;
        for (int i = 0; i < 400; ++i) {
            h.tick();
            rejected = rejected || h.s().handsFreeStatus().rejected == 1;
            if (rejected) {
                ++afterReject;
                if (clicksSince(h, mark) > 0) {
                    break;
                }
            }
        }
        require(rejected && h.s().selection.cancellations >= cancellationsBefore, "no rejection");
        require(clicksSince(h, mark) == 1, "dwell never restarted");
        require(afterReject >= 120, "click came from the discarded dwell progress");
    });
    test("cancelled recognition does not replay accumulated pointer movement", [] {
        HF h;
        h.active();
        moveAway(h);
        h.quiet(400); // neutral again, armed
        const size_t mark = h.transport.reports.size();
        // A fast pitch stroke held too long: rejected as too slow, discarded while suppressed.
        h.run(hold(1, 60, 700));
        const size_t windowEnd = h.transport.reports.size();
        h.quiet(200);
        long suppressedWindow = 0;
        for (size_t i = mark; i < windowEnd; ++i) {
            suppressedWindow += std::abs(int(h.transport.reports[i].dy));
        }
        long afterwards = 0;
        for (size_t i = windowEnd; i < h.transport.reports.size(); ++i) {
            afterwards += std::abs(int(h.transport.reports[i].dy));
        }
        require(h.s().handsFreeStatus().rejected >= 1, "precondition: not rejected");
        require(afterwards < 60, "movement replayed after cancellation");
        require(suppressedWindow > 0, "no pointing at all");
    });
    test("normal pointing and scrolling examples never execute the trained commands", [] {
        HF h;
        h.active();
        const uint32_t executed = h.s().handsFreeStatus().executed;
        for (unsigned axis = 0; axis < 3; ++axis) {
            for (float hz : {.3f, .7f, 1.1f}) {
                std::vector<Rates> motion(900, Rates{});
                for (size_t i = 0; i < motion.size(); ++i) {
                    motion[i][axis] = 35.f * std::sin(6.2831853f * hz * float(i) * .01f);
                }
                h.run(motion);
                h.quiet(500);
            }
        }
        h.run(hold(2, 25, 1500));
        h.quiet(500);
        require(h.s().handsFreeStatus().executed == executed, "command executed");
        require(h.s().state == SystemState::Active && !h.s().dragging, "state changed");
    });
    test("pause gesture takes priority over drag and dwell", [] {
        HF h;
        h.active();
        beginDrag(h);
        perform(h, "nod2");
        require(h.s().state == SystemState::Paused && !h.s().dragging, "pause lost to drag");
        require(h.released(), "button still down");
        require(h.s().handsFreeStatus().refused == 0, "refused");
    });
    test("helper pause outranks drag and a dwell in progress", [] {
        HF h;
        h.active();
        moveAway(h);
        h.quiet(900);
        require(h.s().selection.dwell == DwellState::Progress, "precondition");
        const size_t mark = h.transport.reports.size();
        h.s().pause();
        h.quiet(1500);
        require(h.s().state == SystemState::Paused && clicksSince(h, mark) == 0,
                "click after pause");
        require(h.released(), "released");
    });
    test("simultaneous or ambiguous candidates execute no unsafe action", [] {
        HF h;
        h.active();
        std::vector<Rates> both;
        for (int sign : {1, -1, 1, -1}) {
            std::vector<Rates> pitch, roll;
            sim::stroke(pitch, 1, sign, 70, 160);
            sim::stroke(roll, 2, sign, 70, 160);
            for (size_t i = 0; i < pitch.size(); ++i) {
                both.push_back({0, pitch[i][1], roll[i][2]});
            }
            sim::neutral(both, 20);
        }
        h.run(both);
        h.quiet(500);
        require(h.s().handsFreeStatus().executed == 0, "executed");
        require(h.s().state == SystemState::Active && !h.s().dragging, "unsafe action");
    });
    test("recognition beginning during a drag holds the button and suppresses movement", [] {
        HF h;
        h.active();
        beginDrag(h);
        const size_t mark = h.transport.reports.size();
        std::vector<Rates> partial;
        sim::stroke(partial, 1, 1, 70, 160); // start of the pause pattern, then nothing
        bool seen = false;
        for (const auto& rate : partial) {
            h.tick(rate);
            if (h.s().recognizer.suppressing()) {
                seen = true;
                require(h.last().down && !anyMovement(h.last()), "drag broken during candidate");
            }
        }
        require(seen, "candidate never opened");
        h.quiet(500); // gap timeout rejects the candidate; the drag continues
        require(h.s().dragging && h.last().down, "drag lost after rejection");
        require(h.s().handsFreeStatus().rejected == 1, "not rejected");
        h.run(hold(0, 30, 200));
        bool moved = false;
        for (size_t i = mark; i < h.transport.reports.size(); ++i) {
            moved = moved || (h.transport.reports[i].dx != 0 && h.transport.reports[i].down);
        }
        require(moved, "pointer did not resume dragging");
    });

    // ------------------------------------------------------------------- E: drag
    test("drag gesture starts and ends a drag with a dwell lockout afterwards", [] {
        HF h;
        h.active();
        beginDrag(h);
        h.run(hold(0, 30, 300));
        require(h.last().down || h.s().dragging, "not dragging while moving");
        perform(h, "tilt2");
        require(!h.s().dragging && !h.last().down, "drag not released");
        require(h.s().selection.dwell == DwellState::Lockout, "no lockout after the drag");
        const size_t mark = h.transport.reports.size();
        h.quiet(3000);
        require(clicksSince(h, mark) == 0, "click at the drop point without movement");
    });
    test("no dwell clicks while dragging", [] {
        HF h;
        h.active();
        moveAway(h);
        beginDrag(h);
        const size_t mark = h.transport.reports.size();
        h.quiet(4000);
        require(h.s().dragging, "drag ended");
        for (size_t i = mark; i < h.transport.reports.size(); ++i) {
            require(h.transport.reports[i].down, "button released or pulsed during drag");
        }
    });
    {
        struct Cause {
            const char* label;
            std::function<void(HF&)> apply;
            bool deliverable; // a release report can be delivered immediately
        };
        const std::vector<Cause> causes = {
            {"helper pause", [](HF& h) { h.s().pause(); }, true},
            {"pause gesture", [](HF& h) { perform(h, "nod2"); }, true},
            {"enable switch OFF",
             [](HF& h) {
                 h.sw = false;
                 h.s().setControlSwitch(false, h.now);
             },
             true},
            {"calibration", [](HF& h) { h.s().calibrate(h.now); }, true},
            {"training", [](HF& h) { h.s().trainStart(GestureId::Drag, h.now); }, true},
            {"profile change",
             [](HF& h) {
                 auto changed = h.s().profile;
                 changed.gain[0] = 33;
                 require(h.s().setProfile(changed), "profile");
             },
             true},
            {"configuration change", [](HF& h) { require(h.s().commitHandsFree(), "commit"); },
             true},
            {"sensor fault",
             [](HF& h) {
                 h.now += 10;
                 h.s().tick({h.now, {NAN, 0, 0}, {0, 0, 1}, true}, h.now, false);
             },
             true},
            {"timing fault",
             [](HF& h) {
                 h.now += 150;
                 h.s().tick({h.now, {0, 0, 0}, {0, 0, 1}, true}, h.now, false);
             },
             true},
            {"transport failure",
             [](HF& h) {
                 h.transport.fail = true;
                 h.tick();
             },
             false},
            {"disconnect",
             [](HF& h) {
                 h.transport.online = false;
                 h.tick();
             },
             false},
        };
        for (const Cause& cause : causes) {
            test(std::string("drag is released immediately on ") + cause.label, [cause] {
                HF h;
                h.active();
                beginDrag(h);
                const size_t before = h.transport.reports.size();
                cause.apply(h);
                require(!h.s().dragging, "drag state survived");
                require(h.s().state != SystemState::Active, "still active");
                if (cause.deliverable) {
                    require(h.transport.reports.size() > before && h.released(),
                            "no immediate release report");
                }
                h.transport.fail = false;
                h.transport.online = true;
                h.sw = true;
                h.quiet(600);
                require(!h.last().down && !h.s().dragging, "re-pressed after the cause ended");
                require(h.s().state != SystemState::Active, "reactivated automatically");
            });
        }
    }
    test("no automatic re-press after fault recovery", [] {
        HF h;
        h.active();
        beginDrag(h);
        h.now += 10;
        h.s().tick({h.now, {NAN, 0, 0}, {0, 0, 1}, true}, h.now, false);
        h.quiet(300);
        require(h.s().state == SystemState::Ready && !h.s().dragging, "recovery");
        const size_t mark = h.transport.reports.size();
        require(h.s().resume(), "resume");
        h.quiet(3000);
        for (size_t i = mark; i < h.transport.reports.size(); ++i) {
            require(!h.transport.reports[i].down, "button pressed after recovery");
        }
    });

    // --------------------------------------------- F: enable switch and recovery
    test("switch OFF always inhibits output", [] {
        HF h;
        h.active();
        h.run(hold(0, 40, 200));
        bool moved = false;
        for (const auto& r : h.transport.reports) {
            moved = moved || r.dx != 0;
        }
        require(moved, "precondition: no output while ON");
        h.sw = false;
        h.tick({40, 0, 0});
        require(h.s().state == SystemState::Paused, "not paused");
        const size_t mark = h.transport.reports.size();
        h.run(hold(0, 40, 1000));
        require(quietSince(h, mark), "output while OFF");
        require(!h.s().resume() && contains(h.s().activationBlocker(), "OFF"), "resumed while OFF");
    });
    test("switch OFF releases without waiting for another sensor sample", [] {
        HF h;
        h.active();
        beginDrag(h);
        const size_t before = h.transport.reports.size();
        h.s().setControlSwitch(false, h.now + 1); // no tick follows
        require(h.transport.reports.size() > before && h.released(), "no immediate release");
        require(h.s().state == SystemState::Paused && !h.s().dragging, "state");
    });
    test("switch ON returns to an inhibited state, never ACTIVE", [] {
        HF h;
        h.active();
        h.sw = false;
        h.quiet(200);
        h.sw = true;
        h.quiet(1000);
        require(h.s().state == SystemState::Paused, "reactivated by the switch");
        require(h.s().handsFreeStatus().permitted, "not permitted after ON");
        const size_t mark = h.transport.reports.size();
        h.run(hold(0, 40, 500));
        require(quietSince(h, mark), "output without an explicit resume");
    });
    test("switch bounce does not produce unintended activation", [] {
        HF h;
        h.active();
        h.sw = false;
        h.tick();
        require(h.s().state == SystemState::Paused, "paused");
        uint32_t t = h.now;
        for (int i = 0; i < 20; ++i) { // 5 ms chatter, never stable for 30 ms
            t += 5;
            h.s().setControlSwitch(i % 2 == 0, t);
            require(!h.s().handsFreeStatus().permitted || i == 19, "permitted during chatter");
        }
        t += 5;
        h.s().setControlSwitch(false, t);
        require(!h.s().handsFreeStatus().permitted, "chatter ended OFF but permitted");
        h.s().setControlSwitch(true, t + 1);
        h.s().setControlSwitch(true, t + 20);
        require(!h.s().handsFreeStatus().permitted, "ON accepted before the debounce window");
        h.s().setControlSwitch(true, t + 31);
        require(h.s().handsFreeStatus().permitted, "stable ON refused");
        require(h.s().state == SystemState::Paused, "bounce activated control");
    });
    test("a brief OFF flicker while active pauses and does not resume by itself", [] {
        HF h;
        h.active();
        h.sw = false;
        h.tick();
        h.sw = true;
        h.quiet(1000);
        require(h.s().state == SystemState::Paused, "auto-resume after flicker");
    });
    test("boot with the switch ON remains inhibited", [] {
        HF h;
        h.setup();
        h.boot();
        const size_t mark = h.transport.reports.size();
        h.run(hold(0, 30, 3000)); // motion while the switch is ON but nothing resumed it
        require(h.s().state == SystemState::Ready, "state");
        require(h.s().handsFreeStatus().permitted, "switch not recognised");
        require(quietSince(h, mark), "output before an explicit resume");
    });
    test("resume gesture cannot override the switch being OFF", [] {
        HF h;
        h.setup();
        h.quiet(500);
        h.sw = false;
        h.quiet(500);
        h.run(script("nod2"));
        h.quiet(400);
        require(h.s().state != SystemState::Active, "activated while OFF");
        require(h.s().handsFreeStatus().executed == 0, "gesture accepted while disabled");
    });
    test("resume gesture cannot override faults", [] {
        HF h;
        h.active();
        h.transport.online = false;
        h.tick();
        require(h.s().state == SystemState::SafeState, "fault");
        h.transport.online = true;
        for (int i = 0; i < 10; ++i) { // too few healthy samples for qualification
            h.tick();
        }
        h.run(script("nod2"));
        h.quiet(200);
        require(h.s().state != SystemState::Active, "resumed during recovery");
        require(h.s().handsFreeStatus().executed == 0, "gesture accepted during recovery");
    });
    test("resume works after healthy qualification and a valid intentional gesture", [] {
        HF h;
        h.active();
        h.now += 10;
        h.s().tick({h.now, {NAN, 0, 0}, {0, 0, 1}, true}, h.now, false);
        h.quiet(1000);
        require(h.s().state == SystemState::Ready, "recovered");
        perform(h, "nod2");
        require(h.s().state == SystemState::Active, "resume gesture did not resume");
        perform(h, "nod2");
        require(h.s().state == SystemState::Paused, "pause gesture");
        perform(h, "nod2");
        require(h.s().state == SystemState::Active, "second resume (qualification retained)");
    });
    test("a refused resume gesture is counted and changes nothing", [] {
        HF h;
        h.setup();
        h.quiet(500);
        require(h.s().setProfile(UserProfile{}, false), "temporary profile without dwell");
        h.quiet(500);
        h.run(script("nod2"));
        h.quiet(300);
        require(h.s().state != SystemState::Active, "resumed");
        require(h.s().handsFreeStatus().refused == 1, "refusal not counted");
    });
    test("an unconfigured switch cannot silently enable output; qualification can", [] {
        HF h;
        h.setup();
        h.boot();
        h.s().configureEnableInput(false); // no switch wired
        h.quiet(600);
        require(!h.s().resume() && contains(h.s().activationBlocker(), "not configured"),
                "resumed");
        h.run(script("nod2"));
        h.quiet(300);
        require(h.s().state != SystemState::Active, "gesture enabled output without a switch");
        h.s().stageSwitchless(true);
        require(h.s().commitHandsFree(), "qualification commit");
        h.quiet(600);
        require(h.s().resume(), "qualified switchless configuration refused");
        h.boot();
        h.s().configureEnableInput(false);
        h.quiet(600);
        require(h.s().handsFreeStatus().switchless && h.s().resume(),
                "qualification not persistent");
    });
    test("reconnect and fault recovery never activate hands-free control", [] {
        HF h;
        h.active();
        h.transport.online = false;
        h.tick();
        h.transport.online = true;
        h.quiet(2000);
        require(h.s().state == SystemState::Ready, "reconnect activated control");
        require(h.released(), "button down after reconnect");
    });
    test("drag gesture is ignored unless control is active", [] {
        HF h;
        h.setup();
        h.quiet(500);
        h.run(script("tilt2"));
        h.quiet(300);
        require(!h.s().dragging && h.s().handsFreeStatus().refused == 1, "drag outside ACTIVE");
    });

    // ------------------------------------------------------- additional coverage
    test("enable gate: unknown input is OFF, OFF is immediate, ON needs a stable window", [] {
        EnableGate gate;
        gate.configure(true, false);
        require(!gate.permitted(), "unknown input permitted");
        require(!gate.update(true, 100) && !gate.update(true, 120), "ON accepted too early");
        require(gate.update(true, 130), "stable ON refused");
        require(!gate.update(false, 131) && !gate.permitted(), "OFF not immediate");
        require(!gate.update(true, 140) && !gate.update(true, 169), "restart of the window");
        require(gate.update(true, 170), "second window");
        gate.configure(true, false);
        require(!gate.permitted(), "reconfigure kept the old state");
    });
    test("enable gate: an absent switch never permits unless qualified", [] {
        EnableGate gate;
        gate.configure(false, false);
        require(!gate.update(true, 0) && !gate.update(true, 1000), "raw level honoured");
        require(gate.blocked() != nullptr && contains(gate.blocked(), "not configured"), "reason");
        gate.setSwitchless(true);
        require(gate.permitted() && gate.blocked() == nullptr, "qualified alternative");
        gate.configure(true, true);
        require(!gate.permitted(), "a present switch must still be turned ON");
    });
    test("legacy mode save failure is reported and leaves the mode unchanged", [] {
        HF h;
        h.setup();
        h.configStorage.failWrite = true;
        require(!h.s().useLegacyMode(), "success reported");
        require(h.s().diagnostics.faultCode == FaultCode::Storage, "fault");
        require(h.s().interaction == InteractionMode::HandsFree, "mode changed in memory");
        h.configStorage.failWrite = false;
        h.boot();
        require(h.s().interaction == InteractionMode::HandsFree, "mode changed on disk");
    });
    test("a torn first configuration write fails closed and a helper can repair it", [] {
        HF h;
        require(h.trainGesture(GestureId::PauseResume, "nod2") &&
                    h.trainGesture(GestureId::Drag, "tilt2"),
                "training");
        h.configStorage.tearWrite = true;
        require(!h.s().commitHandsFree(), "torn write accepted");
        h.configStorage.tearWrite = false;
        UserProfile saved;
        require(h.repo.load(saved) && !saved.dwellEnabled, "profile left converted");
        require(h.s().interaction == InteractionMode::Legacy, "in-memory mode changed");
        h.boot();
        // The half-written record cannot be trusted and nothing older exists: inhibited, not
        // enabled.
        require(h.s().interaction == InteractionMode::ConfigInvalid, "torn record not detected");
        h.quiet(600);
        require(!h.s().resume(), "resumed with an unreadable configuration");
        require(h.s().useLegacyMode(), "helper repair");
        h.quiet(600);
        require(h.s().interaction == InteractionMode::Legacy && h.s().resume(), "repair");
    });
    test("training and commit need a profile, a healthy idle system and both patterns", [] {
        HF none(false);
        require(!none.s().trainStart(GestureId::PauseResume, none.now), "training without profile");
        require(!none.s().commitHandsFree(), "commit without profile");
        HF h;
        require(!h.s().commitHandsFree(), "commit without any pattern");
        require(h.trainGesture(GestureId::PauseResume, "nod2"), "one pattern");
        require(!h.s().commitHandsFree(), "commit with one pattern");
        h.now += 10;
        h.s().tick({h.now, {NAN, 0, 0}, {0, 0, 1}, true}, h.now, false);
        require(h.s().state == SystemState::SafeState, "fault");
        require(!h.s().trainStart(GestureId::Drag, h.now), "training during a fault");
        require(!h.s().commitHandsFree() && !h.s().useLegacyMode(), "setup during a fault");
    });
    test("gestures are not recognised while calibrating or training", [] {
        HF h;
        h.setup();
        h.quiet(500);
        h.s().calibrate(h.now);
        h.quiet(400);
        h.run(script("nod2"));
        h.quiet(300);
        require(h.s().state == SystemState::Calibrating, "calibration interrupted");
        require(h.s().handsFreeStatus().executed == 0, "gesture accepted while calibrating");
        h.s().cancelCalibration();
        require(h.s().trainStart(GestureId::Drag, h.now), "training");
        h.quiet(400);
        h.run(script("nod2"));
        h.quiet(300);
        require(h.s().handsFreeStatus().executed == 0, "gesture accepted while training");
    });
    test("after a resume gesture the user must move before the first dwell click", [] {
        HF h;
        h.active();
        const size_t mark = h.transport.reports.size();
        h.quiet(4000); // still after resuming: no click without meaningful movement
        require(clicksSince(h, mark) == 0, "click without movement after resume");
        moveAway(h);
        const size_t before = h.transport.reports.size();
        h.quiet(2500);
        require(clicksSince(h, before) == 1, "dwell does not click after movement");
    });
    test("dwell click is exactly one press and release pair", [] {
        HF h;
        h.active();
        moveAway(h);
        const size_t mark = h.transport.reports.size();
        h.quiet(3000);
        bool pairFound = false;
        for (size_t i = std::max<size_t>(mark, 1); i < h.transport.reports.size(); ++i) {
            if (h.transport.reports[i - 1].down && !h.transport.reports[i].down) {
                pairFound = true;
            }
        }
        require(pairFound && clicksSince(h, mark) == 1, "press without release or repeats");
        require(h.released(), "button left down");
    });
    test("staged patterns are discarded by a reboot and never persisted implicitly", [] {
        HF h;
        require(h.trainGesture(GestureId::PauseResume, "nod2"), "stage");
        require(h.s().handsFreeStatus().staged[0], "staged");
        h.boot();
        require(!h.s().handsFreeStatus().staged[0] && !h.s().handsFreeStatus().stored[0],
                "staged pattern survived");
    });
    test("replaying the same samples through two systems gives identical output", [] {
        std::vector<Rates> motion;
        sim::neutral(motion, 500);
        for (const auto& rate : script("nod2")) {
            motion.push_back(rate);
        }
        sim::neutral(motion, 400);
        HF a, b;
        a.setup();
        b.setup();
        a.s().pause();
        b.s().pause();
        require(a.transport.reports.size() > 0, "setup produced no reports");
        const size_t markA = a.transport.reports.size(), markB = b.transport.reports.size();
        a.run(motion);
        b.run(motion);
        require(a.transport.reports.size() - markA == b.transport.reports.size() - markB,
                "report counts differ");
        for (size_t i = 0; i < a.transport.reports.size() - markA; ++i) {
            const Report &x = a.transport.reports[markA + i], &y = b.transport.reports[markB + i];
            require(x.dx == y.dx && x.dy == y.dy && x.wheel == y.wheel && x.down == y.down,
                    "reports differ");
        }
        require(a.s().state == b.s().state && a.s().state == SystemState::Active, "end state");
    });

    // ------------------------------------------------ J: pointing versus candidates
    // The first stroke of a pattern looks like the start of pointing in that direction. These cases
    // reproduce what the Lab block showed (keyboard pointing at 20 deg/s opened rejected
    // candidates) with the learned templates, and pin down the cost and the safety outcome.
    test("held pointing in a pattern's first-stroke direction opens one bounded rejected candidate",
         [] {
             HF h;
             h.active();
             const auto input = held(1, 20.f, 1000); // pitch+, as the S key does
             const auto base = legacyActive();
             const Pointing hf = pointing(h, input), plain = pointing(*base, input);
             require(hf.candidates == 1 && hf.rejected == 1, "one candidate, one rejection");
             require(hf.lastReject == Reject::TooSlow, "a held movement is rejected as too slow");
             require(hf.executed == 0, "a command executed on pointing");
             require(hf.suppressedMs > 0 && hf.longestMs <= maxSuppressionMs,
                     "suppression unbounded");
             require(!hf.dragSeen && !hf.leftActive, "drag or pause on pointing");
             require(h.s().recognizer.executed == 0 &&
                         h.s().interaction == InteractionMode::HandsFree,
                     "state changed");
             // The cost is real: movement during the window is gone, and nothing is replayed after
             // it.
             require(plain.dy - hf.dy > 100.f, "no pointer movement was discarded");
             const float discardedShare = (plain.dy - hf.dy) / plain.dy;
             require(discardedShare < .4f, "discard exceeds the suppression window");
             float biggestHF = 0, biggestPlain = 0;
             for (size_t i = h.transport.reports.size() - 100; i < h.transport.reports.size();
                  ++i) {
                 biggestHF = std::max(biggestHF, std::abs(float(h.transport.reports[i].dy)));
             }
             for (size_t i = base->transport.reports.size() - 100;
                  i < base->transport.reports.size(); ++i) {
                 biggestPlain =
                     std::max(biggestPlain, std::abs(float(base->transport.reports[i].dy)));
             }
             require(biggestHF <= biggestPlain, "suppressed movement was replayed as a burst");
         });
    test("the suppression window is the same at every pointing speed, the discard grows with speed",
         [] {
             float previous = 0;
             for (float rate : {20.f, 30.f, 40.f, 60.f}) {
                 HF h;
                 h.active();
                 const auto base = legacyActive();
                 const auto input = held(1, rate, 1000);
                 const Pointing hf = pointing(h, input), plain = pointing(*base, input);
                 require(hf.candidates == 1 && hf.executed == 0 && hf.longestMs <= maxSuppressionMs,
                         "speed changed the outcome");
                 require(plain.dy - hf.dy > previous, "discard did not grow with speed");
                 previous = plain.dy - hf.dy;
             }
         });
    test("roll pointing in the second pattern's first-stroke direction is suppressed the same way",
         [] {
             HF h;
             h.active();
             const Pointing hf = pointing(h, held(2, 20.f, 1000)); // roll+
             require(hf.candidates == 1 && hf.rejected == 1 && hf.lastReject == Reject::TooSlow,
                     "roll+ candidate");
             require(hf.executed == 0 && !hf.dragSeen && !hf.leftActive, "roll pointing acted");
             require(hf.longestMs <= maxSuppressionMs, "suppression unbounded");
         });
    test("directions and speeds that cannot start a pattern never open a candidate", [] {
        for (unsigned axis = 0; axis < 3; ++axis) {
            for (float sign : {1.f, -1.f}) {
                for (float rate : {5.f, 10.f, 15.f, 20.f, 40.f, 60.f}) {
                    const bool firstStroke = sign > 0 && axis != 0 && rate >= 20.f;
                    if (firstStroke) {
                        continue; // covered above
                    }
                    HF h;
                    h.active();
                    const Pointing hf = pointing(h, held(axis, sign * rate, 1000));
                    require(hf.candidates == 0 && hf.executed == 0 && hf.suppressedMs == 0,
                            "unexpected candidate");
                }
            }
        }
    });
    test("a slow onset (ramp) is treated as pointing and never opens a candidate", [] {
        HF h;
        h.active();
        std::vector<Rates> input;
        sim::neutral(input, 600);
        for (unsigned t = 10; t <= 500; t += 10) {
            input.push_back({0, 20.f * float(t) / 500.f, 0});
        }
        input.insert(input.end(), 100, Rates{0, 20.f, 0});
        sim::neutral(input, 600);
        const Pointing hf = pointing(h, input);
        require(hf.candidates == 0 && hf.suppressedMs == 0 && hf.executed == 0, "ramp suppressed");
    });
    test("keyed pointing corpus: every candidate is rejected, bounded, and nothing executes", [] {
        HF h;
        h.active();
        const auto base = legacyActive();
        uint32_t seed = 987654321; // fixed LCG: the corpus is reproducible
        auto next = [&] {
            seed = seed * 1664525u + 1013904223u;
            return seed >> 16;
        };
        unsigned candidates = 0, rejected = 0, suppressed = 0, longest = 0;
        float discarded = 0, total = 0;
        for (unsigned i = 0; i < 300; ++i) {
            const unsigned axis = next() % 2;
            const float sign = (next() % 2) ? 1.f : -1.f;
            const unsigned hold = 200 + (next() % 19) * 100, rest = 100 + (next() % 20) * 50;
            const auto input = held(axis, sign * 20.f, hold, 0, rest);
            const Pointing hf = pointing(h, input), plain = pointing(*base, input);
            require(hf.executed == 0, "a command executed on keyed pointing");
            require(!hf.dragSeen && !hf.leftActive, "drag or pause on keyed pointing");
            candidates += hf.candidates;
            rejected += hf.rejected;
            suppressed += hf.suppressedMs;
            longest = std::max(longest, hf.longestMs);
            discarded += std::hypot(plain.dx - hf.dx, plain.dy - hf.dy);
            total += std::hypot(plain.dx, plain.dy);
        }
        require(candidates >= 40, "the corpus no longer reproduces the observed candidates");
        require(candidates == rejected, "a candidate was left open or accepted");
        require(longest <= maxSuppressionMs, "suppression unbounded");
        require(suppressed <= candidates * maxSuppressionMs, "total suppression unbounded");
        require(discarded > 0 && discarded < .10f * total, "discard share regressed");
        require(h.s().recognizer.executed == 0 && h.s().state == SystemState::Active, "end state");
    });
    test("deliberate gestures still execute after pointing candidates were rejected", [] {
        HF h;
        h.active();
        require(pointing(h, held(1, 30.f, 800)).candidates == 1, "setup candidate");
        h.quiet(600);
        h.run(script("nod2"));
        h.quiet(400);
        require(h.s().state == SystemState::Paused, "the pause gesture was lost");
        h.quiet(400);
        h.run(script("nod2"));
        h.quiet(400);
        require(h.s().state == SystemState::Active, "resume gesture was lost");
    });
    test("patterns that both start on roll leave yaw/pitch pointing completely untouched", [] {
        // Mitigation for a helper to choose: when no pattern begins with a yaw or pitch stroke,
        // pointing never opens a candidate. Pause = tilt + - + -, drag = tilt - + - +.
        HF h;
        auto reversed = script("tilt2");
        for (auto& rate : reversed) {
            rate[2] = -rate[2];
        }
        auto train = [&](GestureId id, const std::vector<Rates>& pattern) {
            require(h.s().trainStart(id, h.now), "train start");
            h.quiet(1100);
            for (unsigned i = 0; i < start::trainExamples; ++i) {
                h.quiet(400);
                h.run(pattern);
                h.quiet(500);
            }
            h.quiet(400);
            h.run(pattern);
            h.quiet(400);
            require(h.s().trainAccept(), "train accept");
        };
        train(GestureId::PauseResume, script("tilt2"));
        train(GestureId::Drag, reversed);
        h.s().stageEnableKind(EnableKind::Maintained);
        require(h.s().commitHandsFree(), "commit");
        h.quiet(500);
        require(h.s().resume(), "resume");
        h.quiet(400);
        const auto base = legacyActive();
        for (unsigned axis : {0u, 1u}) {
            for (float sign : {1.f, -1.f}) {
                for (float rate : {20.f, 40.f, 60.f}) {
                    const auto input = held(axis, sign * rate, 1000);
                    const Pointing hf = pointing(h, input), plain = pointing(*base, input);
                    require(hf.candidates == 0 && hf.suppressedMs == 0,
                            "pointing opened a candidate");
                    require(hf.dx == plain.dx && hf.dy == plain.dy, "pointing movement differs");
                }
            }
        }
        h.run(script("tilt2"));
        h.quiet(400);
        require(h.s().state == SystemState::Paused, "roll pattern no longer pauses");
    });

    // ------------------------------------------------ K: momentary enable push button
    test("button: boot released, control stays disabled until one debounced press", [] {
        GateRig g;
        g.raw(false, 100);
        require(!g.gate.permitted() && g.gate.armed(), "armed after a stable release");
        g.raw(true, 29);
        require(!g.gate.permitted(), "enabled before the debounce window");
        g.raw(true, 2);
        require(g.gate.permitted() && g.gate.on() && g.gate.pressed(), "one press did not enable");
        require(g.toggles == 1, "toggle count");
    });
    test("button: held at boot never enables; a release and a new press are required", [] {
        GateRig g;
        g.raw(true, 5000);
        require(!g.gate.permitted() && !g.gate.armed() && g.toggles == 0, "enabled while held");
        g.raw(false, 20);
        g.raw(true, 200); // a press after only 20 ms of release is not a stable release
        require(!g.gate.permitted(), "enabled without a stable release");
        g.raw(false, 100);
        g.raw(true, 100);
        require(g.gate.permitted() && g.toggles == 1, "new press after release did not enable");
    });
    test("button: press and release bounce produce exactly one toggle", [] {
        GateRig g;
        g.raw(false, 100);
        g.bouncyPress(24, 100);
        require(g.gate.permitted() && g.toggles == 1, "bouncy press");
        g.bouncyRelease(24, 100); // release chatter must not look like another press
        require(g.gate.permitted() && g.toggles == 1, "release bounce toggled");
        g.bouncyPress(24, 100); // disable press: its bounce must not re-enable
        require(!g.gate.permitted() && g.toggles == 2, "bouncy disable");
        g.bouncyRelease(24, 100);
        require(!g.gate.permitted() && g.toggles == 2, "release bounce re-enabled");
    });
    test("button: a long hold toggles once, repeated presses toggle once each", [] {
        GateRig g;
        g.raw(false, 100);
        g.raw(true, 30000);
        require(g.gate.permitted() && g.toggles == 1, "long hold");
        g.raw(false, 100);
        g.raw(true, 30000);
        require(!g.gate.permitted() && g.toggles == 2, "long hold disable");
        for (unsigned press = 3; press <= 12; ++press) {
            g.raw(false, 60);
            g.raw(true, 60);
            require(g.toggles == press, "not exactly one toggle per press");
            require(g.gate.permitted() == (press % 2 == 1), "wrong permission after press");
        }
    });
    test("button: a second press needs a stable release first", [] {
        GateRig g;
        g.raw(false, 100);
        g.raw(true, 60);
        require(g.gate.permitted(), "enabled");
        g.raw(false, 20); // too short to count as released
        g.raw(true, 60);
        require(g.gate.permitted() && g.toggles == 1, "accepted without a stable release");
        g.raw(false, 40);
        g.raw(true, 5);
        require(!g.gate.permitted() && g.toggles == 2, "stable release then press did not disable");
    });
    test("button: disable acts at the first press edge; a glitch can only disable, never enable",
         [] {
             GateRig g;
             g.raw(false, 100);
             g.raw(true, 60);
             g.raw(false, 100);
             g.raw(true, 1); // a one-sample spike while permitted
             require(!g.gate.permitted(), "a press edge did not disable immediately");
             g.raw(false, 100);
             g.raw(true, 10); // a spike shorter than the debounce window while disabled
             g.raw(false, 100);
             require(!g.gate.permitted() && g.toggles == 2, "a short spike enabled control");
         });
    test("button: clearing the latch (fault) requires a release and a new press", [] {
        GateRig g;
        g.raw(false, 100);
        g.raw(true, 60);
        require(g.gate.permitted(), "enabled");
        g.gate.clearLatch();
        g.raw(true, 500); // still held: the same press does not count
        require(!g.gate.permitted(), "re-enabled without a new press");
        g.raw(false, 100);
        g.raw(true, 60);
        require(g.gate.permitted(), "new press after the fault did not enable");
    });
    test("button: pressed and latched are reported separately", [] {
        HF h = buttonRig();
        h.setup();
        h.quiet(300);
        auto st = h.s().handsFreeStatus();
        require(std::string(st.switchKind) == "MOMENTARY" && !st.switchPressed &&
                    !st.switchLatched && !st.permitted && st.switchArmed,
                "initial status");
        h.sw = true;
        h.quiet(100);
        st = h.s().handsFreeStatus();
        require(st.switchPressed && st.switchLatched && st.permitted, "pressed and latched");
        h.sw = false;
        h.quiet(100);
        st = h.s().handsFreeStatus();
        require(!st.switchPressed && st.switchLatched && st.permitted && st.switchArmed,
                "released but still latched");
        char buffer[1024];
        require(handsFreeJson(buffer, sizeof(buffer), st) > 0, "telemetry does not fit");
        require(std::strstr(buffer, "\"kind\":\"MOMENTARY\"") &&
                    std::strstr(buffer, "\"pressed\":false") &&
                    std::strstr(buffer, "\"latched\":true"),
                "telemetry fields");
    });
    test("button: boot released and boot held both start disabled and never resume", [] {
        for (bool heldAtBoot : {false, true}) {
            HF h = buttonRig();
            h.setup();
            h.quiet(300);
            h.click(); // permit, then reboot with the button in the state under test
            require(h.s().handsFreeStatus().permitted, "setup press");
            h.boot();
            h.sw = heldAtBoot;
            h.quiet(1000);
            require(!h.s().handsFreeStatus().permitted, "permission survived the reboot");
            require(h.s().state != SystemState::Active, "reboot resumed control");
            require(!h.s().resume(), "resume allowed while disabled");
            if (heldAtBoot) {
                h.quiet(3000);
                require(!h.s().handsFreeStatus().permitted, "held button enabled control");
                h.sw = false;
                h.quiet(100);
            }
            h.click();
            require(h.s().handsFreeStatus().permitted, "press after boot did not enable");
            require(h.s().state != SystemState::Active, "enabling resumed control");
        }
    });
    test("button: enabling needs the explicit resume gesture, then daily use works", [] {
        HF h = buttonRig();
        h.setup();
        h.quiet(300);
        require(!h.s().resume(), "resume before enabling");
        h.click();
        h.quiet(1000);
        require(h.s().state == SystemState::Ready, "enabling changed the state");
        perform(h, "nod2"); // the resume gesture
        require(h.s().state == SystemState::Active, "resume gesture refused after enabling");
        perform(h, "nod2");
        require(h.s().state == SystemState::Paused, "pause gesture");
        h.click(); // disable by button
        require(!h.s().handsFreeStatus().permitted, "second press did not disable");
        perform(h, "nod2");
        require(h.s().state != SystemState::Active, "gesture resumed while disabled");
    });
    test("button: the disabling press stops movement at once, without another sample", [] {
        HF h = buttonRig();
        h.active();
        h.run(hold(0, 25, 500)); // pointer moving
        require(anyMovement(h.last()), "no movement precondition");
        const size_t before = h.transport.reports.size();
        h.s().setControlSwitch(true, h.now + 1); // the press edge; no tick follows
        require(h.transport.reports.size() > before && h.released(), "movement not stopped");
        require(h.s().state == SystemState::Paused && !h.s().handsFreeStatus().permitted, "state");
    });
    test("button: the disabling press stops scrolling at once", [] {
        HF h = buttonRig();
        h.active();
        {
            UserProfile profile = h.s().profile;
            profile.scrollThreshold = 5;
            profile.scrollGain = 3;
            profile.scrollEnabled = true;
            require(h.s().setProfile(profile, false), "profile");
            h.quiet(300);
            require(h.s().resume(), "resume");
            h.quiet(400);
        }
        h.run(hold(2, -60, 1000));
        bool scrolled = false;
        for (const auto& r : h.transport.reports) {
            scrolled = scrolled || r.wheel != 0;
        }
        require(scrolled, "no scrolling precondition");
        h.s().setControlSwitch(true, h.now + 1);
        require(h.released() && h.s().state == SystemState::Paused, "scrolling not stopped");
    });
    test("button: the disabling press cancels a dwell in progress, no click follows", [] {
        HF h = buttonRig();
        h.active();
        moveAway(h);
        h.quiet(900);
        require(h.s().selection.dwell == DwellState::Progress, "dwell precondition");
        h.s().setControlSwitch(true, h.now + 1);
        const size_t mark = h.transport.reports.size();
        h.sw = true;
        h.quiet(60);
        h.sw = false;
        h.quiet(3000);
        require(clicksSince(h, mark - 1) == 0 && quietSince(h, mark), "a dwell click survived");
        require(h.s().state == SystemState::Paused, "state");
    });
    test("button: the disabling press releases a drag at once and never re-presses it", [] {
        HF h = buttonRig();
        h.active();
        beginDrag(h);
        const size_t before = h.transport.reports.size();
        h.s().setControlSwitch(true, h.now + 1);
        require(h.transport.reports.size() > before && h.released(), "no immediate release");
        require(!h.s().dragging && h.s().state == SystemState::Paused, "drag survived");
        h.sw = true;
        h.quiet(100);
        h.sw = false;
        h.quiet(100);
        h.click(); // enable again
        perform(h, "nod2");
        h.quiet(1000);
        require(!h.last().down && !h.s().dragging, "drag came back");
    });
    test("button: failed neutral delivery keeps output inhibited and presses cannot bypass it", [] {
        HF h = buttonRig();
        h.active();
        h.run(hold(0, 25, 300));
        h.transport.fail = true; // the neutral report cannot be delivered
        h.s().setControlSwitch(true, h.now + 1);
        require(h.s().state == SystemState::SafeState, "undeliverable release was not a fault");
        require(!h.s().handsFreeStatus().permitted, "permission survived the fault");
        h.sw = true;
        h.quiet(100);
        h.sw = false;
        h.quiet(100);
        for (int press = 0; press < 3; ++press) { // presses while still failing
            h.click();
            require(h.s().state == SystemState::SafeState && !h.s().resume(), "bypassed fault");
        }
        h.transport.fail = false;
        h.quiet(600);
        require(h.s().state != SystemState::Active, "recovered into ACTIVE");
        require(!h.s().handsFreeStatus().permitted || h.s().state != SystemState::Active,
                "recovery bypassed");
        const size_t mark = h.transport.reports.size();
        h.run(hold(0, 40, 300));
        require(quietSince(h, mark), "output without an explicit resume");
    });
    test("button: sensor fault recovery is not bypassed by presses; a new press is needed", [] {
        HF h = buttonRig();
        h.active();
        h.now += 10;
        h.s().tick({h.now, {NAN, 0, 0}, {0, 0, 1}, true}, h.now, false);
        require(h.s().state == SystemState::SafeState, "fault");
        require(!h.s().handsFreeStatus().permitted, "permission survived the fault");
        auto badTicks = [&](unsigned count) {
            for (unsigned i = 0; i < count; ++i) {
                h.now += 10;
                h.s().setControlSwitch(h.sw, h.now);
                h.s().tick({h.now, {NAN, 0, 0}, {0, 0, 1}, true}, h.now, false);
            }
        };
        for (int press = 0; press < 4; ++press) { // presses while the sensor is still bad
            h.sw = true;
            badTicks(6);
            h.sw = false;
            badTicks(6);
            require(h.s().state == SystemState::SafeState && !h.s().resume(),
                    "press bypassed fault");
        }
        h.quiet(600);
        require(h.s().state == SystemState::Ready || h.s().state == SystemState::Paused,
                "no recovery");
        // The press count above was even, so permission is off again; an odd count permits.
        if (!h.s().handsFreeStatus().permitted) {
            h.click();
        }
        require(h.s().handsFreeStatus().permitted && h.s().state != SystemState::Active,
                "enabling resumed control");
        perform(h, "nod2");
        require(h.s().state == SystemState::Active, "explicit gesture resume");
    });
    test("button: an invalid stored configuration stays inhibited whatever the button does", [] {
        HF h = buttonRig();
        h.setup();
        h.s().pause();
        for (auto& slot : h.configStorage.slots) {
            if (!slot.empty()) {
                slot[10] ^= 0xff;
            }
        }
        h.boot();
        require(h.s().interaction == InteractionMode::ConfigInvalid, "not invalid");
        h.quiet(300);
        for (int press = 0; press < 3; ++press) {
            h.click();
            require(!h.s().resume() && h.s().state != SystemState::Active,
                    "bypassed invalid config");
        }
    });
    test("button: a saved maintained-switch configuration keeps its meaning after reboot", [] {
        HF h; // maintained rig
        h.setup();
        h.quiet(300);
        h.boot();
        h.sw = true;
        h.quiet(300);
        auto st = h.s().handsFreeStatus();
        require(std::string(st.switchKind) == "MAINTAINED", "kind changed on reboot");
        require(st.permitted && st.switchLatched == false, "maintained switch ON did not permit");
        h.sw = false;
        h.quiet(50);
        require(!h.s().handsFreeStatus().permitted, "maintained OFF did not inhibit at once");
        // Retraining and saving again must not silently convert it to the button.
        require(h.trainGesture(GestureId::PauseResume, "nod2"), "retrain");
        require(h.s().commitHandsFree(), "commit");
        require(std::string(h.s().handsFreeStatus().switchKind) == "MAINTAINED", "converted");
        h.boot();
        require(std::string(h.s().handsFreeStatus().switchKind) == "MAINTAINED",
                "converted on boot");
    });
    test("button: records written before the button existed decode as maintained, byte for byte",
         [] {
             // Golden record produced by the previous revision's encoder (generation 7).
             const std::vector<uint8_t> golden = {
                 0x4e, 0x44, 0x46, 0x48, 0x01, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x01, 0x00,
                 0x00, 0x00, 0x00, 0x00, 0x40, 0x40, 0x04, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00,
                 0x01, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
                 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x70, 0x41, 0x00, 0x00, 0xf0, 0x41,
                 0x00, 0x00, 0x0c, 0x43, 0x3c, 0x00, 0x00, 0x00, 0x90, 0x01, 0x00, 0x00, 0xc8, 0x00,
                 0x00, 0x00, 0xdc, 0x05, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x02, 0x01, 0x00, 0x00,
                 0x02, 0x00, 0x00, 0x00, 0x02, 0x01, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
                 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x70, 0x41, 0x00, 0x00, 0xf0, 0x41,
                 0x00, 0x00, 0x0c, 0x43, 0x3c, 0x00, 0x00, 0x00, 0x90, 0x01, 0x00, 0x00, 0xc8, 0x00,
                 0x00, 0x00, 0xdc, 0x05, 0x00, 0x00, 0xd3, 0xad, 0x1a, 0xe9};
             require(golden.size() == HandsFreeConfig::wireSize, "golden size");
             HandsFreeConfig decoded;
             uint32_t generation = 0;
             require(decode(golden, decoded, generation) == ConfigState::Valid && generation == 7,
                     "old record not accepted");
             require(decoded.enabled && decoded.enableKind == EnableKind::Maintained,
                     "reinterpreted");
             require(encode(decoded, 7) == golden, "re-encoding an old record changed its bytes");
             decoded.enableKind = EnableKind::Momentary;
             const auto button = encode(decoded, 7);
             HandsFreeConfig back;
             require(decode(button, back, generation) == ConfigState::Valid &&
                         back.enableKind == EnableKind::Momentary,
                     "button record round trip");
             HandsFreeConfig other = back;
             other.enableKind = EnableKind::Maintained;
             require(configId(other) != configId(back),
                     "kind is not part of the configuration identity");
         });
    test("button: changing the input kind at commit starts disabled again", [] {
        HF h; // saved maintained, switch ON and permitted
        h.setup();
        h.quiet(300);
        require(h.s().handsFreeStatus().permitted, "maintained precondition");
        h.s().stageEnableKind(EnableKind::Momentary);
        require(h.s().commitHandsFree(), "commit");
        require(std::string(h.s().handsFreeStatus().switchKind) == "MOMENTARY", "kind");
        h.quiet(300); // the switch is still ON (pressed): not an enabling press
        require(!h.s().handsFreeStatus().permitted, "kind change kept permission");
        h.sw = false;
        h.quiet(100);
        h.click();
        require(h.s().handsFreeStatus().permitted, "press after conversion");
    });
    test("button: an unconfigured enable input still inhibits and the button cannot be bypassed",
         [] {
             HF h = buttonRig();
             h.setup();
             h.s().configureEnableInput(false); // no pin wired
             h.quiet(300);
             h.click();
             require(!h.s().handsFreeStatus().permitted && !h.s().resume(),
                     "unwired input permitted");
         });

    // ------------------------------------------------ L: temporary movement-only demo mode
    // No dwell click, no drag, no wheel, bounded steps; every safety check and the enable input
    // stay. RAM only: never written to the profile or the configuration record, off at every boot.
    auto demoActive = [](HF& h) {
        h.active();
        require(h.s().setDemoMovementOnly(true), "demo on");
        require(h.s().state == SystemState::Paused,
                "turning the demo on while active pauses control");
        h.quiet(300);
        require(h.s().resume(), "explicit resume");
        h.quiet(400);
    };
    test("demo: a completed dwell produces no click while normal hands-free mode does",
         [demoActive] {
             HF normal;
             normal.active();
             const size_t markNormal = normal.transport.reports.size();
             moveAway(normal);
             normal.quiet(2500);
             require(clicksSince(normal, markNormal) >= 1,
                     "precondition: dwell clicks in normal mode");
             HF h;
             demoActive(h);
             const size_t mark = h.transport.reports.size();
             moveAway(h);
             h.quiet(2500);
             require(clicksSince(h, mark) == 0, "a dwell click in the movement-only demo");
             for (size_t i = mark; i < h.transport.reports.size(); ++i) {
                 require(!h.transport.reports[i].down, "a button was pressed in the demo");
             }
         });
    test("demo: the drag gesture is refused and nothing is pressed", [demoActive] {
        HF h;
        demoActive(h);
        const uint32_t refusedBefore = h.s().handsFreeStatus().refused;
        const size_t mark = h.transport.reports.size();
        perform(h, "tilt2");
        require(!h.s().dragging, "drag started in the demo");
        require(h.s().state == SystemState::Active, "refusing the drag changed the state");
        require(h.s().handsFreeStatus().refused == refusedBefore + 1, "refusal not counted");
        for (size_t i = mark; i < h.transport.reports.size(); ++i) {
            require(!h.transport.reports[i].down, "a button was pressed");
        }
    });
    test("demo: no wheel reports even while rolled past the scroll threshold", [demoActive] {
        auto rolled = [](HF& h) {
            UserProfile profile = h.s().profile;
            profile.scrollThreshold = 5;
            profile.scrollGain = 3;
            profile.scrollEnabled = true;
            require(h.s().setProfile(profile, false), "profile");
            h.quiet(300);
            require(h.s().resume(), "resume");
            h.quiet(400);
            const size_t mark = h.transport.reports.size();
            h.run(hold(2, -60, 1000));
            for (size_t i = mark; i < h.transport.reports.size(); ++i) {
                if (h.transport.reports[i].wheel != 0) {
                    return true;
                }
            }
            return false;
        };
        HF normal;
        normal.active();
        require(rolled(normal), "precondition: the same roll scrolls in normal mode");
        HF h;
        demoActive(h);
        require(!rolled(h), "a wheel report in the movement-only demo");
    });
    test("demo: the pointer still moves and every step is bounded", [demoActive] {
        HF h;
        demoActive(h);
        const size_t mark = h.transport.reports.size();
        h.run(hold(0, 90, 1000)); // fast yaw: far above the bound in normal mode
        float total = 0, biggest = 0;
        for (size_t i = mark; i < h.transport.reports.size(); ++i) {
            total += std::abs(float(h.transport.reports[i].dx));
            biggest = std::max(biggest, std::abs(float(h.transport.reports[i].dx)));
        }
        require(total > 50, "the demo does not move the pointer");
        require(biggest <= start::demoMaxStep, "a step exceeded the demo bound");
        HF normal;
        normal.active();
        const size_t markNormal = normal.transport.reports.size();
        normal.run(hold(0, 90, 1000));
        float biggestNormal = 0;
        for (size_t i = markNormal; i < normal.transport.reports.size(); ++i) {
            biggestNormal =
                std::max(biggestNormal, std::abs(float(normal.transport.reports[i].dx)));
        }
        require(biggestNormal > start::demoMaxStep, "precondition: normal mode exceeds the bound");
    });
    test("demo: nothing is saved, and the mode is off after a reboot", [] {
        HF h;
        h.active();
        const auto profileBefore = h.profileStorage.read(0);
        const auto profileBefore1 = h.profileStorage.read(1);
        const auto configBefore0 = h.configStorage.slots[0],
                   configBefore1 = h.configStorage.slots[1];
        require(h.s().setDemoMovementOnly(true), "on");
        h.quiet(500);
        require(h.profileStorage.read(0) == profileBefore &&
                    h.profileStorage.read(1) == profileBefore1,
                "the saved profile changed");
        require(h.configStorage.slots[0] == configBefore0 &&
                    h.configStorage.slots[1] == configBefore1,
                "the saved configuration changed");
        require(h.s().profile.dwellEnabled, "the hands-free profile lost dwell");
        h.boot();
        require(!h.s().demoMovementOnly(), "the demo survived a reboot");
        require(!h.s().handsFreeStatus().demoMovementOnly, "status");
        require(h.s().interaction == InteractionMode::HandsFree, "mode");
    });
    test("demo: switching the mode while active pauses control and releases outputs", [] {
        HF h;
        h.active();
        beginDrag(h);
        require(h.s().setDemoMovementOnly(true), "on");
        require(h.s().state == SystemState::Paused && !h.s().dragging && h.released(),
                "a drag survived the mode change");
        h.quiet(300);
        require(h.s().resume(), "resume");
        h.quiet(300);
        require(h.s().setDemoMovementOnly(false), "off");
        require(h.s().state == SystemState::Paused && h.released(), "turning it off pauses");
        require(h.s().setDemoMovementOnly(false), "idempotent");
    });
    test("demo: the enable input and every fault still stop output", [demoActive] {
        HF h(true, EnableKind::Momentary);
        h.active();
        require(h.s().setDemoMovementOnly(true), "on");
        h.quiet(300);
        h.click(); // the second press disables, so permission is off; enable again for the test
        h.click();
        require(h.s().handsFreeStatus().permitted, "permission");
        require(h.s().resume(), "resume");
        h.quiet(300);
        h.run(hold(0, 40, 300));
        h.s().setControlSwitch(true, h.now + 1); // the disabling press edge
        require(h.s().state == SystemState::Paused && h.released(),
                "the button did not stop the demo");
        HF faulty;
        demoActive(faulty);
        faulty.now += 10;
        faulty.s().tick({faulty.now, {NAN, 0, 0}, {0, 0, 1}, true}, faulty.now, false);
        require(faulty.s().state == SystemState::SafeState, "a sensor fault did not stop the demo");
    });
    test("demo: the flag is reported in the hands-free status", [] {
        HF h;
        h.active();
        char buffer[1024];
        require(handsFreeJson(buffer, sizeof(buffer), h.s().handsFreeStatus()) > 0, "json");
        require(std::strstr(buffer, "\"demoMovementOnly\":false"), "off");
        require(h.s().setDemoMovementOnly(true), "on");
        require(handsFreeJson(buffer, sizeof(buffer), h.s().handsFreeStatus()) > 0, "json");
        require(std::strstr(buffer, "\"demoMovementOnly\":true"), "on");
    });

    // ------------------------------------------------ M: temporary UNCALIBRATED pointer demo
    // Real sensor path, validated RAM-only demo profile, physical enable button, movement only.
    // Missing/failed calibration alone is bypassed; every other fault still stops output.
    auto uncalRig = [](bool saveProfile = false) {
        HF h(saveProfile, EnableKind::Momentary);
        h.sw = false;
        h.quiet(400); // healthy samples; the button has not been pressed
        return h;
    };
    auto uncalStart = [](HF& h) {
        require(h.s().startUncalibratedDemo(h.now), "demo start refused");
        h.quiet(300);
    };
    auto movedSince = [](HF& h, size_t mark) {
        float dx = 0, dy = 0;
        for (size_t i = mark; i < h.transport.reports.size(); ++i) {
            dx += h.transport.reports[i].dx;
            dy += h.transport.reports[i].dy;
        }
        return std::array<float, 2>{dx, dy};
    };
    auto onlyMovement = [](HF& h, size_t mark) {
        for (size_t i = mark; i < h.transport.reports.size(); ++i) {
            const auto& r = h.transport.reports[i];
            require(!r.down && r.wheel == 0, "a button or wheel report in the uncalibrated demo");
        }
    };
    test("uncal demo: missing profile, no gestures, no button: starts only on an explicit start",
         [=] {
             HF h = uncalRig();
             require(h.s().profileState() == ProfileState::Missing, "profile state");
             h.click(); // a button press never starts it
             h.quiet(1000);
             require(h.s().state == SystemState::CalibrationRequired && !h.s().uncalibratedDemo(),
                     "the demo started by itself");
             require(!h.s().resume(), "normal resume still needs a calibrated profile");
             require(h.s().startUncalibratedDemo(h.now), "explicit start refused");
             require(h.s().uncalibratedDemo() && h.s().state == SystemState::Active, "active");
             require(!h.s().hasProfile && h.s().calibration.phase == CalPhase::Idle,
                     "the demo must not report a profile or a calibration");
             require(h.profileStorage.read(0).empty() && h.profileStorage.read(1).empty() &&
                         h.configStorage.slots[0].empty() && h.configStorage.slots[1].empty(),
                     "the demo wrote storage");
         });
    test("uncal demo: after a FAILED calibration the failure stays reported", [=] {
        HF h = uncalRig();
        h.s().calibrate(h.now);
        h.run(hold(0, 40, 4000)); // rest phase moved: 'rest too unstable' / insufficient
        for (int i = 0; i < 1000 && h.s().calibration.phase != CalPhase::Failed; ++i) {
            h.run(hold(0, 40, 100));
        }
        require(h.s().calibration.phase == CalPhase::Failed, "precondition: calibration failed");
        h.quiet(400);
        require(h.s().startUncalibratedDemo(h.now), "demo after failed calibration");
        h.quiet(300);
        require(h.s().calibration.phase == CalPhase::Failed && !h.s().hasProfile,
                "the demo changed the calibration result");
    });
    test("uncal demo: a corrupt saved profile stays corrupt and is never repaired", [=] {
        HF h(false, EnableKind::Momentary);
        h.sw = false;
        require(h.repo.save(UserProfile{}), "save");
        h.profileStorage.slots[0][20] ^= 0x55;
        h.profileStorage.slots[1] = h.profileStorage.slots[0];
        const auto slot0 = h.profileStorage.slots[0], slot1 = h.profileStorage.slots[1];
        h.boot();
        h.quiet(400);
        require(h.s().profileState() == ProfileState::Corrupt && !h.s().hasProfile, "corrupt");
        uncalStart(h);
        h.run(hold(0, 60, 500));
        require(h.s().uncalibratedDemo(), "demo runs without touching the record");
        require(h.s().profileState() == ProfileState::Corrupt, "corruption silently accepted");
        h.s().stopUncalibratedDemo("test");
        require(h.s().profileState() == ProfileState::Corrupt && !h.s().hasProfile,
                "corruption repaired");
        require(h.profileStorage.slots[0] == slot0 && h.profileStorage.slots[1] == slot1,
                "corrupt record rewritten");
        char buffer[1536];
        require(handsFreeJson(buffer, sizeof(buffer), h.s().handsFreeStatus()) > 0, "json");
        require(std::strstr(buffer, "\"profileState\":\"CORRUPT\""), "reported as corrupt");
    });
    test("uncal demo: real sensor movement controls the pointer in all four directions",
         [=] {
             HF h = uncalRig();
             uncalStart(h);
             struct Case {
                 unsigned axis;
                 float rate;
                 int dx, dy;
             };
             for (const Case c : {Case{0, 60, 1, 0}, Case{0, -60, -1, 0}, Case{1, 60, 0, 1},
                                  Case{1, -60, 0, -1}}) {
                 h.quiet(600);
                 const size_t mark = h.transport.reports.size();
                 h.run(hold(c.axis, c.rate, 400));
                 const auto moved = movedSince(h, mark);
                 require(moved[0] * c.dx > 5 || c.dx == 0, "wrong or no horizontal movement");
                 require(moved[1] * c.dy > 5 || c.dy == 0, "wrong or no vertical movement");
                 require(std::abs(moved[c.dx ? 1 : 0]) < 2, "crosstalk into the other axis");
                 onlyMovement(h, mark);
             }
         });
    test("uncal demo: steps stay below the demo bound and below the normal bound", [=] {
        HF h = uncalRig();
        uncalStart(h);
        const size_t mark = h.transport.reports.size();
        h.run(hold(0, 200, 1500)); // violent rotation
        float biggest = 0;
        for (size_t i = mark; i < h.transport.reports.size(); ++i) {
            biggest = std::max({biggest, std::abs(float(h.transport.reports[i].dx)),
                                std::abs(float(h.transport.reports[i].dy))});
        }
        require(biggest > 0, "no movement");
        require(biggest <= start::uncalDemoMaxStep, "a step exceeded the uncalibrated bound");
        require(start::uncalDemoMaxStep < start::demoMaxStep, "bound is not the conservative one");
        require(start::uncalDemoGain < start::gain, "gain is not conservative");
    });
    test("uncal demo: no button, no dwell click, no drag, no wheel; idle gyro bias is ignored",
         [=] {
             HF h = uncalRig();
             uncalStart(h);
             const size_t mark = h.transport.reports.size();
             h.run(hold(2, -90, 1500)); // roll far past the scroll threshold
             h.quiet(3000);             // dwell time would have clicked
             h.run(hold(0, 40, 300));
             h.quiet(2500);
             onlyMovement(h, mark);
             require(clicksSince(h, mark) == 0, "a click in the uncalibrated demo");
             // Idle bias of the bench sensor (about -1.3, -0.9 deg/s) sits inside the deadzone.
             const size_t idle = h.transport.reports.size();
             for (unsigned i = 0; i < 300; ++i) {
                 h.tick({-1.3f, -0.9f, 0.1f});
             }
             const auto drift = movedSince(h, idle);
             require(std::abs(drift[0]) < 1 && std::abs(drift[1]) < 1, "idle bias moves the cursor");
         });
    test("uncal demo: Stop demo releases at once and needs an explicit restart", [=] {
        HF h = uncalRig();
        uncalStart(h);
        h.run(hold(0, 60, 300));
        h.s().stopUncalibratedDemo("stopped by the user");
        require(!h.s().uncalibratedDemo() && h.s().state != SystemState::Active, "still active");
        require(h.released(), "the stop did not release the pointer");
        const size_t mark = h.transport.reports.size();
        h.run(hold(0, 60, 500));
        require(movedSince(h, mark)[0] == 0, "movement after Stop demo");
        require(h.s().startUncalibratedDemo(h.now), "explicit restart");
    });
    test("uncal demo: a button press only stops it at the press edge; restart is explicit",
         [=] {
             HF h = uncalRig();
             uncalStart(h);
             h.run(hold(0, 60, 200));
             h.sw = true;
             h.tick(); // one pass: the press edge alone disables
             require(!h.s().uncalibratedDemo() && h.released(), "not stopped at the press edge");
             h.sw = false;
             h.quiet(300);
             require(!h.s().uncalibratedDemo() && h.s().state != SystemState::Active,
                     "the button restarted the demo");
             // Without any further sensor sample: the press itself must already release.
             HF direct = uncalRig();
             uncalStart(direct);
             direct.run(hold(0, 60, 200));
             direct.s().setControlSwitch(true, direct.now + 1);
             require(!direct.s().uncalibratedDemo() && direct.released(),
                     "the press edge waited for a sensor sample");
             require(h.s().startUncalibratedDemo(h.now), "explicit restart (no press needed)");
         });
    test("uncal demo: a start is rejected for an unhealthy sensor, bad mapping, no BLE, no button",
         [=] {
             {
                 HF h(false, EnableKind::Momentary);
                 h.sw = false;
                 h.tick();
                 require(!h.s().startUncalibratedDemo(h.now), "started before healthy samples");
                 require(std::string(h.s().diagnostics.reason) ==
                             "waiting for healthy sensor samples",
                         "unhealthy sensor reason");
             }
             {
                 HF h = uncalRig();
                 h.s().axes.axes = {0, 0, 0};
                 require(!h.s().axes.valid(), "precondition: mapping invalid");
                 require(!h.s().startUncalibratedDemo(h.now), "started with an invalid mapping");
             }
             {
                 HF h = uncalRig();
                 h.transport.online = false;
                 require(!h.s().startUncalibratedDemo(h.now), "started without BLE");
                 require(std::string(h.s().diagnostics.reason) == "BLE link unavailable",
                         "BLE reason");
             }
             {
                 HF h = uncalRig(); // no enable button wired at all: still allowed
                 h.s().configureEnableInput(false);
                 require(h.s().startUncalibratedDemo(h.now), "the demo must not need a button");
             }
             {
                 HF h = uncalRig();
                 require(h.s().startUncalibratedDemo(h.now), "healthy start");
                 require(!h.s().startUncalibratedDemo(h.now), "double start");
             }
         });
    test("uncal demo: calibration, training and gestures take over and end the demo", [=] {
        HF h = uncalRig();
        uncalStart(h);
        h.s().calibrate(h.now);
        require(h.s().state == SystemState::Calibrating && !h.s().uncalibratedDemo(),
                "calibration did not end the demo");
        require(h.released(), "calibration start did not release output");
        HF g = uncalRig();
        uncalStart(g);
        g.s().pause();
        require(!g.s().uncalibratedDemo() && g.released(), "pause did not end the demo");
    });
    test("uncal demo: sensor faults stop output and the demo; restart is explicit", [=] {
        for (int kind = 0; kind < 4; ++kind) {
            HF h = uncalRig();
            uncalStart(h);
            h.run(hold(0, 40, 200));
            h.now += 10;
            MotionSample bad{h.now, {0, 0, 0}, {0, 0, 1}, true};
            if (kind == 0) {
                bad.gyro = {NAN, 0, 0};
            } else if (kind == 1) {
                bad.gyro = {1e6f, 0, 0};
            } else if (kind == 2) {
                bad.valid = false;
            } else {
                bad.timestampMs = h.now - 400; // stale sample
            }
            h.s().tick(bad, h.now, false);
            require(h.s().state == SystemState::SafeState, "fault did not stop control");
            require(!h.s().uncalibratedDemo() && h.released(), "demo survived a sensor fault");
            h.quiet(600); // recovery
            require(h.s().state != SystemState::Active && !h.s().uncalibratedDemo(),
                    "demo resumed by itself after recovery");
            require(h.s().startUncalibratedDemo(h.now), "explicit restart after recovery");
        }
    });
    test("uncal demo: BLE disconnect and delivery failure stop it; reconnect never restarts it",
         [=] {
             HF h = uncalRig();
             uncalStart(h);
             h.transport.online = false;
             h.quiet(100);
             require(h.s().state == SystemState::SafeState && !h.s().uncalibratedDemo(),
                     "disconnect did not stop the demo");
             h.transport.online = true; // reconnect
             h.quiet(800);
             require(!h.s().uncalibratedDemo() && h.s().state != SystemState::Active,
                     "reconnect restarted the demo");
             require(h.s().startUncalibratedDemo(h.now), "explicit restart after reconnect");
             h.quiet(200);
             h.transport.fail = true; // delivery failure while connected
             h.run(hold(0, 60, 100));
             require(h.s().state == SystemState::SafeState && !h.s().uncalibratedDemo(),
                     "delivery failure did not stop the demo");
         });
    test("uncal demo: a reboot never starts it", [=] {
        HF h = uncalRig();
        uncalStart(h);
        h.boot();
        require(std::string(h.s().handsFreeStatus().uncalBlocked) ==
                    "waiting for healthy sensor samples",
                "a fresh boot must re-qualify the sensor before any start");
        h.quiet(600);
        require(!h.s().uncalibratedDemo() && h.s().state != SystemState::Active, "boot started it");
    });
    test("uncal demo: normal calibrated control keeps its requirements", [=] {
        HF h = uncalRig();
        require(!h.s().resume() && std::string(h.s().handsFreeStatus().blocked) ==
                                       "calibrated profile required",
                "resume without a profile");
        HF normal(true, EnableKind::Momentary); // calibrated profile saved: normal path
        normal.sw = false;
        normal.setup();
        normal.quiet(400);
        normal.click();
        require(normal.s().resume() && !normal.s().uncalibratedDemo(), "normal resume works");
        require(!normal.s().startUncalibratedDemo(normal.now), "demo start while active");
    });
    test("link: an idle link sends no repeated zero reports; stops and reconnects still send", [=] {
        HF h = uncalRig();
        h.quiet(1000);
        require(h.transport.reports.size() <= 3, "idle zero reports flood the link");
        uncalStart(h);
        h.run(hold(0, 60, 300));
        const size_t moving = h.transport.reports.size();
        require(moving > 3, "movement must be reported");
        h.s().stopUncalibratedDemo("test");
        require(h.released(), "a stop must send the release");
        const size_t afterStop = h.transport.reports.size();
        h.quiet(1000);
        require(h.transport.reports.size() == afterStop, "idle repeats after the release");
        h.transport.online = false;
        h.quiet(100);
        h.transport.online = true;
        h.quiet(1000);
        require(h.transport.reports.size() >= afterStop, "reconnect");
    });
    test("uncal demo: status exposes the demo, its parameters and the profile state", [=] {
        HF h = uncalRig();
        char buffer[1536];
        require(handsFreeJson(buffer, sizeof(buffer), h.s().handsFreeStatus()) > 0, "json");
        require(std::strstr(buffer, "\"uncalDemo\":{\"active\":false") &&
                    std::strstr(buffer, "\"profileState\":\"MISSING\"") &&
                    std::strstr(buffer, "\"blocked\":\"\""),
                "idle status: ready, nothing blocking");
        uncalStart(h);
        require(handsFreeJson(buffer, sizeof(buffer), h.s().handsFreeStatus()) > 0, "json");
        require(std::strstr(buffer, "\"uncalDemo\":{\"active\":true") &&
                    std::strstr(buffer, "\"gain\":12.00") && std::strstr(buffer, "\"maxStep\":4.00"),
                "active status");
    });

    // ------------------------------------------------ N: dwell clicking in the uncalibrated demo
    // Explicitly enabled, RAM only, one primary-button click per completed dwell. The normal
    // SelectionManager -> SafetyManager -> HIDManager path; no drag, no scroll, no profile needed.
    auto dwellStart = [=](HF& h) {
        require(h.s().startUncalibratedDemo(h.now), "demo start refused");
        h.quiet(300);
        require(!h.s().uncalibratedDwell(), "dwell must be off after a start");
    };
    auto dwellOn = [](HF& h) {
        require(h.s().setUncalibratedDwell(true, h.now), "dwell enable refused");
    };
    test("dwell demo: movement-only is the default and never clicks", [=] {
        HF h = uncalRig();
        dwellStart(h);
        const size_t mark = h.transport.reports.size();
        h.quiet(6000);
        require(clicksSince(h, mark) == 0, "a click without dwell being enabled");
        require(!h.s().handsFreeStatus().uncalDwellEnabled, "status");
    });
    test("dwell demo: enabling needs a running demo and is cleared by every stop", [=] {
        HF h = uncalRig();
        require(!h.s().setUncalibratedDwell(true, h.now), "enabled without a running demo");
        dwellStart(h);
        dwellOn(h);
        h.s().stopUncalibratedDemo("test");
        require(!h.s().uncalibratedDwell(), "dwell survived the stop");
        dwellStart(h); // a restart is movement-only again
        require(!h.s().uncalibratedDwell(), "dwell carried into the restart");
    });
    test("dwell demo: exactly one primary click per completed dwell, release always sent", [=] {
        HF h = uncalRig();
        dwellStart(h);
        dwellOn(h);
        const size_t mark = h.transport.reports.size();
        float peak = 0;
        for (unsigned i = 0; i < 300; ++i) { // 3 s: 250 ms arming + 1200 ms dwell fits
            h.tick();
            peak = std::max(peak, h.s().handsFreeStatus().uncalDwellProgress);
        }
        require(peak > .9f, "progress was never shown");
        require(clicksSince(h, mark) == 1, "not exactly one click");
        require(h.released(), "the release was not the last report");
        bool pressed = false;
        unsigned downs = 0;
        for (size_t i = mark; i < h.transport.reports.size(); ++i) {
            const auto& r = h.transport.reports[i];
            if (r.down) {
                ++downs;
                require(r.dx == 0 && r.dy == 0 && r.wheel == 0, "movement inside the click report");
            }
            pressed = pressed || r.down;
        }
        require(pressed && downs == 1, "one press report expected");
        require(h.s().handsFreeStatus().uncalClicks == 1, "click counter");
    });
    test("dwell demo: staying still never repeats the click", [=] {
        HF h = uncalRig();
        dwellStart(h);
        dwellOn(h);
        const size_t mark = h.transport.reports.size();
        h.quiet(15000);
        require(clicksSince(h, mark) == 1, "repeated clicks while stationary");
    });
    test("dwell demo: a small movement does not rearm, a deliberate one does", [=] {
        HF h = uncalRig();
        dwellStart(h);
        dwellOn(h);
        const size_t mark = h.transport.reports.size();
        h.quiet(2200);
        require(clicksSince(h, mark) == 1, "first click");
        // A nudge inside the lockout radius (1.5 x tolerance) must not rearm.
        h.run(hold(0, 8, 100));
        h.quiet(4000);
        require(clicksSince(h, mark) == 1, "a tiny movement rearmed the dwell");
        // A deliberate move well outside the radius, then stillness, clicks once more.
        h.run(hold(0, 70, 600));
        h.quiet(3000);
        require(clicksSince(h, mark) == 2, "deliberate movement did not rearm exactly once");
    });
    test("dwell demo: excessive movement cancels a running dwell", [=] {
        HF h = uncalRig();
        dwellStart(h);
        dwellOn(h);
        h.quiet(900); // inside progress
        require(h.s().handsFreeStatus().uncalDwellProgress > 0, "dwell not running");
        const uint32_t before = h.s().selection.cancellations;
        const size_t mark = h.transport.reports.size();
        h.run(hold(0, 90, 300));
        require(h.s().selection.cancellations > before, "movement did not cancel the dwell");
        require(clicksSince(h, mark) == 0, "click during movement");
        h.quiet(600);
        require(clicksSince(h, mark) == 0, "click without a fresh full dwell");
    });
    test("dwell demo: a failed release inhibits output and enters recovery", [=] {
        HF h = uncalRig();
        dwellStart(h);
        dwellOn(h);
        h.transport.failRelease = true;
        for (unsigned i = 0; i < 300 && h.s().state == SystemState::Active; ++i) {
            h.tick();
        }
        require(h.s().state == SystemState::SafeState, "release failure did not fault");
        require(!h.s().uncalibratedDemo() && !h.s().uncalibratedDwell(), "demo survived");
        h.transport.failRelease = false;
        const size_t mark = h.transport.reports.size();
        h.run(hold(0, 60, 500));
        for (size_t i = mark; i < h.transport.reports.size(); ++i) {
            require(!h.transport.reports[i].down && h.transport.reports[i].dx == 0,
                    "output continued after the release failure");
        }
        h.quiet(600);
        require(h.s().state != SystemState::Active, "recovery restarted control");
        dwellStart(h);
        require(!h.s().uncalibratedDwell(), "explicit restart is movement-only first");
    });
    test("dwell demo: button, pause, calibration and demo stop each cancel the dwell", [=] {
        for (int how = 0; how < 4; ++how) {
            HF h = uncalRig();
            dwellStart(h);
            dwellOn(h);
            h.quiet(900);
            require(h.s().handsFreeStatus().uncalDwellProgress > 0, "dwell not running");
            const size_t mark = h.transport.reports.size();
            if (how == 0) {
                h.sw = true;
                h.tick(); // physical button: stop at the press edge
                h.sw = false;
            } else if (how == 1) {
                h.s().pause();
            } else if (how == 2) {
                h.s().calibrate(h.now);
            } else {
                h.s().stopUncalibratedDemo("test");
            }
            require(!h.s().uncalibratedDemo() && !h.s().uncalibratedDwell(), "demo still on");
            require(h.s().handsFreeStatus().uncalDwellProgress == 0, "progress not reset");
            h.quiet(3000);
            require(clicksSince(h, mark) == 0, "click after the dwell was cancelled");
        }
    });
    test("dwell demo: invalid samples cancel the dwell and nothing clicks", [=] {
        for (int kind = 0; kind < 3; ++kind) {
            HF h = uncalRig();
            dwellStart(h);
            dwellOn(h);
            h.quiet(900);
            const size_t mark = h.transport.reports.size();
            h.now += 10;
            MotionSample bad{h.now, {0, 0, 0}, {0, 0, 1}, true};
            if (kind == 0) {
                bad.gyro = {NAN, 0, 0};
            } else if (kind == 1) {
                bad.valid = false;
            } else {
                bad.timestampMs = h.now - 400;
            }
            h.s().tick(bad, h.now, false);
            require(h.s().state == SystemState::SafeState && !h.s().uncalibratedDwell(),
                    "fault did not stop the dwell");
            h.quiet(3000);
            require(clicksSince(h, mark) == 0, "click after an invalid sample");
        }
    });
    test("dwell demo: disconnect and reconnect reset progress and need an explicit restart", [=] {
        HF h = uncalRig();
        dwellStart(h);
        dwellOn(h);
        h.quiet(900);
        const size_t mark = h.transport.reports.size();
        h.transport.online = false;
        h.quiet(100);
        require(h.s().state == SystemState::SafeState && !h.s().uncalibratedDwell(), "not stopped");
        require(h.s().handsFreeStatus().uncalDwellProgress == 0, "progress survived");
        h.transport.online = true;
        h.quiet(4000);
        require(clicksSince(h, mark) == 0, "click around a disconnect");
        require(!h.s().uncalibratedDemo(), "reconnect restarted the demo");
        dwellStart(h);
        h.quiet(4000);
        require(clicksSince(h, mark) == 0, "restart clicked before dwell was re-enabled");
    });
    test("dwell demo: works with a missing or failed calibration and a corrupt profile", [=] {
        {
            HF h = uncalRig(); // missing profile
            dwellStart(h);
            dwellOn(h);
            const size_t mark = h.transport.reports.size();
            h.quiet(2500);
            require(clicksSince(h, mark) == 1 && !h.s().hasProfile, "missing profile");
        }
        {
            HF h = uncalRig();
            h.s().calibrate(h.now);
            for (int i = 0; i < 1000 && h.s().calibration.phase != CalPhase::Failed; ++i) {
                h.run(hold(0, 40, 100));
            }
            require(h.s().calibration.phase == CalPhase::Failed, "precondition");
            h.quiet(400);
            dwellStart(h);
            dwellOn(h);
            const size_t mark = h.transport.reports.size();
            h.quiet(2500);
            require(clicksSince(h, mark) == 1, "failed calibration");
            require(h.s().calibration.phase == CalPhase::Failed, "calibration result changed");
        }
        {
            HF h(false, EnableKind::Momentary);
            h.sw = false;
            require(h.repo.save(UserProfile{}), "save");
            h.profileStorage.slots[0][20] ^= 0x55;
            h.profileStorage.slots[1] = h.profileStorage.slots[0];
            const auto kept = h.profileStorage.slots[0];
            h.boot();
            h.quiet(400);
            dwellStart(h);
            dwellOn(h);
            h.quiet(2500);
            require(h.s().profileState() == ProfileState::Corrupt, "corruption hidden");
            require(h.profileStorage.slots[0] == kept, "corrupt record rewritten");
        }
    });
    test("dwell demo: no drag, double click, right click or scrolling can come out", [=] {
        HF h = uncalRig();
        dwellStart(h);
        dwellOn(h);
        const size_t mark = h.transport.reports.size();
        h.run(hold(2, -90, 1500)); // roll far past the scroll threshold
        h.quiet(4000);
        h.run(hold(0, 60, 400));
        h.quiet(4000);
        unsigned downs = 0;
        for (size_t i = mark; i < h.transport.reports.size(); ++i) {
            const auto& r = h.transport.reports[i];
            require(r.wheel == 0, "scroll report");
            if (r.down) {
                ++downs;
                require(i + 1 < h.transport.reports.size() && !h.transport.reports[i + 1].down,
                        "a held button (drag) or double press");
            }
        }
        require(downs >= 1, "expected at least one dwell click");
        require(clicksSince(h, mark) == downs, "click accounting");
    });
    test("dwell demo: settings are validated, RAM only, and restart a running dwell", [=] {
        HF h = uncalRig();
        require(!h.s().setUncalibratedDwellSettings(400, 8) &&
                    !h.s().setUncalibratedDwellSettings(6000, 8) &&
                    !h.s().setUncalibratedDwellSettings(1200, 1) &&
                    !h.s().setUncalibratedDwellSettings(1200, NAN),
                "an out-of-range setting was accepted");
        dwellStart(h);
        dwellOn(h);
        h.quiet(900);
        require(h.s().setUncalibratedDwellSettings(2000, 12), "valid setting refused");
        require(h.s().handsFreeStatus().uncalDwellMs == 2000 &&
                    h.s().handsFreeStatus().uncalDwellTolerance == 12,
                "settings not reported");
        require(h.s().handsFreeStatus().uncalDwellProgress == 0, "dwell not restarted");
        require(h.profileStorage.read(0).empty() && h.configStorage.slots[0].empty(),
                "settings were saved");
        const size_t mark = h.transport.reports.size();
        h.quiet(1800);
        require(clicksSince(h, mark) == 0, "clicked before the longer dwell completed");
        h.quiet(1200);
        require(clicksSince(h, mark) == 1, "no click after the longer dwell");
        HF fresh = uncalRig();
        require(fresh.s().handsFreeStatus().uncalDwellMs == start::uncalDwellMs &&
                    fresh.s().handsFreeStatus().uncalDwellTolerance == start::uncalDwellTolerance,
                "defaults are the named START values");
    });

    // ------------------------------------------------ O: fallback permission and reversal
    auto permRig = [] {
        HF h(false, EnableKind::Momentary);
        h.sw = false;
        h.uncalNeedsEnable = true; // the default for real use: physical enable permission required
        h.boot();
        h.quiet(400);
        return h;
    };
    test("fallback: the website start authorises it without the physical button; checks remain", [=] {
        HF h = permRig(); // configured control would need the button here; the fallback does not
        require(std::string(h.s().handsFreeStatus().uncalBlocked).empty(),
                "nothing but the website start should be missing");
        h.transport.online = false;
        require(!h.s().startUncalibratedDemo(h.now) &&
                    std::string(h.s().diagnostics.reason) == "BLE link unavailable",
                "started without BLE");
        h.transport.online = true;
        HF fresh(false, EnableKind::Momentary);
        fresh.sw = false;
        fresh.uncalNeedsEnable = true;
        fresh.boot();
        require(!fresh.s().startUncalibratedDemo(fresh.now), "started before healthy samples");
        fresh.s().axes.axes = {0, 0, 0};
        fresh.quiet(300);
        require(!fresh.s().startUncalibratedDemo(fresh.now), "started with an invalid mapping");
        require(h.s().startUncalibratedDemo(h.now), "website start refused");
        require(h.s().uncalibratedDemo() && !h.s().configuredControl(), "session");
        char buffer[1536];
        require(handsFreeJson(buffer, sizeof buffer, h.s().handsFreeStatus()) > 0, "json");
        require(std::strstr(buffer, "\"permission\":\"WEBSITE_START\""),
                "website-start permission must be shown in the status");
        h.run(hold(0, 60, 300));
        const float* none = nullptr;
        (void)none;
    });
    test("fallback: the physical button stops it at once and revokes the permission", [=] {
        HF h = permRig();
        require(h.s().startUncalibratedDemo(h.now), "start");
        h.run(hold(0, 60, 300));
        // the press edge alone, with no further sensor sample, releases and stops
        h.s().setControlSwitch(true, h.now + 1);
        require(!h.s().uncalibratedDemo() && h.released(), "not stopped at the press edge");
        h.s().setControlSwitch(false, h.now + 100);
        h.quiet(1000);
        require(!h.s().uncalibratedDemo() && h.s().state != SystemState::Active,
                "the button or the passing of time restarted it");
        std::array<float, 2> moved{};
        const size_t mark = h.transport.reports.size();
        h.run(hold(0, 60, 500));
        for (size_t i = mark; i < h.transport.reports.size(); ++i) {
            moved[0] += std::abs(float(h.transport.reports[i].dx));
        }
        require(moved[0] == 0, "movement after the stop");
        require(std::string(h.s().handsFreeStatus().uncalPermission) == "NONE", "permission revoked");
        require(h.s().startUncalibratedDemo(h.now), "another explicit website start works");
    });
    test("fallback: a fault or disconnect revokes it; reconnect and reboot never restart it", [=] {
        HF h = permRig();
        require(h.s().startUncalibratedDemo(h.now), "start");
        h.now += 10;
        h.s().tick({h.now, {NAN, 0, 0}, {0, 0, 1}, true}, h.now, false);
        h.quiet(800);
        require(!h.s().uncalibratedDemo() && h.s().state != SystemState::Active,
                "a fault left or restarted the session");
        require(h.s().startUncalibratedDemo(h.now), "explicit restart");
        h.transport.online = false;
        h.quiet(60);
        require(h.s().state == SystemState::SafeState && !h.s().uncalibratedDemo(), "disconnect");
        h.transport.online = true;
        h.quiet(1500);
        require(!h.s().uncalibratedDemo() && h.s().state != SystemState::Active,
                "reconnect restarted it");
        require(h.s().startUncalibratedDemo(h.now), "explicit restart after reconnect");
        h.boot();
        h.quiet(600);
        require(!h.s().uncalibratedDemo() && h.s().state != SystemState::Active, "boot started it");
        HF g = permRig();
        g.s().configureEnableInput(false); // no button wired at all
        require(g.s().startUncalibratedDemo(g.now), "the website start does not need a wired button");
    });
    test("fallback: horizontal and vertical reversal flip the pointer, RAM only", [=] {
        auto move = [&](bool rx, bool ry, unsigned axis, float rate) {
            HF h = uncalRig();
            h.s().setUncalibratedReversal(rx, ry);
            require(h.s().startUncalibratedDemo(h.now), "start");
            h.quiet(300);
            const size_t mark = h.transport.reports.size();
            h.run(hold(axis, rate, 400));
            float dx = 0, dy = 0;
            for (size_t i = mark; i < h.transport.reports.size(); ++i) {
                dx += h.transport.reports[i].dx;
                dy += h.transport.reports[i].dy;
            }
            return std::array<float, 2>{dx, dy};
        };
        const auto plainX = move(false, false, 0, 60), plainY = move(false, false, 1, 60);
        const auto revX = move(true, false, 0, 60), revY = move(false, true, 1, 60);
        require(plainX[0] > 5 && revX[0] < -5, "horizontal reversal");
        require(plainY[1] > 5 && revY[1] < -5, "vertical reversal");
        const auto mixed = move(true, false, 1, 60); // reversing X must not touch Y
        require(mixed[1] > 5, "vertical changed by the horizontal setting");
        HF h = uncalRig();
        h.s().setUncalibratedReversal(true, true);
        char buffer[1536];
        require(handsFreeJson(buffer, sizeof(buffer), h.s().handsFreeStatus()) > 0, "json");
        require(std::strstr(buffer, "\"reverseX\":true") && std::strstr(buffer, "\"reverseY\":true"),
                "reversal not reported");
        require(h.profileStorage.read(0).empty() && h.configStorage.slots[0].empty(),
                "reversal was saved");
        h.boot();
        require(!std::strstr(buffer, "\"reverseX\":false"), "sanity");
        require(handsFreeJson(buffer, sizeof(buffer), h.s().handsFreeStatus()) > 0, "json");
        require(std::strstr(buffer, "\"reverseX\":false"), "reversal survived a reboot");
    });

    // ------------------------------------------------ P: guided mapping and configured control
    // A modelled sensor with a fixed mounting: sensor = mount x body (gyro and gravity).
    using M3 = std::array<std::array<float, 3>, 3>;
    const M3 mountIdentity = {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    const M3 mountSideways = {{{0, -1, 0}, {1, 0, 0}, {0, 0, 1}}}; // 90 deg about z
    const M3 mountUpsideDown = {{{1, 0, 0}, {0, -1, 0}, {0, 0, -1}}}; // 180 deg about x
    auto rawTick = [](HF& h, const M3& m, std::array<float, 3> body) {
        h.now += 10;
        h.sys->setControlSwitch(h.sw, h.now);
        std::array<float, 3> g{}, a{};
        for (unsigned i = 0; i < 3; ++i) {
            g[i] = m[i][0] * body[0] + m[i][1] * body[1] + m[i][2] * body[2] + (i == 0 ? -1.2f : 0.f);
            a[i] = m[i][2]; // gravity along body z
        }
        h.sys->tick({h.now, g, a, true}, h.now, false);
    };
    auto teachRig = [](bool momentary = false) {
        HF h(false, EnableKind::Momentary);
        h.sw = false;
        h.quiet(400);
        (void)momentary;
        return h;
    };
    // Runs the whole guided teaching through the System, one half-sine movement per GO cue.
    auto teachAll = [=](HF& h, const M3& m, const std::function<std::array<float, 3>(int, bool, unsigned)>& body = nullptr) {
        static const std::array<float, 3> dirs[4] = {{1, 0, 0}, {-1, 0, 0}, {0, -1, 0}, {0, 1, 0}};
        require(h.s().teachStart(h.now), "teaching refused");
        MapCue last = MapCue::None;
        uint32_t moveStart = 0, moveMs = 600;
        std::array<float, 3> dir{};
        unsigned attempt = 0;
        bool moving = false;
        for (unsigned i = 0; i < 40000 && h.s().state == SystemState::Teaching; ++i) {
            const MappingStatus st = h.s().mappingStatus(h.now);
            if (st.phase == MapPhase::Preview) {
                break;
            }
            if (st.cue == MapCue::Go && last != MapCue::Go) {
                ++attempt;
                dir = body ? body(st.direction, st.validation, attempt) : dirs[st.direction];
                moving = true;
                moveStart = h.now + 300;
            }
            last = st.cue;
            std::array<float, 3> b{};
            if (moving && h.now >= moveStart) {
                const float t = float(h.now - moveStart) / float(moveMs);
                if (t > 1) {
                    moving = false;
                } else {
                    const float level = 40.f * std::sin(3.14159265f * t);
                    b = {dir[0] * level, dir[1] * level, dir[2] * level};
                }
            }
            rawTick(h, m, b);
        }
        return h.s().mappingStatus(h.now).phase == MapPhase::Preview;
    };
    auto pointerAfter = [=](HF& h, const M3& m, std::array<float, 3> dir) {
        h.quiet(400);
        const size_t mark = h.transport.reports.size();
        for (unsigned i = 0; i < 80; ++i) {
            const float level = i < 60 ? 40.f * std::sin(3.14159265f * float(i) / 60.f) : 0.f;
            rawTick(h, m, {dir[0] * level, dir[1] * level, dir[2] * level});
        }
        for (unsigned i = 0; i < 60; ++i) {
            rawTick(h, m, {0, 0, 0});
        }
        float dx = 0, dy = 0;
        for (size_t i = mark; i < h.transport.reports.size(); ++i) {
            dx += h.transport.reports[i].dx;
            dy += h.transport.reports[i].dy;
        }
        return std::array<float, 2>{dx, dy};
    };
    test("mapping: teaching is explicit, blocked until healthy, and never moves the pointer", [=] {
        HF fresh(false, EnableKind::Momentary);
        fresh.sw = false;
        require(!fresh.s().teachStart(fresh.now), "teaching before healthy samples");
        HF h = teachRig();
        h.s().calibrate(h.now);
        require(!h.s().teachStart(h.now), "teaching during calibration");
        h.s().cancelCalibration();
        h.quiet(300);
        const size_t mark = h.transport.reports.size();
        require(teachAll(h, mountSideways), "no preview");
        require(h.s().state == SystemState::Teaching, "state");
        for (size_t i = mark; i < h.transport.reports.size(); ++i) {
            require(h.transport.reports[i].dx == 0 && h.transport.reports[i].dy == 0 &&
                        !h.transport.reports[i].down,
                    "pointer output while teaching");
        }
        require(!h.s().startConfiguredControl(h.now), "control started while teaching");
        require(!h.s().learnedValid(), "nothing is learned before accepting");
    });
    test("mapping: accepted mapping works in RAM for a sideways mounting, all four directions", [=] {
        HF h = teachRig();
        require(teachAll(h, mountSideways), "no preview");
        require(h.s().teachAccept(), "accept");
        require(h.s().state != SystemState::Teaching && h.s().learnedValid(), "state");
        const MappingStatus st = h.s().mappingStatus(h.now);
        require(st.unsaved && std::string(st.stored) == "MISSING", "must be reported as unsaved");
        h.quiet(300);
        require(!h.s().configuredControl(), "accepting must not start control");
        require(h.s().startConfiguredControl(h.now), "configured start refused");
        h.quiet(200);
        require(std::string(h.s().mappingStatus(h.now).mode) == "CONFIGURED", "mode");
        const std::array<float, 3> dirs[4] = {{1, 0, 0}, {-1, 0, 0}, {0, -1, 0}, {0, 1, 0}};
        const int wantX[4] = {1, -1, 0, 0}, wantY[4] = {0, 0, -1, 1};
        for (unsigned d = 0; d < 4; ++d) {
            const auto moved = pointerAfter(h, mountSideways, dirs[d]);
            require(moved[0] * float(wantX[d]) + moved[1] * float(wantY[d]) > 6.f, "wrong or no movement");
            require(std::abs(moved[0] * float(wantY[d])) + std::abs(moved[1] * float(wantX[d])) <
                        .2f * (std::abs(moved[0]) + std::abs(moved[1])),
                    "crosstalk");
        }
        for (const auto& r : h.transport.reports) {
            require(!r.down && r.wheel == 0 && std::abs(r.dx) <= start::uncalDemoMaxStep &&
                        std::abs(r.dy) <= start::uncalDemoMaxStep,
                    "movement-only bounds");
        }
    });
    test("mapping: upside down also works, and a changed mounting asks to teach again", [=] {
        HF h = teachRig();
        require(teachAll(h, mountUpsideDown) && h.s().teachAccept(), "teach");
        h.quiet(300);
        // still upside down: fine
        for (unsigned i = 0; i < 40; ++i) {
            rawTick(h, mountUpsideDown, {0, 0, 0});
        }
        require(h.s().startConfiguredControl(h.now), "same mounting refused");
        h.s().stopUncalibratedDemo("test");
        // now mounted the other way up: gravity direction differs by far more than 25 degrees
        for (unsigned i = 0; i < 40; ++i) {
            rawTick(h, mountIdentity, {0, 0, 0});
        }
        require(!h.s().startConfiguredControl(h.now), "start allowed after the mounting changed");
        require(std::string(h.s().diagnostics.reason) == "mounting changed: teach the movements again",
                "reason");
    });
    test("mapping: saving is explicit and transactional; failure keeps RAM use; reboot loads it inactive",
         [=] {
             HF h = teachRig();
             require(teachAll(h, mountIdentity) && h.s().teachAccept(), "teach");
             require(h.controlStorage.slots[0].empty(), "accept must not write storage");
             h.controlStorage.failWrite = true;
             require(!h.s().teachSave(), "failed save reported as success");
             require(std::string(h.s().mappingStatus(h.now).saveResult) == "SAVE_FAILED_RAM_ONLY" &&
                         h.s().mappingStatus(h.now).unsaved,
                     "failure must say RAM only");
             require(h.s().learnedValid(), "settings lost by a failed save");
             h.quiet(300);
             require(h.s().startConfiguredControl(h.now), "RAM-only control must still work");
             require(!h.s().teachSave(), "saving while control is active");
             h.s().stopUncalibratedDemo("test");
             h.controlStorage.failWrite = false;
             require(h.s().teachSave(), "save");
             require(!h.s().mappingStatus(h.now).unsaved &&
                         std::string(h.s().mappingStatus(h.now).stored) == "VALID",
                     "saved status");
             require(h.profileStorage.read(0).empty() && h.configStorage.slots[0].empty(),
                     "the 84-byte profile and the hands-free record must be untouched");
             h.boot();
             h.quiet(300);
             require(h.s().learnedValid() && !h.s().configuredControl(),
                     "stored mapping loaded but never activated at boot");
             require(!h.s().mappingStatus(h.now).unsaved, "loaded settings are saved ones");
         });
    test("mapping: a corrupt stored record is reported corrupt and never used or overwritten", [=] {
        HF h = teachRig();
        require(teachAll(h, mountIdentity) && h.s().teachAccept() && h.s().teachSave(), "setup");
        h.controlStorage.slots[0][12] ^= 0x5a;
        h.controlStorage.slots[1] = h.controlStorage.slots[0];
        const auto kept = h.controlStorage.slots[0];
        h.boot();
        h.quiet(400);
        require(!h.s().learnedValid(), "corrupt settings were used");
        require(std::string(h.s().mappingStatus(h.now).stored) == "CORRUPT", "not reported corrupt");
        require(h.controlStorage.slots[0] == kept, "the corrupt record was rewritten");
        require(!h.s().startConfiguredControl(h.now), "configured start with no valid mapping");
        // the uncalibrated fallback still works
        require(h.s().startUncalibratedDemo(h.now), "fallback must remain available");
    });
    test("mapping: failure, cancel, faults and pauses keep the previous valid settings", [=] {
        HF h = teachRig();
        require(teachAll(h, mountIdentity) && h.s().teachAccept(), "first teaching");
        const LearnedControl before = h.s().learned();
        h.quiet(300);
        // a cancelled teaching
        require(h.s().teachStart(h.now), "start");
        h.quiet(200);
        h.s().teachCancel();
        require(h.s().state != SystemState::Teaching && h.s().learned().horizontal == before.horizontal,
                "cancel changed the settings");
        // teaching that fails (the user never holds still)
        require(h.s().teachStart(h.now), "start");
        for (unsigned i = 0; i < 1200 && h.s().state == SystemState::Teaching; ++i) {
            rawTick(h, mountIdentity, {(i / 100) % 2 ? 50.f : -50.f, 0, 0});
        }
        require(h.s().state != SystemState::Teaching, "teaching should have failed");
        require(h.s().learnedValid() && h.s().learned().horizontal == before.horizontal &&
                    std::string(h.s().diagnostics.reason).find("hold still") != std::string::npos,
                "failure must keep the previous settings and say why");
        // a disconnect during teaching
        h.quiet(400);
        require(h.s().teachStart(h.now), "start");
        h.transport.online = false;
        h.quiet(50);
        require(h.s().state == SystemState::SafeState && h.s().learnedValid(), "disconnect");
        h.transport.online = true;
        h.quiet(600);
        require(h.s().state != SystemState::Teaching, "reconnect resumed teaching");
        // an invalid sample during teaching
        h.quiet(400);
        require(h.s().teachStart(h.now), "start");
        h.now += 10;
        h.s().tick({h.now, {NAN, 0, 0}, {0, 0, 1}, true}, h.now, false);
        require(h.s().state == SystemState::SafeState && h.s().learnedValid(), "invalid sample");
    });
    test("mapping: configured control needs the enable permission and never auto-starts", [=] {
        HF h(false, EnableKind::Momentary);
        h.sw = false;
        h.uncalNeedsEnable = true;
        h.boot();
        h.quiet(400);
        require(teachAll(h, mountIdentity) && h.s().teachAccept(), "teach");
        h.quiet(300);
        require(!h.s().startConfiguredControl(h.now) &&
                    std::string(h.s().diagnostics.reason) == "press the enable button first",
                "started without permission");
        h.click();
        h.quiet(300);
        require(!h.s().configuredControl(), "the press started it");
        require(h.s().startConfiguredControl(h.now), "start with permission");
        h.click(); // next press disables
        require(!h.s().configuredControl() && h.released(), "physical disable");
        require(h.s().learnedValid(), "settings must survive");
        h.click();
        require(h.s().startConfiguredControl(h.now), "restart");
        h.now += 10;
        h.s().tick({h.now, {NAN, 0, 0}, {0, 0, 1}, true}, h.now, false);
        h.quiet(800);
        require(!h.s().configuredControl() && h.s().state != SystemState::Active,
                "a fault must not leave or restart control");
    });
    test("mapping: status JSON is complete, bounded and finite", [=] {
        HF h = teachRig();
        char buffer[mappingJsonCapacity];
        require(mappingJson(buffer, sizeof buffer, h.s().mappingStatus(h.now)) > 0, "idle json");
        require(teachAll(h, mountSideways), "preview");
        const size_t n = mappingJson(buffer, sizeof buffer, h.s().mappingStatus(h.now));
        require(n > 100 && n < mappingJsonCapacity * 3 / 4, "length headroom");
        for (const char* bad : {"nan", "inf", "null"}) {
            require(!std::strstr(buffer, bad), "non-finite token");
        }
        for (const char* key : {"\"phase\":\"PREVIEW\"", "\"learnedValid\"", "\"unsaved\"", "\"stored\"",
                                "\"preview\"", "\"retries\"", "\"interruptions\"", "\"reason\""}) {
            require(std::strstr(buffer, key), key);
        }
    });

    // ------------------------------------------------ Q: optional gesture click
    // Fallback frame: the default axis mapping is the identity in the rig, so the side tilt is axis 2.
    struct TiltShape {
        float peak = 60;
        unsigned ms = 600;
        std::array<float, 3> axis{0, 0, 1};
    };
    auto tiltAt = [](const TiltShape& g, float t) {
        std::array<float, 3> out{0, 0, 0};
        if (t >= 0 && t <= float(g.ms)) {
            const float level = g.peak * std::sin(2 * 3.14159265f * t / float(g.ms));
            out = {g.axis[0] * level, g.axis[1] * level, g.axis[2] * level};
        }
        return out;
    };
    auto pointingAt = [](std::mt19937& rng, uint32_t now, uint32_t& until, uint32_t& from,
                         std::array<float, 3>& dir, float& peak) {
        std::uniform_real_distribution<float> u(0.f, 1.f);
        if (now >= until) {
            from = now;
            until = now + 400 + unsigned(1200 * u(rng));
            const float a = 2 * 3.14159265f * u(rng);
            dir = {std::cos(a), std::sin(a), .2f * (u(rng) - .5f) * 2.f};
            peak = 15 + 70 * u(rng);
        }
        const float level = peak * std::sin(3.14159265f * float(now - from) / float(until - from));
        return std::array<float, 3>{dir[0] * level, dir[1] * level, dir[2] * level};
    };
    // Trains the gesture through the System (cue driven); returns once it is READY.
    auto clickTrain = [=](HF& h, const M3& m, bool configuredFrame,
                          const std::function<TiltShape(unsigned)>& shape = nullptr) {
        require(h.s().clickTrainStart(h.now, configuredFrame), "click training refused");
        std::mt19937 rng(99);
        uint32_t until = 0, from = 0;
        std::array<float, 3> dir{1, 0, 0};
        float peak = 30;
        ClickCue last = ClickCue::None;
        unsigned attempt = 0;
        bool moving = false;
        uint32_t moveStart = 0;
        TiltShape current;
        for (unsigned i = 0; i < 40000 && h.s().state == SystemState::Teaching; ++i) {
            const ClickStatus st = h.s().clickStatus(h.now);
            if (st.train.phase == ClickPhase::Ready) {
                break;
            }
            if (st.train.cue == ClickCue::Go && last != ClickCue::Go) {
                ++attempt;
                current = shape ? shape(attempt) : TiltShape{};
                moving = true;
                moveStart = h.now + 300;
            }
            last = st.train.cue;
            std::array<float, 3> body{};
            if (st.train.phase == ClickPhase::Confusion) {
                body = pointingAt(rng, h.now, until, from, dir, peak);
            } else if (moving && h.now >= moveStart) {
                if (h.now - moveStart > current.ms) {
                    moving = false;
                } else {
                    body = tiltAt(current, float(h.now - moveStart));
                }
            }
            rawTick(h, m, body);
        }
        return h.s().clickStatus(h.now).train.phase == ClickPhase::Ready;
    };
    auto sessionRig = [=]() {
        HF h = uncalRig();
        require(clickTrain(h, mountIdentity, false) && h.s().clickTrainAccept(), "train");
        h.quiet(300);
        require(h.s().startUncalibratedDemo(h.now), "session");
        h.quiet(300);
        return h;
    };
    auto doTilt = [=](HF& h, const TiltShape& shape, unsigned tailMs, const M3* mount = nullptr) {
        const M3& m = mount ? *mount : mountIdentity;
        for (unsigned t = 0; t <= shape.ms; t += 10) {
            rawTick(h, m, tiltAt(shape, float(t)));
        }
        for (unsigned t = 0; t < tailMs; t += 10) {
            rawTick(h, m, {0, 0, 0});
        }
    };
    test("gesture click: off by default, and the pointer never clicks before it is enabled", [=] {
        HF h = sessionRig();
        require(!h.s().clickGestureEnabled(), "must start disabled");
        const size_t mark = h.transport.reports.size();
        doTilt(h, TiltShape{}, 1000);
        h.run(hold(0, 60, 500));
        require(clicksSince(h, mark) == 0, "a click before the gesture was enabled");
        HF fresh = uncalRig();
        require(!fresh.s().setClickGesture(true, fresh.now), "enabled without a session");
        require(clickTrain(fresh, mountIdentity, false) && fresh.s().clickTrainAccept(), "train");
        require(!fresh.s().setClickGesture(true, fresh.now), "enabled without a running session");
        require(fresh.s().startUncalibratedDemo(fresh.now), "session");
        require(fresh.s().setClickGesture(true, fresh.now), "enable refused after training");
    });
    test("gesture click: one press and release per gesture, pointer held still during the candidate", [=] {
        HF h = sessionRig();
        require(h.s().setClickGesture(true, h.now), "enable");
        h.quiet(700);
        const size_t mark = h.transport.reports.size();
        doTilt(h, TiltShape{}, 500);
        require(clicksSince(h, mark) == 1, "not exactly one click");
        require(h.released(), "the release was not sent last");
        unsigned downs = 0;
        for (size_t i = mark; i < h.transport.reports.size(); ++i) {
            const auto& r = h.transport.reports[i];
            if (r.down) {
                ++downs;
                require(r.dx == 0 && r.dy == 0 && r.wheel == 0, "movement inside the click");
            }
            require(r.dx == 0 && r.dy == 0 && r.wheel == 0, "pointer moved during the gesture");
        }
        require(downs == 1 && h.s().handsFreeStatus().uncalClicks == 1, "one press only");
        require(h.s().clickStatus(h.now).accepted == 1, "accepted counter");
    });
    test("gesture click: yaw leaking into the gesture is suppressed and never replayed", [=] {
        HF h = sessionRig();
        require(h.s().setClickGesture(true, h.now), "enable");
        h.quiet(700);
        TiltShape leaky; // the same side tilt, with real pointer-moving motion mixed in
        leaky.axis = {.2f, 0, 1};
        leaky.peak = 60;
        const size_t mark = h.transport.reports.size();
        doTilt(h, leaky, 160); // through the click, before recovery would matter
        float during = 0;
        for (size_t i = mark; i < h.transport.reports.size(); ++i) {
            during += std::abs(float(h.transport.reports[i].dx));
        }
        h.quiet(1500);
        float after = 0;
        for (size_t i = mark; i < h.transport.reports.size(); ++i) {
            after += std::abs(float(h.transport.reports[i].dx));
        }
        require(h.s().clickStatus(h.now).candidates >= 1, "the gesture should have opened a candidate");
        require(clicksSince(h, mark) <= 1, "at most one click");
        require(during <= 4.f, "the pointer moved while a candidate was open");
        require(after - during <= 1.f, "suppressed movement was replayed afterwards");
        // control: the same yaw WITHOUT the gesture does move the pointer
        HF g = sessionRig();
        const size_t markG = g.transport.reports.size();
        g.run(hold(0, 27, 600));
        float moved = 0;
        for (size_t i = markG; i < g.transport.reports.size(); ++i) {
            moved += std::abs(float(g.transport.reports[i].dx));
        }
        require(moved > 20.f, "precondition: that yaw moves the pointer");
    });
    test("gesture click: no second click while still, rearming needs a neutral stretch", [=] {
        HF h = sessionRig();
        require(h.s().setClickGesture(true, h.now), "enable");
        h.quiet(700);
        const size_t mark = h.transport.reports.size();
        doTilt(h, TiltShape{}, 250);
        doTilt(h, TiltShape{}, 100); // immediately again: still locked out
        h.quiet(20000);
        require(clicksSince(h, mark) == 1, "repeat or early second click");
        doTilt(h, TiltShape{}, 700);
        require(clicksSince(h, mark) == 2, "rearmed after neutral");
    });
    test("gesture click: ordinary pointing never clicks, weak or wrong gestures never click", [=] {
        HF h = sessionRig();
        require(h.s().setClickGesture(true, h.now), "enable");
        h.quiet(700);
        const size_t mark = h.transport.reports.size();
        std::mt19937 rng(5);
        uint32_t until = 0, from = 0;
        std::array<float, 3> dir{1, 0, 0};
        float peak = 30;
        for (unsigned i = 0; i < 30000; ++i) {
            rawTick(h, mountIdentity, pointingAt(rng, h.now, until, from, dir, peak));
        }
        require(clicksSince(h, mark) == 0, "false click while pointing");
        require(h.s().uncalibratedDemo(), "pointing must not stop the session");
        TiltShape weak;
        weak.peak = 18;
        TiltShape yaw;
        yaw.axis = {1, 0, 0};
        TiltShape reversed;
        reversed.peak = -60;
        for (const TiltShape& g : {weak, yaw, reversed}) {
            const size_t before = h.transport.reports.size();
            doTilt(h, g, 900);
            require(clicksSince(h, before) == 0, "a wrong gesture clicked");
        }
    });
    test("gesture click: a release failure faults, stops the session and disables the gesture", [=] {
        HF h = sessionRig();
        require(h.s().setClickGesture(true, h.now), "enable");
        h.quiet(700);
        const uint32_t faultsBefore = h.s().diagnostics.faults;
        h.transport.failRelease = true;
        doTilt(h, TiltShape{}, 170); // calm for 150 ms accepts it; recovery needs 200 ms more
        require(h.s().state == SystemState::SafeState &&
                    h.s().diagnostics.faults == faultsBefore + 1,
                "no fault after a failed release");
        require(!h.s().uncalibratedDemo() && !h.s().clickGestureEnabled(), "session survived");
        h.transport.failRelease = false;
        const size_t mark = h.transport.reports.size();
        doTilt(h, TiltShape{}, 900);
        for (size_t i = mark; i < h.transport.reports.size(); ++i) {
            require(!h.transport.reports[i].down, "output continued after the fault");
        }
        h.quiet(600);
        require(h.s().state != SystemState::Active, "recovery restarted control");
        require(h.s().startUncalibratedDemo(h.now), "explicit restart");
        require(!h.s().clickGestureEnabled(), "gesture must be re-enabled explicitly");
    });
    test("gesture click: button, invalid sample, disconnect and pause cancel a candidate", [=] {
        for (int how = 0; how < 4; ++how) {
            HF h = sessionRig();
            require(h.s().setClickGesture(true, h.now), "enable");
            h.quiet(700);
            const size_t mark = h.transport.reports.size();
            for (unsigned t = 0; t <= 250; t += 10) {
                rawTick(h, mountIdentity, tiltAt(TiltShape{}, float(t)));
            }
            require(h.s().clickStatus(h.now).suppressing, "candidate not open");
            if (how == 0) {
                h.sw = true;
                h.tick();
                h.sw = false;
            } else if (how == 1) {
                h.now += 10;
                h.s().tick({h.now, {NAN, 0, 0}, {0, 0, 1}, true}, h.now, false);
            } else if (how == 2) {
                h.transport.online = false;
                h.quiet(50);
            } else {
                h.s().pause();
            }
            require(!h.s().clickGestureEnabled() && !h.s().uncalibratedDemo(), "still enabled");
            h.transport.online = true;
            h.quiet(1500);
            for (unsigned t = 250; t <= 600; t += 10) {
                rawTick(h, mountIdentity, tiltAt(TiltShape{}, float(t)));
            }
            h.quiet(900);
            require(clicksSince(h, mark) == 0, "click after a cancelled candidate");
        }
    });
    test("gesture click: works with a missing or failed calibration and a corrupt profile", [=] {
        {
            HF h = sessionRig(); // missing profile (uncalRig)
            require(h.s().setClickGesture(true, h.now) && !h.s().hasProfile, "enable");
            h.quiet(700);
            const size_t mark = h.transport.reports.size();
            doTilt(h, TiltShape{}, 500);
            require(clicksSince(h, mark) == 1, "missing profile");
        }
        {
            HF h = uncalRig();
            h.s().calibrate(h.now);
            for (int i = 0; i < 1000 && h.s().calibration.phase != CalPhase::Failed; ++i) {
                h.run(hold(0, 40, 100));
            }
            h.quiet(400);
            require(clickTrain(h, mountIdentity, false) && h.s().clickTrainAccept(), "train");
            h.quiet(300);
            require(h.s().startUncalibratedDemo(h.now) && h.s().setClickGesture(true, h.now), "enable");
            h.quiet(700);
            const size_t mark = h.transport.reports.size();
            doTilt(h, TiltShape{}, 500);
            require(clicksSince(h, mark) == 1 && h.s().calibration.phase == CalPhase::Failed,
                    "failed calibration");
        }
        {
            HF h(false, EnableKind::Momentary);
            h.sw = false;
            require(h.repo.save(UserProfile{}), "save");
            h.profileStorage.slots[0][20] ^= 0x55;
            h.profileStorage.slots[1] = h.profileStorage.slots[0];
            const auto kept = h.profileStorage.slots[0];
            h.boot();
            h.quiet(400);
            require(clickTrain(h, mountIdentity, false) && h.s().clickTrainAccept(), "train");
            h.quiet(300);
            require(h.s().startUncalibratedDemo(h.now) && h.s().setClickGesture(true, h.now), "enable");
            h.quiet(700);
            const size_t mark = h.transport.reports.size();
            doTilt(h, TiltShape{}, 500);
            require(clicksSince(h, mark) == 1, "corrupt profile");
            require(h.s().profileState() == ProfileState::Corrupt && h.profileStorage.slots[0] == kept,
                    "corrupt record must stay reported and untouched");
        }
    });
    test("gesture click: a gesture is bound to the frame it was taught in; relearning invalidates it", [=] {
        HF h = teachRig();
        require(teachAll(h, mountSideways) && h.s().teachAccept(), "mapping");
        h.quiet(300);
        require(clickTrain(h, mountSideways, true) && h.s().clickTrainAccept(), "train (configured)");
        h.quiet(300);
        require(h.s().startUncalibratedDemo(h.now), "fallback session");
        require(!h.s().setClickGesture(true, h.now) &&
                    std::string(h.s().diagnostics.reason).find("configured") != std::string::npos,
                "a configured-frame gesture must not enable in the fallback");
        h.s().stopUncalibratedDemo("test");
        h.quiet(300);
        require(h.s().startConfiguredControl(h.now) && h.s().setClickGesture(true, h.now), "configured");
        h.quiet(700);
        const size_t mark = h.transport.reports.size();
        // the body gesture is a side tilt; in the configured frame that is the third row
        doTilt(h, TiltShape{}, 500, &mountSideways);
        require(clicksSince(h, mark) == 1, "configured gesture click");
        h.s().stopUncalibratedDemo("test");
        require(teachAll(h, mountSideways) && h.s().teachAccept(), "relearn");
        require(!h.s().clickStatus(h.now).ready, "a gesture from the old learned frame survived");
        HF g = teachRig();
        require(!g.s().clickTrainStart(g.now, true), "configured gesture needs a learned mapping");
    });
    test("gesture click: training failures keep the previous gesture; status JSON is bounded", [=] {
        HF h = sessionRig();
        require(h.s().clickStatus(h.now).ready, "precondition");
        h.s().stopUncalibratedDemo("test");
        h.quiet(300);
        require(h.s().clickTrainStart(h.now, false), "start");
        for (unsigned i = 0; i < 1000 && h.s().state == SystemState::Teaching; ++i) {
            rawTick(h, mountIdentity, {(i / 40) % 2 ? 40.f : -40.f, 0, 0});
        }
        require(h.s().state != SystemState::Teaching && h.s().clickStatus(h.now).ready,
                "a failed training must keep the previous gesture");
        require(h.s().clickTrainStart(h.now, false), "restart");
        h.s().clickTrainCancel();
        require(h.s().state != SystemState::Teaching && h.s().clickStatus(h.now).ready, "cancel");
        char buffer[clickJsonCapacity];
        const size_t n = clickJson(buffer, sizeof buffer, h.s().clickStatus(h.now));
        require(n > 100 && n < clickJsonCapacity * 3 / 4, "length headroom");
        for (const char* bad : {"nan", "inf", "null"}) {
            require(!std::strstr(buffer, bad), "non-finite token");
        }
        h.s().clickClear();
        require(!h.s().clickStatus(h.now).ready, "forget");
    });

    test("mapping: ordinary head tilt never blocks or stops configured control; gross remounts do", [=] {
        HF h = teachRig();
        require(teachAll(h, mountIdentity) && h.s().teachAccept(), "teach");
        h.quiet(300);
        auto tilted = [&](float degrees, const std::array<float, 3>& about) {
            // gravity (0,0,1) rotated by `degrees` about `about` (a unit axis)
            const float t = degrees * 3.14159265f / 180.f, c = std::cos(t), sn = std::sin(t);
            const std::array<float, 3> v{0, 0, 1};
            const std::array<float, 3> cross{about[1] * v[2] - about[2] * v[1],
                                             about[2] * v[0] - about[0] * v[2],
                                             about[0] * v[1] - about[1] * v[0]};
            const float d = about[0] * v[0] + about[1] * v[1] + about[2] * v[2];
            std::array<float, 3> a{};
            for (unsigned i = 0; i < 3; ++i) {
                a[i] = v[i] * c + cross[i] * sn + about[i] * d * (1 - c);
            }
            for (unsigned i = 0; i < 40; ++i) {
                h.now += 10;
                h.sys->setControlSwitch(h.sw, h.now);
                h.sys->tick({h.now, {-1.2f, 0, 0}, a, true}, h.now, false);
            }
        };
        for (float deg : {20.f, 40.f, 60.f, 70.f}) { // nod / lean / tilt: ordinary head movement
            tilted(deg, {1, 0, 0});
            require(h.s().startConfiguredControl(h.now), "ordinary head tilt blocked a start");
            h.s().stopUncalibratedDemo("test");
            tilted(deg, {0, 1, 0});
            require(h.s().startConfiguredControl(h.now), "ordinary sideways head tilt blocked a start");
            h.s().stopUncalibratedDemo("test");
        }
        tilted(40.f, {1, 0, 0});
        require(h.s().mappingStatus(h.now).mountingWarning, "a large tilt should show a warning");
        tilted(0.f, {1, 0, 0});
        require(!h.s().mappingStatus(h.now).mountingWarning, "no warning at the taught posture");
        // a running session keeps the mapping through head movement (the guard is a start check only)
        require(h.s().startConfiguredControl(h.now), "start");
        tilted(60.f, {1, 0, 0});
        tilted(0.f, {0, 1, 0});
        require(h.s().configuredControl(), "head movement stopped the session");
        h.s().stopUncalibratedDemo("test");
        // gross re-orientation (the board flipped over): blocked
        tilted(110.f, {1, 0, 0});
        require(!h.s().startConfiguredControl(h.now) &&
                    std::string(h.s().diagnostics.reason) ==
                        "mounting changed: teach the movements again",
                "a flipped sensor must be refused");
        tilted(0.f, {1, 0, 0});
        // KNOWN LIMIT: a turn about the gravity axis is not visible in the gravity direction at all.
        // (Here: gravity along the z axis, the board turned about z: the accelerometer reading is the
        // same, but left/right/up/down no longer match the taught gyro axes.)
        tilted(90.f, {0, 0, 1});
        require(h.s().startConfiguredControl(h.now),
                "documented limit: a turn about the gravity axis cannot be detected");
    });

    // ------------------------------------------------ R: EXPERIMENTAL quick tilt-and-return click
    struct Tilt {
        std::array<float, 3> dir{0, 0, 1};
        float excursionDeg = 13.f;
        unsigned ms = 600;
        float returnFrac = 1.f;
    };
    auto tiltRate = [](const Tilt& g, float t) {
        std::array<float, 3> out{0, 0, 0};
        if (t < 0 || t > float(g.ms)) {
            return out;
        }
        const float half = float(g.ms) / 2.f;
        const float amp = g.excursionDeg * 3.14159265f / (half / 1000.f) / 2.f;
        const float level = t < half ? amp * std::sin(3.14159265f * t / half)
                                     : -g.returnFrac * amp * std::sin(3.14159265f * (t - half) / half);
        return std::array<float, 3>{g.dir[0] * level, g.dir[1] * level, g.dir[2] * level};
    };
    // cue-driven practice through the System; ends in the preview
    auto quickPractice = [=](HF& h, const M3& m, bool configuredFrame,
                             const std::function<Tilt(unsigned)>& shape = nullptr) {
        require(h.s().quickPracticeStart(h.now, configuredFrame), "practice refused");
        QuickCue last = QuickCue::None;
        unsigned attempt = 0;
        bool moving = false;
        uint32_t moveStart = 0;
        Tilt current;
        std::mt19937 pointRng(31);
        uint32_t pointUntil = 0, pointFrom = 0;
        std::array<float, 3> pointDir{1, 0, 0};
        float pointPeak = 30;
        for (unsigned i = 0; i < 40000 && h.s().state == SystemState::Teaching; ++i) {
            const QuickStatus st = h.s().quickStatus(h.now);
            if (st.phase == QuickPhase::Preview) {
                break;
            }
            if (st.cue == QuickCue::Go && last != QuickCue::Go) {
                ++attempt;
                current = shape ? shape(attempt) : Tilt{};
                moving = true;
                moveStart = h.now + 300;
            }
            last = st.cue;
            std::array<float, 3> b{};
            if (st.phase == QuickPhase::Pointing) {
                // the measured ordinary-pointing sample: yaw/pitch sweeps in random directions
                b = pointingAt(pointRng, h.now, pointUntil, pointFrom, pointDir, pointPeak);
            } else if (moving && h.now >= moveStart) {
                if (h.now - moveStart > current.ms) {
                    moving = false;
                } else {
                    b = tiltRate(current, float(h.now - moveStart));
                }
            }
            rawTick(h, m, b);
        }
        return h.s().quickStatus(h.now).phase == QuickPhase::Preview;
    };
    auto quickSession = [=]() {
        HF h = uncalRig();
        require(quickPractice(h, mountIdentity, false) && h.s().quickPracticeAccept(), "practice");
        h.quiet(300);
        require(h.s().startUncalibratedDemo(h.now), "session");
        h.quiet(300);
        return h;
    };
    auto doQuick = [=](HF& h, const Tilt& g, unsigned tailMs, const M3* mount = nullptr) {
        const M3& m = mount ? *mount : mountIdentity;
        for (unsigned t = 0; t <= g.ms; t += 10) {
            rawTick(h, m, tiltRate(g, float(t)));
        }
        for (unsigned t = 0; t < tailMs; t += 10) {
            rawTick(h, m, {0, 0, 0});
        }
    };
    test("quick gesture: off by default; practice needs no saved profile; enabling is explicit", [=] {
        HF h = quickSession();
        require(!h.s().quickGestureEnabled(), "must start disabled");
        require(h.profileStorage.read(0).empty() && h.configStorage.slots[0].empty() &&
                    h.controlStorage.slots[0].empty(),
                "practice must not write any storage");
        const size_t mark = h.transport.reports.size();
        doQuick(h, Tilt{}, 1000);
        require(clicksSince(h, mark) == 0, "a click before the gesture was enabled");
        HF fresh = uncalRig();
        require(!fresh.s().setQuickGesture(true, fresh.now), "enabled without practice or session");
        require(quickPractice(fresh, mountIdentity, false) && fresh.s().quickPracticeAccept(), "p");
        require(!fresh.s().setQuickGesture(true, fresh.now), "enabled without a running session");
        require(fresh.s().startUncalibratedDemo(fresh.now), "session");
        require(fresh.s().setQuickGesture(true, fresh.now), "enable refused after practice");
        require(h.s().quickStatus(h.now).ready && !h.s().quickStatus(h.now).enabled, "status");
    });
    test("quick gesture: an ambiguous practice is explained and never silently enabled", [=] {
        HF h = uncalRig();
        auto shape = [](unsigned attempt) {
            Tilt g;
            if (attempt == 1) {
                g.excursionDeg = 3; // tiny
            } else if (attempt == 2) {
                g.dir = {1, 0, 0}; // a pointing movement
            }
            return g;
        };
        std::string seen;
        std::mt19937 prng(77);
        uint32_t puntil = 0, pfrom = 0;
        std::array<float, 3> pdir{1, 0, 0};
        float ppeak = 30;
        require(h.s().quickPracticeStart(h.now, false), "start");
        QuickCue last = QuickCue::None;
        unsigned attempt = 0;
        bool moving = false;
        uint32_t moveStart = 0;
        Tilt cur;
        for (unsigned i = 0; i < 40000 && h.s().state == SystemState::Teaching; ++i) {
            const QuickStatus st = h.s().quickStatus(h.now);
            seen += std::string(st.reason) + "|";
            if (st.phase == QuickPhase::Preview) {
                break;
            }
            if (st.cue == QuickCue::Go && last != QuickCue::Go) {
                cur = shape(++attempt);
                moving = true;
                moveStart = h.now + 300;
            }
            last = st.cue;
            std::array<float, 3> b{};
            if (st.phase == QuickPhase::Pointing) {
                b = pointingAt(prng, h.now, puntil, pfrom, pdir, ppeak);
            } else if (moving && h.now >= moveStart && h.now - moveStart <= cur.ms) {
                b = tiltRate(cur, float(h.now - moveStart));
            }
            rawTick(h, mountIdentity, b);
        }
        require(seen.find("too small") != std::string::npos, "tiny tilt not explained");
        require(seen.find("ordinary pointing") != std::string::npos, "pointing-like tilt not explained");
        require(!h.s().quickStatus(h.now).ready, "ready before the practice was accepted");
        require(h.s().quickStatus(h.now).phase == QuickPhase::Preview, "recovered on the third tilt");
    });
    test("quick gesture: one press and release, pointer frozen during the candidate, nothing replayed", [=] {
        HF h = quickSession();
        require(h.s().setQuickGesture(true, h.now), "enable");
        h.quiet(800);
        Tilt leaky;
        leaky.dir = {.2f, 0, .98f}; // a tilt with some yaw in it: pointer-moving, but still a tilt
        const size_t mark = h.transport.reports.size();
        doQuick(h, leaky, 300);
        h.quiet(1500);
        require(clicksSince(h, mark) == 1, "not exactly one click");
        require(h.released(), "the release was not sent last");
        float during = 0, total = 0;
        unsigned downs = 0;
        size_t clickAt = h.transport.reports.size();
        for (size_t i = mark; i < h.transport.reports.size(); ++i) {
            const auto& r = h.transport.reports[i];
            total += std::abs(float(r.dx)) + std::abs(float(r.dy));
            if (r.down) {
                ++downs;
                clickAt = std::min(clickAt, i);
                require(r.dx == 0 && r.dy == 0 && r.wheel == 0, "movement inside the click report");
            }
        }
        for (size_t i = mark; i < clickAt; ++i) {
            during += std::abs(float(h.transport.reports[i].dx));
        }
        require(downs == 1, "one press report only");
        // Before candidate detection nothing can be undone; once it opens, movement is discarded.
        require(total <= 12.f, "the pointer jumped around the gesture (frozen movement replayed?)");
        const QuickStatus st = h.s().quickStatus(h.now);
        require(st.accepted == 1 && st.suppressedMs > 300 && st.lastDurationMs > 400,
                "accepted/suppressed/duration must be reported");
        require(st.lastExcursionDeg > 8.f, "excursion must be reported");
        (void)during;
        // control: the same yaw alone (no gesture) DOES move the pointer
        HF g = quickSession();
        const size_t markG = g.transport.reports.size();
        g.run(hold(0, 20, 600));
        float moved = 0;
        for (size_t i = markG; i < g.transport.reports.size(); ++i) {
            moved += std::abs(float(g.transport.reports[i].dx));
        }
        require(moved > 15.f, "precondition: yaw moves the pointer");
    });
    test("quick gesture: no repeat while still, a fresh neutral period and the interval rearm it", [=] {
        HF h = quickSession();
        require(h.s().setQuickGesture(true, h.now), "enable");
        h.quiet(800);
        const size_t mark = h.transport.reports.size();
        doQuick(h, Tilt{}, 250);
        doQuick(h, Tilt{}, 100); // immediately again: not armed
        h.quiet(20000);
        require(clicksSince(h, mark) == 1, "repeat or early second click");
        doQuick(h, Tilt{}, 400);
        require(clicksSince(h, mark) == 2, "rearmed after neutral and the minimum interval");
    });
    test("quick gesture: ordinary pointing, reversals and tremor never click", [=] {
        HF h = quickSession();
        require(h.s().setQuickGesture(true, h.now), "enable");
        h.quiet(800);
        const size_t mark = h.transport.reports.size();
        std::mt19937 rng(12);
        std::uniform_real_distribution<float> u(0.f, 1.f);
        uint32_t until = 0, from = 0;
        std::array<float, 3> dir{1, 0, 0};
        float peak = 30;
        bool reversal = false;
        for (unsigned i = 0; i < 30000; ++i) { // 300 s
            if (h.now >= until) {
                from = h.now;
                until = from + 250 + unsigned(1200 * u(rng));
                const float a = 2 * 3.14159265f * u(rng);
                dir = {std::cos(a), std::sin(a), .25f * (u(rng) - .5f) * 2.f};
                peak = 15 + 90 * u(rng);
                reversal = u(rng) < .3f;
            }
            const float x = float(h.now - from) / float(until - from);
            const float level = reversal ? peak * std::sin(2 * 3.14159265f * x)
                                         : peak * std::sin(3.14159265f * x);
            const float tremor = 4.f * std::sin(2 * 3.14159265f * 9.f * float(h.now) / 1000.f);
            rawTick(h, mountIdentity, {dir[0] * level + tremor, dir[1] * level + tremor, dir[2] * level + tremor});
        }
        const QuickStatus st = h.s().quickStatus(h.now);
        std::printf("INFO synthetic (System): 300 s of pointing: %lu clicks, %lu candidates, %lu ms suppressed\n",
                    static_cast<unsigned long>(clicksSince(h, mark)), static_cast<unsigned long>(st.candidates),
                    static_cast<unsigned long>(st.suppressedMs));
        require(clicksSince(h, mark) == 0, "false click while pointing");
        require(h.s().uncalibratedDemo(), "pointing must not stop the session");
    });
    test("quick gesture: a release failure faults, stops the session and disables it", [=] {
        HF h = quickSession();
        require(h.s().setQuickGesture(true, h.now), "enable");
        h.quiet(800);
        const uint32_t faultsBefore = h.s().diagnostics.faults;
        h.transport.failRelease = true;
        doQuick(h, Tilt{}, 170); // settled confirmation at 150 ms; recovery needs 200 ms more
        require(h.s().state == SystemState::SafeState && h.s().diagnostics.faults == faultsBefore + 1,
                "no fault after a failed release");
        require(!h.s().uncalibratedDemo() && !h.s().quickGestureEnabled(), "session survived");
        h.transport.failRelease = false;
        const size_t mark = h.transport.reports.size();
        doQuick(h, Tilt{}, 900);
        for (size_t i = mark; i < h.transport.reports.size(); ++i) {
            require(!h.transport.reports[i].down, "output continued after the fault");
        }
        h.quiet(600);
        require(h.s().state != SystemState::Active, "recovery restarted control");
        require(h.s().startUncalibratedDemo(h.now) && !h.s().quickGestureEnabled(),
                "an explicit restart must come back with the gesture off");
    });
    test("quick gesture: button, website stop, fault, disconnect, calibration and practice cancel it", [=] {
        for (int how = 0; how < 7; ++how) {
            HF h = quickSession();
            require(h.s().setQuickGesture(true, h.now), "enable");
            h.quiet(800);
            const size_t mark = h.transport.reports.size();
            for (unsigned t = 0; t <= 250; t += 10) {
                rawTick(h, mountIdentity, tiltRate(Tilt{}, float(t)));
            }
            require(h.s().quickStatus(h.now).suppressing, "candidate not open");
            if (how == 0) {
                h.sw = true;
                h.tick();
                h.sw = false;
            } else if (how == 1) {
                h.s().stopUncalibratedDemo("website stop");
            } else if (how == 2) {
                h.now += 10;
                h.s().tick({h.now, {NAN, 0, 0}, {0, 0, 1}, true}, h.now, false);
            } else if (how == 3) {
                h.transport.online = false;
                h.quiet(50);
            } else if (how == 4) {
                h.s().calibrate(h.now);
            } else if (how == 5) {
                h.s().quickPracticeStart(h.now, false);
            } else {
                h.s().pause();
            }
            require(!h.s().quickGestureEnabled() && !h.s().uncalibratedDemo(), "still enabled");
            h.transport.online = true;
            h.s().cancelCalibration();
            h.s().quickPracticeCancel();
            h.quiet(1500);
            for (unsigned t = 250; t <= 600; t += 10) {
                rawTick(h, mountIdentity, tiltRate(Tilt{}, float(t)));
            }
            h.quiet(900);
            require(clicksSince(h, mark) == 0, "a click after a cancelled candidate");
            require(h.s().state != SystemState::Active, "something restarted control");
        }
    });
    test("quick gesture: a reboot or a mode change never leaves it enabled", [=] {
        HF h = quickSession();
        require(h.s().setQuickGesture(true, h.now), "enable");
        h.boot();
        h.quiet(600);
        require(!h.s().quickGestureEnabled() && !h.s().quickStatus(h.now).ready,
                "a reboot kept the gesture or its practice (RAM only)");
        HF g = teachRig();
        require(teachAll(g, mountSideways) && g.s().teachAccept(), "mapping");
        g.quiet(300);
        require(quickPractice(g, mountSideways, true) && g.s().quickPracticeAccept(), "configured practice");
        g.quiet(300);
        require(g.s().startConfiguredControl(g.now) && g.s().setQuickGesture(true, g.now), "configured");
        g.s().stopUncalibratedDemo("mode change");
        require(g.s().startUncalibratedDemo(g.now), "fallback session");
        require(!g.s().quickGestureEnabled(), "the gesture survived a mode change");
        require(!g.s().setQuickGesture(true, g.now) &&
                    std::string(g.s().diagnostics.reason).find("configured") != std::string::npos,
                "a configured practice must not enable in the fallback");
    });
    test("quick gesture: mutually exclusive with dwell and the trained gesture", [=] {
        HF h = quickSession();
        h.s().stopUncalibratedDemo("test");
        require(clickTrain(h, mountIdentity, false) && h.s().clickTrainAccept(), "trained gesture");
        h.quiet(300);
        require(h.s().startUncalibratedDemo(h.now), "session");
        require(h.s().setUncalibratedDwell(true, h.now), "dwell on");
        require(h.s().setQuickGesture(true, h.now), "quick on");
        require(!h.s().handsFreeStatus().uncalDwellEnabled && h.s().quickGestureEnabled(),
                "enabling the quick gesture must turn dwell off");
        require(h.s().setUncalibratedDwell(true, h.now), "dwell on again");
        require(!h.s().quickGestureEnabled(), "enabling dwell must turn the quick gesture off");
        require(h.s().setQuickGesture(true, h.now) && h.s().setClickGesture(true, h.now),
                "enabling the trained gesture");
        require(!h.s().quickGestureEnabled() && h.s().clickGestureEnabled(),
                "the trained gesture must turn the quick one off");
        require(h.s().setQuickGesture(true, h.now) && !h.s().clickGestureEnabled(),
                "and vice versa");
    });
    test("quick gesture: works in configured pointing for a rotated mounting, no saved profile", [=] {
        HF h = teachRig();
        require(teachAll(h, mountSideways) && h.s().teachAccept(), "mapping");
        h.quiet(300);
        require(quickPractice(h, mountSideways, true) && h.s().quickPracticeAccept(), "practice");
        h.quiet(300);
        require(h.s().startConfiguredControl(h.now) && h.s().setQuickGesture(true, h.now), "enable");
        h.quiet(800);
        const size_t mark = h.transport.reports.size();
        doQuick(h, Tilt{}, 400, &mountSideways);
        require(clicksSince(h, mark) == 1, "configured quick click");
        require(h.profileStorage.read(0).empty() && h.controlStorage.slots[0].empty(),
                "nothing may be saved by the practice");
        h.s().stopUncalibratedDemo("test");
        require(teachAll(h, mountSideways) && h.s().teachAccept(), "relearn");
        require(!h.s().quickStatus(h.now).ready, "a practice from the old learned frame survived");
    });
    test("quick gesture: settings are validated and shown; status JSON is bounded", [=] {
        HF h = quickSession();
        require(!h.s().setQuickSettings(.4f, .35f) && !h.s().setQuickSettings(1.f, .7f) &&
                    !h.s().setQuickSettings(NAN, .3f) && !h.s().setQuickSettings(1.f, .1f),
                "an invalid setting was accepted");
        require(h.s().setQuickSettings(1.5f, .25f), "valid setting refused");
        require(h.s().setQuickGesture(true, h.now), "enable");
        const QuickStatus st = h.s().quickStatus(h.now);
        require(std::abs(st.sensitivity - 1.5f) < .001f && std::abs(st.returnTolerance - .25f) < .001f,
                "settings not reported");
        char buffer[quickJsonCapacity];
        const size_t n = quickJson(buffer, sizeof buffer, st);
        require(n > 100 && n < quickJsonCapacity * 3 / 4, "length headroom");
        for (const char* bad : {"nan", "inf", "null"}) {
            require(!std::strstr(buffer, bad), "non-finite token");
        }
        for (const char* key : {"\"state\":\"", "\"suppressedMs\"", "\"lastReject\"", "\"excursion\"",
                                "\"residual\"", "\"durationMs\"", "\"sensitivity\""}) {
            require(std::strstr(buffer, key), key);
        }
    });

    test("quick gesture: movements outside the designated direction keep pointing normally, unsuppressed", [=] {
        HF h = quickSession();
        require(h.s().setQuickGesture(true, h.now), "enable");
        h.quiet(800);
        auto movedAndStatus = [&](const std::function<void()>& move) {
            const size_t mark = h.transport.reports.size();
            const uint32_t suppressedBefore = h.s().quickStatus(h.now).suppressedMs;
            const uint32_t candidatesBefore = h.s().quickStatus(h.now).candidates;
            move();
            float moved = 0;
            for (size_t i = mark; i < h.transport.reports.size(); ++i) {
                moved += std::abs(float(h.transport.reports[i].dx)) + std::abs(float(h.transport.reports[i].dy));
            }
            const QuickStatus st = h.s().quickStatus(h.now);
            require(st.suppressedMs == suppressedBefore && st.candidates == candidatesBefore,
                    "the pointer was suppressed for a movement outside the designated direction");
            return moved;
        };
        // ordinary pointing: yaw, pitch and diagonals
        require(movedAndStatus([&] { h.run(hold(0, 60, 500)); h.quiet(500); }) > 20.f, "yaw did not point");
        require(movedAndStatus([&] { h.run(hold(1, 60, 500)); h.quiet(500); }) > 20.f, "pitch did not point");
        // the OPPOSITE tilt (with some yaw so the pointer would move) must not freeze the pointer
        Tilt opposite;
        opposite.dir = {.3f, 0, -.95f};
        opposite.excursionDeg = 20.f;
        require(movedAndStatus([&] { doQuick(h, opposite, 500); }) > 5.f,
                "the opposite tilt was not treated as normal pointing");
        // a perpendicular tilt (about the pitch axis)
        Tilt perpendicular;
        perpendicular.dir = {.2f, .97f, 0};
        require(movedAndStatus([&] { doQuick(h, perpendicular, 500); }) > 5.f,
                "a perpendicular movement was not treated as normal pointing");
        require(h.s().quickStatus(h.now).clicks == 0, "a click from non-designated movement");
        // while the designated tilt still works afterwards
        const size_t mark = h.transport.reports.size();
        doQuick(h, Tilt{}, 700);
        require(clicksSince(h, mark) == 1, "the designated gesture stopped working");
    });
    test("quick gesture: deviation after the candidate began cancels it; pointing resumes, nothing replays", [=] {
        auto run = [&](bool quick) {
            HF h = quickSession();
            if (quick) {
                require(h.s().setQuickGesture(true, h.now), "enable");
            }
            h.quiet(800);
            Tilt drifting;
            const size_t mark = h.transport.reports.size();
            // an aligned start, then a strong sideways drift (a pointing move) that never returns
            for (unsigned t = 0; t <= 900; t += 10) {
                auto b = tiltRate(drifting, float(t));
                if (t >= 100) {
                    b[0] += 30.f; // a strong yaw: the user starts pointing instead
                }
                rawTick(h, mountIdentity, b);
            }
            h.quiet(300);
            float moved = 0, biggest = 0;
            for (size_t i = mark; i < h.transport.reports.size(); ++i) {
                const float step = std::abs(float(h.transport.reports[i].dx));
                moved += step;
                biggest = std::max(biggest, step);
            }
            return std::array<float, 4>{moved, biggest, float(clicksSince(h, mark)),
                                        float(h.s().quickStatus(h.now).rejected)};
        };
        const auto control = run(false), gated = run(true);
        require(gated[2] == 0, "clicked after a deviating gesture");
        require(gated[3] >= 1, "the candidate should have been rejected");
        require(gated[0] > 5.f, "pointing must resume after the cancel");
        require(gated[0] <= control[0] + 1.f, "frozen movement was replayed");
        require(gated[1] <= start::uncalDemoMaxStep, "a jump after the cancel");
        require(gated[0] < control[0], "the frozen part of the movement must be discarded");
    });
    test("quick gesture: the designated direction changes only through another practice", [=] {
        HF h = quickSession();
        const QuickStatus first = h.s().quickStatus(h.now);
        require(first.designated && std::abs(first.direction[2]) > .9f, "direction not reported");
        require(h.s().setQuickSettings(1.f, .35f, 45.f) &&
                    std::abs(h.s().quickStatus(h.now).directionToleranceDeg - 45.f) < .01f,
                "the angular tolerance is a validated setting");
        require(!h.s().setQuickSettings(1.f, .35f, 5.f) && !h.s().setQuickSettings(1.f, .35f, 75.f),
                "an out-of-range angle was accepted");
        const auto kept = h.s().quickStatus(h.now).direction;
        require(h.s().quickStatus(h.now).direction == kept, "settings must not change the direction");
        h.s().stopUncalibratedDemo("test");
        // practise a DIFFERENT direction (a tilt with a large yaw-free component along axis 1 is a
        // pointing move, so use another out-of-plane direction)
        require(quickPractice(h, mountIdentity, false, [](unsigned) {
                    Tilt g;
                    g.dir = {.35f, .2f, -.91f};
                    return g;
                }) && h.s().quickPracticeAccept(),
                "second practice");
        const QuickStatus second = h.s().quickStatus(h.now);
        require(second.designated && second.direction[2] < -.6f && first.direction[2] > .6f,
                "the new practice must replace the designated direction (and its sign)");
        h.s().quickClear();
        require(!h.s().quickStatus(h.now).designated, "forgetting must clear the designated direction");
        char buffer[quickJsonCapacity];
        quickJson(buffer, sizeof buffer, second);
        require(std::strstr(buffer, "\"designated\":true") && std::strstr(buffer, "\"direction\":["),
                "direction fields");
    });

    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
