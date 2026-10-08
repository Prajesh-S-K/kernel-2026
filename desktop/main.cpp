#include "nodx/engine.hpp"
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
        if (bytes.size() != 84) {
            return false;
        }
        std::string temp = path(slot) + ".tmp";
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
        std::filesystem::rename(temp, path(slot), ec);
        if (ec) {
            return false;
        }
        int directory = ::open(root_.c_str(), O_RDONLY);
        if (directory < 0) {
            return false;
        }
        synced = ::fsync(directory) == 0;
        ::close(directory);
        return synced;
    }

private:
    std::string root_;
    std::string path(unsigned slot) {
        return root_ + "/profile" + std::to_string(slot) + ".bin";
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
              << "{\"protocol\":1,\"protocolRevision\":2,\"softwareVersion\":\"0.2.0\",\"source\":"
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
    std::cout << ",\"reports\":[";
    for (size_t j = 0; j < hid.reports.size(); ++j) {
        const auto& r = hid.reports[j];
        std::cout << (j ? "," : "") << "[" << int(r.dx) << ',' << int(r.dy) << ',' << int(r.wheel)
                  << ',' << (r.down ? 1 : 0) << ']';
    }
    std::cout << "]}" << std::endl;
    hid.reports.clear();
}
std::vector<MotionSample> readReplay(const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        throw std::runtime_error("cannot open replay");
    }
    std::vector<MotionSample> rows;
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
        if (fields.size() != 7 && fields.size() != 8) {
            throw std::runtime_error("malformed replay row");
        }
        MotionSample s;
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
        if (fields.size() == 8) {
            if (fields[7] != "0" && fields[7] != "1") {
                throw std::runtime_error("invalid valid flag");
            }
            s.valid = fields[7] == "1";
        }
        if (!rows.empty() && s.timestampMs <= last) {
            throw std::runtime_error("replay timestamps must increase");
        }
        last = s.timestampMs;
        rows.push_back(s);
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
        SimHID transport;
        System sys(transport, repo);
        sys.axes.axes = {0, 1, 2}; // desktop inputs already yaw/pitch/roll
        sys.axes.accelAxes = {0, 1, 2};
        sys.axes.accelSigns = {1, 1, 1};
        uint32_t now = 0;
        std::ofstream recording;
        if (argc > 3 && std::string(argv[2]) == "--replay") {
            auto rows = readReplay(argv[3]);
            ReplaySensor replay(rows);
            sys.setProfile(UserProfile{}, false);
            bool started = false;
            for (const auto& row : rows) {
                now = row.timestampMs;
                sys.tick(replay.read(now), now, false);
                if (!started) {
                    started = sys.resume();
                }
                print(sys, transport, now);
            }
            return 0;
        }
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
                if (!(cmd >> countValue >> yaw >> pitch >> roll >> pressed >> online >> automatic >>
                      fault) ||
                    countValue > 50 || countValue < 1 || std::floor(countValue) != countValue ||
                    !(cmd >> std::ws).eof() || !std::isfinite(yaw) || !std::isfinite(pitch) ||
                    !std::isfinite(roll) || std::abs(yaw) > 1000 || std::abs(pitch) > 1000 ||
                    std::abs(roll) > 1000 || (pressed != 0 && pressed != 1) ||
                    (online != 0 && online != 1) || (automatic != 0 && automatic != 1) ||
                    fault < 0 || fault > 6) {
                    print(sys, transport, now, false);
                    continue;
                }
                count = unsigned(countValue);
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
                    MotionSample row = sensor.read(now);
                    if (recording.is_open()) {
                        recording << row.timestampMs;
                        for (float v : row.gyro) {
                            recording << ',' << v;
                        }
                        for (float v : row.accel) {
                            recording << ',' << v;
                        }
                        recording << ',' << (row.valid ? 1 : 0) << '\n';
                    }
                    sys.tick(row, now, pressed);
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
                        recording << "timestampMs,gyroX,gyroY,gyroZ,accelX,accelY,accelZ,valid\n";
                        ok = bool(recording);
                    }
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
