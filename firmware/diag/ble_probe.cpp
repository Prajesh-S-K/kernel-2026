// NodX Adapt BENCH BLE PROBE (bring-up stage 4). Uses the firmware's REAL BLE HID adapter (BLEHID
// in firmware/src/adapters.hpp) but none of the control engine: it never moves the host pointer by
// itself. Reports are sent ONLY when a serial command asks for one. Pair the board from the host's
// Bluetooth settings first, then watch the connection events below.
//
// Commands (native USB serial):  help | status | bonds | ping | nudge | down confirm | up | adv
//   ping          one report with no movement and no buttons (tests delivery only)
//   nudge         +2 then -2 pixels on X (net zero), so the host pointer does not drift
//   down confirm  presses the LEFT BUTTON on the host; it stays down until `up` or a disconnect
//   test up            releases the button adv           (re)starts advertising
// Every line starts with "BLE," and is key=value so scripts/bench_analyze.py can read it.
#include "adapters.hpp"

namespace {
BLEHID ble;
bool lastConnected = false, lastSecured = false, lastSubscribed = false, buttonDown = false;
bool commandSeen = false;
uint32_t lastHeartbeat = 0;
uint32_t sentOk = 0, sentFailed = 0;
String command;

void state(const char* why) {
    Serial.printf("BLE,state,why=%s,atMs=%lu,secured=%d,subscribed=%d,connected=%d,buttonDown=%d\n",
                  why, static_cast<unsigned long>(millis()), int(ble.secured.load()),
                  int(ble.subscribed.load()), int(ble.connected()), int(buttonDown));
}
void send(int8_t dx, bool down, const char* what) {
    Report report;
    report.dx = dx;
    report.dy = 0;
    report.wheel = 0;
    report.down = down;
    const bool ok = ble.send(report);
    (ok ? sentOk : sentFailed) += 1;
    Serial.printf("BLE,send,what=%s,delivered=%d,sentOk=%lu,sentFailed=%lu\n", what, int(ok),
                  static_cast<unsigned long>(sentOk), static_cast<unsigned long>(sentFailed));
}
} // namespace

void setup() {
    // Native USB CDC: size the TX buffer first; anything printed before the host opens the port is
    // lost, so a heartbeat repeats the banner until the first command arrives.
    Serial.setTxBufferSize(4096);
    Serial.begin(115200);
    delay(1500);
    Serial.println(
        "BLE,banner,name=NodX-bench-ble-probe,stage=4,reports=only-on-command,engine=none");
    ble.begin();
    state("boot-advertising");
    Serial.println("BLE,help,commands=help|status|bonds|ping|nudge|down confirm|up|adv");
}
void loop() {
    if (!commandSeen && millis() - lastHeartbeat >= 3000) {
        lastHeartbeat = millis();
        state("idle-heartbeat");
    }
    // Connection events are polled from the adapter's own flags; the adapter itself is unchanged.
    const bool connected = ble.connected();
    if (connected != lastConnected || ble.secured != lastSecured ||
        ble.subscribed != lastSubscribed) {
        if (lastConnected && !connected && buttonDown) {
            Serial.println("BLE,warn,link-lost-with-button-down=host-side-release-must-be-checked");
        }
        lastConnected = connected;
        lastSecured = ble.secured;
        lastSubscribed = ble.subscribed;
        state("link-change");
    }
    while (Serial.available()) {
        const char c = char(Serial.read());
        if (c != '\n' && c != '\r') {
            if (command.length() < 40) {
                command += c;
            }
            continue;
        }
        command.trim();
        if (command.length()) {
            commandSeen = true;
        }
        if (command == "status") {
            state("status");
        } else if (command == "bonds") {
            Serial.printf("BLE,bonds,stored=%d\n", NimBLEDevice::getNumBonds());
        } else if (command == "ping") {
            send(0, false, "ping");
        } else if (command == "nudge") {
            send(2, buttonDown, "nudge+2");
            delay(50);
            send(-2, buttonDown, "nudge-2");
        } else if (command == "down confirm") {
            buttonDown = true;
            send(0, true, "button-down");
        } else if (command == "up") {
            buttonDown = false;
            send(0, false, "button-up");
        } else if (command == "adv") {
            NimBLEDevice::startAdvertising();
            Serial.println("BLE,adv,restarted=1");
        } else if (command.length()) {
            Serial.println("BLE,help,commands=help|status|bonds|ping|nudge|down confirm|up|adv");
        }
        command = "";
    }
    delay(5);
}
