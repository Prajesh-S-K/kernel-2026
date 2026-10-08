#include "nodx/protocol.hpp"
#include "runtime.hpp"
#include <cmath>
#include <limits>
#include <sstream>

void command(const std::string& line, uint32_t now, bool truncated) {
    auto& system = *systemEngine;
    const auto envelope = nodx::parseEnvelope(line, 80, truncated);
    std::istringstream input(envelope.body);
    std::string operation;
    input >> operation;
    const uint32_t requestId = envelope.requestId;
    bool ok = envelope.valid;
    if (ok && (operation == "dwell" || operation == "scroll")) {
        std::string value;
        input >> value;
        ok = (value == "on" || value == "off") && (input >> std::ws).eof() && system.hasProfile;
        if (ok) {
            auto candidate = system.profile;
            if (operation == "dwell") {
                candidate.dwellEnabled = value == "on";
            } else {
                candidate.scrollEnabled = value == "on";
            }
            ok = system.setProfile(candidate);
        }
    } else if (ok && operation == "settings") {
        int dwell, scroll;
        ok = bool(input >> dwell >> scroll) && (input >> std::ws).eof() &&
             (dwell == 0 || dwell == 1) && (scroll == 0 || scroll == 1);
        if (ok) {
            ok = system.temporarySettings(dwell, scroll);
        }
#ifdef NODX_SIMULATED
    } else if (ok && operation == "motion") {
        float yaw, pitch, roll;
        ok = bool(input >> yaw >> pitch >> roll) && (input >> std::ws).eof() &&
             std::isfinite(yaw) && std::isfinite(pitch) && std::isfinite(roll) &&
             std::abs(yaw) <= 1000 && std::abs(pitch) <= 1000 && std::abs(roll) <= 1000;
        if (ok) {
            simulator.gyro = {yaw, pitch, roll};
        }
    } else if (ok && operation == "fault") {
        int fault;
        ok = bool(input >> fault) && (input >> std::ws).eof() && fault >= 0 && fault <= 5;
        if (ok) {
            simulator.fault = static_cast<Fault>(fault);
        }
#endif
    } else if (ok && !(input >> std::ws).eof()) {
        ok = false;
    } else if (ok && operation == "calibrate") {
        system.calibrate(now);
        ok = system.state == SystemState::Calibrating;
    } else if (ok && operation == "cancel") {
        system.cancelCalibration();
    } else if (ok && operation == "resume") {
        ok = system.resume();
    } else if (ok && operation == "pause") {
        system.pause();
        ok = system.state != SystemState::SafeState;
    } else if (ok && operation == "load") {
        UserProfile candidate;
        ok = repository.load(candidate) && system.setProfile(candidate, false);
    } else if (ok && operation == "generic") {
        ok = system.setProfile(UserProfile{}, false);
    } else if (operation != "status") {
        ok = false;
    }
    diagnostic(now, ok, requestId);
}
