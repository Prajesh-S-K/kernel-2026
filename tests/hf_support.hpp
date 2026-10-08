#pragma once
// Shared rigs for the hands-free tests. Kept separate from test_main.cpp so the original
// regression file stays untouched.
#include "nodx/engine.hpp"
#include "nodx/gesture_script.hpp"
#include <cmath>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

using namespace nodx;
inline void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
inline void near(float a, float b, float eps = .001f) {
    require(std::abs(a - b) < eps, "numeric mismatch");
}
class TestHID : public HIDTransport {
public:
    bool online = true, fail = false;
    bool failRelease = false; // fail only the report that releases a pressed button
    std::vector<Report> reports;
    bool connected() const override {
        return online;
    }
    bool send(const Report& r) override {
        const bool releasing = !r.down && !reports.empty() && reports.back().down;
        reports.push_back(r);
        return !fail && !(failRelease && releasing);
    }
};
inline std::vector<Rates> script(const std::string& name, float scale = 1.f) {
    std::vector<Rates> out;
    require(sim::named(name, scale, out), "unknown script");
    return out;
}
// Hand-made templates for direct recognizer tests (not learned).
inline GestureTemplate makeTemplate(unsigned axis, unsigned strokes) {
    GestureTemplate item;
    item.strokes = uint8_t(strokes);
    for (unsigned i = 0; i < strokes; ++i) {
        item.axis[i] = uint8_t(axis);
        item.sign[i] = (i % 2) ? -1 : 1;
    }
    item.enterRate = 15;
    item.peakMin = 30;
    item.peakMax = 140;
    item.strokeMinMs = 60;
    item.strokeMaxMs = 400;
    item.gapMaxMs = 200;
    item.totalMaxMs = 1500;
    return item;
}
inline GestureSet makeSet() {
    GestureSet set;
    set.neutralRate = 3;
    set.templates[0] = makeTemplate(1, 4); // nod
    set.templates[1] = makeTemplate(2, 4); // tilt
    return set;
}
// Direct recognizer driver.
struct Recog {
    GestureRecognizer r;
    uint32_t now = 0;
    unsigned events = 0;
    int lastId = -1;
    explicit Recog(const GestureSet& set = makeSet(), unsigned mask = 3) {
        r.configure(set, mask);
        r.reset(0);
    }
    GestureEvent tick(const Rates& rate, unsigned ms = 10) {
        now += ms;
        const auto event = r.update(rate, now);
        if (event.executed) {
            ++events;
            lastId = int(event.id);
        }
        return event;
    }
    void run(const std::vector<Rates>& rates, unsigned ms = 10) {
        for (const auto& rate : rates) {
            tick(rate, ms);
        }
    }
    void quiet(unsigned ms) {
        for (unsigned i = 0; i < ms / 10; ++i) {
            tick({});
        }
    }
};
// Power-loss model: once the write budget is spent every further write is lost.
struct Budget {
    int writes = 1 << 30;
};
class LossyProfileStorage : public ProfileStorage {
public:
    LossyProfileStorage(MemoryStorage& inner, Budget& budget) : inner_(inner), budget_(budget) {}
    std::vector<uint8_t> read(unsigned slot) override {
        return inner_.read(slot);
    }
    bool write(unsigned slot, const std::vector<uint8_t>& bytes) override {
        if (budget_.writes <= 0) {
            return false;
        }
        --budget_.writes;
        return inner_.write(slot, bytes);
    }

private:
    MemoryStorage& inner_;
    Budget& budget_;
};
class LossyConfigStorage : public ConfigStorage {
public:
    LossyConfigStorage(MemoryConfigStorage& inner, Budget& budget)
        : inner_(inner), budget_(budget) {}
    std::vector<uint8_t> read(unsigned slot) override {
        return inner_.read(slot);
    }
    bool write(unsigned slot, const std::vector<uint8_t>& bytes) override {
        if (budget_.writes <= 0) {
            return false;
        }
        --budget_.writes;
        return inner_.write(slot, bytes);
    }

private:
    MemoryConfigStorage& inner_;
    Budget& budget_;
};
// Whole-system rig with persistent storage so a "reboot" can be simulated.
struct HF {
    MemoryStorage profileStorage;
    MemoryConfigStorage configStorage;
    Budget budget;
    LossyProfileStorage profileLossy{profileStorage, budget};
    LossyConfigStorage configLossy{configStorage, budget};
    ProfileRepository repo{profileLossy};
    HandsFreeRepository configRepo{configLossy};
    TestHID transport;
    std::unique_ptr<System> sys;
    uint32_t now = 0;
    // Raw enable input: maintained switch ON / push button pressed.
    bool sw = true;
    // Existing tests exercise the maintained-switch compatibility configuration; button tests build
    // the rig with EnableKind::Momentary (input released at power-up).
    EnableKind kind = EnableKind::Maintained;
    explicit HF(bool saveProfile = true, EnableKind enableKind = EnableKind::Maintained)
        : kind(enableKind) {
        sw = kind == EnableKind::Maintained;
        if (saveProfile) {
            require(repo.save(UserProfile{}), "profile save");
        }
        boot();
    }
    void boot() {
        sys = std::make_unique<System>(transport, repo, configRepo);
        sys->axes.axes = {0, 1, 2};
        sys->axes.accelAxes = {0, 1, 2};
        sys->axes.accelSigns = {1, 1, 1};
        sys->configureEnableInput(true);
    }
    System& s() {
        return *sys;
    }
    void tick(const Rates& rate = {}) {
        now += 10;
        sys->setControlSwitch(sw, now);
        sys->tick({now, {rate[0], rate[1], rate[2]}, {0, 0, 1}, true}, now, false);
    }
    void run(const std::vector<Rates>& rates) {
        for (const auto& rate : rates) {
            tick(rate);
        }
    }
    void quiet(unsigned ms) {
        for (unsigned i = 0; i < ms / 10; ++i) {
            tick();
        }
    }
    bool trainGesture(GestureId id, const std::string& pattern, float scale = 1.f) {
        if (!s().trainStart(id, now)) {
            return false;
        }
        quiet(1100);
        for (unsigned i = 0; i < start::trainExamples; ++i) {
            quiet(400);
            run(script(pattern, scale));
            quiet(500);
        }
        quiet(400);
        run(script(pattern, scale));
        quiet(400);
        return s().trainAccept();
    }
    void setup() {
        require(trainGesture(GestureId::PauseResume, "nod2"), "train pause");
        require(trainGesture(GestureId::Drag, "tilt2"), "train drag");
        s().stageEnableKind(kind);
        require(s().commitHandsFree(), "commit");
    }
    // One complete push-button press and stable release (button kind only).
    void click(unsigned pressMs = 60, unsigned releaseMs = 60) {
        sw = true;
        quiet(pressMs);
        sw = false;
        quiet(releaseMs);
    }
    // Setup, then qualify and resume. Leaves control ACTIVE and neutral. With the push button the
    // permission is first latched by one press; the resume itself stays a separate explicit step.
    void active() {
        setup();
        quiet(500);
        if (kind == EnableKind::Momentary) {
            sw = false;
            quiet(100);
            click();
            require(s().handsFreeStatus().permitted, "button did not permit control");
        }
        require(s().resume(), "resume");
        quiet(400);
    }
    const Report& last() const {
        return transport.reports.back();
    }
    bool released() const {
        return !transport.reports.empty() && !last().down && last().dx == 0 && last().dy == 0 &&
               last().wheel == 0;
    }
};
