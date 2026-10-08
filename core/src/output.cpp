#include "nodx/output.hpp"
#include <algorithm>
#include <cmath>

namespace nodx {
Command SafetyManager::gate(Command command, bool active, bool healthy, bool valid,
                            bool connected) {
    calculationFault =
        !std::isfinite(command.dx) || !std::isfinite(command.dy) || !std::isfinite(command.wheel);
    if (calculationFault || !active || !healthy || !valid || !connected) {
        return {};
    }
    command.dx = std::clamp(command.dx, -float(start::maxPointer), float(start::maxPointer));
    command.dy = std::clamp(command.dy, -float(start::maxPointer), float(start::maxPointer));
    command.wheel = std::clamp(command.wheel, -float(start::maxWheel), float(start::maxWheel));
    return command;
}
void HIDManager::reset() {
    remainderX_ = remainderY_ = remainderWheel_ = 0;
}
bool HIDManager::emit(Command command) {
    auto quantize = [](float value, float& remainder, int bound) {
        float total = std::clamp(value + remainder, -float(bound), float(bound));
        int whole = int(total);
        remainder = total - whole;
        return static_cast<int8_t>(whole);
    };
    const bool secondary = command.pulse && command.right;
    last = {quantize(command.dx, remainderX_, start::maxPointer),
            quantize(command.dy, remainderY_, start::maxPointer),
            quantize(command.wheel, remainderWheel_, start::maxWheel),
            (command.down || command.pulse) && !secondary, secondary};
    if (!transport_.connected()) {
        reset();
        last = {};
        idleSent_ = false;
        return false;
    }
    const bool idle = last.dx == 0 && last.dy == 0 && last.wheel == 0 && !last.down && !last.right;
    if (idle && idleSent_ && !command.pulse) {
        // Nothing changed since the all-zero report the host already has. Repeating it every
        // 10 ms floods a BLE link that carries far fewer notifications per second.
        return true;
    }
    bool sent = transport_.send(last);
    idleSent_ = sent && idle;
    if (command.pulse) {
        // Every press is followed by its own release; a failed release fails the whole emit, which the
        // System turns into the inhibiting transport fault.
        const unsigned pairs = command.twice ? 2 : 1;
        for (unsigned pair = 0; pair < pairs; ++pair) {
            if (pair > 0) {
                Report again = last;
                again.dx = again.dy = again.wheel = 0;
                sent = transport_.send(again) && sent;
            }
            Report release{};
            const bool released = transport_.send(release);
            sent = sent && released;
            idleSent_ = released;
        }
    }
    return sent;
}
} // namespace nodx
