// Dwell action palette (EXPERIMENTAL demo): every action, palette selection without click-through,
// dwell cancellation, repeat lockout, drag release, scroll and its dwell exit, mode changes, stops,
// faults, failed delivery and disconnect. Everything runs through the real System tick, so the final
// SafetyManager -> HIDManager -> transport order is exercised. Synthetic input only.
#include "hf_support.hpp"
#include "nodx/actions.hpp"
#include <cstdio>
#include <cstring>

namespace {
int passed = 0, failed = 0;
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
// Report analysis over transport.reports[mark..]
struct Seen {
    int primary = 0, secondary = 0; // rising edges of the primary / secondary button
    int wheel = 0, wheelAbs = 0, maxWheel = 0;
    int dx = 0, dy = 0;
    bool primaryHeld = false, secondaryHeld = false; // state of the last report
};
Seen seen(const HF& h, size_t mark) {
    Seen out;
    bool down = false, right = false;
    if (mark > 0 && mark <= h.transport.reports.size()) {
        down = h.transport.reports[mark - 1].down;
        right = h.transport.reports[mark - 1].right;
    }
    for (size_t i = mark; i < h.transport.reports.size(); ++i) {
        const Report& r = h.transport.reports[i];
        out.primary += (r.down && !down) ? 1 : 0;
        out.secondary += (r.right && !right) ? 1 : 0;
        down = r.down;
        right = r.right;
        out.wheel += r.wheel;
        out.wheelAbs += std::abs(int(r.wheel));
        out.maxWheel = std::max(out.maxWheel, std::abs(int(r.wheel)));
        out.dx += r.dx;
        out.dy += r.dy;
    }
    out.primaryHeld = down;
    out.secondaryHeld = right;
    return out;
}
size_t mark(const HF& h) {
    return h.transport.reports.size();
}
// The companion page repeats its report ("outside" or the control the pointer is over). The wrappers
// below send that heartbeat like the real page does, so a test can stop it to model a missing report.
PaletteTarget gCurrent = PaletteTarget::None;
bool gReporting = true;
uint32_t gEpoch = 1000; // the overlay claim epoch in force
bool gOverlay = false, gMenu = false; // overlay mode: the heartbeat repeats "pointer on the overlay or not"
unsigned gTicks = 0;
void report(HF& h, PaletteTarget target) {
    gCurrent = target;
    h.s().setActionHover(target, h.now);
}
void tk(HF& h, const Rates& rate = {}) {
    if (gReporting && gTicks++ % 50 == 0) {
        if (gOverlay) {
            h.s().setOverlayMenu(gMenu, h.now, gEpoch);
        } else {
            h.s().setActionHover(gCurrent, h.now);
        }
    }
    h.tick(rate);
}
void qt(HF& h, unsigned ms) {
    for (unsigned i = 0; i < ms / 10; ++i) {
        tk(h);
    }
}
// A session on the real sensor path with the palette enabled (Left-click selected).
HF rig(bool saveProfile = false) {
    gCurrent = PaletteTarget::None;
    gReporting = true;
    gOverlay = gMenu = false;
    gTicks = 0;
    HF h(saveProfile, EnableKind::Momentary);
    h.sw = false;
    h.quiet(400);
    require(h.s().startUncalibratedDemo(h.now), "demo start refused");
    h.quiet(300);
    return h;
}
HF paletteRig(bool saveProfile = false) {
    HF h = rig(saveProfile);
    require(h.s().setActionPalette(true, h.now), "palette refused");
    report(h, PaletteTarget::None); // the palette window's first report
    qt(h, 100);
    return h;
}
// The desktop overlay: claims the palette, then repeats "pointer on the overlay" like the real overlay does.
bool claimOverlay(HF& h) { // a claim names the current session and an epoch above every earlier one
    ++gEpoch;
    return h.s().setActionOverlay(true, h.now, h.s().sessionSerial(), gEpoch);
}
void menu(HF& h, bool on) {
    gMenu = on;
    h.s().setOverlayMenu(on, h.now, gEpoch);
}
HF overlayRig(bool saveProfile = false) {
    HF h = rig(saveProfile);
    require(claimOverlay(h), "overlay claim refused");
    gOverlay = true;
    menu(h, false); // the overlay's first heartbeat
    qt(h, 100);
    return h;
}
// Open the menu, select through the validated command, collapse (pointer leaves the overlay).
bool ovSelect(HF& h, PaletteTarget target) {
    menu(h, true);
    qt(h, 50);
    const bool ok = h.s().overlaySelect(target, h.now, gEpoch);
    menu(h, false);
    qt(h, 50);
    return ok;
}
constexpr unsigned kDwellTicks = 160; // 1.6 s: the 250 ms arming plus the 1200 ms START dwell
void hold(HF& h, PaletteTarget target, unsigned ticks) {
    report(h, target);
    for (unsigned i = 0; i < ticks; ++i) {
        tk(h);
    }
}
// Dwell on a palette control (the pointer is still, the page reports the hover), then leave it.
void selectControl(HF& h, PaletteTarget target) {
    hold(h, target, kDwellTicks);
    report(h, PaletteTarget::None);
}
void moveAway(HF& h, float rate = 40.f, unsigned ticks = 40) {
    for (unsigned i = 0; i < ticks; ++i) {
        tk(h, {rate, 0, 0});
    }
    qt(h, 300);
}
// Run until `done()` holds; returns the milliseconds it took (maxMs when it never did).
unsigned waitFor(HF& h, const std::function<bool()>& done, unsigned maxMs) {
    unsigned elapsed = 0;
    while (!done() && elapsed < maxMs) {
        tk(h);
        elapsed += 10;
    }
    return elapsed;
}
// The dwell plus the 150 ms commit wait, with margin.
void dwellOnTarget(HF& h) {
    qt(h, 1900);
}
// Move the pointer (head movement at `rate`) until `units` of outgoing movement have accumulated; returns
// the ticks it took. This is what the dwell tolerance and the re-arming lockout actually count.
unsigned moveUnits(HF& h, float units, float rate = 40.f) {
    float sum = 0;
    unsigned ticks = 0;
    while (std::fabs(sum) < units && ticks < 4000) {
        const size_t before = h.transport.reports.size();
        tk(h, {rate, 0, 0});
        for (size_t i = before; i < h.transport.reports.size(); ++i) {
            sum += h.transport.reports[i].dx;
        }
        ++ticks;
    }
    return ticks;
}
float totalDx(const HF& h, size_t from) {
    float sum = 0;
    for (size_t i = from; i < h.transport.reports.size(); ++i) {
        sum += h.transport.reports[i].dx;
    }
    return sum;
}
const char* mode(HF& h) {
    return h.s().actionsStatus(h.now).mode;
}
} // namespace

int main() {
    test("disabled by default; refused outside a running session", [] {
        HF h(false, EnableKind::Momentary);
        h.sw = false;
        qt(h, 400);
        require(!h.s().setActionPalette(true, h.now), "enabled without a session");
        require(!h.s().actionsStatus(h.now).enabled, "enabled flag");
        require(std::strlen(h.s().actionsStatus(h.now).blocked) > 0, "no blocker text");
        require(h.s().startUncalibratedDemo(h.now), "start");
        qt(h, 300);
        require(!h.s().actionPaletteEnabled(), "palette on after a plain start");
        require(!h.s().setActionHover(PaletteTarget::Left, h.now), "hover accepted while off");
        // plain movement-only demo: dwell never clicks
        const size_t m = mark(h);
        dwellOnTarget(h);
        require(seen(h, m).primary == 0, "a click in the movement-only demo");
    });

    test("Left-click is the default: one click per completed dwell, none before", [] {
        HF h = paletteRig();
        require(std::strcmp(mode(h), "LEFT") == 0, "default mode");
        size_t m = mark(h);
        qt(h, 1000);
        require(seen(h, m).primary == 0, "clicked before the dwell completed");
        const auto p = h.s().actionsStatus(h.now);
        require(std::strcmp(p.dwellState, "PROGRESS") == 0 && p.dwellProgress > .4f,
                "no progress feedback");
        qt(h, 600);
        Seen s = seen(h, m);
        require(s.primary == 1 && s.secondary == 0 && !s.primaryHeld, "not exactly one click");
        require(std::strcmp(mode(h), "LEFT") == 0, "left changed");
        require(h.s().actionsStatus(h.now).left == 1, "left counter");
    });

    test("repeat lockout: standing still never repeats; deliberate movement re-arms", [] {
        HF h = paletteRig();
        dwellOnTarget(h);
        const size_t m = mark(h);
        qt(h, 6000);
        require(seen(h, m).primary == 0, "a second click without movement");
        require(std::strcmp(h.s().actionsStatus(h.now).dwellState, "LOCKOUT") == 0, "no lockout");
        // a small tremor is not deliberate movement
        for (int i = 0; i < 30; ++i) {
            tk(h, {i % 2 ? 6.f : -6.f, 0, 0});
        }
        qt(h, 4000);
        require(seen(h, m).primary == 0, "a tremor re-armed the dwell");
        moveAway(h);
        dwellOnTarget(h);
        require(seen(h, m).primary == 1, "no click after deliberate movement");
    });

    test("dwell cancellation: movement beyond the tolerance restarts the dwell", [] {
        HF h = paletteRig();
        qt(h, 700);
        const size_t m = mark(h);
        for (int i = 0; i < 6; ++i) {
            tk(h, {40.f, 0, 0}); // leaves the tolerance
        }
        require(std::strcmp(h.s().actionsStatus(h.now).dwellState, "PROGRESS") != 0 ||
                    h.s().actionsStatus(h.now).dwellProgress < .2f,
                "progress survived the movement");
        qt(h, 900);
        require(seen(h, m).primary == 0, "clicked on the old dwell");
        qt(h, 1100);
        require(seen(h, m).primary == 1, "no click after a fresh dwell");
    });

    test("Right-click: select by palette dwell (no OS click), one right click, back to Left", [] {
        HF h = paletteRig();
        size_t m = mark(h);
        selectControl(h, PaletteTarget::Right);
        Seen s = seen(h, m);
        require(s.primary == 0 && s.secondary == 0, "selecting Right sent an OS click");
        require(std::strcmp(mode(h), "RIGHT") == 0, "Right not selected");
        require(h.s().actionsStatus(h.now).selections == 1, "selection counter");
        moveAway(h);
        m = mark(h);
        dwellOnTarget(h);
        s = seen(h, m);
        require(s.secondary == 1 && s.primary == 0 && !s.secondaryHeld, "not exactly one right click");
        require(std::strcmp(mode(h), "LEFT") == 0, "did not return to Left");
        moveAway(h);
        m = mark(h);
        dwellOnTarget(h);
        s = seen(h, m);
        require(s.primary == 1 && s.secondary == 0, "the next dwell was not a Left click");
        require(h.s().actionsStatus(h.now).right == 1, "right counter");
    });

    test("Double-click: two press/release pairs back to back, then Left", [] {
        HF h = paletteRig();
        selectControl(h, PaletteTarget::Double);
        moveAway(h);
        const size_t m = mark(h);
        dwellOnTarget(h);
        Seen s = seen(h, m);
        require(s.primary == 2 && s.secondary == 0 && !s.primaryHeld, "not exactly two clicks");
        // press, release, press, release in the report stream
        std::string pattern;
        for (size_t i = m; i < h.transport.reports.size(); ++i) {
            const Report& r = h.transport.reports[i];
            if (r.down) {
                pattern += 'P';
            } else if (!pattern.empty() && pattern.back() == 'P') {
                pattern += 'R';
            }
        }
        require(pattern == "PRPR", "report order is not press release press release");
        require(std::strcmp(mode(h), "LEFT") == 0, "did not return to Left");
        moveAway(h);
        const size_t again = mark(h);
        dwellOnTarget(h);
        require(seen(h, again).primary == 1, "a second double-click happened");
    });

    test("Drag: dwell presses and holds, movement keeps it held, dwell again releases", [] {
        HF h = paletteRig();
        selectControl(h, PaletteTarget::Drag);
        moveAway(h);
        size_t m = mark(h);
        dwellOnTarget(h);
        require(seen(h, m).primary == 1 && seen(h, m).primaryHeld, "no press and hold");
        require(h.s().actionsStatus(h.now).dragging, "DRAGGING not reported");
        require(std::strcmp(h.s().diagnostics.cursor, "DRAGGING") == 0, "cursor label");
        // move while held: every report keeps the button down, the pointer really moves
        m = mark(h);
        for (int i = 0; i < 30; ++i) {
            tk(h, {30.f, 10.f, 0});
        }
        require(seen(h, m).dx != 0, "the pointer did not move while dragging");
        for (size_t i = m; i < h.transport.reports.size(); ++i) {
            require(h.transport.reports[i].down, "button dropped while moving");
        }
        qt(h, 300);
        // still dwell: the release; first it needs the deliberate movement we just made
        const size_t before = mark(h);
        dwellOnTarget(h);
        Seen s = seen(h, before);
        require(!s.primaryHeld, "button still down after the release dwell");
        require(!h.s().actionsStatus(h.now).dragging, "still DRAGGING after release");
        require(h.s().actionsStatus(h.now).dragStarts == 1 &&
                    h.s().actionsStatus(h.now).dragReleases == 1,
                "drag counters");
        // the release is not followed by another press from the same stillness
        const size_t after = mark(h);
        qt(h, 5000);
        require(seen(h, after).primary == 0, "a press right after the release");
    });

    test("Drag cancel path: standing still after the press releases it (no movement needed)", [] {
        HF h = paletteRig();
        selectControl(h, PaletteTarget::Drag);
        moveAway(h);
        dwellOnTarget(h);
        require(h.s().actionsStatus(h.now).dragging, "precondition");
        require(h.transport.reports.back().down, "button not held");
        // no deliberate movement at all: the release dwell restarts where the press happened
        const size_t m = mark(h);
        const unsigned took = waitFor(h, [&] { return !h.s().actionsStatus(h.now).dragging; }, 4000);
        require(!h.s().actionsStatus(h.now).dragging, "a drag held without moving cannot be released");
        require(took < 1700, "the release dwell took longer than the dwell plus the arming time");
        require(!seen(h, m).primaryHeld, "button still down after the release");
        require(h.s().actionsStatus(h.now).dragReleases == 1, "release counter");
        // and it does not press again from the same stillness
        const size_t after = mark(h);
        qt(h, 6000);
        require(seen(h, after).primary == 0, "a press right after the release");
    });

    test("Drag: movement during the release dwell restarts it (a real drag is not cut short)", [] {
        HF h = paletteRig();
        selectControl(h, PaletteTarget::Drag);
        moveAway(h);
        waitFor(h, [&] { return h.s().actionsStatus(h.now).dragging; }, 4000);
        require(h.s().actionsStatus(h.now).dragging, "no press");
        for (int round = 0; round < 4; ++round) {
            qt(h, 800); // most of a release dwell of stillness...
            for (int i = 0; i < 12; ++i) {
                tk(h, {40.f, 0, 0}); // ...then movement restarts it
            }
        }
        require(h.s().actionsStatus(h.now).dragging, "released while the user kept moving");
    });

    test("Drag release works even when no palette report is fresh", [] {
        HF h = paletteRig();
        selectControl(h, PaletteTarget::Drag);
        moveAway(h);
        dwellOnTarget(h);
        require(h.s().actionsStatus(h.now).dragging, "precondition");
        gReporting = false; // the palette window went away
        for (int i = 0; i < 400; ++i) {
            tk(h, {i % 40 < 20 ? 40.f : -40.f, 0, 0}); // keep moving: a real drag in progress
        }
        require(!h.s().actionsStatus(h.now).reporting, "report still fresh");
        require(h.s().actionsStatus(h.now).dragging, "released by staleness alone");
        qt(h, 3000); // stop: the release dwell still works without any report
        require(!h.s().actionsStatus(h.now).dragging && h.released(), "no release without reports");
    });

    test("entering the palette while dragging releases the button BEFORE any selection", [] {
        HF h = paletteRig();
        selectControl(h, PaletteTarget::Drag);
        moveAway(h);
        dwellOnTarget(h);
        require(h.s().actionsStatus(h.now).dragging && h.transport.reports.back().down, "precondition");
        for (int i = 0; i < 20; ++i) {
            tk(h, {40.f, 0, 0}); // dragging toward the palette
        }
        const size_t m = mark(h);
        const uint32_t baseline = h.s().actionsStatus(h.now).selections; // choosing Drag was one
        report(h, PaletteTarget::Left); // the report that the pointer entered the palette
        tk(h);
        tk(h);
        require(!seen(h, m).primaryHeld, "the button was still down after the palette entry");
        require(h.s().actionsStatus(h.now).paletteReleases == 1, "palette release not counted");
        require(h.s().actionsStatus(h.now).selections == baseline, "a selection happened at entry");
        require(!h.s().actionsStatus(h.now).dragging, "still dragging");
        // the selection needs a fresh dwell afterwards, and clicks nothing
        hold(h, PaletteTarget::Left, kDwellTicks);
        require(h.s().actionsStatus(h.now).selections == baseline + 1, "no selection after the release");
        require(seen(h, m).primary == 0 && seen(h, m).secondary == 0, "a click during the palette entry");
    });

    test("entering the palette while dragging releases even from a drag with no movement", [] {
        HF h = paletteRig();
        selectControl(h, PaletteTarget::Drag);
        moveAway(h);
        dwellOnTarget(h);
        require(h.s().actionsStatus(h.now).dragging, "precondition");
        const size_t m = mark(h);
        report(h, PaletteTarget::Stop);
        qt(h, 50);
        require(!seen(h, m).primaryHeld, "the held button survived the palette entry");
        require(h.s().state == SystemState::Active, "the palette entry itself stopped control");
    });

    test("the palette window background (Frame) inhibits target actions and selects nothing", [] {
        HF h = paletteRig();
        moveAway(h);
        const size_t m = mark(h);
        const uint32_t selections = h.s().actionsStatus(h.now).selections;
        report(h, PaletteTarget::Frame); // pointer rests on the bare window, not on a control
        qt(h, 6000);
        require(seen(h, m).primary == 0 && seen(h, m).secondary == 0, "a click on the palette window");
        require(h.s().actionsStatus(h.now).selections == selections, "the bare window selected something");
        require(h.s().actionsStatus(h.now).inPalette, "not reported as on the palette");
        require(std::strcmp(mode(h), "LEFT") == 0, "mode changed");
        // leaving it re-arms only after movement
        report(h, PaletteTarget::None);
        qt(h, 4000);
        require(seen(h, m).primary == 0, "clicked right after leaving the window");
        moveAway(h);
        dwellOnTarget(h);
        require(seen(h, m).primary == 1, "no click after moving on");
    });

    test("a drag held when the pointer reaches the bare palette window is released first", [] {
        HF h = paletteRig();
        selectControl(h, PaletteTarget::Drag);
        moveAway(h);
        dwellOnTarget(h);
        require(h.s().actionsStatus(h.now).dragging, "precondition");
        const size_t m = mark(h);
        report(h, PaletteTarget::Frame);
        qt(h, 50);
        require(!seen(h, m).primaryHeld && !h.s().actionsStatus(h.now).dragging, "drag survived");
    });

    test("missing reports: with no palette window nothing acts on a target", [] {
        HF h = rig();
        require(h.s().setActionPalette(true, h.now), "palette refused");
        gReporting = false; // the palette window is not open
        const size_t m = mark(h);
        qt(h, 12000);
        require(seen(h, m).primary == 0 && seen(h, m).secondary == 0, "a click without a palette report");
        const auto st = h.s().actionsStatus(h.now);
        require(!st.reporting, "reported as reporting");
        require(st.inhibited >= 1, "the inhibited dwell was not counted");
        // the window opens: after deliberate movement, targets work again
        gReporting = true;
        report(h, PaletteTarget::None);
        moveAway(h);
        dwellOnTarget(h);
        require(seen(h, m).primary == 1, "no click once the window reports");
    });

    test("stale reports: reports that stop make every action inhibited, and recovery needs fresh reports", [] {
        HF h = paletteRig();
        moveAway(h);
        gReporting = false;
        qt(h, 3200); // older than the 2.5 s freshness
        require(!h.s().actionsStatus(h.now).reporting, "still fresh");
        moveAway(h); // re-arm the dwell (a click may already have landed while the report was fresh)
        const size_t m = mark(h);
        dwellOnTarget(h);
        require(seen(h, m).primary == 0, "a click on a stale report");
        // a stale hover over a control does not select either
        report(h, PaletteTarget::Right);
        gReporting = false;
        qt(h, 3200); // the last report is now older than the 2.5 s freshness
        const uint32_t before = h.s().actionsStatus(h.now).selections;
        qt(h, 6000);
        require(h.s().actionsStatus(h.now).selections == before, "selected on a stale hover");
        require(seen(h, m).secondary == 0 && seen(h, m).primary == 0, "a click on a stale hover");
        gReporting = true;
        report(h, PaletteTarget::None);
        moveAway(h);
        const size_t again = mark(h);
        dwellOnTarget(h);
        // (the Right control was chosen while its report was still fresh, so this is a right click)
        require(seen(h, again).primary + seen(h, again).secondary == 1, "no click after the reports resumed");
    });

    test("the commit wait: a palette entry reported shortly after the dwell completes cancels the click", [] {
        HF h = paletteRig();
        moveAway(h);
        const size_t m = mark(h);
        // run until the target dwell has completed and the click is waiting out the commit time
        for (unsigned i = 0; i < 400 && !h.s().actionsStatus(h.now).pending; ++i) {
            tk(h);
        }
        require(h.s().actionsStatus(h.now).pending, "no pending action");
        require(seen(h, m).primary == 0, "clicked before the commit wait");
        report(h, PaletteTarget::Left); // the (late) report that the pointer is on the palette
        qt(h, 400);
        require(seen(h, m).primary == 0, "the click was not cancelled");
        require(h.s().actionsStatus(h.now).cancelled == 1, "cancellation not counted");
        require(!h.s().actionsStatus(h.now).pending, "still pending");
    });

    test("KNOWN LIMIT: a palette entry reported after the commit wait cannot stop the click", [] {
        HF h = paletteRig();
        moveAway(h);
        const size_t m = mark(h);
        for (unsigned i = 0; i < 400 && !h.s().actionsStatus(h.now).pending; ++i) {
            tk(h);
        }
        qt(h, 400); // the report is later than the 150 ms commit wait
        report(h, PaletteTarget::Left);
        qt(h, 100);
        require(seen(h, m).primary == 1, "expected the documented leak to be visible to the test");
    });

    test("delayed hover reports: where the leak begins (pointer stops inside the palette)", [] {
        // The pointer travels to a palette control and stops. The report that it is there arrives
        // `delay` ms later. Below about 1.6 s (the 250 ms arming + 1200 ms dwell + 150 ms commit) the
        // entry arrives in time; beyond it a click reaches the target. Both sides are asserted.
        for (unsigned delay : {50u, 200u, 500u, 1000u, 1400u, 1550u, 1900u, 3000u}) {
            HF h = paletteRig();
            for (int i = 0; i < 30; ++i) {
                tk(h, {40.f, 0, 0}); // travel
            }
            const size_t m = mark(h);
            const uint32_t stopped = h.now;
            while (h.now - stopped < delay) {
                tk(h);
            }
            report(h, PaletteTarget::Right); // the delayed report
            qt(h, 2500);
            const Seen s = seen(h, m);
            const bool leaked = s.primary + s.secondary > 0;
            std::printf("   INFO hover report delayed %u ms after the pointer stopped: %s\n", delay,
                        leaked ? "CLICK REACHED THE TARGET" : "no click");
            if (delay <= 1400) {
                require(!leaked, "a click leaked although the report was in time");
                require(h.s().actionsStatus(h.now).selections == 1, "the palette control was not selected");
            } else if (delay >= 1900) {
                require(leaked, "expected the documented late-report leak");
            }
        }
    });

    test("movement while a click waits out the commit time cancels it", [] {
        HF h = paletteRig();
        moveAway(h);
        const size_t m = mark(h);
        for (unsigned i = 0; i < 400 && !h.s().actionsStatus(h.now).pending; ++i) {
            tk(h);
        }
        require(h.s().actionsStatus(h.now).pending, "no pending action");
        for (int i = 0; i < 8; ++i) {
            tk(h, {40.f, 0, 0});
        }
        qt(h, 600);
        require(seen(h, m).primary == 0, "clicked after the pointer left");
        require(h.s().actionsStatus(h.now).cancelled == 1, "cancellation not counted");
    });

    test("Scroll: select without click, pointer free until dwell on the content starts it", [] {
        HF h = paletteRig();
        size_t m = mark(h);
        selectControl(h, PaletteTarget::Scroll);
        require(seen(h, m).primary == 0 && seen(h, m).secondary == 0, "select clicked");
        auto st = h.s().actionsStatus(h.now);
        require(std::strcmp(st.scroll, "ARMED") == 0 && !st.frozen, "not armed");
        m = mark(h);
        moveAway(h); // the pointer is free while armed
        require(seen(h, m).dx != 0, "pointer frozen before scrolling started");
        m = mark(h);
        dwellOnTarget(h);
        st = h.s().actionsStatus(h.now);
        require(std::strcmp(st.scroll, "ACTIVE") == 0 && st.frozen, "scrolling did not start");
        require(seen(h, m).primary == 0 && seen(h, m).secondary == 0, "starting scroll clicked");
        require(std::strcmp(h.s().diagnostics.cursor, "SCROLL_ACTIVE") == 0, "cursor label");
    });

    test("Scroll: pointer frozen, vertical movement drives a bounded wheel, deadzone respected", [] {
        HF h = paletteRig();
        selectControl(h, PaletteTarget::Scroll);
        moveAway(h);
        dwellOnTarget(h);
        require(h.s().actionsStatus(h.now).frozen, "precondition");
        size_t m = mark(h);
        for (int i = 0; i < 150; ++i) {
            tk(h, {40.f, 0, 0}); // horizontal movement: pointer stays frozen
        }
        Seen s = seen(h, m);
        require(h.s().actionsStatus(h.now).frozen, "sideways movement ended scrolling");
        require(s.dx == 0 && s.dy == 0, "the pointer moved while scrolling");
        require(s.wheelAbs == 0, "horizontal movement scrolled");
        // small vertical movement inside the neutral deadzone: nothing
        m = mark(h);
        for (int i = 0; i < 60; ++i) { // under the 1.2 s exit dwell
            tk(h, {0, 4.f, 0}); // 1.5 deg/s after the pointer deadzone: neutral
        }
        require(seen(h, m).wheelAbs == 0, "the deadzone let small movement scroll");
        // head down: wheel negative (scroll down), bounded by the wheel limit
        m = mark(h);
        for (int i = 0; i < 150; ++i) {
            tk(h, {0, 40.f, 0});
        }
        s = seen(h, m);
        require(s.wheel < -3, "no scroll down for downward movement");
        require(s.maxWheel <= start::maxWheel, "wheel report above the bound");
        require(s.dx == 0 && s.dy == 0, "pointer moved while scrolling down");
        qt(h, 300);
        m = mark(h);
        for (int i = 0; i < 150; ++i) {
            tk(h, {0, -40.f, 0});
        }
        require(seen(h, m).wheel > 3, "no scroll up for upward movement");
        // an extreme rate never exceeds the bound
        m = mark(h);
        for (int i = 0; i < 100; ++i) {
            tk(h, {0, 2000.f, 0});
        }
        require(seen(h, m).maxWheel <= start::maxWheel, "extreme movement beat the wheel bound");
        require(h.s().actionsStatus(h.now).wheelUnits > 0, "wheel counter");
    });

    test("Scroll follows the pointer reversal setting", [] {
        HF h = paletteRig();
        h.s().setUncalibratedReversal(false, true);
        selectControl(h, PaletteTarget::Scroll);
        moveAway(h);
        dwellOnTarget(h);
        const size_t m = mark(h);
        for (int i = 0; i < 150; ++i) {
            tk(h, {0, 40.f, 0});
        }
        require(seen(h, m).wheel > 3, "reversed vertical did not reverse the wheel");
    });

    test("Scroll Exit: holding still for the dwell duration leaves scroll; movement defers it", [] {
        HF h = paletteRig();
        selectControl(h, PaletteTarget::Scroll);
        moveAway(h);
        dwellOnTarget(h);
        require(h.s().actionsStatus(h.now).frozen, "precondition");
        // continuous scrolling longer than the dwell never exits
        for (int i = 0; i < 300; ++i) {
            tk(h, {0, 30.f, 0});
        }
        require(h.s().actionsStatus(h.now).frozen, "left scroll while still scrolling");
        // stillness: progress is shown and grows
        qt(h, 600);
        const float half = h.s().actionsStatus(h.now).exitProgress;
        require(half > .2f && half < .9f, "no exit progress while still");
        // a movement restarts it
        for (int i = 0; i < 20; ++i) {
            tk(h, {0, 30.f, 0});
        }
        require(h.s().actionsStatus(h.now).exitProgress < .2f, "movement did not restart the exit");
        const size_t m = mark(h);
        qt(h, 1700);
        const auto st = h.s().actionsStatus(h.now);
        require(!st.frozen && std::strcmp(st.scroll, "OFF") == 0, "did not exit scroll");
        require(std::strcmp(st.mode, "LEFT") == 0 && st.scrollExits == 1, "mode after exit");
        require(seen(h, m).primary == 0, "exiting scroll clicked");
        // no click straight after: deliberate movement first, then the pointer is free again
        const size_t quiet = mark(h);
        qt(h, 5000);
        require(seen(h, quiet).primary == 0, "a click after leaving scroll without movement");
        const size_t moved = mark(h);
        moveAway(h);
        require(seen(h, moved).dx != 0, "the pointer stayed frozen after the exit");
    });

    test("Scroll Exit uses the same adjustable dwell duration (no per-action wait)", [] {
        HF h = paletteRig();
        require(h.s().setUncalibratedDwellSettings(2000, 8.f), "settings");
        report(h, PaletteTarget::Scroll);
        hold(h, PaletteTarget::Scroll, 260); // 250 ms arming + the 2000 ms dwell
        report(h, PaletteTarget::None);
        moveAway(h);
        qt(h, 2500);
        require(h.s().actionsStatus(h.now).frozen, "scroll did not start at 2000 ms dwell");
        qt(h, 1100); // still inside the 2000 ms dwell, which began when scrolling did
        require(h.s().actionsStatus(h.now).frozen, "exited before the longer dwell");
        qt(h, 1100);
        require(!h.s().actionsStatus(h.now).frozen, "did not exit after the longer dwell");
    });

    test("palette selection never clicks through, for every control", [] {
        for (PaletteTarget target : {PaletteTarget::Left, PaletteTarget::Right, PaletteTarget::Double,
                                     PaletteTarget::Drag, PaletteTarget::Scroll}) {
            HF h = paletteRig();
            const size_t m = mark(h);
            selectControl(h, target);
            Seen s = seen(h, m);
            require(s.primary == 0 && s.secondary == 0 && s.wheelAbs == 0,
                    "an OS click or wheel report came out of a palette selection");
            require(h.s().actionsStatus(h.now).selections == 1, "selection not recorded");
            require(!h.s().actionsStatus(h.now).dragging, "drag started by selecting it");
        }
    });

    test("no chained selection: staying on the palette selects once", [] {
        HF h = paletteRig();
        report(h, PaletteTarget::Right);
        hold(h, PaletteTarget::Right, kDwellTicks);
        require(h.s().actionsStatus(h.now).selections == 1, "first selection");
        hold(h, PaletteTarget::Right, 600); // 6 more seconds on the same control
        require(h.s().actionsStatus(h.now).selections == 1, "re-selected without movement");
        // moving to a different control with deliberate movement selects again
        for (int i = 0; i < 40; ++i) {
            tk(h, {40.f, 0, 0});
        }
        qt(h, 300);
        report(h, PaletteTarget::Left);
        hold(h, PaletteTarget::Left, kDwellTicks);
        require(h.s().actionsStatus(h.now).selections == 2, "no second selection");
        require(std::strcmp(mode(h), "LEFT") == 0, "second selection not applied");
    });

    test("leaving the palette clears the dwell and re-arms it only after deliberate movement", [] {
        HF h = paletteRig();
        report(h, PaletteTarget::Drag);
        hold(h, PaletteTarget::Drag, 90); // part-way through the dwell
        require(h.s().actionsStatus(h.now).dwellProgress > .1f, "no progress on the palette");
        report(h, PaletteTarget::None);
        qt(h, 50);
        require(h.s().actionsStatus(h.now).dwellProgress == 0.f, "progress survived leaving");
        const size_t m = mark(h);
        qt(h, 5000);
        require(seen(h, m).primary == 0 && seen(h, m).secondary == 0, "clicked after leaving");
        require(std::strcmp(mode(h), "LEFT") == 0 && h.s().actionsStatus(h.now).selections == 0,
                "a half-finished selection took effect");
        moveAway(h);
        dwellOnTarget(h);
        require(seen(h, m).primary == 1, "no click after moving on from the palette");
    });

    test("Stop is selectable by dwell in Left, Right, Drag and Armed Scroll", [] {
        for (PaletteTarget before : {PaletteTarget::Left, PaletteTarget::Right, PaletteTarget::Drag,
                                     PaletteTarget::Scroll}) {
            HF h = paletteRig();
            if (before != PaletteTarget::Left) {
                selectControl(h, before);
                moveAway(h);
            }
            const size_t m = mark(h);
            selectControl(h, PaletteTarget::Stop);
            require(!h.s().uncalibratedDemo() && !h.s().actionPaletteEnabled(), "Stop did not stop");
            require(h.s().state != SystemState::Active, "still active after Stop");
            Seen s = seen(h, m);
            require(s.primary == 0 && s.secondary == 0, "Stop clicked");
            require(h.released(), "no all-zero release after Stop");
            qt(h, 3000);
            require(h.s().state != SystemState::Active, "restarted by itself");
        }
    });

    test("Stop while dragging releases the button first", [] {
        HF h = paletteRig();
        selectControl(h, PaletteTarget::Drag);
        moveAway(h);
        dwellOnTarget(h);
        require(h.s().actionsStatus(h.now).dragging && h.transport.reports.back().down, "precondition");
        h.s().stopUncalibratedDemo("website stop");
        require(h.released(), "the button was not released by the stop");
        require(!h.s().actionsStatus(h.now).dragging && !h.s().actionPaletteEnabled(), "state kept");
        qt(h, 1000);
        require(h.released() && h.s().state != SystemState::Active, "output after the stop");
    });

    test("website Stop while scroll is active (pointer frozen) stops at once", [] {
        HF h = paletteRig();
        selectControl(h, PaletteTarget::Scroll);
        moveAway(h);
        dwellOnTarget(h);
        require(h.s().actionsStatus(h.now).frozen, "precondition");
        for (int i = 0; i < 40; ++i) {
            tk(h, {0, 40.f, 0});
        }
        h.s().stopUncalibratedDemo("website stop");
        require(h.released() && !h.s().actionsStatus(h.now).frozen, "not stopped");
        const size_t m = mark(h);
        qt(h, 2000);
        require(seen(h, m).wheelAbs == 0, "wheel after the stop");
    });

    test("physical enable button stops a drag and releases it", [] {
        HF h = paletteRig();
        selectControl(h, PaletteTarget::Drag);
        moveAway(h);
        dwellOnTarget(h);
        require(h.s().actionsStatus(h.now).dragging, "precondition");
        h.click(); // the physical button is a stop in the fallback session
        require(h.s().state != SystemState::Active, "the button did not stop");
        require(h.released(), "button not released");
        require(!h.s().actionsStatus(h.now).dragging, "drag state kept");
        qt(h, 2000);
        require(h.s().state != SystemState::Active, "restarted by itself");
    });

    test("a fault during a drag releases, clears the action and never restarts by itself", [] {
        HF h = paletteRig();
        selectControl(h, PaletteTarget::Drag);
        moveAway(h);
        dwellOnTarget(h);
        require(h.s().actionsStatus(h.now).dragging, "precondition");
        h.s().axes.axes = {0, 0, 1}; // an invalid axis mapping is a sensor-path fault
        tk(h);
        require(h.s().state == SystemState::SafeState, "no safe state");
        require(h.released(), "the button was not released on the fault");
        h.s().axes.axes = {0, 1, 2};
        qt(h, 3000);
        require(h.s().state != SystemState::Active && !h.s().uncalibratedDemo(), "auto restart");
        require(!h.s().actionPaletteEnabled() && !h.s().actionsStatus(h.now).dragging,
                "palette survived the fault");
        require(h.released(), "output after the fault");
    });

    test("disconnect clears pending actions; reconnecting never resumes or presses", [] {
        HF h = paletteRig();
        selectControl(h, PaletteTarget::Drag);
        moveAway(h);
        dwellOnTarget(h);
        require(h.s().actionsStatus(h.now).dragging, "precondition");
        h.transport.online = false;
        qt(h, 300);
        require(h.s().state == SystemState::SafeState, "no safe state on disconnect");
        require(!h.s().actionsStatus(h.now).dragging && !h.s().actionPaletteEnabled(),
                "pending action kept across a disconnect");
        h.transport.online = true;
        const size_t m = mark(h);
        qt(h, 4000);
        require(h.s().state != SystemState::Active, "resumed by itself");
        Seen s = seen(h, m);
        require(s.primary == 0 && s.secondary == 0 && !h.transport.reports.back().down,
                "a press after reconnecting");
        // NOT claimed here: that the host releases a button that was held when the link dropped.
    });

    test("failed delivery of the drag release inhibits all further output", [] {
        HF h = paletteRig();
        selectControl(h, PaletteTarget::Drag);
        moveAway(h);
        dwellOnTarget(h);
        // deliberate movement was made by moveAway; now the release dwell with a failing release
        h.transport.failRelease = true;
        for (int i = 0; i < 30; ++i) {
            tk(h, {30.f, 0, 0});
        }
        qt(h, 300);
        const uint32_t faults = h.s().diagnostics.faults;
        for (unsigned i = 0; i < 160 && h.s().diagnostics.faults == faults; ++i) {
            tk(h); // run until the release dwell completes
        }
        require(h.s().diagnostics.faults == faults + 1, "failed release did not raise a fault");
        require(h.s().state == SystemState::SafeState, "failed release did not stop output");
        require(h.s().diagnostics.faultCode == FaultCode::Transport, "fault code");
        require(!h.s().uncalibratedDemo() && !h.s().actionPaletteEnabled(), "session kept");
        h.transport.failRelease = false;
        const size_t m = mark(h);
        qt(h, 4000);
        require(h.s().state != SystemState::Active, "restarted automatically");
        require(seen(h, m).primary == 0 && seen(h, m).dx == 0, "output after the failed release");
    });

    test("failed release of a click pulse inhibits output; right and double likewise", [] {
        for (PaletteTarget action : {PaletteTarget::Left, PaletteTarget::Right, PaletteTarget::Double}) {
            HF h = paletteRig();
            if (action != PaletteTarget::Left) {
                selectControl(h, action);
                moveAway(h);
            }
            h.transport.failRelease = true;
            const uint32_t faults = h.s().diagnostics.faults;
            for (unsigned i = 0; i < 200 && h.s().diagnostics.faults == faults; ++i) {
                tk(h);
            }
            require(h.s().diagnostics.faults == faults + 1 && h.s().state == SystemState::SafeState,
                    "failed click release kept output on");
            h.transport.failRelease = false;
            const size_t m = mark(h);
            qt(h, 3000);
            require(h.s().state != SystemState::Active && seen(h, m).primary == 0 &&
                        seen(h, m).secondary == 0,
                    "output after the failed release");
        }
    });

    test("mode changes: dwell clicking turns the palette off and releases a held drag", [] {
        HF h = paletteRig();
        selectControl(h, PaletteTarget::Drag);
        moveAway(h);
        dwellOnTarget(h);
        require(h.s().actionsStatus(h.now).dragging, "precondition");
        require(h.s().setUncalibratedDwell(true, h.now), "dwell enable");
        require(!h.s().actionPaletteEnabled(), "palette stayed on beside dwell clicking");
        require(h.released(), "the held button stayed down when the mode changed");
        require(h.s().uncalibratedDwell(), "dwell not on");
        // and back: the palette turns dwell clicking off
        require(h.s().setActionPalette(true, h.now), "palette refused");
        require(!h.s().uncalibratedDwell(), "dwell clicking stayed on beside the palette");
        require(std::strcmp(mode(h), "LEFT") == 0 && !h.s().actionsStatus(h.now).dragging,
                "palette started in a stale mode");
    });

    test("turning the palette off releases a drag and clears the action", [] {
        HF h = paletteRig();
        selectControl(h, PaletteTarget::Drag);
        moveAway(h);
        dwellOnTarget(h);
        require(h.s().setActionPalette(false, h.now), "off refused");
        require(h.released() && !h.s().actionPaletteEnabled(), "not released");
        require(h.s().state == SystemState::Active, "turning the palette off stopped the session");
        const size_t m = mark(h);
        qt(h, 3000);
        require(seen(h, m).primary == 0, "a click while the palette is off");
    });

    test("every stop clears the palette; a restart needs it enabled again", [] {
        HF h = paletteRig();
        selectControl(h, PaletteTarget::Right);
        require(std::strcmp(mode(h), "RIGHT") == 0, "precondition");
        h.s().stopUncalibratedDemo("test");
        require(h.s().startUncalibratedDemo(h.now), "restart");
        qt(h, 300);
        require(!h.s().actionPaletteEnabled(), "palette on after a restart");
        require(h.s().setActionPalette(true, h.now), "enable again");
        require(std::strcmp(mode(h), "LEFT") == 0, "pending Right survived the stop");
        const size_t m = mark(h);
        dwellOnTarget(h);
        require(seen(h, m).primary == 1 && seen(h, m).secondary == 0, "stale action executed");
    });

    test("settings are temporary and profiles are preserved", [] {
        HF h(true, EnableKind::Momentary);
        h.sw = false;
        qt(h, 400);
        UserProfile before;
        require(h.repo.load(before), "profile");
        const auto writes = h.profileStorage.read(0);
        require(h.s().startUncalibratedDemo(h.now), "start");
        qt(h, 300);
        require(h.s().setActionPalette(true, h.now), "palette");
        require(h.s().setUncalibratedDwellSettings(1800, 12.f), "dwell settings");
        selectControl(h, PaletteTarget::Drag);
        moveAway(h);
        dwellOnTarget(h);
        h.s().stopUncalibratedDemo("done");
        UserProfile after;
        require(h.repo.load(after), "profile after");
        require(before.dwellMs == after.dwellMs && before.dwellEnabled == after.dwellEnabled &&
                    before.scrollEnabled == after.scrollEnabled &&
                    before.dwellTolerance == after.dwellTolerance,
                "the saved profile changed");
        require(h.profileStorage.read(0) == writes, "profile storage written");
        require(h.s().profile.dwellMs == before.dwellMs, "live profile changed");
        // a fresh System (a reboot): nothing of the palette is remembered
        h.boot();
        qt(h, 400);
        require(!h.s().actionPaletteEnabled(), "palette on after a reboot");
        require(h.s().actionsStatus(h.now).dwellMs == start::uncalDwellMs, "dwell setting persisted");
    });

    test("the one dwell duration drives every action (a longer dwell delays all of them)", [] {
        for (PaletteTarget action :
             {PaletteTarget::Left, PaletteTarget::Right, PaletteTarget::Double, PaletteTarget::Drag}) {
            HF h = paletteRig();
            require(h.s().setUncalibratedDwellSettings(3000, 8.f), "settings");
            if (action != PaletteTarget::Left) {
                report(h, action);
                hold(h, action, 200); // 2 s: not enough for a 3 s dwell
                require(h.s().actionsStatus(h.now).selections == 0, "selected before the long dwell");
                hold(h, action, 200);
                require(h.s().actionsStatus(h.now).selections == 1, "not selected after it");
                report(h, PaletteTarget::None);
                moveAway(h);
            }
            const size_t m = mark(h);
            qt(h, 2500);
            require(seen(h, m).primary == 0 && seen(h, m).secondary == 0 &&
                        !h.s().actionsStatus(h.now).dragging,
                    "acted before the long dwell");
            qt(h, 1200);
            const bool acted = seen(h, m).primary + seen(h, m).secondary > 0 ||
                               h.s().actionsStatus(h.now).dragging;
            require(acted, "did not act after the long dwell");
        }
    });

    test("palette state machine alone: counts, labels and the JSON frame fit", [] {
        ActionPalette p;
        require(p.mode() == ActionMode::Left && !p.dragging() && !p.scrollActive(), "initial");
        PaletteTarget parsed;
        require(parsePaletteTarget("Scroll", parsed) && parsed == PaletteTarget::Scroll, "parse");
        require(!parsePaletteTarget("middle", parsed) && !parsePaletteTarget("", parsed) &&
                    !parsePaletteTarget("leftx", parsed),
                "parse accepted junk");
        ActionsStatus st;
        st.enabled = true;
        st.mode = "DOUBLE";
        st.scroll = "ACTIVE";
        st.hover = "SCROLL";
        st.last = "DOUBLE";
        st.dwellState = "PROGRESS";
        st.blocked = "start the control session first";
        st.dwellMs = 5000;
        st.tolerance = 50;
        st.selections = st.left = st.right = st.doubles = st.dragStarts = st.dragReleases =
            st.scrollStarts = st.scrollExits = st.wheelUnits = 4000000000u;
        char buffer[actionsJsonCapacity];
        const size_t length = actionsJson(buffer, sizeof buffer, st);
        require(length > 100 && length < actionsJsonCapacity - 64, "JSON too large for its buffer");
        char tiny[40];
        require(actionsJson(tiny, sizeof tiny, st) == 0, "overflow was not reported");
    });

    test("status frame stays well inside the telemetry buffer with the palette block", [] {
        HF h = paletteRig();
        char buffer[handsFreeJsonCapacity];
        require(handsFreeJson(buffer, sizeof buffer, h.s().handsFreeStatus()) > 0, "hands-free json");
        char actions[actionsJsonCapacity];
        require(actionsJson(actions, sizeof actions, h.s().actionsStatus(h.now)) > 0, "actions json");
    });

    test("one selection per hover: a chosen control must be left before it can be chosen again", [] {
        HF h = paletteRig();
        report(h, PaletteTarget::Right);
        hold(h, PaletteTarget::Right, kDwellTicks);
        require(h.s().actionsStatus(h.now).selections == 1, "first selection");
        require(std::strcmp(h.s().actionsStatus(h.now).locked, "RIGHT") == 0, "no selection lock shown");
        // still on the same control, even after deliberate movement and another full dwell: nothing
        for (int i = 0; i < 40; ++i) {
            tk(h, {40.f, 0, 0});
        }
        qt(h, 300);
        hold(h, PaletteTarget::Right, 400);
        require(h.s().actionsStatus(h.now).selections == 1, "chosen again without leaving the control");
        // leave it (onto the palette background), come back, dwell: chosen again
        report(h, PaletteTarget::Frame);
        qt(h, 100);
        require(std::strcmp(h.s().actionsStatus(h.now).locked, "NONE") == 0, "lock kept after leaving");
        for (int i = 0; i < 40; ++i) {
            tk(h, {40.f, 0, 0});
        }
        qt(h, 300);
        hold(h, PaletteTarget::Right, kDwellTicks);
        require(h.s().actionsStatus(h.now).selections == 2, "not chosen again after leaving and re-entering");
        // leaving to the target works too
        report(h, PaletteTarget::None);
        qt(h, 100);
        require(std::strcmp(h.s().actionsStatus(h.now).locked, "NONE") == 0, "lock kept after leaving to the target");
    });

    test("Drop releases a drag without any movement; the mode returns to Left-click", [] {
        HF h = paletteRig();
        selectControl(h, PaletteTarget::Drag);
        moveAway(h);
        dwellOnTarget(h);
        require(h.s().actionsStatus(h.now).dragging, "precondition");
        const size_t m = mark(h);
        report(h, PaletteTarget::Drop); // the pointer reaches Drop; nothing else moves
        hold(h, PaletteTarget::Drop, kDwellTicks);
        require(!seen(h, m).primaryHeld, "the button is still held after Drop");
        const auto st = h.s().actionsStatus(h.now);
        require(!st.dragging && st.drops == 1, "Drop not recorded");
        require(std::strcmp(st.mode, "LEFT") == 0, "mode did not return to Left-click");
        require(seen(h, m).primary == 0 && seen(h, m).secondary == 0, "Drop clicked");
        require(h.s().state == SystemState::Active, "Drop ended control");
    });

    test("Cancel clears a chosen one-shot and an armed Scroll, and releases a drag, without movement", [] {
        // a chosen one-shot that was never executed
        HF a = paletteRig();
        selectControl(a, PaletteTarget::Right);
        require(std::strcmp(mode(a), "RIGHT") == 0, "precondition");
        selectControl(a, PaletteTarget::Cancel);
        require(std::strcmp(mode(a), "LEFT") == 0 && a.s().actionsStatus(a.now).cancels == 1, "Cancel");
        moveAway(a);
        const size_t ma = mark(a);
        dwellOnTarget(a);
        require(seen(a, ma).primary == 1 && seen(a, ma).secondary == 0, "the cancelled right click ran");
        // an armed Scroll
        HF b = paletteRig();
        selectControl(b, PaletteTarget::Scroll);
        require(std::strcmp(b.s().actionsStatus(b.now).scroll, "ARMED") == 0, "precondition");
        selectControl(b, PaletteTarget::Cancel);
        require(std::strcmp(b.s().actionsStatus(b.now).scroll, "OFF") == 0, "armed scroll survived Cancel");
        moveAway(b);
        dwellOnTarget(b);
        require(!b.s().actionsStatus(b.now).frozen, "a cancelled scroll started");
        // a held drag
        HF c = paletteRig();
        selectControl(c, PaletteTarget::Drag);
        moveAway(c);
        dwellOnTarget(c);
        require(c.s().actionsStatus(c.now).dragging, "precondition");
        const size_t mc = mark(c);
        hold(c, PaletteTarget::Cancel, kDwellTicks);
        require(!seen(c, mc).primaryHeld && !c.s().actionsStatus(c.now).dragging, "drag survived Cancel");
        require(std::strcmp(mode(c), "LEFT") == 0 && c.s().actionsStatus(c.now).cancels == 1, "Cancel state");
    });

    test("Cancel is the palette-level cancel of a pending commit (unit level)", [] {
        ActionPalette p;
        uint32_t now = 1000;
        p.setHover(PaletteTarget::None, now);
        ActionInput in;
        in.dwellPulse = true;
        p.update(in, now); // a target dwell completed: the click waits out the commit time
        require(p.pending(), "no pending click");
        p.setHover(PaletteTarget::Cancel, now + 20);
        in.dwellPulse = false;
        p.update(in, now + 30);
        require(!p.pending() && p.cancelled == 1, "palette entry did not cancel the pending click");
        in.dwellPulse = true;
        const ActionOutput out = p.update(in, now + 40); // the dwell on Cancel itself
        require(!out.pulse && p.cancels == 1 && p.mode() == ActionMode::Left, "Cancel");
    });

    test("release on palette entry is confirmed before any selection is possible (unit level)", [] {
        ActionPalette p;
        uint32_t now = 1000;
        p.setHover(PaletteTarget::Drag, now);
        ActionInput in;
        in.dwellPulse = true;
        p.update(in, now); // choose Drag
        p.setHover(PaletteTarget::None, now + 10);
        in.dwellPulse = true;
        p.update(in, now + 20); // starts the commit wait
        ActionOutput out = p.update(in = ActionInput{}, now + 20 + start::actionCommitMs + 10);
        require(p.dragging() && out.down, "drag did not start");
        p.setHover(PaletteTarget::Left, now + 400); // pointer enters the palette while dragging
        out = p.update(in, now + 410);
        require(!out.down && !p.dragging() && p.paletteReleases == 1, "no release on entry");
        require(p.releaseUnconfirmed(), "release not awaiting confirmation");
        in.dwellPulse = true;
        out = p.update(in, now + 420); // a dwell completes before the release is confirmed
        require(p.selections == 1 && p.mode() == ActionMode::Drag, "selected before the release was confirmed");
        require(p.inhibited == 1, "the early dwell was not counted as inhibited");
        p.confirmRelease();
        out = p.update(in, now + 430);
        require(p.selections == 2 && p.mode() == ActionMode::Left, "no selection once confirmed");
        require(p.confirmedReleases == 1, "confirmation not counted");
    });

    test("the System confirms the entry release only after it was delivered", [] {
        HF h = paletteRig();
        selectControl(h, PaletteTarget::Drag);
        moveAway(h);
        dwellOnTarget(h);
        require(h.s().actionsStatus(h.now).dragging, "precondition");
        const uint32_t selections = h.s().actionsStatus(h.now).selections;
        report(h, PaletteTarget::Left);
        tk(h);
        tk(h);
        require(!h.transport.reports.back().down, "release not sent");
        require(h.s().actionsStatus(h.now).paletteReleases == 1, "release on entry not counted");
        require(h.s().actionsStatus(h.now).selections == selections, "selection at entry");
        // a failed release inhibits everything instead of being "confirmed"
        HF f = paletteRig();
        selectControl(f, PaletteTarget::Drag);
        moveAway(f);
        dwellOnTarget(f);
        f.transport.failRelease = true;
        const uint32_t faults = f.s().diagnostics.faults;
        report(f, PaletteTarget::Left);
        for (int i = 0; i < 5; ++i) {
            tk(f);
        }
        require(f.s().diagnostics.faults == faults + 1 && f.s().state == SystemState::SafeState,
                "a failed entry release did not inhibit output");
        f.transport.failRelease = false;
        qt(f, 4000);
        require(f.s().state != SystemState::Active, "restarted after a failed release");
        require(f.s().actionsStatus(f.now).selections == selections, "a selection after the failed release");
    });

    test("immediate stopping: Stop while a click waits out the commit time never clicks", [] {
        HF h = paletteRig();
        moveAway(h);
        const size_t m = mark(h);
        for (unsigned i = 0; i < 400 && !h.s().actionsStatus(h.now).pending; ++i) {
            tk(h);
        }
        require(h.s().actionsStatus(h.now).pending, "no pending action");
        h.s().stopUncalibratedDemo("website stop");
        qt(h, 600);
        require(seen(h, m).primary == 0 && h.released(), "a click after the stop");
        require(h.s().state != SystemState::Active, "restarted");
    });

    test("communication failures: a disconnect during a pending click or a scroll never acts or resumes", [] {
        HF h = paletteRig();
        moveAway(h);
        for (unsigned i = 0; i < 400 && !h.s().actionsStatus(h.now).pending; ++i) {
            tk(h);
        }
        h.transport.online = false;
        qt(h, 400);
        h.transport.online = true;
        const size_t m = mark(h);
        qt(h, 5000);
        require(seen(h, m).primary == 0 && h.s().state != SystemState::Active, "acted after a disconnect");
        HF g = paletteRig();
        selectControl(g, PaletteTarget::Scroll);
        moveAway(g);
        dwellOnTarget(g);
        require(g.s().actionsStatus(g.now).frozen, "precondition");
        g.transport.online = false;
        qt(g, 400);
        g.transport.online = true;
        qt(g, 3000);
        require(!g.s().actionsStatus(g.now).frozen && g.s().state != SystemState::Active, "scroll survived");
    });

    test("pointer speed: validated, RAM only, default 1x, kept across session restarts until a reboot", [] {
        HF h = rig();
        require(h.s().uncalibratedSpeed() == 1.f, "default speed");
        for (float bad : {0.f, -1.f, 0.24f, 2.01f, 5.f, std::nanf(""), INFINITY}) {
            require(!h.s().setUncalibratedSpeed(bad, h.now), "an invalid speed was accepted");
            require(h.s().uncalibratedSpeed() == 1.f, "an invalid speed changed the speed");
        }
        for (float good : {0.25f, 0.5f, 1.f, 1.5f, 2.f}) {
            require(h.s().setUncalibratedSpeed(good, h.now) && h.s().uncalibratedSpeed() == good, "valid speed");
        }
        require(h.s().handsFreeStatus().uncalSpeed == 2.f, "speed not reported");
        h.s().stopUncalibratedDemo("test");
        require(h.s().uncalibratedSpeed() == 2.f, "a stop cleared the speed");
        require(h.s().startUncalibratedDemo(h.now), "restart");
        require(h.s().uncalibratedSpeed() == 2.f, "a restart changed the speed");
        require(h.s().setUncalibratedSpeed(1.f, h.now), "reset");
        h.s().setUncalibratedSpeed(0.5f, h.now);
        h.boot(); // a reboot: nothing is remembered
        require(h.s().uncalibratedSpeed() == 1.f, "the speed survived a reboot");
        // saved profile untouched
        HF p(true, EnableKind::Momentary);
        UserProfile before, after;
        require(p.repo.load(before), "profile");
        p.s().setUncalibratedSpeed(2.f, p.now);
        require(p.repo.load(after) && before.gain[0] == after.gain[0] && before.dwellMs == after.dwellMs,
                "speed touched the saved profile");
    });

    test("pointer speed scales the pointer step before the output bounds, at 0.25x, 1x and 2x", [] {
        float total[3];
        const float speeds[3] = {.25f, 1.f, 2.f};
        for (int i = 0; i < 3; ++i) {
            HF h = rig();
            require(h.s().setUncalibratedSpeed(speeds[i], h.now), "speed");
            const size_t m = mark(h);
            for (int t = 0; t < 150; ++t) {
                tk(h, {10.f, 0, 0}); // a gentle movement: well inside the bounds at every speed
            }
            total[i] = totalDx(h, m);
        }
        std::printf("   INFO gentle 1.5 s movement: %.0f / %.0f / %.0f HID units at 0.25x / 1x / 2x\n",
                    total[0], total[1], total[2]);
        require(total[1] > 20, "no movement at 1x");
        require(std::fabs(total[0] / total[1] - .25f) < .05f, "0.25x is not a quarter of 1x");
        require(std::fabs(total[2] / total[1] - 2.f) < .2f, "2x is not double 1x");
    });

    test("pointer speed never lets a report beat the existing output bounds", [] {
        for (float speed : {.25f, 1.f, 2.f}) {
            HF h = rig();
            require(h.s().setUncalibratedSpeed(speed, h.now), "speed");
            const size_t m = mark(h);
            for (int t = 0; t < 200; ++t) {
                tk(h, {t % 2 ? 230.f : -230.f, 230.f, 0}); // the sensor range is 240 deg/s
            }
            int worst = 0;
            for (size_t i = m; i < h.transport.reports.size(); ++i) {
                worst = std::max({worst, std::abs(int(h.transport.reports[i].dx)),
                                  std::abs(int(h.transport.reports[i].dy))});
            }
            require(worst <= int(start::uncalDemoMaxStep), "a report beat the demo step bound");
            require(worst <= start::maxPointer, "a report beat the pointer bound");
        }
        // at 2x a strong movement is clipped by the SAME bound (the bound is not scaled)
        HF h = rig();
        h.s().setUncalibratedSpeed(2.f, h.now);
        const size_t m = mark(h);
        for (int t = 0; t < 100; ++t) {
            tk(h, {230.f, 0, 0});
        }
        int worst = 0;
        for (size_t i = m; i < h.transport.reports.size(); ++i) {
            worst = std::max(worst, int(h.transport.reports[i].dx));
        }
        std::printf("   INFO strongest report at 2x: %d (bound %d)\n", worst, int(start::uncalDemoMaxStep));
        require(worst == int(start::uncalDemoMaxStep), "the step bound was changed by the speed");
    });

    test("pointer speed does not change the wheel speed or button behaviour", [] {
        float wheel[2];
        const float speeds[2] = {.25f, 2.f};
        for (int i = 0; i < 2; ++i) {
            HF h = paletteRig();
            require(h.s().setUncalibratedSpeed(speeds[i], h.now), "speed");
            selectControl(h, PaletteTarget::Scroll);
            moveUnits(h, 40.f);
            qt(h, 300);
            dwellOnTarget(h);
            require(h.s().actionsStatus(h.now).frozen, "scroll did not start");
            const size_t m = mark(h);
            for (int t = 0; t < 200; ++t) {
                tk(h, {0, 40.f, 0});
            }
            wheel[i] = float(seen(h, m).wheelAbs);
            require(seen(h, m).primary == 0 && seen(h, m).secondary == 0, "a button report while scrolling");
        }
        std::printf("   INFO wheel notches in 2 s of scrolling: %.0f at 0.25x, %.0f at 2x\n", wheel[0], wheel[1]);
        require(wheel[0] > 5 && wheel[0] == wheel[1], "the wheel speed depends on the pointer speed");
    });

    test("dwell cancellation and re-arming at minimum, default and maximum pointer speed", [] {
        unsigned ticksFor[3];
        const float speeds[3] = {.25f, 1.f, 2.f};
        for (int i = 0; i < 3; ++i) {
            const float speed = speeds[i];
            HF h = paletteRig();
            require(h.s().setUncalibratedSpeed(speed, h.now), "speed");
            // cancellation: part of a dwell, then movement beyond the tolerance (3 x 8 units)
            qt(h, 700);
            require(seen(h, 0).primary == 0, "clicked early");
            const size_t m = mark(h);
            ticksFor[i] = moveUnits(h, 24.f, 12.f);
            qt(h, 900);
            require(seen(h, m).primary == 0, "the interrupted dwell still clicked");
            qt(h, 1500);
            require(seen(h, m).primary == 1, "no click after the fresh dwell");
            // lockout: movement under 1.5 x tolerance (12 units) does not re-arm, over it does
            const size_t after = mark(h);
            moveUnits(h, 6.f, 10.f); // a gentle movement; the filter tail adds a little more
            qt(h, 300);
            require(std::fabs(totalDx(h, after)) < 11.5f, "the test movement is not under the lockout radius");
            qt(h, 4000);
            require(seen(h, after).primary == 0, "re-armed by movement under the lockout radius");
            moveUnits(h, 20.f, 12.f);
            qt(h, 2300);
            require(seen(h, after).primary == 1, "not re-armed by deliberate movement");
        }
        std::printf("   INFO ticks of head movement to accumulate 24 units: %u at 0.25x, %u at 1x, %u at 2x\n",
                    ticksFor[0], ticksFor[1], ticksFor[2]);
        require(ticksFor[0] >= 3 * ticksFor[2] && ticksFor[0] > ticksFor[1] && ticksFor[1] > ticksFor[2],
                "a slower pointer did not need more head movement");
    });

    test("changing the pointer speed restarts a dwell in progress (its movement units changed meaning)", [] {
        HF h = paletteRig();
        qt(h, 900);
        require(h.s().actionsStatus(h.now).dwellProgress > .2f, "no dwell progress to lose");
        const size_t m = mark(h);
        require(h.s().setUncalibratedSpeed(.5f, h.now), "speed");
        tk(h);
        require(h.s().actionsStatus(h.now).dwellProgress < .05f, "the old dwell survived a speed change");
        qt(h, 900);
        require(seen(h, m).primary == 0, "clicked on the old dwell");
        qt(h, 1500);
        require(seen(h, m).primary == 1, "no click after the fresh dwell");
    });

    test("the same head movement re-arms at 1x but not at 0.25x (the documented effect of speed)", [] {
        // find a movement length that gives about 20 units at 1x
        unsigned n = 0;
        {
            HF probe = paletteRig();
            n = moveUnits(probe, 20.f, 12.f);
        }
        require(n >= 2 && n < 200, "calibration of the movement failed");
        auto rearms = [&](float speed) {
            HF h = paletteRig();
            require(h.s().setUncalibratedSpeed(speed, h.now), "speed");
            dwellOnTarget(h);
            const size_t m = mark(h);
            for (unsigned t = 0; t < n; ++t) {
                tk(h, {12.f, 0, 0});
            }
            qt(h, 2500);
            return seen(h, m).primary == 1;
        };
        require(rearms(1.f), "1x did not re-arm");
        require(rearms(2.f), "2x did not re-arm");
        require(!rearms(.25f), "0.25x re-armed from a movement that is too small in HID units");
    });

    test("keep selected action: off by default; Right and Double return to Left unless it is on", [] {
        HF h = paletteRig();
        require(!h.s().actionsStatus(h.now).keep, "keep is on by default");
        selectControl(h, PaletteTarget::Right);
        moveAway(h);
        dwellOnTarget(h);
        require(std::strcmp(mode(h), "LEFT") == 0, "Right did not return to Left by default");
        // turn it on: Right stays through three executions
        require(h.s().setActionKeep(true), "keep refused");
        selectControl(h, PaletteTarget::Right);
        const size_t m = mark(h);
        for (int i = 0; i < 3; ++i) {
            moveAway(h);
            dwellOnTarget(h);
            require(std::strcmp(mode(h), "RIGHT") == 0, "Right did not stay selected");
        }
        require(seen(h, m).secondary == 3 && seen(h, m).primary == 0, "not three right clicks");
        // Double likewise
        selectControl(h, PaletteTarget::Double);
        const size_t d = mark(h);
        for (int i = 0; i < 2; ++i) {
            moveAway(h);
            dwellOnTarget(h);
            require(std::strcmp(mode(h), "DOUBLE") == 0, "Double did not stay selected");
        }
        require(seen(h, d).primary == 4, "not two double-clicks");
        // turning it off: the next execution is the last
        h.s().setActionKeep(false);
        moveAway(h);
        dwellOnTarget(h);
        require(std::strcmp(mode(h), "LEFT") == 0, "mode did not return after keep was turned off");
    });

    test("keep selected action never applies to Drag or Scroll, and Cancel still clears the action", [] {
        for (bool keep : {false, true}) {
            HF h = paletteRig();
            h.s().setActionKeep(keep);
            // Scroll: the exit always returns to Left; starting again needs its own dwell on the content
            selectControl(h, PaletteTarget::Scroll);
            moveAway(h);
            dwellOnTarget(h);
            require(h.s().actionsStatus(h.now).frozen, "scroll did not start");
            qt(h, 2000);
            require(!h.s().actionsStatus(h.now).frozen && std::strcmp(mode(h), "LEFT") == 0,
                    "scroll did not exit to Left");
            moveAway(h);
            dwellOnTarget(h);
            require(!h.s().actionsStatus(h.now).frozen, "scroll restarted by itself");
            // Drag: a release never presses again without its own dwell
            selectControl(h, PaletteTarget::Drag);
            moveAway(h);
            dwellOnTarget(h);
            require(h.s().actionsStatus(h.now).dragging, "no press");
            qt(h, 2000);
            require(!h.s().actionsStatus(h.now).dragging, "no release");
            const size_t m = mark(h);
            qt(h, 4000);
            require(seen(h, m).primary == 0, "a drag restarted by itself");
            // Cancel clears the selected action even with keep on
            selectControl(h, PaletteTarget::Right);
            selectControl(h, PaletteTarget::Cancel);
            require(std::strcmp(mode(h), "LEFT") == 0, "Cancel did not clear the action");
            require(h.s().actionsStatus(h.now).keep == keep, "Cancel changed the keep option");
        }
    });

    test("session restart defaults: keep off, palette off, Left-click; speed is kept (RAM)", [] {
        HF h = paletteRig();
        h.s().setActionKeep(true);
        h.s().setUncalibratedSpeed(1.5f, h.now);
        selectControl(h, PaletteTarget::Right);
        require(std::strcmp(mode(h), "RIGHT") == 0 && h.s().actionsStatus(h.now).keep, "precondition");
        h.s().stopUncalibratedDemo("test");
        require(h.s().startUncalibratedDemo(h.now), "restart");
        qt(h, 300);
        require(!h.s().actionPaletteEnabled(), "palette on after a restart");
        require(!h.s().setActionKeep(true), "keep accepted with the palette off");
        require(h.s().setActionPalette(true, h.now), "palette refused");
        report(h, PaletteTarget::None);
        qt(h, 100);
        const auto st = h.s().actionsStatus(h.now);
        require(!st.keep && std::strcmp(st.mode, "LEFT") == 0, "stale keep or action after a restart");
        require(h.s().uncalibratedSpeed() == 1.5f, "speed lost across a restart");
    });

    test("website Stop and the physical button stop immediately at 0.25x and 2x with keep on", [] {
        for (float speed : {.25f, 2.f}) {
            for (bool physical : {false, true}) {
                HF h = paletteRig();
                h.s().setUncalibratedSpeed(speed, h.now);
                h.s().setActionKeep(true);
                selectControl(h, PaletteTarget::Drag);
                moveUnits(h, 30.f);
                qt(h, 300);
                dwellOnTarget(h);
                require(h.s().actionsStatus(h.now).dragging, "precondition");
                if (physical) {
                    h.click();
                } else {
                    h.s().stopUncalibratedDemo("website stop");
                }
                require(h.s().state != SystemState::Active && h.released(), "not stopped at once");
                qt(h, 2000);
                require(h.s().state != SystemState::Active && h.released(), "output after the stop");
            }
        }
    });

    // ================= desktop overlay controller =================
    test("overlay: exactly one controller owns the palette at a time", [] {
        HF b = rig();
        require(b.s().setActionPalette(true, b.now), "browser claim");
        require(std::strcmp(b.s().actionsStatus(b.now).controller, "BROWSER") == 0, "controller");
        require(!claimOverlay(b), "overlay claimed over the browser palette");
        require(!b.s().setOverlayMenu(true, b.now, gEpoch) && !b.s().overlaySelect(PaletteTarget::Left, b.now, gEpoch) &&
                    !b.s().setOverlayKeyboard(true, gEpoch),
                "overlay commands accepted while the browser controls");
        require(b.s().setActionHover(PaletteTarget::None, b.now), "browser hover refused");
        HF o = rig();
        require(claimOverlay(o), "overlay claim");
        gOverlay = true;
        require(std::strcmp(o.s().actionsStatus(o.now).controller, "OVERLAY") == 0, "controller");
        require(!o.s().setActionPalette(true, o.now) && !o.s().setActionPalette(false, o.now),
                "the browser took the palette from the overlay");
        require(!o.s().setActionHover(PaletteTarget::Left, o.now), "browser hover accepted during overlay");
        require(o.s().setOverlayMenu(false, o.now, gEpoch), "overlay menu refused");
        require(o.s().actionPaletteEnabled(), "overlay controls not on");
        require(o.s().setActionOverlay(false, o.now, 0, gEpoch) && !o.s().actionPaletteEnabled(), "overlay release");
        require(std::strcmp(o.s().actionsStatus(o.now).controller, "NONE") == 0, "controller after release");
        require(!o.s().setOverlayMenu(true, o.now, gEpoch), "menu accepted with nothing claimed");
        // never without a running session
        HF n(false, EnableKind::Momentary);
        n.sw = false;
        n.quiet(400);
        require(!claimOverlay(n), "claimed without a session");
    });

    test("overlay: the device keeps timing the TARGET dwell and clicks once, as Left-click by default", [] {
        HF h = overlayRig();
        const size_t m = mark(h);
        qt(h, 1900);
        require(seen(h, m).primary == 1 && seen(h, m).secondary == 0, "no single Left click");
        require(std::strcmp(mode(h), "LEFT") == 0, "mode");
    });

    test("overlay: menu selection happens only by the validated command; the device never times the menu", [] {
        HF h = overlayRig();
        moveAway(h);
        menu(h, true);
        const uint32_t before = h.s().actionsStatus(h.now).selections;
        qt(h, 8000); // a long rest on the overlay: the device must not select anything by itself
        require(h.s().actionsStatus(h.now).selections == before, "the device selected from a resting pointer");
        require(std::strcmp(mode(h), "LEFT") == 0, "mode changed by itself");
        require(h.s().overlaySelect(PaletteTarget::Right, h.now, gEpoch), "validated select refused");
        require(std::strcmp(mode(h), "RIGHT") == 0, "select not applied");
        menu(h, false);
        require(!h.s().overlaySelect(PaletteTarget::Left, h.now, gEpoch), "select accepted with the menu closed");
        for (PaletteTarget bad : {PaletteTarget::None, PaletteTarget::Frame}) {
            menu(h, true);
            require(!h.s().overlaySelect(bad, h.now, gEpoch), "a non-action was selectable");
        }
    });

    test("overlay: the menu-active state is acknowledged, inhibits target clicks, and rearming needs movement", [] {
        HF h = overlayRig();
        moveAway(h);
        menu(h, true);
        tk(h);
        const auto st = h.s().actionsStatus(h.now);
        require(st.menu && st.ready && st.inPalette, "menu state not acknowledged in the status");
        const size_t m = mark(h);
        qt(h, 6000); // a target dwell completes while the pointer is on the overlay
        require(seen(h, m).primary == 0 && seen(h, m).secondary == 0, "a target click while the menu was active");
        menu(h, false); // the menu closes: pending cleared, deliberate movement needed
        qt(h, 5000);
        require(seen(h, m).primary == 0, "target actions re-armed without movement after the menu closed");
        require(!h.s().actionsStatus(h.now).menu, "menu state not cleared");
        moveAway(h);
        qt(h, 1900);
        require(seen(h, m).primary == 1, "no click after deliberate movement");
    });

    test("overlay: a held drag is released and the release confirmed BEFORE any selection can execute", [] {
        HF h = overlayRig();
        require(ovSelect(h, PaletteTarget::Drag), "drag select");
        moveAway(h);
        qt(h, 1900);
        require(h.s().actionsStatus(h.now).dragging && h.transport.reports.back().down, "no drag held");
        const size_t m = mark(h);
        menu(h, true); // the pointer reaches the overlay
        // at this instant the button is still down: a selection must be refused
        require(!h.s().overlaySelect(PaletteTarget::Left, h.now, gEpoch), "a selection executed over a held button");
        tk(h);
        tk(h);
        require(!seen(h, m).primaryHeld && !h.s().actionsStatus(h.now).dragging, "the drag was not released");
        const auto st = h.s().actionsStatus(h.now);
        require(st.paletteReleases == 1 && st.ready, "release not confirmed in the status");
        require(h.s().overlaySelect(PaletteTarget::Left, h.now, gEpoch), "selection refused after the release");
    });

    test("overlay: Drop releases a drag without movement; Cancel releases and clears pending actions", [] {
        HF h = overlayRig();
        require(ovSelect(h, PaletteTarget::Drag), "drag select");
        moveAway(h);
        qt(h, 1900);
        require(h.s().actionsStatus(h.now).dragging, "precondition");
        menu(h, true);
        qt(h, 100);
        require(h.s().overlaySelect(PaletteTarget::Drop, h.now, gEpoch), "Drop refused");
        require(std::strcmp(mode(h), "LEFT") == 0 && h.s().actionsStatus(h.now).drops == 1, "Drop");
        require(h.released(), "button still held after Drop");
        // Cancel clears a chosen one-shot and an armed scroll
        require(h.s().overlaySelect(PaletteTarget::Right, h.now, gEpoch), "Right");
        require(h.s().overlaySelect(PaletteTarget::Cancel, h.now, gEpoch), "Cancel refused");
        require(std::strcmp(mode(h), "LEFT") == 0, "Cancel kept the action");
        require(h.s().overlaySelect(PaletteTarget::Scroll, h.now, gEpoch), "Scroll");
        require(std::strcmp(h.s().actionsStatus(h.now).scroll, "ARMED") == 0, "scroll not armed");
        require(h.s().overlaySelect(PaletteTarget::Cancel, h.now, gEpoch), "Cancel refused");
        require(std::strcmp(h.s().actionsStatus(h.now).scroll, "OFF") == 0, "Cancel kept the armed scroll");
    });

    test("overlay: one-shot Right and Double return to Left; keep mode holds them; Cancel clears", [] {
        HF h = overlayRig();
        require(ovSelect(h, PaletteTarget::Right), "Right");
        moveAway(h);
        qt(h, 1900);
        require(std::strcmp(mode(h), "LEFT") == 0, "Right was not one-shot");
        require(h.s().setActionKeep(true), "keep refused");
        require(ovSelect(h, PaletteTarget::Double), "Double");
        const size_t m = mark(h);
        for (int i = 0; i < 2; ++i) {
            moveAway(h);
            qt(h, 1900);
            require(std::strcmp(mode(h), "DOUBLE") == 0, "Double did not stay selected in keep mode");
        }
        require(seen(h, m).primary == 4, "not two double-clicks");
        require(ovSelect(h, PaletteTarget::Cancel) && std::strcmp(mode(h), "LEFT") == 0, "Cancel with keep on");
    });

    test("overlay: a menu entry reported shortly after a target dwell completes cancels the click; late does not", [] {
        for (bool lateEntry : {false, true}) {
            HF h = overlayRig();
            moveAway(h);
            const size_t m = mark(h);
            for (unsigned i = 0; i < 400 && !h.s().actionsStatus(h.now).pending; ++i) {
                tk(h);
            }
            require(h.s().actionsStatus(h.now).pending, "no pending click");
            if (lateEntry) {
                qt(h, 400); // later than the 150 ms commit wait: the click is already out
            }
            menu(h, true);
            qt(h, 400);
            const bool clicked = seen(h, m).primary == 1;
            std::printf("   INFO overlay entry %s the commit wait: %s\n", lateEntry ? "AFTER" : "within",
                        clicked ? "click executed (residual race)" : "click cancelled");
            require(clicked == lateEntry, lateEntry ? "late entry did not behave as documented"
                                                      : "an in-time entry did not cancel the click");
        }
    });

    test("overlay: lost heartbeat inhibits actions, then switches the controls off and releases; no auto resume", [] {
        HF h = overlayRig();
        require(ovSelect(h, PaletteTarget::Drag), "drag select");
        moveAway(h);
        qt(h, 1900);
        require(h.s().actionsStatus(h.now).dragging, "precondition");
        gReporting = false; // the overlay process died or the bridge went away
        for (int i = 0; i < 300; ++i) { // 3 s: reports stale, target actions inhibited
            tk(h, {i % 40 < 20 ? 30.f : -30.f, 0, 0});
        }
        require(!h.s().actionsStatus(h.now).reporting, "still reporting");
        qt(h, 3500); // beyond the 5 s loss limit
        require(!h.s().actionPaletteEnabled(), "the overlay controls stayed on after the loss");
        require(h.released() && !h.s().actionsStatus(h.now).dragging, "the held button was not released");
        require(h.s().state == SystemState::Active, "the session was stopped (pointing must continue)");
        // a late heartbeat does NOT bring it back
        gReporting = true;
        require(!h.s().setOverlayMenu(false, h.now, gEpoch), "a late report revived the overlay controls");
        qt(h, 3000);
        const size_t m = mark(h);
        qt(h, 4000);
        require(seen(h, m).primary == 0 && seen(h, m).secondary == 0, "a click without the overlay controls");
        // only an explicit new claim resumes
        require(claimOverlay(h), "re-claim refused");
        require(std::strcmp(mode(h), "LEFT") == 0 && !h.s().actionsStatus(h.now).keep, "stale state after a re-claim");
    });

    test("overlay: Stop from the menu ends control at once (also with a drag held); refused off the menu", [] {
        HF h = overlayRig();
        require(!h.s().overlaySelect(PaletteTarget::Stop, h.now, gEpoch), "Stop accepted with the menu closed");
        require(ovSelect(h, PaletteTarget::Drag), "drag select");
        moveAway(h);
        qt(h, 1900);
        require(h.s().actionsStatus(h.now).dragging, "precondition");
        menu(h, true);
        require(h.s().overlaySelect(PaletteTarget::Stop, h.now, gEpoch), "Stop refused");
        require(h.s().state != SystemState::Active && h.released(), "not stopped and released");
        qt(h, 3000);
        require(h.s().state != SystemState::Active && h.released(), "restarted by itself");
    });

    test("overlay: website Stop and the physical button stop immediately with the overlay in control", [] {
        for (bool physical : {false, true}) {
            HF h = overlayRig();
            require(ovSelect(h, PaletteTarget::Drag), "drag select");
            moveAway(h);
            qt(h, 1900);
            require(h.s().actionsStatus(h.now).dragging, "precondition");
            if (physical) {
                h.click();
            } else {
                h.s().stopUncalibratedDemo("website stop");
            }
            require(h.s().state != SystemState::Active && h.released(), "not stopped at once");
            require(!h.s().actionPaletteEnabled(), "overlay controls survived the stop");
            qt(h, 2000);
            require(h.s().state != SystemState::Active && h.released(), "output after the stop");
        }
    });

    test("overlay: keyboard mode suppresses target dwell clicks, keeps pointing, and any action returns", [] {
        HF h = overlayRig();
        require(h.s().setOverlayKeyboard(true, gEpoch), "keyboard refused");
        require(h.s().actionsStatus(h.now).keyboard, "keyboard flag");
        moveAway(h);
        const size_t m = mark(h);
        qt(h, 6000); // a full target dwell: NodX must NOT click (the system keyboard has its own dwell)
        require(seen(h, m).primary == 0 && seen(h, m).secondary == 0, "NodX clicked during keyboard mode");
        require(h.s().actionsStatus(h.now).keyboard, "keyboard mode ended by itself");
        const size_t mv = mark(h);
        moveUnits(h, 20.f, 12.f);
        require(seen(h, mv).dx != 0, "pointing stopped during keyboard mode");
        // the menu still works and selecting an action returns to ordinary control
        menu(h, true);
        qt(h, 50);
        require(h.s().overlaySelect(PaletteTarget::Left, h.now, gEpoch), "menu selection refused in keyboard mode");
        menu(h, false);
        require(!h.s().actionsStatus(h.now).keyboard, "keyboard mode survived selecting an action");
        moveAway(h);
        const size_t after = mark(h);
        qt(h, 1900);
        require(seen(h, after).primary == 1, "no click after returning to ordinary control");
        // keyboard needs the overlay controller and ends with the session
        require(h.s().setOverlayKeyboard(true, gEpoch), "keyboard again");
        h.s().stopUncalibratedDemo("test");
        require(!h.s().setOverlayKeyboard(true, gEpoch), "keyboard accepted without a session");
    });

    test("overlay: a failed entry release inhibits output; reconnect never resumes", [] {
        HF h = overlayRig();
        require(ovSelect(h, PaletteTarget::Drag), "drag select");
        moveAway(h);
        qt(h, 1900);
        require(h.s().actionsStatus(h.now).dragging, "precondition");
        h.transport.failRelease = true;
        const uint32_t faults = h.s().diagnostics.faults;
        menu(h, true);
        for (int i = 0; i < 5; ++i) {
            tk(h);
        }
        require(h.s().diagnostics.faults == faults + 1 && h.s().state == SystemState::SafeState,
                "a failed release did not inhibit output");
        h.transport.failRelease = false;
        qt(h, 4000);
        require(h.s().state != SystemState::Active && !h.s().actionPaletteEnabled(), "resumed by itself");
        // a disconnect while the overlay controls: nothing acts or resumes afterwards
        HF d = overlayRig();
        d.transport.online = false;
        qt(d, 400);
        d.transport.online = true;
        const size_t m = mark(d);
        qt(d, 5000);
        require(d.s().state != SystemState::Active && seen(d, m).primary == 0, "acted after a disconnect");
    });

    test("overlay: status fields and JSON fit; overlay controls do not touch profiles", [] {
        HF h = overlayRig(true);
        UserProfile before, after;
        require(h.repo.load(before), "profile");
        menu(h, true);
        tk(h);
        char buffer[actionsJsonCapacity];
        const size_t length = actionsJson(buffer, sizeof buffer, h.s().actionsStatus(h.now));
        require(length > 200 && length < actionsJsonCapacity - 64, "JSON too large");
        require(std::strstr(buffer, "\"controller\":\"OVERLAY\"") && std::strstr(buffer, "\"menu\":true") &&
                    std::strstr(buffer, "\"ready\":true") && std::strstr(buffer, "\"keyboard\":false"),
                "overlay fields missing");
        h.s().stopUncalibratedDemo("done");
        require(h.repo.load(after) && before.dwellMs == after.dwellMs && before.gain[0] == after.gain[0],
                "the profile changed");
    });

    // ================= stale overlay commands (old session, timeout, reconnect) =================
    test("stale overlay: after Stop and a NEW session, an old claim, menu, select and keyboard are refused", [] {
        HF h = overlayRig();
        const uint32_t oldSession = h.s().sessionSerial();
        const uint32_t oldEpoch = gEpoch;
        require(ovSelect(h, PaletteTarget::Right), "select Right");
        menu(h, true);
        h.s().stopUncalibratedDemo("website stop"); // Stop: the old overlay's commands may still be queued
        qt(h, 400);
        require(h.s().startUncalibratedDemo(h.now), "an explicit new session");
        qt(h, 300);
        require(h.s().sessionSerial() == oldSession + 1, "the session serial did not advance");
        const auto before = h.s().actionsStatus(h.now);
        require(!h.s().actionPaletteEnabled() && std::strcmp(before.controller, "NONE") == 0, "palette on");
        // delayed commands from the OLD overlay session arrive now
        require(!h.s().setActionOverlay(true, h.now, oldSession, oldEpoch), "an old claim (old session) reclaimed");
        require(!h.s().setActionOverlay(true, h.now, oldSession, oldEpoch + 50), "an old-session claim with a higher epoch");
        require(!h.s().setOverlayMenu(true, h.now, oldEpoch), "an old menu report reopened the menu");
        require(!h.s().overlaySelect(PaletteTarget::Left, h.now, oldEpoch), "an old selection executed");
        require(!h.s().setOverlayKeyboard(true, oldEpoch), "an old keyboard command was accepted");
        require(!h.s().actionPaletteEnabled(), "the controls came back by themselves");
        // a claim of the CURRENT session with an epoch that is not newer is stale too
        require(!h.s().setActionOverlay(true, h.now, h.s().sessionSerial(), oldEpoch), "a non-newer epoch reclaimed");
        const size_t m = mark(h);
        qt(h, 4000);
        require(seen(h, m).primary == 0 && seen(h, m).secondary == 0, "an action ran from a stale command");
        // only a fresh claim of this session works, and the old epoch stays refused afterwards
        ++gEpoch;
        require(h.s().setActionOverlay(true, h.now, h.s().sessionSerial(), gEpoch), "the fresh claim was refused");
        gOverlay = true;
        menu(h, true);
        require(!h.s().overlaySelect(PaletteTarget::Right, h.now, oldEpoch), "old epoch accepted after a new claim");
        // an old menu report must not reopen or close the menu of the NEW claim, nor an old keyboard command act
        menu(h, false);
        qt(h, 50);
        require(!h.s().actionsStatus(h.now).menu, "precondition: menu closed");
        require(!h.s().setOverlayMenu(true, h.now, oldEpoch), "an old-epoch menu report was accepted");
        tk(h);
        require(!h.s().actionsStatus(h.now).menu, "an old menu report reopened the menu");
        require(!h.s().setOverlayKeyboard(true, oldEpoch), "an old-epoch keyboard command was accepted");
        require(!h.s().actionsStatus(h.now).keyboard, "an old keyboard command paused the clicks");
        menu(h, true);
        require(h.s().overlaySelect(PaletteTarget::Right, h.now, gEpoch), "the current epoch was refused");
    });

    test("stale overlay: after a timeout a queued claim, menu or selection cannot revive the controls", [] {
        HF h = overlayRig();
        const uint32_t session = h.s().sessionSerial();
        const uint32_t epoch = gEpoch;
        require(ovSelect(h, PaletteTarget::Drag), "drag select");
        moveAway(h);
        qt(h, 1900);
        require(h.s().actionsStatus(h.now).dragging, "precondition");
        gReporting = false; // the overlay stops reporting
        qt(h, 6500);        // beyond the 5 s limit: controls off, button released
        require(!h.s().actionPaletteEnabled() && h.released(), "the timeout did not switch the controls off");
        // everything the old overlay had queued arrives late, in the worst order
        require(!h.s().setActionOverlay(true, h.now, session, epoch), "a delayed claim (same epoch) reclaimed");
        require(!h.s().setOverlayMenu(true, h.now, epoch), "a delayed menu report reopened the menu");
        require(!h.s().overlaySelect(PaletteTarget::Cancel, h.now, epoch), "a delayed selection executed");
        require(!h.s().overlaySelect(PaletteTarget::Stop, h.now, epoch), "a delayed Stop executed");
        require(!h.s().setOverlayKeyboard(true, epoch), "a delayed keyboard command executed");
        require(h.s().state == SystemState::Active, "the session itself was disturbed by stale commands");
        require(!h.s().actionPaletteEnabled(), "the controls came back");
        // a new claim needs a higher epoch
        require(!h.s().setActionOverlay(true, h.now, session, epoch - 1), "a lower epoch reclaimed");
        ++gEpoch;
        require(h.s().setActionOverlay(true, h.now, session, gEpoch), "the explicit new claim was refused");
    });

    test("stale overlay: after a disconnect and reconnect no old command resumes anything", [] {
        HF h = overlayRig();
        const uint32_t session = h.s().sessionSerial();
        const uint32_t epoch = gEpoch;
        menu(h, true);
        h.transport.online = false;
        qt(h, 400);
        h.transport.online = true;
        qt(h, 3000);
        require(h.s().state != SystemState::Active && !h.s().actionPaletteEnabled(), "precondition");
        for (int i = 0; i < 3; ++i) { // the overlay's queue drains after the reconnect
            require(!h.s().setActionOverlay(true, h.now, session, epoch), "an old claim after the reconnect");
            require(!h.s().setOverlayMenu(true, h.now, epoch), "an old menu report after the reconnect");
            require(!h.s().overlaySelect(PaletteTarget::Left, h.now, epoch), "an old selection after the reconnect");
            qt(h, 100);
        }
        require(h.s().state != SystemState::Active, "the session resumed");
        // an explicit restart does not make the old claim valid either
        require(h.s().startUncalibratedDemo(h.now), "explicit restart");
        qt(h, 300);
        require(!h.s().setActionOverlay(true, h.now, session, epoch + 1), "a pre-restart claim reclaimed");
        require(!h.s().actionPaletteEnabled(), "controls on without a new overlay claim");
    });

    test("stale overlay: refused commands change nothing the user can see", [] {
        HF h = overlayRig();
        const uint32_t epoch = gEpoch;
        require(ovSelect(h, PaletteTarget::Double), "select Double");
        const auto before = h.s().actionsStatus(h.now);
        h.s().stopUncalibratedDemo("stop");
        require(h.s().startUncalibratedDemo(h.now), "restart");
        qt(h, 300);
        (void)h.s().overlaySelect(PaletteTarget::Right, h.now, epoch);
        (void)h.s().setOverlayMenu(true, h.now, epoch);
        const auto after = h.s().actionsStatus(h.now);
        require(std::strcmp(after.mode, "LEFT") == 0 && !after.menu && !after.enabled, "stale commands changed state");
        require(after.selections == 0 && after.session == before.session + 1, "counters or session wrong");
        require(after.epoch == 0, "an epoch is shown without a claim");
    });

    test("overlay tokens: status reports the session serial and the claim epoch", [] {
        HF h = overlayRig();
        const auto st = h.s().actionsStatus(h.now);
        require(st.session == h.s().sessionSerial() && st.session >= 1, "session not reported");
        require(st.epoch == gEpoch, "epoch not reported");
        char buffer[actionsJsonCapacity];
        require(actionsJson(buffer, sizeof buffer, st) > 0, "json");
        require(std::strstr(buffer, "\"session\":") && std::strstr(buffer, "\"epoch\":"), "fields missing in the JSON");
    });

    std::printf("%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
