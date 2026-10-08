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
    last = {quantize(command.dx, remainderX_, start::maxPointer),
            quantize(command.dy, remainderY_, start::maxPointer),
            quantize(command.wheel, remainderWheel_, start::maxWheel),
            command.down || command.pulse};
    if (!transport_.connected()) {
        reset();
        last = {};
        return false;
    }
    bool sent = transport_.send(last);
    if (command.pulse) {
        Report release{};
        bool released = transport_.send(release);
        sent = sent && released;
    }
    return sent;
}
} // namespace nodx
