// Guided mapping, One Euro smoothing and the learned-settings record. Software tests on SYNTHETIC
// recordings (a modelled sensor with noise, bias and a rotated mounting); nothing here is a
// hardware measurement. The comparison table printed at the end is also synthetic.
#include "nodx/mapping.hpp"
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

using Mat = std::array<Vec3, 3>; // rows: sensor axis = row . body vector
Vec3 mul(const Mat& m, const Vec3& v) {
    return {dot(m[0], v), dot(m[1], v), dot(m[2], v)};
}
Mat rotation(const Vec3& axisIn, float degrees) {
    const Vec3 a = {axisIn[0] / norm(axisIn), axisIn[1] / norm(axisIn), axisIn[2] / norm(axisIn)};
    const float t = degrees * 3.14159265f / 180.f, c = std::cos(t), s = std::sin(t), k = 1 - c;
    return {Vec3{c + a[0] * a[0] * k, a[0] * a[1] * k - a[2] * s, a[0] * a[2] * k + a[1] * s},
            Vec3{a[1] * a[0] * k + a[2] * s, c + a[1] * a[1] * k, a[1] * a[2] * k - a[0] * s},
            Vec3{a[2] * a[0] * k - a[1] * s, a[2] * a[1] * k + a[0] * s, c + a[2] * a[2] * k}};
}
const Mat identity = {Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1}};
// Body frame: x = pointer right, y = pointer down, z = toward the user (gravity along +z at rest).
const Vec3 bodyDirection[4] = {{1, 0, 0}, {-1, 0, 0}, {0, -1, 0}, {0, 1, 0}}; // R L U D
const Vec3 biasTruth{-1.2f, -0.9f, 0.1f};

struct Rig {
    Mat mount = identity;
    std::mt19937 rng{12345};
    std::normal_distribution<float> noise{0.f, .8f};
    uint32_t now = 0;
    MotionSample sample(const Vec3& bodyRate) {
        const Vec3 g = mul(mount, bodyRate);
        const Vec3 a = mul(mount, {0, 0, 1});
        MotionSample s;
        s.timestampMs = now;
        s.gyro = {g[0] + biasTruth[0] + noise(rng), g[1] + biasTruth[1] + noise(rng),
                  g[2] + biasTruth[2] + noise(rng)};
        s.accel = {a[0] + .002f * noise(rng), a[1] + .002f * noise(rng), a[2] + .002f * noise(rng)};
        return s;
    }
};

struct Move {
    Vec3 body{};      // direction in the body frame (unit)
    float peak = 40;  // deg/s
    unsigned ms = 600;
    unsigned latency = 300;
    bool wobble = false;
};
using Behavior = std::function<Move(unsigned step, int direction, bool validation, unsigned attempt)>;
Move goodMove(unsigned, int direction, bool, unsigned) {
    Move m;
    m.body = bodyDirection[direction];
    return m;
}

// A synthetic "user": still until the GO cue, then one half-sine movement, then still again.
struct Driver {
    Rig rig;
    MappingTeacher teacher;
    Behavior behavior = goodMove;
    MapCue lastCue = MapCue::None;
    unsigned attempt = 0;
    bool moving = false;
    uint32_t moveStart = 0;
    Move move;
    unsigned rejections = 0;
    std::vector<std::string> reasons;
    void start() {
        teacher.begin(rig.now);
    }
    Vec3 body() {
        if (!moving) {
            return {0, 0, 0};
        }
        if (rig.now < moveStart) {
            return {0, 0, 0}; // human reaction time
        }
        const float t = float(rig.now - moveStart) / float(move.ms);
        if (t > 1) {
            moving = false;
            return {0, 0, 0};
        }
        float level = move.peak * std::sin(3.14159265f * t);
        if (move.wobble) {
            level *= std::sin(6.2831853f * 3.f * t); // back and forth three times
        }
        return {move.body[0] * level, move.body[1] * level, move.body[2] * level};
    }
    void step() {
        rig.now += 10;
        const MappingStatus st = teacher.status(rig.now);
        if (st.cue == MapCue::Go && lastCue != MapCue::Go) {
            ++attempt;
            move = behavior(st.step, st.direction, st.validation, attempt);
            moving = true;
            moveStart = rig.now + move.latency;
        }
        lastCue = st.cue;
        const Vec3 b = body();
        teacher.tick(rig.sample(b), rig.now);
        if (std::string(teacher.status(rig.now).reason) != "idle" && teacher.phase() == MapPhase::Example) {
            const char* r = teacher.status(rig.now).reason;
            if (reasons.empty() || reasons.back() != r) {
                reasons.push_back(r);
            }
        }
    }
    void run(unsigned ms) {
        for (unsigned i = 0; i < ms / 10; ++i) {
            step();
        }
    }
    bool until(MapPhase phase, unsigned maxMs) {
        for (unsigned i = 0; i < maxMs / 10 && teacher.phase() != phase; ++i) {
            if (!teacher.active() && teacher.phase() != phase) {
                return false;
            }
            step();
        }
        return teacher.phase() == phase;
    }
};

// Pointer outputs for a fresh body movement using the learned settings.
std::array<float, 2> pointerFor(const LearnedControl& c, const Mat& mount, const Vec3& bodyDir,
                                float peak = 40) {
    Rig rig;
    rig.mount = mount;
    ControlProcessor processor;
    float x = 0, y = 0;
    for (unsigned i = 0; i < 300; ++i) {
        rig.now += 10;
        Vec3 b{};
        if (i >= 50 && i < 110) {
            const float level = peak * std::sin(3.14159265f * float(i - 50) / 60.f);
            b = {bodyDir[0] * level, bodyDir[1] * level, bodyDir[2] * level};
        }
        const Motion m = processor.process(rig.sample(b), c);
        x += m.x * .01f;
        y += m.y * .01f;
    }
    return {x, y};
}

LearnedControl teach(const Mat& mount, Driver& d) {
    d.rig.mount = mount;
    d.start();
    if (!d.until(MapPhase::Preview, 240000)) {
        const MappingStatus st = d.teacher.status(d.rig.now);
        static char why[200];
        std::snprintf(why, sizeof why, "teaching stopped in %s step %u cue %s: %s", name(st.phase),
                      st.step, name(st.cue), st.reason);
        for (const auto& r : d.reasons) {
            std::printf("INFO   reason seen: %s\n", r.c_str());
        }
        throw std::runtime_error(why);
    }
    require(d.teacher.accept(), "accept refused in the preview");
    return d.teacher.candidate();
}
} // namespace

int main() {
    test("one euro: parameters are bounded and bad input restarts the filter", [] {
        require(OneEuroParams{}.valid(), "defaults");
        require(!(OneEuroParams{0.f, .02f, 2.f}.valid()) && !(OneEuroParams{2.f, -1.f, 2.f}.valid()) &&
                    !(OneEuroParams{NAN, .02f, 2.f}.valid()),
                "bad parameters accepted");
        OneEuroFilter f;
        const OneEuroParams p;
        require(f.filter(10.f, .01f, p) == 10.f, "first sample passes");
        require(f.filter(NAN, .01f, p) == 0.f, "NaN handled");
        require(f.filter(5.f, .01f, p) == 5.f, "restart after NaN");
        float out = 0;
        for (int i = 0; i < 500; ++i) {
            out = f.filter(20.f, .01f, p);
        }
        require(std::abs(out - 20.f) < .5f, "converges to a constant");
        require(f.filter(100.f, .5f, p) == 100.f, "an oversize dt restarts, never extrapolates");
        require(f.filter(100.f, 0.f, p) == 100.f, "a zero dt restarts");
    });
    test("one euro: uses the timestamp interval, not a fixed rate", [] {
        const OneEuroParams p;
        OneEuroFilter fast, slow;
        fast.filter(0, .01f, p);
        slow.filter(0, .01f, p);
        float a = 0, b = 0;
        for (int i = 0; i < 20; ++i) {
            a = fast.filter(10.f, .01f, p); // 20 samples over 0.2 s
        }
        for (int i = 0; i < 10; ++i) {
            b = slow.filter(10.f, .02f, p); // 10 samples over 0.2 s
        }
        require(std::abs(a - b) < .6f, "same elapsed time must give about the same response");
    });
    test("record: round trip, size, checksum, magic, finiteness and orthogonality", [] {
        LearnedControl c;
        c.horizontal = {0, 1, 0};
        c.vertical = {1, 0, 0};
        c.gravity = {0, 0, 1};
        require(c.valid(), "precondition");
        const auto bytes = encode(c, 7);
        require(bytes.size() == LearnedControl::recordBytes, "120 bytes");
        LearnedControl back;
        uint32_t generation = 0;
        require(decode(bytes, back, generation) && generation == 7 && back.horizontal == c.horizontal,
                "round trip");
        auto bad = bytes;
        bad[30] ^= 1;
        require(!decode(bad, back, generation), "checksum");
        bad = bytes;
        bad.pop_back();
        require(!decode(bad, back, generation), "size");
        LearnedControl skew = c;
        skew.vertical = {.8f, .6f, 0}; // not orthogonal
        require(!skew.valid() && !decode(encode(skew, 1), back, generation), "orthogonality");
        LearnedControl nan = c;
        nan.bias[0] = NAN;
        require(!nan.valid(), "NaN");
        auto wrong = bytes;
        wrong[0] ^= 0xff;
        require(!decode(wrong, back, generation), "magic");
    });
    test("repository: transactional two-slot save, corruption reported, previous kept", [] {
        MemoryStorage storage;
        ControlRepository repo(storage);
        LearnedControl c, loaded;
        require(!repo.load(loaded) && repo.state() == ControlRecordState::Missing, "missing");
        require(repo.save(c) && repo.load(loaded) && repo.state() == ControlRecordState::Valid,
                "save and load");
        LearnedControl second = c;
        second.gain = {20, 20, 20, 20};
        require(repo.save(second) && repo.load(loaded) && loaded.gain[0] == 20.f, "newest wins");
        storage.failWrite = true; // a failed save must not touch the newest valid slot
        LearnedControl third = c;
        third.gain = {30, 30, 30, 30};
        require(!repo.save(third), "failed write reported");
        storage.failWrite = false;
        require(repo.load(loaded) && loaded.gain[0] == 20.f, "previous settings preserved");
        storage.tearWrite = true;
        require(!repo.save(third), "torn write reported");
        storage.tearWrite = false;
        require(repo.load(loaded) && loaded.gain[0] == 20.f, "previous settings after a torn write");
        require(!repo.save(LearnedControl{.horizontal = {2, 0, 0}}), "invalid settings never saved");
        storage.slots[0][10] ^= 1;
        storage.slots[1][10] ^= 1;
        require(!repo.load(loaded) && repo.state() == ControlRecordState::Corrupt,
                "corruption must be reported, not treated as missing");
    });
    test("teacher: needs two contiguous qualified seconds; interruptions are counted and explained",
         [] {
             Driver d;
             d.start();
             d.run(1500);
             require(d.teacher.status(d.rig.now).stillMs >= 1400, "stillness accumulating");
             // A bump interrupts the run: it restarts and says why.
             for (int i = 0; i < 10; ++i) {
                 d.rig.now += 10;
                 d.teacher.tick(d.rig.sample({60, 0, 0}), d.rig.now);
             }
             const MappingStatus st = d.teacher.status(d.rig.now);
             require(st.interruptions >= 1 && std::string(st.reason).find("movement") != std::string::npos,
                     "interruption not explained");
             require(st.stillMs < 500, "the qualified run did not restart");
             d.run(2300);
             require(d.teacher.phase() == MapPhase::Example, "stillness not accepted after 2 s");
             const MappingStatus after = d.teacher.status(d.rig.now);
             require(std::abs(after.bias[0] - biasTruth[0]) < .5f &&
                         std::abs(after.bias[1] - biasTruth[1]) < .5f,
                     "gyro bias estimate");
             require(after.noise[0] > .4f && after.noise[0] < 1.4f, "noise estimate");
         });
    test("teacher: fails after ten seconds without two qualified seconds", [] {
        Driver d;
        d.start();
        for (unsigned i = 0; i < 1200 && d.teacher.active(); ++i) {
            d.rig.now += 10;
            const float shake = (i / 100) % 2 ? 50.f : -50.f; // never still for a second
            d.teacher.tick(d.rig.sample({shake, 0, 0}), d.rig.now);
        }
        require(d.teacher.phase() == MapPhase::Failed, "must fail");
        require(std::string(d.teacher.status(d.rig.now).reason).find("10 seconds") != std::string::npos,
                "reason");
    });

    struct Mount {
        const char* name;
        Mat matrix;
    };
    const Mount mounts[] = {
        {"identity", identity},
        {"upside down (180 deg about x)", rotation({1, 0, 0}, 180)},
        {"sideways (90 deg about z)", rotation({0, 0, 1}, 90)},
        {"sideways (90 deg about y)", rotation({0, 1, 0}, 90)},
        {"oblique (35 deg about 1,1,1)", rotation({1, 1, 1}, 35)},
        {"oblique (65 deg about 1,-2,.5)", rotation({1, -2, .5f}, 65)},
    };
    for (const Mount& mount : mounts) {
        test((std::string("teacher: learns right/left/up/down for ") + mount.name).c_str(), [mount] {
            Driver d;
            const LearnedControl c = teach(mount.matrix, d);
            require(c.valid(), "learned settings invalid");
            for (unsigned dir = 0; dir < 4; ++dir) {
                const auto out = pointerFor(c, mount.matrix, bodyDirection[dir]);
                const float wanted[4][2] = {{1, 0}, {-1, 0}, {0, -1}, {0, 1}};
                const float along = out[0] * wanted[dir][0] + out[1] * wanted[dir][1];
                const float across = std::abs(out[0] * wanted[dir][1]) + std::abs(out[1] * wanted[dir][0]);
                require(along > 8.f, "too little movement in the taught direction");
                require(across < .15f * along, "crosstalk into the other axis");
            }
            // gentle movements still respond
            const auto gentle = pointerFor(c, mount.matrix, bodyDirection[0], 15);
            require(gentle[0] > 2.f, "a gentle movement produced almost nothing");
            require(d.rejections == 0, "good teaching was rejected");
        });
    }
    test("teacher: a weak example is rejected and only that example is retried", [] {
        Driver d;
        d.behavior = [](unsigned step, int direction, bool validation, unsigned attempt) {
            Move m = goodMove(step, direction, validation, attempt);
            if (step == 4 && attempt == 5) { // the 5th attempt overall is step 4's first try
                m.peak = 9.f;                   // barely above the noise: too small
            }
            return m;
        };
        d.start();
        const MappingStatus first = [&] {
            d.until(MapPhase::Preview, 400000);
            return d.teacher.status(d.rig.now);
        }();
        require(first.phase == MapPhase::Preview, "recovered and finished");
        bool sawSmall = false;
        for (const auto& r : d.reasons) {
            sawSmall = sawSmall || r.find("too small") != std::string::npos ||
                       r.find("no movement") != std::string::npos;
        }
        require(sawSmall, "the weak example was not rejected with a reason");
        require(first.retries == 0, "retry counter must reset after success");
        require(d.attempt == 17, "exactly one extra attempt: 16 examples plus one retry");
    });
    test("teacher: a wobbling movement and the wrong direction are rejected", [] {
        Driver d;
        d.behavior = [](unsigned step, int direction, bool validation, unsigned attempt) {
            Move m = goodMove(step, direction, validation, attempt);
            if (attempt == 2) {
                m.wobble = true; // back-and-forth is not one steady turn
            }
            if (attempt == 4) {
                m.body = {0, 1, 0}; // second right example goes down instead
            }
            return m;
        };
        d.start();
        d.until(MapPhase::Preview, 400000);
        require(d.teacher.phase() == MapPhase::Preview, "should recover");
        bool wobble = false, different = false;
        for (const auto& r : d.reasons) {
            wobble = wobble || r.find("steady") != std::string::npos ||
                     r.find("too small") != std::string::npos; // a wobble cancels itself out
            different = different || r.find("different way") != std::string::npos;
        }
        require(wobble && different, "both rejections need plain reasons");
    });
    test("teacher: indistinguishable directions are refused", [] {
        Driver d;
        d.behavior = [](unsigned step, int direction, bool validation, unsigned attempt) {
            Move m = goodMove(step, direction, validation, attempt);
            if (direction == 2) {
                m.body = {1, 0, 0}; // "up" is taught as the same movement as right
            } else if (direction == 3) {
                m.body = {-1, 0, 0};
            }
            return m;
        };
        d.start();
        d.until(MapPhase::Preview, 400000);
        require(d.teacher.phase() == MapPhase::Failed, "must not produce settings");
        require(std::string(d.teacher.status(d.rig.now).reason).find("overlap") != std::string::npos,
                "reason should say the directions overlap");
    });
    test("teacher: left taught as right is refused as not opposite", [] {
        Driver d;
        d.behavior = [](unsigned step, int direction, bool validation, unsigned attempt) {
            Move m = goodMove(step, direction, validation, attempt);
            if (direction == 1) {
                m.body = {1, 0, 0};
            }
            return m;
        };
        d.start();
        d.until(MapPhase::Preview, 400000);
        require(d.teacher.phase() == MapPhase::Failed &&
                    std::string(d.teacher.status(d.rig.now).reason).find("opposite") != std::string::npos,
                "must say right and left were not opposite");
    });
    test("teacher: a failed validation example is retried alone, then accepted", [] {
        Driver d;
        d.behavior = [](unsigned step, int direction, bool validation, unsigned attempt) {
            Move m = goodMove(step, direction, validation, attempt);
            if (validation && direction == 2 && attempt == 15) {
                m.body = {0, 1, 0}; // validation "up" goes down
            }
            return m;
        };
        d.start();
        d.until(MapPhase::Preview, 400000);
        require(d.teacher.phase() == MapPhase::Preview, "should recover");
        bool noted = false;
        for (const auto& r : d.reasons) {
            noted = noted || r.find("did not match") != std::string::npos;
        }
        require(noted, "validation mismatch not explained");
        require(d.attempt == 17, "one retry only");
    });
    test("teacher: too many failed attempts at one example fail the teaching", [] {
        Driver d;
        d.behavior = [](unsigned step, int direction, bool validation, unsigned attempt) {
            Move m = goodMove(step, direction, validation, attempt);
            m.peak = 6; // never enough
            return m;
        };
        d.start();
        d.until(MapPhase::Preview, 600000);
        require(d.teacher.phase() == MapPhase::Failed, "should give up");
    });
    test("teacher: cancel and preview timeout stop cleanly; accept only in the preview", [] {
        Driver d;
        d.start();
        require(!d.teacher.accept(), "accept before the preview");
        d.teacher.cancel();
        require(d.teacher.phase() == MapPhase::Failed && !d.teacher.active(), "cancel");
        Driver e;
        e.start();
        require(e.until(MapPhase::Preview, 240000), "preview");
        e.run(start::mapPreviewTimeoutMs + 1000);
        require(e.teacher.phase() == MapPhase::Failed, "preview timeout");
    });
    test("teacher: the preview shows the learned directions", [] {
        Driver d;
        d.start();
        require(d.until(MapPhase::Preview, 240000), "preview");
        for (int i = 0; i < 60; ++i) {
            d.rig.now += 10;
            const float level = 30.f;
            d.teacher.tick(d.rig.sample({level, 0, 0}), d.rig.now); // pointer right
        }
        const MappingStatus st = d.teacher.status(d.rig.now);
        require(st.previewX > 10.f && std::abs(st.previewY) < 4.f && st.previewAngleX > 1.f,
                "preview must follow the learned mapping");
    });

    test("processor: deadzone hysteresis does not chatter at the threshold", [] {
        LearnedControl c;
        c.bias = {0, 0, 0};
        c.deadzoneEnter = {2.f, 2.f};
        c.deadzoneExit = {1.2f, 1.2f};
        c.filter = {20.f, 0.f, 20.f}; // nearly transparent filter: isolate the deadzone
        ControlProcessor p;
        Rig rig;
        unsigned transitions = 0;
        bool prev = false;
        for (unsigned i = 0; i < 400; ++i) {
            rig.now += 10;
            MotionSample s;
            s.timestampMs = rig.now;
            s.gyro = {1.6f + ((i % 2) ? .35f : -.35f), 0, 0}; // wobbles around 1.6 inside the band
            const Motion m = p.process(s, c);
            const bool active = m.x != 0.f;
            transitions += (active != prev) ? 1 : 0;
            prev = active;
        }
        require(transitions <= 2, "output chattered on and off");
    });
    test("processor: noise-based deadzone keeps a still sensor silent", [] {
        LearnedControl c;
        c.bias = biasTruth;
        c.deadzoneEnter = {3.f, 3.f};
        c.deadzoneExit = {1.8f, 1.8f};
        ControlProcessor p;
        Rig rig;
        float total = 0;
        for (unsigned i = 0; i < 2000; ++i) {
            rig.now += 10;
            const Motion m = p.process(rig.sample({0, 0, 0}), c);
            total += std::abs(m.x) + std::abs(m.y);
        }
        require(total < 20.f, "a still sensor moved the pointer");
    });

    // ---------------------------------------------------- measured comparison (synthetic)
    struct Metrics {
        float jitter, drift, gentleGain, stopMs;
    };
    // `residual` is the gyro bias left after bias removal (a bias estimate is never perfect).
    auto measure = [](const std::function<Motion(const MotionSample&)>& run, float residual) {
        Metrics m{};
        Rig rig;
        auto next = [&](float rate) {
            rig.now += 10;
            MotionSample s = rig.sample({rate, 0, 0});
            for (float& g : s.gyro) {
                g -= 0.f;
            }
            s.gyro[0] -= biasTruth[0] - residual; // what the processors see after bias removal
            s.gyro[1] -= biasTruth[1];
            s.gyro[2] -= biasTruth[2];
            return s;
        };
        float sum = 0, sum2 = 0;
        unsigned n = 0;
        for (unsigned i = 0; i < 3000; ++i) {
            const Motion out = run(next(0));
            if (i >= 300) {
                sum += out.x;
                sum2 += out.x * out.x;
                ++n;
            }
        }
        m.drift = sum / float(n);
        m.jitter = std::sqrt(std::max(0.f, sum2 / float(n) - m.drift * m.drift));
        float steady = 0;
        for (unsigned i = 0; i < 600; ++i) {
            const Motion out = run(next(15.f));
            if (i >= 400) {
                steady += out.x / 200.f;
            }
        }
        m.gentleGain = steady / 15.f;
        m.stopMs = 0;
        for (unsigned i = 0; i < 300; ++i) {
            const Motion out = run(next(0));
            if (std::abs(out.x) > .1f * std::max(1.f, steady)) {
                m.stopMs = float((i + 1) * 10);
            }
        }
        return m;
    };
    test("comparison (synthetic): provisional criterion, stopping delay at most +30 ms vs the EMA", [] {
        // Worst case over speeds and seeds, filter alone (deadzone out of the way).
        auto stopDelay = [](const std::function<float(const MotionSample&)>& run, float speed,
                            unsigned seed) {
            std::mt19937 rng(seed);
            std::normal_distribution<float> n(0.f, .8f);
            uint32_t now = 0;
            auto next = [&](float rate) {
                now += 10;
                MotionSample s;
                s.timestampMs = now;
                s.gyro = {rate + n(rng), n(rng), n(rng)};
                return s;
            };
            float steady = 0;
            for (unsigned i = 0; i < 500; ++i) {
                const float out = run(next(speed));
                if (i >= 400) {
                    steady += out / 100.f;
                }
            }
            float last = 0;
            for (unsigned i = 0; i < 300; ++i) {
                if (std::abs(run(next(0))) > .1f * std::max(1.f, steady)) {
                    last = float((i + 1) * 10);
                }
            }
            return last;
        };
        float worstEma = 0, worstEuro = 0;
        for (float speed : {15.f, 40.f, 80.f}) {
            for (unsigned seed = 1; seed <= 7; ++seed) {
                UserProfile profile;
                profile.bias = {0, 0, 0};
                profile.deadzone = {.1f, .1f};
                MotionProcessor ema;
                worstEma = std::max(worstEma, stopDelay([&](const MotionSample& s) {
                                        return ema.process(s, profile, .01f).x;
                                    }, speed, seed));
                LearnedControl c;
                c.bias = {0, 0, 0};
                c.deadzoneEnter = {.1f, .1f};
                c.deadzoneExit = {.06f, .06f};
                ControlProcessor euro; // the shipped START defaults
                worstEuro = std::max(worstEuro, stopDelay([&](const MotionSample& s) {
                                         return euro.process(s, c).x;
                                     }, speed, seed));
            }
        }
        std::printf("INFO stopping delay, worst over 3 speeds x 7 seeds: EMA %.0f ms, One Euro %.0f ms (criterion +30 ms)\n",
                    worstEma, worstEuro);
        require(worstEuro <= worstEma + 30.f, "UNMET: One Euro stops more than 30 ms later than the EMA");
    });
    test("comparison (synthetic): One Euro vs the current EMA, filter alone and end to end",
         [measure] {
             auto emaRun = [&measure](float deadzone, float residual) {
                 UserProfile profile;
                 profile.bias = {0, 0, 0};
                 profile.deadzone = {deadzone, deadzone};
                 MotionProcessor ema;
                 return measure(
                     [&](const MotionSample& s) { return ema.process(s, profile, .01f); }, residual);
             };
             auto euroRun = [&measure](float enter, float exit, float residual) {
                 LearnedControl c;
                 c.bias = {0, 0, 0};
                 c.deadzoneEnter = {enter, enter};
                 c.deadzoneExit = {exit, exit};
                 ControlProcessor euro;
                 return measure([&](const MotionSample& s) { return euro.process(s, c); }, residual);
             };
             auto row = [](const char* label, const Metrics& m) {
                 std::printf("INFO   %-34s jitter %.3f  drift %+.3f  gentle %.2f  stop %3.0f ms\n",
                             label, m.jitter, m.drift, m.gentleGain, m.stopMs);
             };
             std::printf("INFO synthetic comparison (modelled sensor, sigma 0.8 deg/s; NOT hardware):\n");
             const Metrics emaFilter = emaRun(.1f, 0.f), euroFilter = euroRun(.1f, .06f, 0.f);
             row("filter only, current EMA", emaFilter);
             row("filter only, One Euro", euroFilter);
             const Metrics emaFull = emaRun(.6f, .5f), euroFull = euroRun(3.2f, 1.9f, .5f);
             row("end to end, EMA + fixed dz 0.6", emaFull);
             row("end to end, One Euro + noise dz", euroFull);
             require(euroFilter.jitter < .6f * emaFilter.jitter, "filter alone is not steadier");
             require(euroFilter.gentleGain > .6f && euroFull.gentleGain > .6f, "gentle motion lost");
             require(euroFull.jitter <= emaFull.jitter + 1e-4f, "end to end is not steadier");
             require(std::abs(euroFull.drift) <= std::abs(emaFull.drift) + 1e-4f,
                     "residual bias drifts more than before");
             require(euroFilter.stopMs <= emaFilter.stopMs + 30.f &&
                         euroFull.stopMs <= emaFull.stopMs + 30.f,
                     "stopping delay grew too much");
         });

    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
