#include "nodx/actions.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace nodx {
const char* name(ActionMode mode) {
    switch (mode) {
    case ActionMode::Left:
        return "LEFT";
    case ActionMode::Right:
        return "RIGHT";
    case ActionMode::Double:
        return "DOUBLE";
    case ActionMode::Drag:
        return "DRAG";
    case ActionMode::Scroll:
        return "SCROLL";
    }
    return "LEFT";
}
const char* name(PaletteTarget target) {
    switch (target) {
    case PaletteTarget::None:
        return "NONE";
    case PaletteTarget::Left:
        return "LEFT";
    case PaletteTarget::Right:
        return "RIGHT";
    case PaletteTarget::Double:
        return "DOUBLE";
    case PaletteTarget::Drag:
        return "DRAG";
    case PaletteTarget::Scroll:
        return "SCROLL";
    case PaletteTarget::Stop:
        return "STOP";
    case PaletteTarget::Frame:
        return "FRAME";
    }
    return "NONE";
}
const char* name(ScrollPhase phase) {
    switch (phase) {
    case ScrollPhase::Off:
        return "OFF";
    case ScrollPhase::Armed:
        return "ARMED";
    case ScrollPhase::Active:
        return "ACTIVE";
    }
    return "OFF";
}
bool parsePaletteTarget(const char* text, PaletteTarget& out) {
    static const PaletteTarget all[] = {PaletteTarget::None,   PaletteTarget::Left,
                                        PaletteTarget::Right,  PaletteTarget::Double,
                                        PaletteTarget::Drag,   PaletteTarget::Scroll,
                                        PaletteTarget::Stop,   PaletteTarget::Frame};
    for (const PaletteTarget target : all) {
        const char* label = name(target);
        size_t i = 0;
        while (label[i] && text[i] && (text[i] | 0x20) == (label[i] | 0x20)) {
            ++i;
        }
        if (!label[i] && !text[i]) {
            out = target;
            return true;
        }
    }
    return false;
}

void ActionPalette::reset() {
    mode_ = ActionMode::Left;
    scroll_ = ScrollPhase::Off;
    dragging_ = false;
    hover_ = PaletteTarget::None;
    everReported_ = false; // a new session has no palette report yet: nothing acts until one arrives
    pending_ = false;
}
void ActionPalette::clearCounters() {
    last = PaletteTarget::None;
    selections = left = right = doubles = dragStarts = dragReleases = scrollStarts = scrollExits = 0;
    inhibited = cancelled = paletteReleases = 0;
}
bool ActionPalette::reporting(uint32_t now) const {
    return everReported_ && uint32_t(now - hoverAt_) <= start::actionReportFreshMs;
}
uint32_t ActionPalette::reportAge(uint32_t now) const {
    return everReported_ ? uint32_t(now - hoverAt_) : 0;
}
PaletteTarget ActionPalette::hover(uint32_t now) const {
    return reporting(now) ? hover_ : PaletteTarget::None; // a stale report never keeps the palette "occupied"
}
bool ActionPalette::setHover(PaletteTarget target, uint32_t now) {
    const PaletteTarget before = hover(now);
    hover_ = target;
    hoverAt_ = now;
    everReported_ = true;
    return before != target;
}
float ActionPalette::exitProgress(uint32_t now, uint32_t dwellMs) const {
    if (scroll_ != ScrollPhase::Active || dwellMs == 0) {
        return 0;
    }
    const float p = float(uint32_t(now - neutralSince_)) / float(dwellMs);
    return p < 0 ? 0 : p > 1 ? 1 : p;
}
void ActionPalette::select(PaletteTarget target) {
    dragging_ = false; // choosing another action releases a held button (the release is sent at once)
    scroll_ = ScrollPhase::Off;
    switch (target) {
    case PaletteTarget::Left:
        mode_ = ActionMode::Left;
        break;
    case PaletteTarget::Right:
        mode_ = ActionMode::Right;
        break;
    case PaletteTarget::Double:
        mode_ = ActionMode::Double;
        break;
    case PaletteTarget::Drag:
        mode_ = ActionMode::Drag;
        break;
    case PaletteTarget::Scroll:
        mode_ = ActionMode::Scroll;
        scroll_ = ScrollPhase::Armed; // the pointer is still free: dwell on the content to begin
        break;
    default:
        break;
    }
}
// A target dwell that survived the commit wait: now it acts.
void ActionPalette::execute(ActionOutput& out, uint32_t now) {
    switch (mode_) {
    case ActionMode::Left:
        out.pulse = true;
        ++left;
        break;
    case ActionMode::Right:
        out.pulse = out.right = true;
        ++right;
        mode_ = ActionMode::Left;
        break;
    case ActionMode::Double:
        out.pulse = out.twice = true;
        ++doubles;
        mode_ = ActionMode::Left;
        break;
    case ActionMode::Drag:
        dragging_ = true;
        ++dragStarts;
        // The release dwell may start where the press happened: standing still releases the button, so a
        // drag that was never started can always be ended without moving.
        out.resetDwell = true;
        break;
    case ActionMode::Scroll:
        if (scroll_ == ScrollPhase::Armed) {
            scroll_ = ScrollPhase::Active;
            neutralSince_ = now;
            ++scrollStarts;
            out.freezePointer = true;
            out.resetDwell = true; // this dwell started scrolling; it is not a click
        }
        break;
    }
}
ActionOutput ActionPalette::update(const ActionInput& in, uint32_t now) {
    ActionOutput out;
    if (scroll_ == ScrollPhase::Active) {
        // Pointer frozen. Vertical head movement beyond the neutral zone drives the wheel; staying
        // inside the neutral zone for the dwell duration leaves scroll (the Exit control).
        out.freezePointer = true;
        const float v = in.verticalRate;
        const float magnitude = std::fabs(v);
        const bool still = std::hypot(v, in.lateralRate) <= start::actionScrollNeutral;
        if (magnitude > start::actionScrollNeutral) {
            // head down (pointer would go down) scrolls down: a negative wheel in the HID convention
            out.wheel = -std::copysign((magnitude - start::actionScrollNeutral) *
                                           start::actionScrollGain * in.dt,
                                       v);
            neutralSince_ = now;
        } else if (!still) {
            neutralSince_ = now; // sideways movement is not stillness: nothing is scrolled, no exit
        } else if (uint32_t(now - neutralSince_) >= in.dwellMs) {
            scroll_ = ScrollPhase::Off;
            mode_ = ActionMode::Left;
            ++scrollExits;
            out.freezePointer = false;
            out.lockAfter = true; // deliberate movement before the next click
            out.resetDwell = true;
        }
        return out;
    }
    const bool fresh = reporting(now);
    const PaletteTarget over = hover(now);
    if (over != PaletteTarget::None) {
        // Entering the palette inhibits every target action before any selection can begin.
        if (pending_) {
            pending_ = false;
            ++cancelled;
        }
        if (dragging_) {
            // The held button is released first; a selection is only possible from a later, fresh dwell.
            dragging_ = false;
            ++dragReleases;
            ++paletteReleases;
            out.resetDwell = true;
            out.down = false;
            return out;
        }
        if (in.dwellPulse) {
            // Selecting a control is NOT a click: the dwell pulse is consumed here. On the bare palette
            // window (Frame) it is consumed and selects nothing.
            if (over != PaletteTarget::Frame) {
                last = over;
                ++selections;
                if (over == PaletteTarget::Stop) {
                    out.stop = true;
                } else {
                    select(over);
                }
            }
            out.lockAfter = true; // no chained selection without deliberate movement
        }
        out.down = dragging_;
        return out;
    }
    if (dragging_ && in.dwellPulse) {
        // Releasing a held button is always allowed (the safe direction), even if no report is fresh.
        dragging_ = false;
        ++dragReleases;
        out.lockAfter = true; // move deliberately before the next press can arm
        out.down = false;
        return out;
    }
    if (!fresh) {
        // No trustworthy word on where the pointer is: nothing acts on a target.
        if (in.dwellPulse) {
            ++inhibited;
        }
        if (pending_) {
            pending_ = false;
            ++cancelled;
        }
        out.down = dragging_;
        return out;
    }
    if (pending_) {
        const float moved = std::hypot(in.x - pendingX_, in.y - pendingY_);
        if (moved > in.tolerance) {
            pending_ = false; // the pointer left the target while the click waited
            ++cancelled;
        } else if (uint32_t(now - pendingSince_) >= start::actionCommitMs) {
            pending_ = false;
            execute(out, now);
        }
    } else if (in.dwellPulse) {
        pending_ = true; // wait out the commit time: a late palette entry can still cancel it
        pendingSince_ = now;
        pendingX_ = in.x;
        pendingY_ = in.y;
    }
    out.down = dragging_;
    return out;
}

size_t actionsJson(char* out, size_t capacity, const ActionsStatus& s) {
    const int written = std::snprintf(
        out, capacity,
        "{\"enabled\":%s,\"mode\":\"%s\",\"dragging\":%s,\"scroll\":\"%s\",\"frozen\":%s,"
        "\"hover\":\"%s\",\"inPalette\":%s,\"last\":\"%s\",\"selections\":%lu,"
        "\"dwellMs\":%lu,\"tolerance\":%.1f,\"neutral\":%.1f,"
        "\"dwell\":{\"state\":\"%s\",\"progress\":%.3f},\"exit\":{\"progress\":%.3f},"
        "\"counts\":{\"left\":%lu,\"right\":%lu,\"double\":%lu,\"dragStart\":%lu,"
        "\"dragRelease\":%lu,\"scrollStart\":%lu,\"scrollExit\":%lu,\"wheel\":%lu},"
        "\"link\":{\"reporting\":%s,\"ageMs\":%lu,\"pending\":%s,\"commitMs\":%lu,"
        "\"inhibited\":%lu,\"cancelled\":%lu,\"paletteReleases\":%lu},"
        "\"blocked\":\"%s\"}",
        s.enabled ? "true" : "false", s.mode, s.dragging ? "true" : "false", s.scroll,
        s.frozen ? "true" : "false", s.hover, s.inPalette ? "true" : "false", s.last,
        static_cast<unsigned long>(s.selections), static_cast<unsigned long>(s.dwellMs),
        s.tolerance, s.neutral, s.dwellState, s.dwellProgress, s.exitProgress,
        static_cast<unsigned long>(s.left), static_cast<unsigned long>(s.right),
        static_cast<unsigned long>(s.doubles), static_cast<unsigned long>(s.dragStarts),
        static_cast<unsigned long>(s.dragReleases), static_cast<unsigned long>(s.scrollStarts),
        static_cast<unsigned long>(s.scrollExits), static_cast<unsigned long>(s.wheelUnits),
        s.reporting ? "true" : "false", static_cast<unsigned long>(s.reportAgeMs),
        s.pending ? "true" : "false", static_cast<unsigned long>(s.commitMs),
        static_cast<unsigned long>(s.inhibited), static_cast<unsigned long>(s.cancelled),
        static_cast<unsigned long>(s.paletteReleases), s.blocked);
    return written > 0 && size_t(written) < capacity ? size_t(written) : 0;
}
} // namespace nodx
