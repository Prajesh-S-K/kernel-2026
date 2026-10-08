// Offline replay of a captured raw sensor stream through the quick tilt-and-return recognizer.
//
//   nodx_replay quick --practice practice.csv --eval session.csv[:1200,5400] [--eval more.csv]
//                     [--axes 2,0,1] [--signs 1,1,1] [--sens 1.0] [--tol 0.35]
//
// CSV (as written by scripts/capture_session.py): t_ms,gx,gy,gz,ax,ay,az in sensor register units
// (gyro 131 LSB per deg/s, accel 16384 LSB per g). The practice file is replayed through the same
// guided practice the device runs (its cue timing follows the sample timestamps, so capture must start
// when the practice starts); the profile it produces configures the recognizer for the eval files.
// The optional ":t1,t2" list gives the times (ms from the file start) of INTENDED gestures so hits,
// misses and false clicks can be counted. Nothing here is a hardware validation by itself: it
// reports what the recorded data does.
#include "nodx/quickgesture.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace nodx;
namespace {
struct Row {
    uint32_t t;
    float g[3], a[3];
};
bool readCsv(const std::string& path, std::vector<Row>& rows) {
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) {
        return false;
    }
    char line[256];
    while (std::fgets(line, sizeof line, f)) {
        unsigned long t;
        int v[6];
        if (std::sscanf(line, "%lu,%d,%d,%d,%d,%d,%d", &t, v, v + 1, v + 2, v + 3, v + 4, v + 5) != 7) {
            continue; // header or junk
        }
        Row r{};
        r.t = uint32_t(t);
        for (unsigned i = 0; i < 3; ++i) {
            r.g[i] = float(v[i]) / 131.f;
            r.a[i] = float(v[3 + i]) / 16384.f;
        }
        rows.push_back(r);
    }
    std::fclose(f);
    return !rows.empty();
}
struct Mapping {
    unsigned axes[3] = {2, 0, 1};
    float signs[3] = {1, 1, 1};
    Vec3 frame(const Row& r) const {
        return {r.g[axes[0]] * signs[0], r.g[axes[1]] * signs[1], r.g[axes[2]] * signs[2]};
    }
};
std::vector<float> parseList(const char* s) {
    std::vector<float> out;
    while (*s) {
        char* end = nullptr;
        out.push_back(std::strtof(s, &end));
        s = (*end == ',') ? end + 1 : end;
        if (end == s - 0 && *end != ',') {
            break;
        }
    }
    return out;
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || std::strcmp(argv[1], "quick") != 0) {
        std::fprintf(stderr, "usage: nodx_replay quick --practice p.csv --eval s.csv[:t1,t2] ...\n");
        return 2;
    }
    std::string practicePath;
    std::vector<std::string> evals;
    Mapping map;
    QuickSettings settings;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--practice") {
            practicePath = next();
        } else if (a == "--eval") {
            evals.push_back(next());
        } else if (a == "--axes") {
            auto v = parseList(next());
            for (unsigned k = 0; k < 3 && k < v.size(); ++k) {
                map.axes[k] = unsigned(v[k]);
            }
        } else if (a == "--signs") {
            auto v = parseList(next());
            for (unsigned k = 0; k < 3 && k < v.size(); ++k) {
                map.signs[k] = v[k];
            }
        } else if (a == "--sens") {
            settings.sensitivity = std::strtof(next(), nullptr);
        } else if (a == "--tol") {
            settings.returnTolerance = std::strtof(next(), nullptr);
        }
    }
    if (!settings.valid()) {
        std::fprintf(stderr, "settings out of range\n");
        return 2;
    }
    std::vector<Row> rows;
    if (!readCsv(practicePath, rows)) {
        std::fprintf(stderr, "cannot read the practice recording\n");
        return 2;
    }
    QuickPractice practice;
    practice.begin(rows.front().t, settings);
    for (const Row& r : rows) {
        practice.tick(map.frame(r), r.t);
        if (!practice.active()) {
            break;
        }
    }
    const QuickStatus ps = practice.status(rows.back().t);
    std::printf("practice: phase %s, reason: %s\n", name(ps.phase), ps.reason);
    if (practice.phase() != QuickPhase::Preview) {
        std::printf("{\"practice\":\"%s\",\"ok\":false}\n", name(ps.phase));
        return 1;
    }
    std::printf("practice: excursion %.1f deg, residual %.1f, wander %.1f, plane share %.2f\n",
                ps.practiceDeg, ps.practiceResidualDeg, ps.practiceCrossDeg, ps.planeShare);
    practice.accept();
    int exit = 0;
    for (const std::string& spec : evals) {
        std::string path = spec;
        std::vector<float> gestures;
        const size_t colon = spec.rfind(':');
        if (colon != std::string::npos) {
            path = spec.substr(0, colon);
            gestures = parseList(spec.c_str() + colon + 1);
        }
        std::vector<Row> data;
        if (!readCsv(path, data)) {
            std::fprintf(stderr, "cannot read %s\n", path.c_str());
            exit = 2;
            continue;
        }
        QuickRecognizer rec;
        rec.configure(practice.profile(), settings);
        std::vector<uint32_t> clicks;
        const uint32_t t0 = data.front().t;
        for (const Row& r : data) {
            if (rec.update(map.frame(r), r.t).accepted) {
                clicks.push_back(r.t - t0);
            }
        }
        unsigned hits = 0, missed = 0, falseClicks = 0;
        std::vector<bool> used(clicks.size(), false);
        for (float g : gestures) {
            bool hit = false;
            for (size_t c = 0; c < clicks.size(); ++c) {
                if (!used[c] && float(clicks[c]) >= g && float(clicks[c]) <= g + 1500.f) {
                    used[c] = hit = true;
                    break;
                }
            }
            hits += hit;
            missed += !hit;
        }
        for (size_t c = 0; c < clicks.size(); ++c) {
            falseClicks += used[c] ? 0u : 1u;
        }
        const float seconds = float(data.back().t - t0) / 1000.f;
        std::printf("eval %s: %.1f s, clicks %zu, candidates %u, rejected %u, suppressed %u ms (%.1f%%)",
                    path.c_str(), seconds, clicks.size(), rec.candidates, rec.rejected,
                    rec.suppressedMs, seconds > 0 ? 100.f * float(rec.suppressedMs) / (seconds * 1000.f) : 0.f);
        if (!gestures.empty()) {
            std::printf(", intended %zu: hits %u, MISSED %u, FALSE clicks %u", gestures.size(), hits,
                        missed, falseClicks);
        }
        std::printf(", last reject %s\n", name(rec.lastReject));
        std::printf("{\"file\":\"%s\",\"seconds\":%.1f,\"clicks\":%zu,\"candidates\":%u,\"rejected\":%u,"
                    "\"suppressedMs\":%u,\"intended\":%zu,\"hits\":%u,\"missed\":%u,\"falseClicks\":%u}\n",
                    path.c_str(), seconds, clicks.size(), rec.candidates, rec.rejected, rec.suppressedMs,
                    gestures.size(), hits, missed, falseClicks);
    }
    return exit;
}
