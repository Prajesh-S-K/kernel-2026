// Host tests of the REAL firmware command path. firmware/src/{runtime,commands,telemetry}.cpp are
// compiled unmodified against stub Arduino/Wire/Preferences/NimBLE headers (tests/firmware_host/
// stubs). Every command goes in as serial bytes through serviceRuntime(), the bounded line parser,
// command() and the telemetry queue, exactly as on the device, and is judged from the bytes the
// firmware writes back.
//
// What this does NOT cover: real NVS atomicity, I2C timing, BLE, GPIO electrical behaviour, the
// Arduino scheduler. The stubs are stand-ins, not models of that hardware.
//
// Built twice: NODX_SIMULATED (simulation commands exist) and without it (hardware configuration,
// where simulation-only commands must be refused).
#include "Arduino.h"
#include "Wire.h"
#include "runtime.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

HostSerial Serial;
HostWire Wire;
static uint32_t clockMs = 1000;
uint32_t millis() {
    return clockMs;
}

namespace {
unsigned checks = 0, failures = 0;
uint32_t nextId = 1;
void expect(bool condition, const char* name) {
    ++checks;
    if (!condition) {
        ++failures;
    }
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", name);
}
bool has(const std::string& text, const char* part) {
    return text.find(part) != std::string::npos;
}
void pump(unsigned milliseconds) {
    for (unsigned i = 0; i < milliseconds; ++i) {
        ++clockMs;
        serviceRuntime();
        if (Serial.output.size() > (1u << 20)) { // bounded memory during long simulated runs
            Serial.output.erase(0, Serial.output.size() - 65536);
        }
    }
}
// Newest complete JSON line that echoes this request id ("" when none was transmitted).
std::string findAck(uint32_t id) {
    const std::string key = "\"requestId\":" + std::to_string(id) + ",";
    std::string found;
    size_t start = 0;
    while (start < Serial.output.size()) {
        size_t end = Serial.output.find('\n', start);
        if (end == std::string::npos) {
            break;
        }
        const std::string line = Serial.output.substr(start, end - start);
        if (line.rfind("{\"protocol\":1", 0) == 0 && has(line, key.c_str())) {
            found = line;
        }
        start = end + 1;
    }
    return found;
}
// One command as serial bytes with a request id; returns the acknowledgement line.
std::string send(const std::string& body, unsigned settleMs = 120) {
    const uint32_t id = nextId++;
    Serial.output.clear();
    Serial.input += "@" + std::to_string(id) + " " + body + "\n";
    pump(settleMs);
    return findAck(id);
}
std::string status() {
    return send("status");
}
bool ok(const std::string& ack) {
    return has(ack, "\"ok\":true");
}
bool refused(const std::string& ack) {
    return has(ack, "\"ok\":false");
}
void parserChecks() {
    std::string ack = status();
    expect(ok(ack), "status acknowledged");
    expect(has(ack, "\"protocol\":1,"), "protocol stays 1");
    expect(has(ack, "\"protocolRevision\":4"), "protocol revision 4");
    expect(has(ack, "\"handsFree\":{"), "handsFree object present");
    expect(has(ack, "\"mode\":\"LEGACY_SWITCH\""), "no record boots as legacy compatibility");
    expect(has(ack, "\"config\":\"MISSING\""), "missing record reported as MISSING");

    expect(refused(send("bogus")), "unknown operation refused");
    expect(refused(send("")), "empty body refused");
    expect(refused(send("status extra")), "trailing text after status refused");
    expect(refused(send("calibrate now")), "trailing text after calibrate refused");
    expect(refused(send("handsfree commit")), "commit without trained gestures refused");
    expect(refused(send("handsfree commit now")), "commit with trailing text refused");
    expect(refused(send("handsfree bogus")), "unknown handsfree verb refused");
    expect(refused(send("handsfree switchless maybe")), "switchless needs on/off");
    expect(refused(send("handsfree demo")), "demo needs a value");
    expect(refused(send("handsfree demo maybe")), "demo value must be on/off");
    expect(refused(send("handsfree demo on extra")), "demo trailing text refused");
    ack = send("handsfree demo on");
    expect(ok(ack) && has(ack, "\"demoMovementOnly\":true"), "demo on is reported");
    ack = send("handsfree demo off");
    expect(ok(ack) && has(ack, "\"demoMovementOnly\":false"), "demo off is reported");
    expect(refused(send("handsfree enable")), "enable kind needs a value");
    expect(refused(send("handsfree enable bogus")), "unknown enable kind refused");
    expect(refused(send("handsfree enable momentary extra")), "enable kind trailing text refused");
    expect(refused(send("handsfree switchless on extra")), "switchless trailing text refused");
    expect(refused(send("train")), "train without verb refused");
    expect(refused(send("train bogus")), "unknown train verb refused");
    expect(refused(send("train start")), "train start without gesture refused");
    expect(refused(send("train start bogus")), "train start unknown gesture refused");
    expect(refused(send("train start pause extra")), "train start trailing text refused");
    expect(refused(send("train cancel extra")), "train cancel trailing text refused");
    expect(refused(send("train accept extra")), "train accept trailing text refused");
    expect(refused(send("train start pause")), "training refused without a profile");
    expect(refused(send("dwell on")), "dwell refused without a profile");
    expect(refused(send("settings 2 0")), "settings out of range refused");
    expect(refused(send("settings 1")), "settings missing field refused");

    ack = send("status");
    expect(ok(ack) && has(ack, ("\"requestId\":" + std::to_string(nextId - 1) + ",").c_str()),
           "request id echoed");
    Serial.input += "@4294967295 status\n";
    Serial.output.clear();
    pump(120);
    expect(has(findAck(4294967295u), "\"ok\":true"), "largest request id echoed");
    Serial.input += "@4294967296 status\n";
    Serial.output.clear();
    pump(120);
    expect(has(Serial.output, "\"requestId\":0,") && has(Serial.output, "\"ok\":false"),
           "out-of-range request id is refused and not echoed");

    // An overlong line is never executed, even when its prefix is a valid command.
    ack = send("handsfree switchless on" + std::string(100, ' '));
    expect(refused(ack), "overlong line refused");
    expect(has(ack, "\"switchlessStaged\":false"), "overlong line did not run its prefix");
    // CRLF is tolerated; a non-printable byte is not.
    Serial.input += "@900 status\r\n";
    Serial.output.clear();
    pump(120);
    expect(has(findAck(900), "\"ok\":true"), "CRLF line accepted");
    Serial.input += "@901 sta\x01tus\n";
    Serial.output.clear();
    pump(120);
    expect(has(findAck(901), "\"ok\":false"), "control character refused");

    // Staging is explicit and reversible.
    ack = send("handsfree switchless on");
    expect(ok(ack) && has(ack, "\"switchlessStaged\":true"), "switchless staged");
    ack = send("handsfree switchless off");
    expect(ok(ack) && has(ack, "\"switchlessStaged\":false"), "switchless unstaged");
    ack = send("handsfree enable maintained");
    expect(ok(ack) && has(ack, "\"kindStaged\":\"MAINTAINED\""), "maintained kind staged");
    ack = send("handsfree enable momentary");
    expect(ok(ack) && has(ack, "\"kindStaged\":\"MOMENTARY\"") &&
               has(ack, "\"kind\":\"MAINTAINED\""),
           "button kind staged, stored kind unchanged until commit");

    // Backpressure: a burst larger than the acknowledgement queue is parsed as capacity frees,
    // so every command is eventually answered, in order, and none is dropped.
    Serial.output.clear();
    const uint32_t first = nextId;
    for (int i = 0; i < 8; ++i) {
        Serial.input += "@" + std::to_string(nextId++) + " status\n";
    }
    pump(1500);
    unsigned answered = 0;
    size_t lastPosition = 0;
    bool inOrder = true;
    for (uint32_t id = first; id < first + 8; ++id) {
        const std::string key = "\"requestId\":" + std::to_string(id) + ",";
        const size_t position = Serial.output.find(key);
        answered += position != std::string::npos;
        inOrder = inOrder && position != std::string::npos && position > lastPosition;
        lastPosition = position;
    }
    expect(answered == 8, "burst of 8 commands all acknowledged");
    expect(inOrder, "burst acknowledged in order");
}

#ifdef NODX_SIMULATED
// Firmware reboot on the same flash: a fresh engine, persisted storage kept, serial cleared.
void reboot() {
    Serial.input.clear();
    Serial.position = 0;
    Serial.output.clear();
    initializeRuntime();
    pump(300);
}
void playGesture(const char* pattern) {
    const std::string ack = send(std::string("gesture ") + pattern, 250);
    expect(ok(ack), "gesture accepted for playback");
    pump(1500); // 400 ms neutral + strokes + 400 ms neutral, then recognition
}

// Guided calibration needs deliberate head movement in four directions. The simulated sensor is
// driven in-process from the engine's own phase; the `motion` command itself is checked separately.
void runCalibration(unsigned milliseconds) {
    for (unsigned i = 0; i < milliseconds; ++i) {
        std::array<float, 3> gyro{0, 0, 0};
        switch (systemEngine->calibration.phase) {
        case CalPhase::Left:
            gyro = {-30, 0, 0};
            break;
        case CalPhase::Right:
            gyro = {30, 0, 0};
            break;
        case CalPhase::Up:
            gyro = {0, -30, 0};
            break;
        case CalPhase::Down:
            gyro = {0, 30, 0};
            break;
        default:
            break;
        }
        simulator.gyro = gyro;
        pump(1);
    }
    simulator.gyro = {0, 0, 0};
}
void simulatedChecks() {
    expect(ok(status()), "simulated build answers status");
    std::string ack = status();
    expect(has(ack, "\"source\":\"FIRMWARE_SIMULATED\""), "source labelled FIRMWARE_SIMULATED");

    // Simulated raw enable input: starts released; `enable 1` presses / turns ON.
    expect(has(ack, "\"pressed\":false") && has(ack, "\"on\":false"),
           "enable input starts released");
    expect(ok(send("enable 1")) && has(status(), "\"pressed\":true"), "enable 1 presses the input");
    expect(ok(send("enable 0")) && has(status(), "\"pressed\":false"),
           "enable 0 releases the input");
    for (const char* bad : {"enable", "enable 2", "enable -1", "enable x", "enable 1 1"}) {
        expect(refused(send(bad)), bad);
    }
    expect(has(status(), "\"pressed\":false"), "refused enable commands changed nothing");

    // Simulated motion and fault injection stay bounded.
    expect(ok(send("motion 1 2 3")), "motion accepted");
    expect(ok(send("motion 0 0 0")), "motion cleared");
    for (const char* bad : {"motion 1 2", "motion 1001 0 0", "motion 0 -1001 0", "motion nan 0 0",
                            "motion inf 0 0", "motion 0 0 0 0"}) {
        expect(refused(send(bad)), bad);
    }
    expect(ok(send("fault 0")), "fault 0 accepted");
    for (const char* bad : {"fault 6", "fault -1", "fault", "fault x", "fault 0 0"}) {
        expect(refused(send(bad)), bad);
    }

    // Synthetic gesture playback: bounded names and scale, one at a time.
    for (const char* bad :
         {"gesture", "gesture bogus", "gesture nod9", "gesture nod0", "gesture nod2 0.1",
          "gesture nod2 4", "gesture nod2 nan", "gesture nod2 2 extra"}) {
        expect(refused(send(bad)), bad);
    }
    expect(ok(send("gesture nod2", 250)), "gesture playback queued");
    expect(refused(send("gesture nod2", 250)), "second gesture refused while one is playing");
    pump(1500);
    expect(ok(send("gesture tilt1 1.5")), "scaled gesture accepted");
    pump(1500);
}

// Whole firmware pipeline on the simulated sensor: calibrate, train, commit, gesture control,
// the enable button (and the maintained switch option), reboot and fail-closed corruption. No
// selection, pause or calibration buttons exist anywhere in this flow.
void endToEndChecks() {
    // Secure, subscribed host link (set directly: pairing itself is not modelled), then a fresh
    // boot so the transport fault left by the disconnected parser checks is cleared.
    ble.secured = true;
    ble.subscribed = true;
    reboot();
    pump(1500);
    expect(refused(send("resume")), "resume refused before calibration");
    expect(refused(send("calibrate sideways")), "unknown calibrate argument refused");
    expect(ok(send("calibrate guided")), "guided calibration started");
    expect(has(status(), "\"calibrationCueMs\":2"),
           "guided calibration reports its countdown cue");
    expect(ok(send("cancel")), "guided calibration cancelled");
    expect(ok(send("calibrate")), "calibration started");
    runCalibration(12000);
    std::string ack = status();
    expect(has(ack, "\"hasProfile\":true"), "calibration produced a profile");
    expect(has(ack, "\"mode\":\"LEGACY_SWITCH\""), "calibration alone does not enable hands-free");
    expect(has(ack, "\"dwellEnabled\":false"), "profile is not converted by calibration");

    auto train = [](const char* gesture, const char* pattern) {
        expect(ok(send(std::string("train start ") + gesture)), "training started");
        pump(1300); // rest measurement
        for (int example = 0; example < 4; ++example) {
            playGesture(pattern);
        }
        playGesture(pattern); // validation repeat
        const std::string progress = status();
        expect(has(progress, "\"phase\":\"READY\"") && has(progress, "\"validated\":true"),
               "training ready and validated");
        expect(ok(send("train accept")), "training accepted");
    };
    train("pause", "nod2");
    train("drag", "tilt2");
    ack = status();
    expect(has(ack, "\"staged\":[true,true]"), "both gestures staged, not yet saved");
    expect(has(ack, "\"dwellEnabled\":false"), "profile unchanged until the explicit commit");
    expect(has(ack, "\"mode\":\"LEGACY_SWITCH\""), "mode unchanged until the explicit commit");

    ack = send("handsfree commit");
    expect(ok(ack), "commit accepted");
    expect(has(ack, "\"mode\":\"HANDS_FREE\""), "commit enables hands-free");
    expect(has(ack, "\"dwellEnabled\":true"), "commit converts the profile to dwell");
    expect(has(ack, "\"config\":\"VALID\""), "config record saved");
    expect(!has(ack, "\"state\":\"ACTIVE\""), "commit never resumes control");

    // The new setup assumes the push button: disabled until one press, never resumed by it.
    expect(has(ack, "\"kind\":\"MOMENTARY\"") && has(ack, "\"latched\":false") &&
               has(ack, "\"permitted\":false"),
           "a new setup starts with the button kind, disabled");
    pump(500);
    playGesture("nod2");
    expect(!has(status(), "\"state\":\"ACTIVE\""), "resume gesture ignored while disabled");
    expect(refused(send("resume")), "helper resume refused while disabled");
    ack = send("enable 1", 250); // press
    pump(100);
    ack = status();
    expect(has(ack, "\"pressed\":true") && has(ack, "\"latched\":true") &&
               has(ack, "\"permitted\":true"),
           "one press latches permission");
    expect(!has(ack, "\"state\":\"ACTIVE\""), "enabling never resumes control");
    expect(ok(send("enable 0", 250)), "button released");
    pump(100);
    ack = status();
    expect(has(ack, "\"pressed\":false") && has(ack, "\"latched\":true"),
           "raw pressed state and latched permission are separate");
    playGesture("nod2");
    expect(has(status(), "\"state\":\"ACTIVE\""), "explicit gesture resumes after enabling");
    playGesture("tilt2");
    expect(has(status(), "\"drag\":true"), "drag gesture starts a drag");
    ack = send("enable 1", 250); // the disabling press: effect visible in its own acknowledgement
    expect(has(ack, "\"state\":\"PAUSED\"") && has(ack, "\"drag\":false") &&
               has(ack, "\"latched\":false"),
           "the disabling press pauses, releases the drag and clears the latch at once");
    pump(1000);
    expect(has(status(), "\"latched\":false"), "holding the button toggled again");
    expect(ok(send("enable 0", 250)), "button released");
    pump(100);

    // Reboot with the button HELD: permission is never persisted and a held press enables nothing.
    expect(ok(send("enable 1", 250)), "button held for the reboot");
    reboot();
    pump(3000);
    ack = status();
    expect(has(ack, "\"mode\":\"HANDS_FREE\"") && has(ack, "\"kind\":\"MOMENTARY\"") &&
               has(ack, "\"config\":\"VALID\""),
           "hands-free setup and button kind persisted across reboot");
    expect(has(ack, "\"permitted\":false") && has(ack, "\"pressed\":true"),
           "a button held at boot enabled control");
    expect(!has(ack, "\"state\":\"ACTIVE\""), "reboot never resumes control");
    expect(ok(send("enable 0", 250)), "button released after boot");
    pump(100);
    expect(has(status(), "\"permitted\":false"), "release alone enabled control");
    expect(ok(send("enable 1", 250)), "new press after boot");
    pump(100);
    expect(ok(send("enable 0", 250)), "button released");
    pump(100);
    expect(has(status(), "\"permitted\":true") && !has(status(), "\"state\":\"ACTIVE\""),
           "a new press after release enables, without resuming");
    playGesture("nod2");
    expect(has(status(), "\"state\":\"ACTIVE\""), "gesture resumes after reboot and press");

    expect(ok(send("fault 1")), "sensor fault injected");
    pump(400);
    ack = status();
    expect(has(ack, "\"state\":\"SAFE_STATE\"") && has(ack, "\"latched\":false"),
           "sensor fault stops control and drops the button permission");
    expect(ok(send("enable 1", 250)), "press during the fault");
    pump(100);
    expect(ok(send("enable 0", 250)), "release during the fault");
    pump(100);
    expect(has(status(), "\"state\":\"SAFE_STATE\"") && refused(send("resume")),
           "a button press bypassed the fault");
    expect(ok(send("fault 0")), "sensor fault cleared");
    pump(1500);
    expect(!has(status(), "\"state\":\"ACTIVE\""), "fault recovery never resumes control");

    // The maintained switch stays available as an explicit, stored configuration option.
    expect(ok(send("pause")), "helper pause");
    expect(ok(send("handsfree enable maintained")), "maintained kind staged");
    ack = send("handsfree commit");
    expect(ok(ack) && has(ack, "\"kind\":\"MAINTAINED\"") && has(ack, "\"permitted\":false"),
           "maintained kind committed; its switch is released so control is inhibited");
    expect(ok(send("enable 1", 250)), "maintained switch ON");
    pump(200);
    ack = status();
    expect(has(ack, "\"permitted\":true") && has(ack, "\"latched\":false") &&
               !has(ack, "\"state\":\"ACTIVE\""),
           "maintained ON permits without a latch and without resuming");
    playGesture("nod2");
    expect(has(status(), "\"state\":\"ACTIVE\""), "gesture resumes under the maintained switch");
    ack = send("enable 0", 250);
    expect(has(ack, "\"state\":\"PAUSED\"") && has(ack, "\"permitted\":false"),
           "maintained OFF pauses at once");
    reboot();
    pump(300);
    expect(has(status(), "\"kind\":\"MAINTAINED\""), "the maintained kind persisted across reboot");

    // The NVS adapter: oversize records are reported as corrupt, never trusted or ignored.
    const std::vector<uint8_t> oversize(600, 0x5a);
    expect(configStorage.write(0, oversize) && configStorage.write(1, oversize),
           "oversize record written");
    const std::vector<uint8_t> readBack = configStorage.read(0);
    expect(readBack.size() == 1 && readBack[0] == 0xff, "oversize record classified corrupt");
    reboot();
    ack = status();
    expect(has(ack, "\"mode\":\"CONFIG_INVALID\"") && has(ack, "\"config\":\"CORRUPT\""),
           "corrupt record fails closed as CONFIG_INVALID");
    expect(refused(send("resume")), "no control while the record is invalid");
    playGesture("nod2");
    expect(!has(status(), "\"state\":\"ACTIVE\""),
           "no gesture control while the record is invalid");
}
#else
void hardwareChecks() {
    std::string ack = status();
    expect(has(ack, "\"source\":\"HARDWARE\""), "hardware build is not labelled simulated");
    expect(!has(ack, "FIRMWARE_SIMULATED"), "no simulated label");
    // Hardware frames carry the raw sensor view; the whole frame must stay well inside the
    // 5120-byte telemetry buffer, otherwise the firmware silently drops it.
    expect(has(ack, "\"sensor\":{\"variant\":\"UNKNOWN\",\"seen\":false"),
           "hardware telemetry has the raw sensor block");
    expect(ack.size() < 4400, "hardware telemetry frame leaves headroom in the 5120-byte buffer");
    std::printf("INFO hardware status frame is %zu bytes\n", ack.size());
    expect(has(ack, "\"present\":false") && has(ack, "\"permitted\":false"),
           "no enable pin configured: control stays inhibited");
    expect(has(ack, "\"axes\":{\"valid\":"), "hardware telemetry reports the active axis mapping");
    expect(has(ack, "\"uncalDemo\":{\"active\":false"), "uncalibrated demo is off at boot");
    expect(has(ack, "\"reset\":\""), "hardware telemetry reports the last reset reason");
    expect(refused(send("handsfree uncal start")), "uncalibrated demo refused without a button");
    expect(refused(send("handsfree uncal sideways")), "bad uncalibrated demo argument refused");
    expect(refused(send("handsfree uncal start extra")), "trailing text refused");
    expect(refused(send("handsfree uncal dwell on")), "dwell needs a running demo");
    expect(refused(send("handsfree uncal dwell sideways")), "bad dwell action refused");
    expect(refused(send("handsfree uncal dwell set 400 8")), "dwell below 500 ms refused");
    expect(refused(send("handsfree uncal dwell set 1200 1")), "tolerance below 2 refused");
    expect(refused(send("handsfree uncal dwell set 1200")), "missing tolerance refused");
    expect(refused(send("handsfree uncal dwell set 1200 8 9")), "trailing value refused");
    expect(ok(send("handsfree uncal dwell set 1500 10")), "valid dwell settings accepted");
    expect(has(status(), "\"ms\":1500,\"tolerance\":10.0"), "settings reported");
    expect(ok(send("handsfree uncal reverse 1 0")), "reversal accepted");
    expect(has(status(), "\"reverseX\":true,\"reverseY\":false"), "reversal reported");
    expect(refused(send("handsfree uncal reverse 2 0")), "bad reversal value refused");
    expect(refused(send("handsfree uncal reverse 1")), "missing reversal value refused");
    expect(ok(send("handsfree uncal reverse 0 0")), "reversal cleared");
    expect(refused(send("map start")), "teaching refused before the sensor is healthy");
    expect(refused(send("map accept")), "accept needs a preview");
    expect(refused(send("map save")), "nothing to save");
    expect(refused(send("map sideways")), "bad map verb refused");
    expect(refused(send("map start now")), "trailing text refused");
    expect(ok(send("map cancel")) && ok(send("map clear")), "cancel and clear are always accepted");
    expect(refused(send("control start")), "configured control needs a learned mapping");
    expect(refused(send("control sideways")), "bad control verb refused");
    expect(ok(send("control stop")), "stop is always accepted");
    expect(has(status(), "\"mapping\":{\"phase\":\"IDLE\""), "mapping status reported");
    expect(has(status(), "\"stored\":\"MISSING\""), "no stored mapping");
    expect(refused(send("click train fallback")), "click training refused before the sensor is healthy");
    expect(refused(send("click train configured")), "configured gesture needs a learned mapping");
    expect(refused(send("click train sideways")), "bad click frame refused");
    expect(refused(send("click enable on")), "click enable needs a taught gesture and a session");
    expect(refused(send("click enable sideways")), "bad click enable value refused");
    expect(refused(send("click accept")), "nothing to accept");
    expect(refused(send("click sideways")), "bad click verb refused");
    expect(ok(send("click cancel")) && ok(send("click clear")) && ok(send("click enable off")),
           "cancel, clear and disable are always accepted");
    expect(has(status(), "\"click\":{\"phase\":\"IDLE\""), "click status reported");
    expect(refused(send("quick practice fallback")), "quick practice refused before the sensor is healthy");
    expect(refused(send("quick practice configured")), "configured quick practice needs a learned mapping");
    expect(refused(send("quick practice sideways")), "bad quick frame refused");
    expect(refused(send("quick enable on")), "quick enable needs a practice and a session");
    expect(refused(send("quick enable sideways")), "bad quick enable value refused");
    expect(refused(send("quick accept")), "nothing to accept");
    expect(refused(send("quick set 0.4 0.3")) && refused(send("quick set 1 0.7")) &&
               refused(send("quick set 1")) && refused(send("quick set 1 0.3 1")),
           "invalid quick settings refused");
    expect(ok(send("quick set 1.5 0.25")) && has(status(), "\"sensitivity\":1.50,\"returnTolerance\":0.25"),
           "valid quick settings accepted and reported");
    expect(ok(send("quick set 1.2 0.3 40")) && has(status(), "\"directionTolerance\":40"),
           "the direction tolerance is a validated setting");
    expect(refused(send("quick set 1 0.3 5")) && refused(send("quick set 1 0.3 75")) &&
               refused(send("quick set 1 0.3 40 1")),
           "an out-of-range direction tolerance is refused");
    expect(ok(send("quick set 1 0.35 30")), "defaults restored");
    expect(ok(send("quick cancel")) && ok(send("quick clear")) && ok(send("quick retry")) &&
               ok(send("quick enable off")),
           "cancel, clear, retry and disable are always accepted");
    expect(has(status(), "\"quick\":{\"phase\":\"IDLE\""), "quick status reported");
    // Dwell action palette: validated commands, refused without a running session, status reported.
    expect(refused(send("actions enable on")), "the action palette needs a running session");
    expect(refused(send("actions enable maybe")) && refused(send("actions enable")) &&
               refused(send("actions enable on now")) && refused(send("actions")) &&
               refused(send("actions bogus on")),
           "malformed action palette commands refused");
    expect(refused(send("actions hover left")), "hover is ignored (refused) while the palette is off");
    expect(refused(send("actions hover middle")) && refused(send("actions hover")) &&
               refused(send("actions hover left extra")),
           "bad hover targets refused");
    expect(refused(send("actions hover drop")) && refused(send("actions hover cancel")) &&
               refused(send("actions hover frame")),
           "new hover targets are parsed but refused while the palette is off");
    expect(refused(send("actions keep on")) && refused(send("actions keep off")) &&
               refused(send("actions keep maybe")) && refused(send("actions keep")),
           "keep selected action needs the palette");
    for (const char* bad : {"handsfree uncal speed", "handsfree uncal speed 0.2", "handsfree uncal speed 2.1",
                            "handsfree uncal speed x", "handsfree uncal speed nan", "handsfree uncal speed 1 1"}) {
        expect(refused(send(bad)), bad);
    }
    expect(ok(send("handsfree uncal speed 0.5")) && has(status(), "\"speed\":0.50"),
           "a valid pointer speed is accepted and reported");
    expect(ok(send("handsfree uncal speed 1")) && has(status(), "\"speed\":1.00"), "speed reset");
    // Desktop overlay controller: validated, refused without a session or the claim; every command needs its token.
    expect(refused(send("actions overlay on 1 1000")), "the overlay needs a running session");
    for (const char* bad :
         {"actions overlay", "actions overlay maybe", "actions overlay on", "actions overlay on 1",
          "actions overlay on 1 0", "actions overlay on 1 1000 now", "actions overlay on -1 1000",
          "actions overlay on 1 4294967296", "actions overlay on x 1000", "actions overlay off",
          "actions menu", "actions menu maybe 5", "actions menu open", "actions menu open 5 now",
          "actions select", "actions select middle 5", "actions select left", "actions select left 5 now",
          "actions select frame 5", "actions select none 5", "actions keyboard", "actions keyboard on",
          "actions keyboard maybe 5"}) {
        expect(refused(send(bad)), bad);
    }
    expect(refused(send("actions menu open 5")) && refused(send("actions menu close 5")) &&
               refused(send("actions select left 5")) && refused(send("actions select stop 5")) &&
               refused(send("actions keyboard on 5")),
           "overlay commands refused without the overlay claim");
    expect(ok(send("actions overlay off 5")), "releasing an unclaimed overlay is accepted");
    expect(has(status(), "\"controller\":\"NONE\"") && has(status(), "\"menu\":false") &&
               has(status(), "\"keyboard\":false"),
           "controller fields reported");
    expect(ok(send("actions enable off")), "turning the palette off is always accepted");
    expect(has(status(), "\"actions\":{\"enabled\":false,\"mode\":\"LEFT\""),
           "action palette status reported");
    expect(refused(send("capture start 0")) && refused(send("capture start 21")) &&
               refused(send("capture start")) && refused(send("capture start 5 now")),
           "capture length is bounded to 1-20 seconds");
    expect(refused(send("capture get 0")), "nothing to read before a capture");
    expect(refused(send("capture sideways")), "bad capture verb refused");
    expect(ok(send("capture start 5")) && has(status(), "\"capture\":{\"active\":true"),
           "capture starts and is reported");
    expect(ok(send("capture stop")) && has(status(), "\"capture\":{\"active\":false,\"count\":0"),
           "capture stops");
    expect(has(status(), "\"capacity\":2000"), "capture capacity reported");
    expect(ok(send("handsfree uncal stop")), "stop is always accepted");
    expect(has(status(), "\"uncalDemo\":{\"active\":false"), "still off after stop");
    // Simulation-only commands do not exist on hardware.
    for (const char* command :
         {"enable 1", "enable 0", "gesture nod2", "motion 0 0 0", "fault 0"}) {
        expect(refused(send(command)), command);
    }
    expect(has(status(), "\"present\":false"), "refused commands changed nothing");
    // Without a sensor the engine never reaches control (fail closed).
    pump(3000);
    ack = status();
    expect(!has(ack, "\"state\":\"ACTIVE\""), "no sensor, no control");
    expect(refused(send("resume")), "resume refused without a profile or sensor");
    expect(refused(send("handsfree commit")), "commit refused with nothing trained");
}
#endif
} // namespace

int main() {
    initializeRuntime();
    expect(has(Serial.output, "[NODX] 0.2.0 pre-hardware"), "boot banner printed");
#ifdef NODX_SIMULATED
    expect(has(Serial.output, "[INPUT] simulated"), "simulated input announced");
#else
    expect(!has(Serial.output, "[INPUT] simulated"), "hardware build does not announce simulation");
#endif
    pump(300);
    parserChecks();
#ifdef NODX_SIMULATED
    simulatedChecks();
    endToEndChecks();
#else
    hardwareChecks();
#endif
    std::printf("%u firmware command checks, %u failed\n", checks, failures);
    return failures ? 1 : 0;
}
