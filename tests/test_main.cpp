#include "nodx/engine.hpp"
#include "nodx/protocol.hpp"
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace nodx;
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
void near(float a, float b, float eps = .001f) {
    require(std::abs(a - b) < eps, "numeric mismatch");
}
class TestHID : public HIDTransport {
public:
    bool online = true, fail = false;
    std::vector<Report> reports;
    bool connected() const override {
        return online;
    }
    bool send(const Report& r) override {
        reports.push_back(r);
        return !fail;
    }
};
struct Rig {
    MemoryStorage storage;
    ProfileRepository repo{storage};
    TestHID transport;
    System system{transport, repo};
    uint32_t now = 0;
    Rig() {
        system.axes.axes = {0, 1, 2};
        system.axes.accelAxes = {0, 1, 2};
        system.axes.accelSigns = {1, 1, 1};
    }
    void tick(float yaw = 0, bool pressed = false) {
        now += 10;
        system.tick({now, {yaw, 0, 0}, {0, 0, 1}, true}, now, pressed);
    }
    void active() {
        require(system.setProfile(UserProfile{}), "save");
        for (unsigned i = 0; i < start::recoverySamples; ++i) {
            tick();
        }
        require(system.resume(), "resume");
    }
};
UserProfile calibrate(float noise, float left, float right, float up = 10, float down = 20) {
    CalibrationEngine cal;
    uint32_t now = 0;
    cal.start(now);
    while (cal.phase != CalPhase::Save && cal.phase != CalPhase::Failed && now < 20000) {
        now += 10;
        float n = (int((now / 10) % 7) - 3) * noise;
        MotionSample s{now, {n, n, n}, {0, 0, 1}, true};
        if (cal.phase == CalPhase::Left) {
            s.gyro[0] -= left;
        }
        if (cal.phase == CalPhase::Right) {
            s.gyro[0] += right;
        }
        if (cal.phase == CalPhase::Up) {
            s.gyro[1] -= up;
        }
        if (cal.phase == CalPhase::Down) {
            s.gyro[1] += down;
        }
        cal.tick(s, now);
    }
    require(cal.phase == CalPhase::Save, "calibration rejected");
    return cal.candidate;
}
int main() {
    unsigned passed = 0, failed = 0;
    auto test = [&](const char* label, std::function<void()> body) {
        try {
            body();
            ++passed;
            std::cout << "PASS " << label << '\n';
        } catch (const std::exception& e) {
            ++failed;
            std::cout << "FAIL " << label << ": " << e.what() << '\n';
        }
    };
    test("profile round trip has stable wire format", [] {
        UserProfile p, out;
        p.gain[0] = 42;
        uint32_t g;
        auto b = encode(p, 8);
        require(b.size() == 84, "size");
        require(decode(b, out, g), "decode");
        require(g == 8, "generation");
        near(out.gain[0], 42);
    });
    test("CRC rejects every single byte corruption", [] {
        auto b = encode(UserProfile{}, 1);
        for (size_t i = 0; i < b.size(); ++i) {
            auto c = b;
            c[i] ^= 1;
            UserProfile p;
            uint32_t g;
            require(!decode(c, p, g), "corruption accepted");
        }
    });
    test("schema mismatch rejected even with correct CRC", [] {
        auto b = encode(UserProfile{}, 1);
        b[4] = 2;
        b.resize(80);
        uint32_t crc = checksum(b);
        for (int i = 0; i < 4; ++i) {
            b.push_back(crc >> (8 * i));
        }
        UserProfile p;
        uint32_t g;
        require(!decode(b, p, g), "schema");
    });
    test("profile rejects NaN thresholds invalid ranges", [] {
        UserProfile p;
        p.bias[0] = NAN;
        require(!p.valid(), "nan");
        p = UserProfile{};
        p.fastThreshold = p.precisionThreshold;
        require(!p.valid(), "threshold");
        p = UserProfile{};
        p.dwellMs = 0;
        require(!p.valid(), "dwell");
    });
    test("transactional save survives torn write", [] {
        MemoryStorage m;
        ProfileRepository r(m);
        UserProfile a, b, out;
        a.gain[0] = 20;
        b.gain[0] = 60;
        require(r.save(a), "save");
        m.tearWrite = true;
        require(!r.save(b), "tear accepted");
        require(r.load(out), "old missing");
        near(out.gain[0], 20);
    });
    test("failed save and invalid candidate preserve profile", [] {
        MemoryStorage m;
        ProfileRepository r(m);
        UserProfile a, b, out;
        require(r.save(a), "save");
        m.failWrite = true;
        b.gain[1] = 80;
        require(!r.save(b), "failed");
        require(r.load(out), "old");
        near(out.gain[1], a.gain[1]);
        m.failWrite = false;
        b.alpha = 0;
        require(!r.save(b), "invalid");
    });
    test("corrupt newest slot falls back to last valid", [] {
        MemoryStorage m;
        ProfileRepository r(m);
        UserProfile a, b, out;
        a.gain[0] = 20;
        b.gain[0] = 60;
        r.save(a);
        r.save(b);
        m.slots[1][20] ^= 2;
        require(r.load(out), "fallback");
        near(out.gain[0], 20);
    });
    test("bounded generation refuses wrap", [] {
        MemoryStorage m;
        m.slots[0] = encode(UserProfile{}, UINT32_MAX);
        ProfileRepository r(m);
        require(!r.save(UserProfile{}), "wrap");
    });
    test("calibration noise raises deadzone", [] {
        auto a = calibrate(.02, 12, 24);
        auto b = calibrate(.2, 12, 24);
        require(b.deadzone[0] > a.deadzone[0], "noise adaptation");
        require(b.alpha < a.alpha, "smoothing adaptation");
    });
    test("calibration compensates directional asymmetry", [] {
        auto p = calibrate(.05, 12, 24);
        require(p.gain[0] > p.gain[1], "left gain");
        require(p.gain[2] > p.gain[3], "up gain");
    });
    test("limited movement rejected without erasing prior profile", [] {
        Rig r;
        r.active();
        float old = r.system.profile.gain[0];
        r.system.calibrate(r.now);
        for (int i = 0; i < 1000; ++i) {
            r.tick();
        }
        require(r.system.calibration.phase == CalPhase::Failed, "small range");
        require(r.system.hasProfile, "old lost");
        near(r.system.profile.gain[0], old);
    });
    test("calibration rejects unstable rest and sparse samples", [] {
        bool rejected = false;
        try {
            calibrate(3, 12, 24);
        } catch (...) {
            rejected = true;
        }
        require(rejected, "unstable rest");
        CalibrationEngine c;
        c.start(0);
        c.tick({1500, {}, {0, 0, 1}, true}, 1500);
        require(c.phase == CalPhase::Failed, "sparse");
    });
    test("failed calibration storage preserves last profile", [] {
        Rig r;
        r.active();
        auto old = r.system.profile;
        r.storage.failWrite = true;
        r.system.calibrate(r.now);
        for (int i = 0; i < 1000; ++i) {
            r.now += 10;
            float x = 0, y = 0;
            auto phase = r.system.calibration.phase;
            if (phase == CalPhase::Left) {
                x = -12;
            }
            if (phase == CalPhase::Right) {
                x = 24;
            }
            if (phase == CalPhase::Up) {
                y = -10;
            }
            if (phase == CalPhase::Down) {
                y = 20;
            }
            r.system.tick({r.now, {x, y, 0}, {0, 0, 1}, true}, r.now, false);
        }
        require(r.system.calibration.phase == CalPhase::Failed, "save");
        near(r.system.profile.gain[0], old.gain[0]);
    });
    test("EMA bias correction and reset", [] {
        MotionProcessor m;
        UserProfile p;
        p.alpha = .5;
        p.bias[0] = 2;
        p.deadzone[0] = .1;
        auto out = m.process({0, {12, 0, 0}, {0, 0, 1}, true}, p, .01);
        near(out.x, 4.9);
        out = m.process({10, {12, 0, 0}, {0, 0, 1}, true}, p, .01);
        near(out.x, 7.4);
        m.reset();
        out = m.process({20, {2, 0, 0}, {0, 0, 1}, true}, p, .01);
        near(out.x, 0);
    });
    test("deadzone suppresses rest but preserves beyond threshold", [] {
        MotionProcessor m;
        UserProfile p;
        p.alpha = 1;
        near(m.process({0, {.5, 0, 0}, {0, 0, 1}, true}, p, .01).x, 0);
        require(m.process({10, {2, 0, 0}, {0, 0, 1}, true}, p, .01).x > 0, "motion lost");
    });
    test("directional gain and acceleration are continuous", [] {
        AdaptiveEngine e;
        UserProfile p;
        p.gain[0] = 60;
        p.gain[1] = 30;
        auto l = e.apply({-10, 0, 0, 0}, p, .01), r = e.apply({10, 0, 0, 0}, p, .01);
        near(-l.dx, 2 * r.dx);
        auto a = e.apply({4.999, 0, 0, 0}, p, .01), b = e.apply({5.001, 0, 0, 0}, p, .01);
        require(std::abs(a.dx - b.dx) < .01, "threshold jump");
    });
    test("roll scroll suppresses pointer and can be disabled", [] {
        AdaptiveEngine a;
        IntentEngine e;
        UserProfile p;
        auto i = a.apply({20, 20, 30, 0}, p, .01);
        require(i.wheel > 0, "scroll");
        i = e.resolve(i, true, true);
        near(i.dx, 0);
        near(i.dy, 0);
        p.scrollEnabled = false;
        near(a.apply({20, 20, 30, 0}, p, .01).wheel, 0);
    });
    test("switch debounce press hold release and bounce", [] {
        DebouncedSwitch s;
        require(!s.update(true, 0), "early");
        require(!s.update(false, 10), "bounce");
        require(!s.update(true, 20), "bounce");
        require(!s.update(true, 49), "early");
        require(s.update(true, 50), "press");
        require(s.update(false, 60), "release early");
        require(!s.update(false, 90), "release");
    });
    test("dwell clicks once and needs meaningful movement", [] {
        SelectionManager s;
        UserProfile p;
        p.dwellEnabled = true;
        s.update(false, 0, 0, true, false, p, 0);
        s.update(false, 0, 0, true, false, p, 250);
        auto hit = s.update(false, 0, 0, true, false, p, 1250);
        require(hit.pulse, "click");
        for (int i = 0; i < 10; ++i) {
            require(!s.update(false, 0, 0, true, false, p, 2000 + i * 1000).pulse, "repeat");
        }
        require(s.dwell == DwellState::Lockout, "lockout");
        s.update(false, 20, 0, true, false, p, 13000);
        require(s.dwell == DwellState::Idle, "unlock");
    });
    test("dwell cancels at 95 percent", [] {
        SelectionManager s;
        UserProfile p;
        p.dwellEnabled = true;
        s.update(false, 0, 0, true, false, p, 0);
        s.update(false, 0, 0, true, false, p, 250);
        near(s.progress(1200, p), .95);
        require(!s.update(false, 9, 0, true, false, p, 1200).pulse, "click");
        require(s.dwell == DwellState::Idle && s.cancellations == 1, "cancel");
    });
    test("switch and scroll cancel pending dwell", [] {
        for (bool scroll : {false, true}) {
            SelectionManager s;
            UserProfile p;
            p.dwellEnabled = true;
            s.update(false, 0, 0, true, false, p, 0);
            s.update(false, 0, 0, true, false, p, 250);
            s.update(!scroll, 0, 0, true, scroll, p, 300);
            require(s.cancellations == 1 && s.dwell == DwellState::Idle, "cancel");
        }
    });
    test("pause releases drag and requires switch release", [] {
        Rig r;
        r.active();
        r.tick(0, true);
        for (int i = 0; i < 4; ++i) {
            r.tick(0, true);
        }
        require(r.system.hid.last.down, "drag");
        r.system.pause();
        r.tick(20, true);
        require(!r.system.hid.last.down && r.system.hid.last.dx == 0, "pause");
        require(r.system.resume(), "resume");
        for (int i = 0; i < 5; ++i) {
            r.tick(0, true);
        }
        require(!r.system.hid.last.down, "held reactivated");
        for (int i = 0; i < 5; ++i) {
            r.tick(0, false);
        }
        for (int i = 0; i < 5; ++i) {
            r.tick(0, true);
        }
        require(r.system.hid.last.down, "new press");
    });
    test("NaN extreme disconnect stale and frozen timestamp fail stationary", [] {
        for (Fault f : {Fault::NaN, Fault::Extreme, Fault::Disconnect, Fault::Timeout,
                        Fault::FrozenTimestamp}) {
            Rig r;
            r.active();
            r.tick(20, true);
            r.now += 10;
            SimulatedSensor s;
            s.fault = f;
            r.system.tick(s.read(r.now), r.now, true);
            require(r.system.state == SystemState::SafeState, "safe state");
            require(r.system.hid.last.dx == 0 && r.system.hid.last.dy == 0 &&
                        r.system.hid.last.wheel == 0 && !r.system.hid.last.down,
                    "stationary");
            require(r.system.selection.dwell == DwellState::Idle, "dwell");
        }
    });
    test("sensor faults during calibration abort candidate", [] {
        Rig r;
        r.system.calibrate(0);
        r.now = 10;
        MotionSample s{10, {NAN, 0, 0}, {0, 0, 1}, true};
        r.system.tick(s, 10, false);
        require(r.system.calibration.phase == CalPhase::Failed, "abort");
        require(!r.system.hasProfile, "fabricated profile");
    });
    test("healthy recovery requires twenty samples then explicit resume", [] {
        Rig r;
        r.active();
        r.now += 10;
        r.system.tick({r.now, {}, {0, 0, 1}, false}, r.now, false);
        for (unsigned i = 0; i < 19; ++i) {
            r.tick(20);
        }
        require(r.system.state == SystemState::SafeState, "premature recovery");
        r.tick(20);
        require(r.system.state == SystemState::Ready, "ready");
        require(r.system.hid.last.dx == 0, "auto movement");
        require(r.system.resume(), "explicit resume");
        r.tick(20);
        require(r.system.hid.last.dx > 0, "output");
    });
    test("BLE loss stops output and reconnect never auto activates", [] {
        Rig r;
        r.active();
        r.transport.online = false;
        r.tick(40, true);
        require(r.system.state == SystemState::SafeState, "BLE safe");
        r.transport.online = true;
        for (unsigned i = 0; i < start::recoverySamples; ++i) {
            r.tick();
        }
        require(r.system.state == SystemState::Ready, "ready");
        require(!r.system.hid.last.down, "release reconnect");
    });
    test("failed HID notifications transition to safe", [] {
        Rig r;
        r.active();
        r.transport.fail = true;
        r.tick(20, true);
        require(r.system.state == SystemState::SafeState, "send fault");
        r.transport.fail = false;
        for (unsigned i = 0; i < start::recoverySamples; ++i) {
            r.tick();
        }
        require(r.system.state == SystemState::Ready, "recover");
        require(r.transport.reports.back().dx == 0 && !r.transport.reports.back().down, "release");
    });
    test("profile invalidation gates active output", [] {
        Rig r;
        r.active();
        r.system.invalidateProfile();
        r.tick(20, true);
        require(r.system.state == SystemState::SafeState && !r.system.hid.last.down,
                "invalid profile");
    });
    test("safety clamps finite commands and rejects nonfinite", [] {
        SafetyManager s;
        auto c = s.gate({10000, -10000, 200, true, true}, true, true, true, true);
        near(c.dx, 40);
        near(c.dy, -40);
        near(c.wheel, 5);
        c = s.gate({NAN, 10, 10, true, true}, true, true, true, true);
        near(c.dx, 0);
        require(!c.down && !c.pulse && s.calculationFault, "nonfinite");
        for (int i = 0; i < 4; ++i) {
            bool a = true, h = true, p = true, t = true;
            if (i == 0) {
                a = false;
            }
            if (i == 1) {
                h = false;
            }
            if (i == 2) {
                p = false;
            }
            if (i == 3) {
                t = false;
            }
            auto z = s.gate({5, 5, 5, true, true}, a, h, p, t);
            require(!z.down && z.dx == 0 && z.wheel == 0, "gate");
        }
    });
    test("fractional HID movement accumulates and dwell pulse releases", [] {
        TestHID t;
        HIDManager h(t);
        int sum = 0;
        for (int i = 0; i < 10; ++i) {
            h.emit({.25f, 0, 0, false, false});
            sum += h.last.dx;
        }
        require(sum == 2, "fractional");
        h.emit({0, 0, 0, false, true});
        require(t.reports[t.reports.size() - 2].down && !t.reports.back().down, "pulse release");
    });
    test("axis permutation and sign validated", [] {
        AxisTransform a;
        MotionSample s{0, {1, 2, 3}, {0, 0, 1}, true};
        a.signs[0] = -1;
        near(a.apply(s).gyro[0], -3);
        a.axes = {3, 1, 2};
        require(!a.valid() && !a.apply(s).valid, "bad mapping");
    });
    test("MPU register conversion handles signed raw readings", [] {
        // The register file remembers writes: begin() now reads every configuration register back.
        class Bus : public RegisterBus {
        public:
            uint8_t written[256] = {};
            bool write(uint8_t reg, uint8_t value) override {
                written[reg] = value;
                return true;
            }
            bool read(uint8_t r, uint8_t* b, size_t n) override {
                for (size_t i = 0; i < n; ++i) {
                    b[i] = written[(r + i) & 0xff];
                }
                if (r == 0x75) {
                    b[0] = 0x68;
                }
                if (r == 0x3a) {
                    b[0] = 1;
                }
                if (r == 0x3b) {
                    b[4] = 0x40;
                    b[8] = 0xff;
                    b[9] = 0x7d;
                }
                return true;
            }
        } bus;
        MPU6050Sensor s(bus);
        require(s.begin(), "begin");
        auto row = s.read(10);
        require(row.valid, "read");
        near(row.accel[2], 1);
        near(row.gyro[0], -1);
    });
    test("MPU missing ID never supplies valid motion", [] {
        class Bus : public RegisterBus {
        public:
            bool write(uint8_t, uint8_t) override {
                return false;
            }
            bool read(uint8_t, uint8_t*, size_t) override {
                return false;
            }
        } bus;
        MPU6050Sensor s(bus);
        require(!s.begin() && !s.read(10).valid, "missing device");
    });
    test("replay respects sample timestamps and EOF is invalid", [] {
        ReplaySensor r({{10, {1, 2, 3}, {0, 0, 1}, true}, {20, {4, 5, 6}, {0, 0, 1}, true}});
        require(!r.read(0).valid, "early");
        near(r.read(10).gyro[0], 1);
        near(r.read(20).gyro[0], 4);
        require(!r.read(30).valid, "EOF");
    });
    test("clock wrap is safe but stalled/backward ticks are rejected", [] {
        SensorManager m;
        require(m.check({UINT32_MAX - 5, {}, {0, 0, 1}, true}, UINT32_MAX - 5), "prewrap");
        require(m.check({4, {}, {0, 0, 1}, true}, 4), "wrap");
        require(!m.check({3, {}, {0, 0, 1}, true}, 4), "backward");
        Rig r;
        r.active();
        r.system.tick({r.now, {}, {0, 0, 1}, true}, r.now, false);
        require(r.system.state == SystemState::SafeState, "stalled");
    });
    test("long simulation obeys bounds and never produces nonfinite", [] {
        Rig r;
        r.active();
        for (unsigned i = 0; i < 100000; ++i) {
            float v = std::sin(i * .017f) * 220;
            r.tick(v, (i % 1000) < 100);
            auto h = r.system.hid.last;
            require(std::abs(int(h.dx)) <= 40 && std::abs(int(h.dy)) <= 40 &&
                        std::abs(int(h.wheel)) <= 5,
                    "bounds");
            require(std::isfinite(r.system.diagnostics.motion.x), "finite");
        }
    });

    test("stop commands release immediately without another sample", [] {
        for (int operation = 0; operation < 3; ++operation) {
            Rig rig;
            rig.active();
            for (int i = 0; i < 5; ++i) {
                rig.tick(20, true);
            }
            require(rig.system.hid.last.down, "drag missing");
            const auto before = rig.transport.reports.size();
            if (operation == 0) {
                rig.system.pause();
            }
            if (operation == 1) {
                rig.system.calibrate(rig.now);
            }
            if (operation == 2) {
                require(rig.system.setProfile(UserProfile{}, false), "profile refused");
            }
            require(rig.transport.reports.size() > before, "no immediate report");
            auto last = rig.transport.reports.back();
            require(!last.down && last.dx == 0 && last.dy == 0 && last.wheel == 0, "not released");
        }
    });
    test("failed stop delivery inhibits output until qualified recovery", [] {
        Rig rig;
        rig.active();
        rig.transport.fail = true;
        rig.system.pause();
        require(rig.system.state == SystemState::SafeState, "failed pause hidden");
        for (int i = 0; i < 40; ++i) {
            rig.tick(30, true);
        }
        rig.transport.fail = false;
        for (int i = 0; i < 19; ++i) {
            rig.tick(30, true);
        }
        require(!rig.system.resume(), "early resume");
        rig.tick(30, true);
        require(rig.system.resume(), "qualified resume");
        for (int i = 0; i < 10; ++i) {
            rig.tick(0, true);
        }
        require(!rig.system.hid.last.down, "held switch reactivated");
    });
    test("recovery resets when a second fault interrupts qualification", [] {
        Rig rig;
        rig.active();
        rig.transport.fail = true;
        rig.tick();
        rig.transport.fail = false;
        for (int i = 0; i < 19; ++i) {
            rig.tick();
        }
        rig.transport.fail = true;
        rig.tick();
        rig.transport.fail = false;
        rig.tick();
        require(rig.system.state == SystemState::SafeState, "partial recovery retained");
        for (int i = 0; i < 19; ++i) {
            rig.tick();
        }
        require(rig.system.state == SystemState::Ready, "recovery missing");
    });
    test("invalid mapping timing and profile skip control calculations", [] {
        Rig rig;
        rig.active();
        rig.system.axes.axes = {0, 0, 2};
        rig.tick(50);
        near(rig.system.diagnostics.motion.x, 0);
        require(rig.system.diagnostics.faultCode == FaultCode::AxisMapping, "typed mapping fault");
        auto invalid = rig.system.profile;
        invalid.alpha = NAN;
        require(!rig.system.setProfile(invalid, false), "invalid profile accepted");
        require(std::isfinite(rig.system.profile.alpha), "invalid profile exposed");
        rig.system.invalidateProfile();
        near(rig.system.diagnostics.motion.x, 0);
    });
    test("settings save failure returns failure and preserves profile", [] {
        Rig rig;
        rig.active();
        const auto previous = rig.system.profile;
        rig.storage.failWrite = true;
        auto candidate = previous;
        candidate.dwellEnabled = !previous.dwellEnabled;
        require(!rig.system.setProfile(candidate, true), "save failure accepted");
        require(rig.system.profile.dwellEnabled == previous.dwellEnabled, "profile replaced");
        require(rig.system.state != SystemState::Active, "output still active");
        require(rig.system.diagnostics.faultCode == FaultCode::Storage, "storage fault missing");
    });
    test("request envelope rejects malformed IDs and truncated commands", [] {
        for (const char* line : {"@0 status", "@-1 status", "@1.5 status", "@4294967296 status",
                                 "@a status", "@ status", "@1"}) {
            require(!parseEnvelope(line, 80).valid, "invalid ID accepted");
        }
        auto valid = parseEnvelope("@4294967295 status", 80);
        require(valid.valid && valid.requestId == UINT32_MAX, "valid ID refused");
        auto truncated = parseEnvelope("@7 pause", 80, true);
        require(!truncated.valid && truncated.requestId == 7, "truncation lost correlation");
        require(!parseEnvelope(std::string(81, 'x'), 80).valid, "overlong command accepted");
    });
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
