#include "nodx/gesture_script.hpp"
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
    } else if (ok && operation == "enable") {
#ifdef NODX_SIMULATED
        int value;
        ok = bool(input >> value) && (input >> std::ws).eof() && (value == 0 || value == 1);
        if (ok) {
            simulatedEnable = value == 1;
            system.setControlSwitch(simulatedEnable, now); // releases at once
        }
#else
        ok = false; // the physical switch is the only enable source on hardware
#endif
    } else if (ok && operation == "gesture") {
#ifdef NODX_SIMULATED
        std::string pattern;
        float scale = 1;
        std::vector<nodx::Rates> motion;
        ok = bool(input >> pattern);
        if (ok && !(input >> std::ws).eof()) {
            ok = bool(input >> scale) && (input >> std::ws).eof();
        }
        ok = ok && nodx::sim::named(pattern, scale, motion) && gestureScript.empty();
        if (ok) {
            nodx::sim::neutral(gestureScript, 400);
            gestureScript.insert(gestureScript.end(), motion.begin(), motion.end());
            nodx::sim::neutral(gestureScript, 400);
        }
#else
        ok = false;
#endif
    } else if (ok && operation == "train") {
        std::string verb, which;
        input >> verb;
        if (verb == "start") {
            input >> which;
            ok = (which == "pause" || which == "drag") && (input >> std::ws).eof() &&
                 system.trainStart(
                     which == "pause" ? nodx::GestureId::PauseResume : nodx::GestureId::Drag, now);
        } else if (verb == "cancel" || verb == "accept") {
            ok = (input >> std::ws).eof();
            if (ok && verb == "cancel") {
                system.trainCancel();
            } else if (ok) {
                ok = system.trainAccept();
            }
        } else {
            ok = false;
        }
    } else if (ok && operation == "map") {
        std::string verb;
        input >> verb;
        ok = (input >> std::ws).eof();
        if (ok && verb == "start") {
            ok = system.teachStart(now);
        } else if (ok && verb == "cancel") {
            system.teachCancel();
        } else if (ok && verb == "accept") {
            ok = system.teachAccept();
        } else if (ok && verb == "save") {
            ok = system.teachSave();
        } else if (ok && verb == "clear") {
            system.clearLearned();
        } else {
            ok = false;
        }
    } else if (ok && operation == "capture") {
#ifdef NODX_SIMULATED
        ok = false;
#else
        std::string verb;
        input >> verb;
        if (verb == "start") {
            unsigned seconds = 0;
            ok = bool(input >> seconds) && (input >> std::ws).eof() && seconds >= 1 && seconds <= 20;
            if (ok) {
                captureState.count = 0;
                captureState.untilMs = now + seconds * 1000u;
                captureState.active = true;
            }
        } else if (verb == "stop") {
            ok = (input >> std::ws).eof();
            captureState.active = false;
        } else if (verb == "get") {
            unsigned long offset = 0;
            ok = bool(input >> offset) && (input >> std::ws).eof() && offset < captureState.count;
            if (ok) {
                captureState.pageOffset = size_t(offset);
            }
        } else {
            ok = false;
        }
#endif
    } else if (ok && operation == "quick") {
        std::string verb, value;
        input >> verb;
        if (verb == "practice") {
            input >> value;
            ok = (value == "fallback" || value == "configured") && (input >> std::ws).eof() &&
                 system.quickPracticeStart(now, value == "configured");
        } else if (verb == "enable") {
            input >> value;
            ok = (value == "on" || value == "off") && (input >> std::ws).eof() &&
                 system.setQuickGesture(value == "on", now);
        } else if (verb == "set") {
            float sensitivity = 0, tolerance = 0, angle = 0;
            ok = bool(input >> sensitivity >> tolerance);
            if (ok && !(input >> std::ws).eof()) {
                ok = bool(input >> angle) && (input >> std::ws).eof() &&
                     system.setQuickSettings(sensitivity, tolerance, angle);
            } else if (ok) {
                ok = system.setQuickSettings(sensitivity, tolerance);
            }
        } else if (verb == "retry" || verb == "cancel" || verb == "accept" || verb == "clear") {
            ok = (input >> std::ws).eof();
            if (ok && verb == "retry") {
                system.quickPracticeRetry(now);
            } else if (ok && verb == "cancel") {
                system.quickPracticeCancel();
            } else if (ok && verb == "accept") {
                ok = system.quickPracticeAccept();
            } else if (ok) {
                system.quickClear();
            }
        } else {
            ok = false;
        }
    } else if (ok && operation == "actions") {
        std::string verb, value;
        input >> verb >> value;
        if (verb == "enable") {
            ok = (value == "on" || value == "off") && (input >> std::ws).eof() &&
                 system.setActionPalette(value == "on", now);
        } else if (verb == "keep") {
            ok = (value == "on" || value == "off") && (input >> std::ws).eof() &&
                 system.setActionKeep(value == "on");
        } else if (verb == "hover") {
            nodx::PaletteTarget target;
            ok = nodx::parsePaletteTarget(value.c_str(), target) && (input >> std::ws).eof() &&
                 system.setActionHover(target, now);
        } else {
            ok = false;
        }
    } else if (ok && operation == "click") {
        std::string verb, value;
        input >> verb;
        if (verb == "train") {
            input >> value;
            ok = (value == "fallback" || value == "configured") && (input >> std::ws).eof() &&
                 system.clickTrainStart(now, value == "configured");
        } else if (verb == "enable") {
            input >> value;
            ok = (value == "on" || value == "off") && (input >> std::ws).eof() &&
                 system.setClickGesture(value == "on", now);
        } else if (verb == "cancel" || verb == "accept" || verb == "clear") {
            ok = (input >> std::ws).eof();
            if (ok && verb == "cancel") {
                system.clickTrainCancel();
            } else if (ok && verb == "accept") {
                ok = system.clickTrainAccept();
            } else if (ok) {
                system.clickClear();
            }
        } else {
            ok = false;
        }
    } else if (ok && operation == "control") {
        std::string verb;
        input >> verb;
        ok = (input >> std::ws).eof();
        if (ok && verb == "start") {
            ok = system.startConfiguredControl(now);
        } else if (ok && verb == "stop") {
            system.stopUncalibratedDemo("control stopped by the user; explicit restart required");
        } else {
            ok = false;
        }
    } else if (ok && operation == "handsfree") {
        std::string verb, value;
        input >> verb;
        if (verb == "commit" || verb == "legacy") {
            ok = (input >> std::ws).eof() &&
                 (verb == "commit" ? system.commitHandsFree() : system.useLegacyMode());
        } else if (verb == "switchless") {
            input >> value;
            ok = (value == "on" || value == "off") && (input >> std::ws).eof();
            if (ok) {
                system.stageSwitchless(value == "on");
            }
        } else if (verb == "demo") {
            input >> value;
            ok = (value == "on" || value == "off") && (input >> std::ws).eof() &&
                 system.setDemoMovementOnly(value == "on");
        } else if (verb == "uncal") {
            input >> value;
            if (value == "reverse") {
                // reverse <horizontal 0|1> <vertical 0|1>   (RAM only)
                int horizontal = -1, vertical = -1;
                ok = bool(input >> horizontal >> vertical) && (input >> std::ws).eof() &&
                     (horizontal == 0 || horizontal == 1) && (vertical == 0 || vertical == 1);
                if (ok) {
                    system.setUncalibratedReversal(horizontal == 1, vertical == 1);
                }
            } else if (value == "speed") {
                // speed <factor 0.25-2>   (RAM only; works whether or not a session is running)
                float factor = 0;
                ok = bool(input >> factor) && (input >> std::ws).eof() &&
                     system.setUncalibratedSpeed(factor, now);
            } else if (value == "dwell") {
                // dwell on|off  |  dwell set <ms> <tolerance>   (RAM only, demo must be running)
                std::string action;
                input >> action;
                if (action == "on" || action == "off") {
                    ok = (input >> std::ws).eof() &&
                         system.setUncalibratedDwell(action == "on", now);
                } else if (action == "set") {
                    unsigned long dwellMs = 0;
                    float tolerance = 0;
                    ok = bool(input >> dwellMs >> tolerance) && (input >> std::ws).eof() &&
                         system.setUncalibratedDwellSettings(uint32_t(dwellMs), tolerance);
                } else {
                    ok = false;
                }
            } else {
                ok = (value == "start" || value == "stop") && (input >> std::ws).eof();
                if (ok && value == "start") {
                    ok = system.startUncalibratedDemo(now);
                } else if (ok) {
                    system.stopUncalibratedDemo(
                        "demo stopped by the user; explicit restart required");
                }
            }
        } else if (verb == "enable") {
            input >> value;
            ok = (value == "maintained" || value == "momentary") && (input >> std::ws).eof();
            if (ok) {
                system.stageEnableKind(value == "momentary" ? nodx::EnableKind::Momentary
                                                            : nodx::EnableKind::Maintained);
            }
        } else {
            ok = false;
        }
    } else if (ok && operation == "calibrate" && !(input >> std::ws).eof()) {
        std::string mode;
        input >> mode;
        ok = mode == "guided" && (input >> std::ws).eof();
        if (ok) {
            system.calibrate(now, nodx::start::calibrationCueMs);
            ok = system.state == SystemState::Calibrating;
        }
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
