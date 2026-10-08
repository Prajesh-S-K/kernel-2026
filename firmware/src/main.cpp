#include "runtime.hpp"
#include <esp_system.h>

namespace {
const char* resetName(esp_reset_reason_t reason) {
    switch (reason) {
    case ESP_RST_POWERON:
        return "POWERON";
    case ESP_RST_SW:
        return "SOFTWARE";
    case ESP_RST_PANIC:
        return "PANIC";
    case ESP_RST_INT_WDT:
        return "INTERRUPT_WDT";
    case ESP_RST_TASK_WDT:
        return "TASK_WDT";
    case ESP_RST_WDT:
        return "OTHER_WDT";
    case ESP_RST_BROWNOUT:
        return "BROWNOUT";
    default:
        return "OTHER";
    }
}
} // namespace

void setup() {
    initializeRuntime();
    bootResetReason = resetName(esp_reset_reason());
    Serial.printf("[BOOT] reset reason: %s\n", bootResetReason);
    // A hung control loop reboots the board (and the reason is reported) instead of staying silent.
    enableLoopWDT();
}
void loop() {
    serviceRuntime();
}
