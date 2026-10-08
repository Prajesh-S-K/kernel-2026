#include "nodx/engine.hpp"
#include "nodx/gesture_script.hpp"
#include <cmath>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <unistd.h>

using namespace nodx;
// Temp file + fsync + atomic rename + directory fsync, shared by every record store.
bool durableWrite(const std::string& root, const std::string& target,
                  const std::vector<uint8_t>& bytes) {
    std::string temp = target + ".tmp";
    std::ofstream f(temp, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    f.close();
    if (!f) {
        return false;
    }
    int fd = ::open(temp.c_str(), O_RDONLY);
    if (fd < 0) {
        return false;
    }
    bool synced = ::fsync(fd) == 0;
    ::close(fd);
    if (!synced) {
        return false;
    }
    std::error_code ec;
    std::filesystem::rename(temp, target, ec);
    if (ec) {
        return false;
    }
    int directory = ::open(root.c_str(), O_RDONLY);
    if (directory < 0) {
        return false;
    }
    synced = ::fsync(directory) == 0;
    ::close(directory);
    return synced;
}
class FileStorage : public ProfileStorage {
public:
    explicit FileStorage(std::string root) : root_(std::move(root)) {
        std::filesystem::create_directories(root_);
    }
    std::vector<uint8_t> read(unsigned slot) override {
        std::error_code error;
        if (std::filesystem::file_size(path(slot), error) != 84 || error) {
            return {};
        }
        std::ifstream file(path(slot), std::ios::binary);
        std::vector<uint8_t> bytes(84);
        file.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
        if (!file) {
            return {};
        }
        return bytes;
    }
    bool write(unsigned slot, const std::vector<uint8_t>& bytes) override {
        return bytes.size() == 84 && durableWrite(root_, path(slot), bytes);
    }

private:
    std::string root_;
    std::string path(unsigned slot) {
        return root_ + "/profile" + std::to_string(slot) + ".bin";
    }
};
// Hands-free configuration slots hf0.bin / hf1.bin. A file that is too large to be a record is
// reported as one invalid byte so it is classified as corrupt rather than silently ignored.
class FileConfigStorage : public ConfigStorage {
public:
    explicit FileConfigStorage(std::string root) : root_(std::move(root)) {
        std::filesystem::create_directories(root_);
    }
    std::vector<uint8_t> read(unsigned slot) override {
        std::error_code error;
        const auto size = std::filesystem::file_size(path(slot), error);
        if (error || size == 0) {
            return {};
        }
        if (size > 512) {
            return {0xff};
        }
        std::ifstream file(path(slot), std::ios::binary);
        std::vector<uint8_t> bytes(size);
        file.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
        return file ? bytes : std::vector<uint8_t>{0xff};
    }
    bool write(unsigned slot, const std::vector<uint8_t>& bytes) override {
        return bytes.size() <= 512 && durableWrite(root_, path(slot), bytes);
    }

private:
    std::string root_;
    std::string path(unsigned slot) {
        return root_ + "/hf" + std::to_string(slot) + ".bin";
    }
};
class SimHID : public HIDTransport {
public:
    bool online = true, fail = false;
    bool connected() const override {
        return online;
    }
    bool send(const Report& r) override {
        reports.push_back(r);
        return !fail;
    }
    std::vector<Report> reports;
};
void profileJson(const UserProfile& p) {
    std::cout << "{\"schema\":1,\"bias\":[" << p.bias[0] << ',' << p.bias[1] << ',' << p.bias[2]
              << "],\"deadzone\":[" << p.deadzone[0] << ',' << p.deadzone[1] << "],\"gain\":[";
    for (unsigned i = 0; i < 4; ++i) {
        std::cout << (i ? "," : "") << p.gain[i];
    }
    std::cout << "],\"alpha\":" << p.alpha << ",\"precisionThreshold\":" << p.precisionThreshold
              << ",\"fastThreshold\":" << p.fastThreshold
              << ",\"dwellTolerance\":" << p.dwellTolerance << ",\"dwellMs\":" << p.dwellMs
              << ",\"scrollThreshold\":" << p.scrollThreshold << ",\"scrollGain\":" << p.scrollGain
              << ",\"dwellEnabled\":" << (p.dwellEnabled ? "true" : "false")
              << ",\"scrollEnabled\":" << (p.scrollEnabled ? "true" : "false") << '}';
}
void print(System& s, SimHID& hid, uint32_t now, bool ok = true) {
    auto& d = s.diagnostics;
    std::cout << std::fixed << std::setprecision(5)
              << "{\"protocol\":1,\"protocolRevision\":4,\"softwareVersion\":\"0.2.0\",\"source\":"
                 "\"SIMULATED\",\"ok\":"
              << (ok ? "true" : "false") << ",\"faultCode\":\"" << name(d.faultCode)
              << "\",\"timeMs\":" << now << ",\"state\":\"" << name(s.state) << "\",\"reason\":\""
              << d.reason << "\",\"cursor\":\"" << d.cursor << "\",\"calibration\":\""
              << name(s.calibration.phase) << "\",\"calibrationReason\":\"" << s.calibration.reason
              << "\",\"calibrationProgress\":" << s.calibration.progress(now) << ",\"dwell\":\""
              << name(s.selection.dwell)
              << "\",\"dwellProgress\":" << s.selection.progress(now, s.profile)
              << ",\"cancellations\":" << s.selection.cancellations << ",\"faults\":" << d.faults
              << ",\"stability\":" << d.motion.stability << ",\"motion\":[" << d.motion.x << ','
              << d.motion.y << ',' << d.motion.roll
              << "],\"connected\":" << (hid.online ? "true" : "false")
              << ",\"hasProfile\":" << (s.hasProfile ? "true" : "false") << ",\"profile\":";
    profileJson(s.profile);
    char hands[1024];
    const size_t handsLength = handsFreeJson(hands, sizeof hands, s.handsFreeStatus());
    std::cout << ",\"handsFree\":" << (handsLength ? hands : "{}");
    std::cout << ",\"reports\":[";
    for (size_t j = 0; j < hid.reports.size(); ++j) {
        const auto& r = hid.reports[j];
        std::cout << (j ? "," : "") << "[" << int(r.dx) << ',' << int(r.dy) << ',' << int(r.wheel)
                  << ',' << (r.down ? 1 : 0) << ']';
    }
    std::cout << "]}" << std::endl;
    hid.reports.clear();
}
struct ReplayRow {
    MotionSample sample;
    bool enable = true;
};
std::vector<ReplayRow> readReplay(const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        throw std::runtime_error("cannot open replay");
    }
    std::vector<ReplayRow> rows;
    std::string line;
    std::getline(f, line); // header
    uint32_t last = 0;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        std::istringstream row(line);
        std::vector<std::string> fields;
        std::string cell;
        while (std::getline(row, cell, ',')) {
            fields.push_back(cell);
        }
        if (fields.size() < 7 || fields.size() > 9) {
            throw std::runtime_error("malformed replay row");
        }
        ReplayRow entry;
        MotionSample& s = entry.sample;
        size_t used = 0;
        auto timestamp = std::stoull(fields[0], &used);
        if (used != fields[0].size() || timestamp > UINT32_MAX) {
            throw std::runtime_error("invalid timestamp");
        }
        s.timestampMs = timestamp;
        for (unsigned i = 0; i < 6; ++i) {
            float v = std::stof(fields[i + 1], &used);
            if (used != fields[i + 1].size()) {
                throw std::runtime_error("invalid number");
            }
            if (i < 3) {
                s.gyro[i] = v;
            } else {
                s.accel[i - 3] = v;
            }
        }
        if (fields.size() >= 8) {
            if (fields[7] != "0" && fields[7] != "1") {
                throw std::runtime_error("invalid valid flag");
            }
            s.valid = fields[7] == "1";
        }
        if (fields.size() == 9) {
            if (fields[8] != "0" && fields[8] != "1") {
                throw std::runtime_error("invalid enable flag");
            }
            entry.enable = fields[8] == "1";
        }
        if (!rows.empty() && s.timestampMs <= last) {
            throw std::runtime_error("replay timestamps must increase");
        }
        last = s.timestampMs;
        rows.push_back(entry);
    }
    if (rows.empty()) {
        throw std::runtime_error("empty replay");
    }
    return rows;
}
int main(int argc, char** argv) {
    try {
        FileStorage storage(argc > 1 ? argv[1] : "runtime");
        ProfileRepository repo(storage);
        FileConfigStorage configStorage(argc > 1 ? argv[1] : "runtime");
        HandsFreeRepository configRepo(configStorage);
        SimHID transport;
        System sys(transport, repo, configRepo);
        sys.configureEnableInput(true); // the simulator has a simulated enable input
        bool simEnable = false;         // raw input: switch ON / button pressed; released at start
        sys.axes.axes = {0, 1, 2};      // desktop inputs already yaw/pitch/roll
        sys.axes.accelAxes = {0, 1, 2};
        sys.axes.accelSigns = {1, 1, 1};
        uint32_t now = 0;
        std::ofstream recording;
        if (argc > 3 && std::string(argv[2]) == "--replay") {
            auto rows = readReplay(argv[3]);
            // --hands-free keeps the stored profile and configuration and never resumes by itself:
            // only a recognised gesture can. Without it the original generic, auto-resume replay.
            const bool handsFree = argc > 4 && std::string(argv[4]) == "--hands-free";
            std::vector<MotionSample> samples;
            for (const auto& row : rows) {
                samples.push_back(row.sample);
            }
            ReplaySensor replay(samples);
            if (!handsFree) {
                sys.setProfile(UserProfile{}, false);
            }
            bool started = handsFree;
            for (const auto& row : rows) {
                now = row.sample.timestampMs;
                sys.setControlSwitch(row.enable, now);
                sys.tick(replay.read(now), now, false);
                if (!started) {
                    started = sys.resume();
                }
                print(sys, transport, now);
            }
            return 0;
        }
        // One sample: record it, present the maintained switch, then tick the shared engine.
        auto feed = [&](const MotionSample& row, bool pressed) {
            if (recording.is_open()) {
                recording << row.timestampMs;
                for (float v : row.gyro) {
                    recording << ',' << v;
                }
                for (float v : row.accel) {
                    recording << ',' << v;
                }
                recording << ',' << (row.valid ? 1 : 0) << ',' << (simEnable ? 1 : 0) << '\n';
            }
            sys.setControlSwitch(simEnable, row.timestampMs);
            sys.tick(row, row.timestampMs, pressed);
        };
        std::string line;
        while (std::getline(std::cin, line)) {
            if (line.size() > 256) {
                print(sys, transport, now, false);
                continue;
            }
            std::istringstream cmd(line);
            std::string op;
            cmd >> op;
            bool ok = true;
            if (op == "step") {
                double countValue;
                unsigned count;
                float yaw, pitch, roll;
                int pressed, online, automatic, fault;
                int enableValue = simEnable ? 1 : 0; // optional ninth field, additive
                if (!(cmd >> countValue >> yaw >> pitch >> roll >> pressed >> online >> automatic >>
                      fault) ||
                    countValue > 50 || countValue < 1 || std::floor(countValue) != countValue ||
                    !((cmd >> std::ws).eof() || ((cmd >> enableValue) && (cmd >> std::ws).eof())) ||
                    (enableValue != 0 && enableValue != 1) || !std::isfinite(yaw) ||
                    !std::isfinite(pitch) || !std::isfinite(roll) || std::abs(yaw) > 1000 ||
                    std::abs(pitch) > 1000 || std::abs(roll) > 1000 ||
                    (pressed != 0 && pressed != 1) || (online != 0 && online != 1) ||
                    (automatic != 0 && automatic != 1) || fault < 0 || fault > 6) {
                    print(sys, transport, now, false);
                    continue;
                }
                count = unsigned(countValue);
                simEnable = enableValue == 1;
                transport.online = online;
                transport.fail = fault == 6;
                for (unsigned j = 0; j < count; ++j) {
                    now += start::sampleMs;
                    SimulatedSensor sensor;
                    sensor.gyro = {yaw, pitch, roll};
                    if (automatic && sys.state == SystemState::Calibrating) {
                        float noise = (int((now / 10) % 7) - 3) * .08f;
                        sensor.gyro = {noise, noise, noise};
                        switch (sys.calibration.phase) {
                        case CalPhase::Left:
                            sensor.gyro[0] -= 12;
                            break;
                        case CalPhase::Right:
                            sensor.gyro[0] += 24;
                            break;
                        case CalPhase::Up:
                            sensor.gyro[1] -= 10;
                            break;
                        case CalPhase::Down:
                            sensor.gyro[1] += 20;
                            break;
                        case CalPhase::Natural:
                            sensor.gyro[0] += std::sin(now * .004f) * 12;
                            break;
                        default:
                            break;
                        }
                    }
                    if (fault >= 1 && fault <= 5) {
                        sensor.fault = static_cast<Fault>(fault);
                    }
                    feed(sensor.read(now), pressed);
                }
                if (recording.is_open()) {
                    recording.flush();
                }
            } else if (op == "dwell" || op == "scroll") {
                int enabled;
                if (!(cmd >> enabled) || (enabled != 0 && enabled != 1) ||
                    !(cmd >> std::ws).eof() || !sys.hasProfile) {
                    ok = false;
                } else {
                    auto candidate = sys.profile;
                    if (op == "dwell") {
                        candidate.dwellEnabled = enabled;
                    } else {
                        candidate.scrollEnabled = enabled;
                    }
                    ok = sys.setProfile(candidate, true);
                }
            } else if (op == "settings") {
                int dwellEnabled, scrollEnabled;
                ok = bool(cmd >> dwellEnabled >> scrollEnabled) && (cmd >> std::ws).eof() &&
                     (dwellEnabled == 0 || dwellEnabled == 1) &&
                     (scrollEnabled == 0 || scrollEnabled == 1);
                if (ok) {
                    ok = sys.temporarySettings(dwellEnabled, scrollEnabled);
                }
            } else if (op == "record") {
                std::string value;
                cmd >> value;
                ok = (value == "on" || value == "off") && (cmd >> std::ws).eof();
                if (ok) {
                    if (recording.is_open()) {
                        recording.close();
                    }
                    if (value == "on") {
                        std::string root = argc > 1 ? argv[1] : "runtime";
                        recording.open(root + "/samples.csv");
                        recording
                            << "timestampMs,gyroX,gyroY,gyroZ,accelX,accelY,accelZ,valid,enable\n";
                        ok = bool(recording);
                    }
                }
            } else if (op == "enable") {
                int value;
                ok = bool(cmd >> value) && (cmd >> std::ws).eof() && (value == 0 || value == 1);
                if (ok) {
                    simEnable = value == 1;
                    sys.setControlSwitch(simEnable, now); // acts at once, no sample needed
                }
            } else if (op == "gesture") {
                // Scripted synthetic head motion through the normal tick path:
                // 400 ms of stillness, the pattern, then 400 ms of stillness.
                std::string pattern;
                float scale = 1;
                std::vector<Rates> motion;
                ok = bool(cmd >> pattern);
                if (ok && !(cmd >> std::ws).eof()) {
                    ok = bool(cmd >> scale) && (cmd >> std::ws).eof();
                }
                ok = ok && sim::named(pattern, scale, motion);
                if (ok) {
                    std::vector<Rates> all;
                    sim::neutral(all, 400);
                    all.insert(all.end(), motion.begin(), motion.end());
                    sim::neutral(all, 400);
                    for (const auto& rate : all) {
                        now += start::sampleMs;
                        feed({now, {rate[0], rate[1], rate[2]}, {0, 0, 1}, true}, false);
                    }
                }
            } else if (op == "train") {
                std::string verb, which;
                cmd >> verb;
                if (verb == "start") {
                    cmd >> which;
                    ok = (which == "pause" || which == "drag") && (cmd >> std::ws).eof() &&
                         sys.trainStart(which == "pause" ? GestureId::PauseResume : GestureId::Drag,
                                        now);
                } else if (verb == "cancel" || verb == "accept") {
                    ok = (cmd >> std::ws).eof();
                    if (ok && verb == "cancel") {
                        sys.trainCancel();
                    } else if (ok) {
                        ok = sys.trainAccept();
                    }
                } else {
                    ok = false;
                }
            } else if (op == "handsfree") {
                std::string verb, value;
                cmd >> verb;
                if (verb == "commit" || verb == "legacy") {
                    ok = (cmd >> std::ws).eof() &&
                         (verb == "commit" ? sys.commitHandsFree() : sys.useLegacyMode());
                } else if (verb == "switchless") {
                    cmd >> value;
                    ok = (value == "on" || value == "off") && (cmd >> std::ws).eof();
                    if (ok) {
                        sys.stageSwitchless(value == "on");
                    }
                } else if (verb == "demo") {
                    cmd >> value;
                    ok = (value == "on" || value == "off") && (cmd >> std::ws).eof() &&
                         sys.setDemoMovementOnly(value == "on");
                } else if (verb == "enable") {
                    cmd >> value;
                    ok = (value == "maintained" || value == "momentary") && (cmd >> std::ws).eof();
                    if (ok) {
                        sys.stageEnableKind(value == "momentary" ? EnableKind::Momentary
                                                                 : EnableKind::Maintained);
                    }
                } else {
                    ok = false;
                }
            } else if (!(cmd >> std::ws).eof()) {
                ok = false;
            } else if (op == "calibrate") {
                sys.calibrate(now);
                ok = sys.state == SystemState::Calibrating;
            } else if (op == "cancel") {
                sys.cancelCalibration();
            } else if (op == "resume") {
                ok = sys.resume();
            } else if (op == "pause") {
                sys.pause();
                ok = sys.state != SystemState::SafeState;
            } else if (op == "generic") {
                ok = sys.setProfile(UserProfile{}, false);
            } else if (op == "load") {
                UserProfile candidate;
                ok = repo.load(candidate) && sys.setProfile(candidate, false);
            } else if (op == "corrupt") {
                for (unsigned i = 0; i < 2; ++i) {
                    auto bytes = storage.read(i);
                    if (!bytes.empty()) {
                        bytes[0] ^= 0xff;
                        storage.write(i, bytes);
                    }
                }
                sys.invalidateProfile();
            } else if (op != "status") {
                ok = false;
            }
            print(sys, transport, now, ok);
        }
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
