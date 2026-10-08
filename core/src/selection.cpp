#include "nodx/selection.hpp"
#include <algorithm>
#include <cmath>

namespace nodx {
const char* name(DwellState s) {
    switch (s) {
    case DwellState::Idle:
        return "IDLE";
    case DwellState::Arming:
        return "ARMING";
    case DwellState::Progress:
        return "PROGRESS";
    case DwellState::Click:
        return "CLICK";
    case DwellState::Lockout:
        return "LOCKOUT";
    }
    return "IDLE";
}
bool DebouncedSwitch::update(bool raw, uint32_t now) {
    if (raw != raw_) {
        raw_ = raw;
        changed_ = now;
    }
    if (uint32_t(now - changed_) >= start::debounceMs) {
        stable_ = raw_;
    }
    return stable_;
}
void DebouncedSwitch::reset(bool raw, uint32_t now) {
    raw_ = raw;
    stable_ = false;
    changed_ = now;
}
void SelectionManager::reset(bool raw, uint32_t now) {
    if (dwell == DwellState::Arming || dwell == DwellState::Progress) {
        ++cancellations;
    }
    dwell = DwellState::Idle;
    switch_.reset(raw, now);
    requireRelease_ = raw;
}
float SelectionManager::progress(uint32_t now, const UserProfile& p) const {
    if (dwell != DwellState::Progress) {
        return 0;
    }
    return std::min(1.f, float(uint32_t(now - since_)) / p.dwellMs);
}
Selection SelectionManager::update(bool raw, double x, double y, bool allowed, bool scrolling,
                                   const UserProfile& p, uint32_t now) {
    if (!allowed) {
        reset(raw, now);
        return {};
    }
    bool pressed = switch_.update(raw, now);
    if (requireRelease_) {
        if (!raw && !pressed) {
            requireRelease_ = false;
        }
        return {};
    }
    if (raw || pressed || scrolling || !p.dwellEnabled) {
        if (dwell == DwellState::Arming || dwell == DwellState::Progress) {
            ++cancellations;
        }
        dwell = DwellState::Idle;
        return {pressed, false};
    }
    double distance = std::hypot(x - anchorX_, y - anchorY_);
    if (dwell == DwellState::Click) {
        dwell = DwellState::Lockout;
    }
    if (dwell == DwellState::Lockout) {
        if (distance > p.dwellTolerance * 1.5f) {
            dwell = DwellState::Idle;
        }
        return {};
    }
    if (dwell != DwellState::Idle && distance > p.dwellTolerance) {
        ++cancellations;
        dwell = DwellState::Idle;
        return {};
    }
    if (dwell == DwellState::Idle) {
        anchorX_ = x;
        anchorY_ = y;
        since_ = now;
        dwell = DwellState::Arming;
    } else if (dwell == DwellState::Arming && uint32_t(now - since_) >= start::armMs) {
        dwell = DwellState::Progress;
        since_ = now;
    } else if (dwell == DwellState::Progress && uint32_t(now - since_) >= p.dwellMs) {
        dwell = DwellState::Click;
        return {false, true};
    }
    return {};
}
} // namespace nodx
