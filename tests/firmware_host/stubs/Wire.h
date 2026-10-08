#pragma once
#include <cstdint>
// I2C stub: every transaction fails, as with no sensor attached. The firmware tests that use
// it run in the simulated-sensor configuration, which never reads this bus.
struct HostWire {
    bool begin(int, int, unsigned) {
        return true;
    }
    void setTimeOut(unsigned) {}
    void beginTransmission(uint8_t) {}
    size_t write(uint8_t) {
        return 1;
    }
    uint8_t endTransmission(bool = true) {
        return 4;
    }
    uint8_t requestFrom(uint8_t, uint8_t) {
        return 0;
    }
    int read() {
        return 0;
    }
};
extern HostWire Wire;
