#pragma once
// Dwell action palette (EXPERIMENTAL demo). The user picks ONE action from a palette shown by the
// companion beside the target application, using the same pointer dwell that clicks a target: one
// adjustable duration for everything, never a different wait per action. This class only decides what
// a completed dwell MEANS; the dwell itself is timed by the existing SelectionManager and every output
// still goes through SafetyManager -> HIDManager.
//
//   Left (default)  dwell on a target = one primary click
//   Right           one right click, then back to Left
//   Double          one double-click, then back to Left
//   Drag            dwell = press and hold the primary button, move, dwell again = release
//   Scroll          dwell on a target starts scrolling there: pointer output is frozen and vertical head
//                   movement drives the wheel. Leaving scroll: hold still for the dwell duration (the
//                   pointer cannot travel to a control while it is frozen, so the exit is dwell on
//                   stillness on BOTH axes, shown as a progress bar on the Exit control)
//   Drop            ends a drag by releasing the held button (no movement needed first); back to Left-click
//   Cancel          like Drop, and also clears everything pending (a waiting click, an armed one-shot or Scroll)
//   Stop            ends the session (also reachable by the website Stop and the physical button)
//
// One selection per hover: after a control is chosen the pointer must leave it (to another control, the
// palette background or the target) before that control can be chosen again. Entering the palette while
// a drag holds the button releases it first, and no selection is possible until that release is confirmed
// as delivered.
//
// While the pointer is over the palette (reported by the companion page) a completed dwell SELECTS a
// palette control and never becomes an OS click.
//
// What the device can and cannot know. The device never sees the screen: it learns that the pointer is over
// the palette only from reports sent by the companion page over a localhost link and a serial link, which can
// be late, lost, or stale. So the rules are fail-closed:
//   - a target action (click, double-click, right-click, drag press, scroll start) needs a FRESH report that
//     the pointer is outside the palette (the page repeats it); with no fresh report nothing acts at all;
//   - a completed target dwell is not executed at once: it waits actionCommitMs, and a palette entry reported
//     in that time cancels it (so a short report delay cannot leak a click);
//   - entering the palette while a drag holds the button releases it first, before any selection.
// Not guaranteed: a report delayed beyond the dwell plus the commit wait cannot be detected by the device,
// so a click can still reach the target in that case. The delay is measured, not assumed.
#include "parameters.hpp"
#include <cstddef>
#include <cstdint>

namespace nodx {
enum class ActionMode { Left, Right, Double, Drag, Scroll };
// Frame = the pointer is on the palette window but not on a control (background, margins): it inhibits every
// target action like a control does, and a completed dwell there selects nothing.
enum class PaletteTarget { None, Left, Right, Double, Drag, Scroll, Drop, Cancel, Stop, Frame };
enum class ScrollPhase { Off, Armed, Active };
const char* name(ActionMode mode);
const char* name(PaletteTarget target);
const char* name(ScrollPhase phase);
bool parsePaletteTarget(const char* text, PaletteTarget& out);

struct ActionInput {
    bool dwellPulse = false;   // a dwell the SelectionManager just completed
    float verticalRate = 0;    // mapped vertical head movement after the pointer deadzone, deg/s (+ = pointer down)
    float lateralRate = 0;     // mapped horizontal movement after the deadzone, deg/s (not still while it moves)
    float dt = 0.01f;          // seconds since the previous tick
    float x = 0, y = 0;        // accumulated outgoing pointer position (for cancelling a pending action)
    float tolerance = 8.f;     // the dwell movement tolerance
    uint32_t dwellMs = start::uncalDwellMs; // the one dwell duration (also used for leaving scroll)
};
struct ActionOutput {
    bool freezePointer = false; // pointer output must be zero this tick
    bool down = false;          // hold the primary button (drag)
    bool pulse = false, right = false, twice = false; // a click: primary, secondary, or double
    float wheel = 0;            // wheel units this tick (the HIDManager quantizes, the gate bounds)
    bool stop = false;          // the Stop control was selected
    bool lockAfter = false;     // require deliberate movement before the next dwell
    bool resetDwell = false;    // start a fresh dwell (the release dwell of a drag can start where it is)
};
struct ActionsStatus {
    bool enabled = false, dragging = false, inPalette = false, frozen = false;
    const char* mode = "LEFT";
    const char* scroll = "OFF";
    const char* hover = "NONE";
    const char* last = "NONE";
    const char* dwellState = "IDLE";
    const char* blocked = "";
    float dwellProgress = 0, exitProgress = 0, tolerance = 0, neutral = 0;
    uint32_t dwellMs = 0, selections = 0;
    uint32_t left = 0, right = 0, doubles = 0, dragStarts = 0, dragReleases = 0, scrollStarts = 0,
             scrollExits = 0, wheelUnits = 0;
    const char* locked = "NONE"; // the control that was just chosen and must be left before it can be chosen again
    uint32_t drops = 0, cancels = 0, confirmedReleases = 0;
    bool reporting = false;      // a fresh palette report exists (otherwise nothing acts on a target)
    bool pending = false;        // a completed target dwell is waiting out the commit time
    uint32_t reportAgeMs = 0, commitMs = 0, inhibited = 0, cancelled = 0, paletteReleases = 0;
};
size_t actionsJson(char* out, size_t capacity, const ActionsStatus& status);
constexpr size_t actionsJsonCapacity = 1024;

class ActionPalette {
public:
    // Everything cleared: Left mode, no held button, no scroll, no hover. Counters are kept until
    // clearCounters() so a demo can be read after it ended.
    void reset();
    void clearCounters();
    // Companion report: the pointer is over this control (None = outside the palette). The page repeats it.
    // Returns true when the usable hover state changed.
    bool setHover(PaletteTarget target, uint32_t now);
    // Where the pointer is according to a FRESH report; None when outside OR when no report is fresh.
    PaletteTarget hover(uint32_t now) const;
    bool reporting(uint32_t now) const; // a report (hover or outside) arrived recently enough to trust
    uint32_t reportAge(uint32_t now) const;
    bool pending() const {
        return pending_;
    }
    // A drag was released because the pointer entered the palette; selection stays blocked until the
    // System confirms that the release report was delivered.
    bool releaseUnconfirmed() const {
        return releaseUnconfirmed_;
    }
    void confirmRelease() {
        if (releaseUnconfirmed_) {
            releaseUnconfirmed_ = false;
            ++confirmedReleases;
        }
    }
    PaletteTarget lockedTarget() const {
        return lockedTarget_;
    }
    ActionOutput update(const ActionInput& in, uint32_t now);

    ActionMode mode() const {
        return mode_;
    }
    bool dragging() const {
        return dragging_;
    }
    ScrollPhase scroll() const {
        return scroll_;
    }
    bool scrollActive() const {
        return scroll_ == ScrollPhase::Active;
    }
    float exitProgress(uint32_t now, uint32_t dwellMs) const;
    PaletteTarget last = PaletteTarget::None;
    uint32_t selections = 0, left = 0, right = 0, doubles = 0, dragStarts = 0, dragReleases = 0,
             scrollStarts = 0, scrollExits = 0, inhibited = 0, cancelled = 0, paletteReleases = 0,
             drops = 0, cancels = 0, confirmedReleases = 0;

private:
    ActionMode mode_ = ActionMode::Left;
    ScrollPhase scroll_ = ScrollPhase::Off;
    bool dragging_ = false;
    PaletteTarget hover_ = PaletteTarget::None;
    uint32_t hoverAt_ = 0, neutralSince_ = 0, pendingSince_ = 0;
    bool everReported_ = false, pending_ = false, releaseUnconfirmed_ = false;
    PaletteTarget lockedTarget_ = PaletteTarget::None;
    float pendingX_ = 0, pendingY_ = 0;
    void select(PaletteTarget target);
    void execute(ActionOutput& out, uint32_t now);
};
} // namespace nodx
