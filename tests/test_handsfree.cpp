// Hands-free revision regression tests. Deterministic, synthetic input only: they show that the
// logic behaves as specified, not accidental-trigger rates, comfort or suitability for any user.
#include "hf_support.hpp"
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
    for (size_t i = from + 1; i < h.transport.reports.size(); ++i) {
        if (h.transport.reports[i].down && !h.transport.reports[i - 1].down) {
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
        require(mutate([](auto& b) { setWord(b, 3, 4); }) == ConfigState::OutOfBounds, "flags");
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
        require(length > 100 && length < 900, "length");
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
        for (size_t i = mark + 1; i < h.transport.reports.size(); ++i) {
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

    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
