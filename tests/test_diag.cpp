// Host tests of the bench-diagnostic helpers (statistics and button burst grouping). Synthetic
// input only: they show the helpers compute what they claim, not anything about a real sensor or
// button.
#include "diag_logic.hpp"
#include <cmath>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>

using namespace nodx::diag;
namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
bool close(double a, double b, double eps = 1e-9) {
    return std::abs(a - b) < eps;
}
} // namespace

int main() {
    unsigned passed = 0, failed = 0;
    auto test = [&](const char* label, const std::function<void()>& body) {
        try {
            body();
            ++passed;
            std::printf("PASS %s\n", label);
        } catch (const std::exception& e) {
            ++failed;
            std::printf("FAIL %s: %s\n", label, e.what());
        }
    };
    test("running stats match the textbook values", [] {
        RunningStats s;
        for (double v : {2., 4., 4., 4., 5., 5., 7., 9.}) {
            s.add(v);
        }
        require(s.count() == 8 && close(s.mean(), 5) && close(s.min(), 2) && close(s.max(), 9),
                "count/mean/min/max");
        require(close(s.stddev(), std::sqrt(32. / 7.)), "sample standard deviation");
    });
    test("running stats ignore non-finite input and report how much", [] {
        RunningStats s;
        s.add(1);
        s.add(NAN);
        s.add(INFINITY);
        s.add(3);
        require(s.count() == 2 && s.rejected() == 2 && close(s.mean(), 2), "non-finite handling");
    });
    test("empty and single-sample stats are zero, not garbage", [] {
        RunningStats s;
        require(s.mean() == 0 && s.stddev() == 0 && s.min() == 0 && s.max() == 0, "empty");
        s.add(5);
        require(s.stddev() == 0 && s.mean() == 5, "single");
    });
    test("a clean press and a clean release are one burst each", [] {
        Edge edges[] = {{1000, false}, {500000, true}};
        Burst out[4];
        const size_t n = groupBursts(edges, 2, 20000, out, 4);
        require(n == 2 && out[0].edges == 1 && out[0].spanUs == 0 && !out[0].settledLevel &&
                    out[1].edges == 1 && out[1].settledLevel,
                "two single-edge bursts");
    });
    test("bounce within the quiet gap is one burst with its span and settled level", [] {
        // press: low, high, low, high, low over 900 us, then a release much later
        Edge edges[] = {{1000, false}, {1200, true},  {1400, false},
                        {1700, true},  {1900, false}, {700000, true}};
        Burst out[4];
        const size_t n = groupBursts(edges, 6, 20000, out, 4);
        require(n == 2, "burst count");
        require(out[0].edges == 5 && out[0].spanUs == 900 && !out[0].settledLevel, "press burst");
        require(out[1].edges == 1 && out[1].settledLevel, "release burst");
    });
    test("a gap of exactly the quiet time starts a new burst", [] {
        Edge edges[] = {{0, false}, {19999, true}, {39999, false}};
        Burst out[4];
        const size_t n = groupBursts(edges, 3, 20000, out, 4);
        require(n == 2 && out[0].edges == 2 && out[1].edges == 1, "boundary");
    });
    test("burst grouping is wrap safe at the 32-bit microsecond rollover", [] {
        Edge edges[] = {{0xffffff00u, false}, {0xffffff80u, true}, {0x00000040u, false}};
        Burst out[4];
        const size_t n = groupBursts(edges, 3, 20000, out, 4);
        require(n == 1 && out[0].edges == 3 && out[0].spanUs == 0x140, "rollover");
    });
    test("burst output is bounded by its capacity", [] {
        Edge edges[] = {{0, false}, {100000, true}, {200000, false}, {300000, true}};
        Burst out[2];
        require(groupBursts(edges, 4, 20000, out, 2) == 2, "capacity");
        require(groupBursts(edges, 0, 20000, out, 2) == 0, "empty input");
    });
    test("the edge log is bounded and counts what it dropped", [] {
        EdgeLog<3> log;
        for (uint32_t i = 0; i < 5; ++i) {
            log.add(i * 10, i % 2 == 0);
        }
        require(log.size() == 3 && log.dropped() == 2, "bounded");
        log.clear();
        require(log.size() == 0 && log.dropped() == 0, "clear");
    });
    std::printf("%u passed, %u failed\n", passed, failed);
    return failed ? 1 : 0;
}
