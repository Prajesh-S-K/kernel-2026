#pragma once
#include "gesture.hpp"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace nodx::sim {
// Deterministic synthetic head-motion for simulators and tests. It is mathematical input,
// not recorded user data: half-sine strokes at a 10 ms sample period.
constexpr unsigned sampleMs = 10;

inline void neutral(std::vector<Rates>& out, unsigned ms) {
    out.insert(out.end(), ms / sampleMs, Rates{});
}
// One stroke on `axis` (0 yaw, 1 pitch, 2 roll) with sign +/-1.
inline void stroke(std::vector<Rates>& out, unsigned axis, int sign, float peak, unsigned ms) {
    const unsigned count = std::max(1u, ms / sampleMs);
    for (unsigned i = 0; i < count; ++i) {
        Rates rate{};
        rate[axis] = float(sign) * peak * std::sin(3.14159265f * (float(i) + .5f) / float(count));
        out.push_back(rate);
    }
}
// `cycles` repeats of a + stroke then a - stroke, separated by `gapMs` of stillness.
inline std::vector<Rates> cycles(unsigned axis, unsigned count, float peak, unsigned strokeMs,
                                 unsigned gapMs) {
    std::vector<Rates> out;
    for (unsigned cycle = 0; cycle < count; ++cycle) {
        for (int sign : {1, -1}) {
            stroke(out, axis, sign, peak, strokeMs);
            if (cycle + 1 < count || sign > 0) {
                neutral(out, gapMs);
            }
        }
    }
    return out;
}
// Names: nod1..nod3 (pitch), turn1..turn3 (yaw), tilt1..tilt3 (roll); the digit is the number
// of +/- cycles. `scale` multiplies the peak rate.
inline bool named(const std::string& name, float scale, std::vector<Rates>& out) {
    if (name.size() != 5 && name.size() != 4) {
        return false;
    }
    const std::string kind = name.substr(0, name.size() - 1);
    const char digit = name.back();
    unsigned axis;
    if (kind == "turn") {
        axis = 0;
    } else if (kind == "nod") {
        axis = 1;
    } else if (kind == "tilt") {
        axis = 2;
    } else {
        return false;
    }
    if (digit < '1' || digit > '3' || !std::isfinite(scale) || scale < .25f || scale > 3.f) {
        return false;
    }
    out = cycles(axis, unsigned(digit - '0'), 70.f * scale, 160, 20);
    return true;
}
} // namespace nodx::sim
