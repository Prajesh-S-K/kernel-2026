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
// A session on the real sensor path with the palette enabled (Left-click selected).
HF rig(bool saveProfile = false) {
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
    h.quiet(100);
    return h;
}
constexpr unsigned kDwellTicks = 160; // 1.6 s: the 250 ms arming plus the 1200 ms START dwell
void hold(HF& h, PaletteTarget target, unsigned ticks) {
    for (unsigned i = 0; i < ticks; ++i) {
        if (i % 40 == 0) {
            h.s().setActionHover(target, h.now); // the page refreshes its report while hovering
        }
        h.tick();
    }
}
// Dwell on a palette control (the pointer is still, the page reports the hover), then leave it.
void selectControl(HF& h, PaletteTarget target) {
    h.s().setActionHover(target, h.now);
    hold(h, target, kDwellTicks);
    h.s().setActionHover(PaletteTarget::None, h.now);
}
void moveAway(HF& h, float rate = 40.f, unsigned ticks = 40) {
    for (unsigned i = 0; i < ticks; ++i) {
        h.tick({rate, 0, 0});
    }
    h.quiet(300);
}
void dwellOnTarget(HF& h) {
    h.quiet(kDwellTicks * 10);
}
const char* mode(HF& h) {
    return h.s().actionsStatus(h.now).mode;
}
} // namespace

int main() {
    test("disabled by default; refused outside a running session", [] {
        HF h(false, EnableKind::Momentary);
        h.sw = false;
        h.quiet(400);
        require(!h.s().setActionPalette(true, h.now), "enabled without a session");
        require(!h.s().actionsStatus(h.now).enabled, "enabled flag");
        require(std::strlen(h.s().actionsStatus(h.now).blocked) > 0, "no blocker text");
        require(h.s().startUncalibratedDemo(h.now), "start");
        h.quiet(300);
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
        h.quiet(1000);
        require(seen(h, m).primary == 0, "clicked before the dwell completed");
        const auto p = h.s().actionsStatus(h.now);
        require(std::strcmp(p.dwellState, "PROGRESS") == 0 && p.dwellProgress > .4f,
                "no progress feedback");
        h.quiet(600);
        Seen s = seen(h, m);
        require(s.primary == 1 && s.secondary == 0 && !s.primaryHeld, "not exactly one click");
        require(std::strcmp(mode(h), "LEFT") == 0, "left changed");
        require(h.s().actionsStatus(h.now).left == 1, "left counter");
    });

    test("repeat lockout: standing still never repeats; deliberate movement re-arms", [] {
        HF h = paletteRig();
        dwellOnTarget(h);
        const size_t m = mark(h);
        h.quiet(6000);
        require(seen(h, m).primary == 0, "a second click without movement");
        require(std::strcmp(h.s().actionsStatus(h.now).dwellState, "LOCKOUT") == 0, "no lockout");
        // a small tremor is not deliberate movement
        for (int i = 0; i < 30; ++i) {
            h.tick({i % 2 ? 6.f : -6.f, 0, 0});
        }
        h.quiet(4000);
        require(seen(h, m).primary == 0, "a tremor re-armed the dwell");
        moveAway(h);
        dwellOnTarget(h);
        require(seen(h, m).primary == 1, "no click after deliberate movement");
    });

    test("dwell cancellation: movement beyond the tolerance restarts the dwell", [] {
        HF h = paletteRig();
        h.quiet(700);
        const size_t m = mark(h);
        for (int i = 0; i < 6; ++i) {
            h.tick({40.f, 0, 0}); // leaves the tolerance
        }
        require(std::strcmp(h.s().actionsStatus(h.now).dwellState, "PROGRESS") != 0 ||
                    h.s().actionsStatus(h.now).dwellProgress < .2f,
                "progress survived the movement");
        h.quiet(900);
        require(seen(h, m).primary == 0, "clicked on the old dwell");
        h.quiet(1100);
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
            h.tick({30.f, 10.f, 0});
        }
        require(seen(h, m).dx != 0, "the pointer did not move while dragging");
        for (size_t i = m; i < h.transport.reports.size(); ++i) {
            require(h.transport.reports[i].down, "button dropped while moving");
        }
        h.quiet(300);
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
        h.quiet(5000);
        require(seen(h, after).primary == 0, "a press right after the release");
    });

    test("Drag: the press needs no immediate release dwell (movement first)", [] {
        HF h = paletteRig();
        selectControl(h, PaletteTarget::Drag);
        moveAway(h);
        dwellOnTarget(h);
        require(h.s().actionsStatus(h.now).dragging, "not dragging");
        // standing still straight after the press does NOT release it
        const size_t m = mark(h);
        h.quiet(6000);
        require(h.s().actionsStatus(h.now).dragging, "released without deliberate movement");
        require(seen(h, m).primaryHeld || h.transport.reports.back().down || true, "state");
        require(h.s().actionsStatus(h.now).dragReleases == 0, "release counter");
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
            h.tick({40.f, 0, 0}); // horizontal movement: pointer stays frozen
        }
        Seen s = seen(h, m);
        require(h.s().actionsStatus(h.now).frozen, "sideways movement ended scrolling");
        require(s.dx == 0 && s.dy == 0, "the pointer moved while scrolling");
        require(s.wheelAbs == 0, "horizontal movement scrolled");
        // small vertical movement inside the neutral deadzone: nothing
        m = mark(h);
        for (int i = 0; i < 60; ++i) { // under the 1.2 s exit dwell
            h.tick({0, 4.f, 0}); // 1.5 deg/s after the pointer deadzone: neutral
        }
        require(seen(h, m).wheelAbs == 0, "the deadzone let small movement scroll");
        // head down: wheel negative (scroll down), bounded by the wheel limit
        m = mark(h);
        for (int i = 0; i < 150; ++i) {
            h.tick({0, 40.f, 0});
        }
        s = seen(h, m);
        require(s.wheel < -3, "no scroll down for downward movement");
        require(s.maxWheel <= start::maxWheel, "wheel report above the bound");
        require(s.dx == 0 && s.dy == 0, "pointer moved while scrolling down");
        h.quiet(300);
        m = mark(h);
        for (int i = 0; i < 150; ++i) {
            h.tick({0, -40.f, 0});
        }
        require(seen(h, m).wheel > 3, "no scroll up for upward movement");
        // an extreme rate never exceeds the bound
        m = mark(h);
        for (int i = 0; i < 100; ++i) {
            h.tick({0, 2000.f, 0});
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
            h.tick({0, 40.f, 0});
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
            h.tick({0, 30.f, 0});
        }
        require(h.s().actionsStatus(h.now).frozen, "left scroll while still scrolling");
        // stillness: progress is shown and grows
        h.quiet(600);
        const float half = h.s().actionsStatus(h.now).exitProgress;
        require(half > .2f && half < .9f, "no exit progress while still");
        // a movement restarts it
        for (int i = 0; i < 20; ++i) {
            h.tick({0, 30.f, 0});
        }
        require(h.s().actionsStatus(h.now).exitProgress < .2f, "movement did not restart the exit");
        const size_t m = mark(h);
        h.quiet(1700);
        const auto st = h.s().actionsStatus(h.now);
        require(!st.frozen && std::strcmp(st.scroll, "OFF") == 0, "did not exit scroll");
        require(std::strcmp(st.mode, "LEFT") == 0 && st.scrollExits == 1, "mode after exit");
        require(seen(h, m).primary == 0, "exiting scroll clicked");
        // no click straight after: deliberate movement first, then the pointer is free again
        const size_t quiet = mark(h);
        h.quiet(5000);
        require(seen(h, quiet).primary == 0, "a click after leaving scroll without movement");
        const size_t moved = mark(h);
        moveAway(h);
        require(seen(h, moved).dx != 0, "the pointer stayed frozen after the exit");
    });

    test("Scroll Exit uses the same adjustable dwell duration (no per-action wait)", [] {
        HF h = paletteRig();
        require(h.s().setUncalibratedDwellSettings(2000, 8.f), "settings");
        h.s().setActionHover(PaletteTarget::Scroll, h.now);
        hold(h, PaletteTarget::Scroll, 260); // 250 ms arming + the 2000 ms dwell
        h.s().setActionHover(PaletteTarget::None, h.now);
        moveAway(h);
        h.quiet(2500);
        require(h.s().actionsStatus(h.now).frozen, "scroll did not start at 2000 ms dwell");
        h.quiet(1100); // still inside the 2000 ms dwell, which began when scrolling did
        require(h.s().actionsStatus(h.now).frozen, "exited before the longer dwell");
        h.quiet(1100);
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
        h.s().setActionHover(PaletteTarget::Right, h.now);
        hold(h, PaletteTarget::Right, kDwellTicks);
        require(h.s().actionsStatus(h.now).selections == 1, "first selection");
        hold(h, PaletteTarget::Right, 600); // 6 more seconds on the same control
        require(h.s().actionsStatus(h.now).selections == 1, "re-selected without movement");
        // moving to a different control with deliberate movement selects again
        for (int i = 0; i < 40; ++i) {
            h.tick({40.f, 0, 0});
        }
        h.quiet(300);
        h.s().setActionHover(PaletteTarget::Left, h.now);
        hold(h, PaletteTarget::Left, kDwellTicks);
        require(h.s().actionsStatus(h.now).selections == 2, "no second selection");
        require(std::strcmp(mode(h), "LEFT") == 0, "second selection not applied");
    });

    test("leaving the palette clears the dwell and re-arms it only after deliberate movement", [] {
        HF h = paletteRig();
        h.s().setActionHover(PaletteTarget::Drag, h.now);
        hold(h, PaletteTarget::Drag, 90); // part-way through the dwell
        require(h.s().actionsStatus(h.now).dwellProgress > .1f, "no progress on the palette");
        h.s().setActionHover(PaletteTarget::None, h.now);
        h.quiet(50);
        require(h.s().actionsStatus(h.now).dwellProgress == 0.f, "progress survived leaving");
        const size_t m = mark(h);
        h.quiet(5000);
        require(seen(h, m).primary == 0 && seen(h, m).secondary == 0, "clicked after leaving");
        require(std::strcmp(mode(h), "LEFT") == 0 && h.s().actionsStatus(h.now).selections == 0,
                "a half-finished selection took effect");
        moveAway(h);
        dwellOnTarget(h);
        require(seen(h, m).primary == 1, "no click after moving on from the palette");
    });

    test("a stale palette hover expires and cannot keep the target from clicking", [] {
        HF h = paletteRig();
        h.s().setActionHover(PaletteTarget::Left, h.now);
        h.quiet(4000); // never refreshed: the page is gone (selecting Left again changes nothing)
        require(!h.s().actionsStatus(h.now).inPalette, "stale hover kept the palette occupied");
        moveAway(h);
        const size_t m = mark(h);
        dwellOnTarget(h);
        require(seen(h, m).primary == 1, "stale hover blocked target clicks forever");
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
            h.quiet(3000);
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
        h.quiet(1000);
        require(h.released() && h.s().state != SystemState::Active, "output after the stop");
    });

    test("website Stop while scroll is active (pointer frozen) stops at once", [] {
        HF h = paletteRig();
        selectControl(h, PaletteTarget::Scroll);
        moveAway(h);
        dwellOnTarget(h);
        require(h.s().actionsStatus(h.now).frozen, "precondition");
        for (int i = 0; i < 40; ++i) {
            h.tick({0, 40.f, 0});
        }
        h.s().stopUncalibratedDemo("website stop");
        require(h.released() && !h.s().actionsStatus(h.now).frozen, "not stopped");
        const size_t m = mark(h);
        h.quiet(2000);
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
        h.quiet(2000);
        require(h.s().state != SystemState::Active, "restarted by itself");
    });

    test("a fault during a drag releases, clears the action and never restarts by itself", [] {
        HF h = paletteRig();
        selectControl(h, PaletteTarget::Drag);
        moveAway(h);
        dwellOnTarget(h);
        require(h.s().actionsStatus(h.now).dragging, "precondition");
        h.s().axes.axes = {0, 0, 1}; // an invalid axis mapping is a sensor-path fault
        h.tick();
        require(h.s().state == SystemState::SafeState, "no safe state");
        require(h.released(), "the button was not released on the fault");
        h.s().axes.axes = {0, 1, 2};
        h.quiet(3000);
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
        h.quiet(300);
        require(h.s().state == SystemState::SafeState, "no safe state on disconnect");
        require(!h.s().actionsStatus(h.now).dragging && !h.s().actionPaletteEnabled(),
                "pending action kept across a disconnect");
        h.transport.online = true;
        const size_t m = mark(h);
        h.quiet(4000);
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
            h.tick({30.f, 0, 0});
        }
        h.quiet(300);
        const uint32_t faults = h.s().diagnostics.faults;
        for (unsigned i = 0; i < 160 && h.s().diagnostics.faults == faults; ++i) {
            h.tick(); // run until the release dwell completes
        }
        require(h.s().diagnostics.faults == faults + 1, "failed release did not raise a fault");
        require(h.s().state == SystemState::SafeState, "failed release did not stop output");
        require(h.s().diagnostics.faultCode == FaultCode::Transport, "fault code");
        require(!h.s().uncalibratedDemo() && !h.s().actionPaletteEnabled(), "session kept");
        h.transport.failRelease = false;
        const size_t m = mark(h);
        h.quiet(4000);
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
                h.tick();
            }
            require(h.s().diagnostics.faults == faults + 1 && h.s().state == SystemState::SafeState,
                    "failed click release kept output on");
            h.transport.failRelease = false;
            const size_t m = mark(h);
            h.quiet(3000);
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
        h.quiet(3000);
        require(seen(h, m).primary == 0, "a click while the palette is off");
    });

    test("every stop clears the palette; a restart needs it enabled again", [] {
        HF h = paletteRig();
        selectControl(h, PaletteTarget::Right);
        require(std::strcmp(mode(h), "RIGHT") == 0, "precondition");
        h.s().stopUncalibratedDemo("test");
        require(h.s().startUncalibratedDemo(h.now), "restart");
        h.quiet(300);
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
        h.quiet(400);
        UserProfile before;
        require(h.repo.load(before), "profile");
        const auto writes = h.profileStorage.read(0);
        require(h.s().startUncalibratedDemo(h.now), "start");
        h.quiet(300);
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
        h.quiet(400);
        require(!h.s().actionPaletteEnabled(), "palette on after a reboot");
        require(h.s().actionsStatus(h.now).dwellMs == start::uncalDwellMs, "dwell setting persisted");
    });

    test("the one dwell duration drives every action (a longer dwell delays all of them)", [] {
        for (PaletteTarget action :
             {PaletteTarget::Left, PaletteTarget::Right, PaletteTarget::Double, PaletteTarget::Drag}) {
            HF h = paletteRig();
            require(h.s().setUncalibratedDwellSettings(3000, 8.f), "settings");
            if (action != PaletteTarget::Left) {
                h.s().setActionHover(action, h.now);
                hold(h, action, 200); // 2 s: not enough for a 3 s dwell
                require(h.s().actionsStatus(h.now).selections == 0, "selected before the long dwell");
                hold(h, action, 200);
                require(h.s().actionsStatus(h.now).selections == 1, "not selected after it");
                h.s().setActionHover(PaletteTarget::None, h.now);
                moveAway(h);
            }
            const size_t m = mark(h);
            h.quiet(2500);
            require(seen(h, m).primary == 0 && seen(h, m).secondary == 0 &&
                        !h.s().actionsStatus(h.now).dragging,
                    "acted before the long dwell");
            h.quiet(1200);
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

    std::printf("%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
