#pragma once
#include "profile.hpp"

namespace nodx {
class DebouncedSwitch {
public:
    bool update(bool raw, uint32_t now);
    void reset(bool raw, uint32_t now);
    bool pressed() const {
        return stable_;
    }

private:
    bool raw_ = false, stable_ = false;
    uint32_t changed_ = 0;
};
enum class DwellState { Idle, Arming, Progress, Click, Lockout };
const char* name(DwellState state);
struct Selection {
    bool down = false;
    bool pulse = false;
};
class SelectionManager {
public:
    Selection update(bool rawSwitch, double x, double y, bool allowed, bool scrolling,
                     const UserProfile& p, uint32_t now);
    void reset(bool rawSwitch, uint32_t now);
    DwellState dwell = DwellState::Idle;
    uint32_t cancellations = 0;
    float progress(uint32_t now, const UserProfile& p) const;

private:
    DebouncedSwitch switch_;
    bool requireRelease_ = false;
    double anchorX_ = 0, anchorY_ = 0;
    uint32_t since_ = 0;
};
} // namespace nodx
