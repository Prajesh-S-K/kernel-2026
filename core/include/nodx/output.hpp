#pragma once
#include "motion.hpp"
#include "selection.hpp"

namespace nodx {
struct Command {
    float dx = 0, dy = 0, wheel = 0;
    bool down = false, pulse = false;
};
class InteractionEngine {
public:
    Command compose(const Intent& intent, const Selection& selection) const {
        return {intent.dx, intent.dy, intent.wheel, selection.down, selection.pulse};
    }
};
struct Report {
    int8_t dx = 0, dy = 0, wheel = 0;
    bool down = false;
};
class SafetyManager {
public:
    Command gate(Command command, bool active, bool healthy, bool profileValid, bool connected);
    bool calculationFault = false;
};
class HIDTransport {
public:
    virtual ~HIDTransport() = default;
    virtual bool connected() const = 0;
    virtual bool send(const Report& report) = 0;
};
class HIDManager {
public:
    explicit HIDManager(HIDTransport& transport) : transport_(transport) {}
    bool emit(Command safeCommand);
    void reset();
    // The next emit() must reach the transport even if it is an idle (all-zero) report. Used by
    // explicit stops so a release is always sent.
    void requireReport() {
        idleSent_ = false;
    }
    Report last;

private:
    HIDTransport& transport_;
    float remainderX_ = 0, remainderY_ = 0, remainderWheel_ = 0;
    bool idleSent_ = false; // the host already holds an all-zero report: do not repeat it
};
} // namespace nodx
