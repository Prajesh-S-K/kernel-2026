#pragma once
// Pure logic for the bench diagnostics (no Arduino includes, so it is unit tested on the host).
// None of this is a pass/fail judgement about hardware: it only turns raw observations into
// numbers.
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace nodx::diag {
// Welford running statistics; finite input only (non-finite samples are counted, not added).
class RunningStats {
public:
    void add(double value) {
        if (!std::isfinite(value)) {
            ++rejected_;
            return;
        }
        ++count_;
        const double delta = value - mean_;
        mean_ += delta / double(count_);
        m2_ += delta * (value - mean_);
        if (count_ == 1 || value < min_) {
            min_ = value;
        }
        if (count_ == 1 || value > max_) {
            max_ = value;
        }
    }
    uint32_t count() const {
        return count_;
    }
    uint32_t rejected() const {
        return rejected_;
    }
    double mean() const {
        return count_ ? mean_ : 0;
    }
    double stddev() const {
        return count_ > 1 ? std::sqrt(m2_ / double(count_ - 1)) : 0;
    }
    double min() const {
        return count_ ? min_ : 0;
    }
    double max() const {
        return count_ ? max_ : 0;
    }

private:
    uint32_t count_ = 0, rejected_ = 0;
    double mean_ = 0, m2_ = 0, min_ = 0, max_ = 0;
};

// One observed level change of the button pin.
struct Edge {
    uint32_t atUs = 0;
    bool level = false; // true = pin HIGH (button released with a pull-up)
};
// A burst is a run of edges whose neighbours are closer than `quietUs`: one physical press or
// release including its contact bounce.
struct Burst {
    uint32_t startUs = 0, spanUs = 0;
    unsigned edges = 0;
    bool settledLevel = false; // level after the last edge
};
// Edges must be in time order (wrap-safe differences). Returns the number of bursts written.
inline size_t groupBursts(const Edge* edges, size_t count, uint32_t quietUs, Burst* out,
                          size_t capacity) {
    size_t bursts = 0;
    size_t i = 0;
    while (i < count && bursts < capacity) {
        Burst burst;
        burst.startUs = edges[i].atUs;
        burst.edges = 1;
        burst.settledLevel = edges[i].level;
        size_t last = i;
        while (last + 1 < count && uint32_t(edges[last + 1].atUs - edges[last].atUs) < quietUs) {
            ++last;
            ++burst.edges;
            burst.settledLevel = edges[last].level;
        }
        burst.spanUs = uint32_t(edges[last].atUs - edges[i].atUs);
        out[bursts++] = burst;
        i = last + 1;
    }
    return bursts;
}
// Bounded edge recorder: never grows, counts what it had to drop.
template <size_t N> class EdgeLog {
public:
    void add(uint32_t atUs, bool level) {
        if (size_ < N) {
            edges_[size_++] = {atUs, level};
        } else {
            ++dropped_;
        }
    }
    const Edge* data() const {
        return edges_;
    }
    size_t size() const {
        return size_;
    }
    uint32_t dropped() const {
        return dropped_;
    }
    void clear() {
        size_ = 0;
        dropped_ = 0;
    }

private:
    Edge edges_[N];
    size_t size_ = 0;
    uint32_t dropped_ = 0;
};
} // namespace nodx::diag
