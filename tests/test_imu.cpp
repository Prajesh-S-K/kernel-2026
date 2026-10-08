// IMU identity, initialisation, read-back and conversion tests for the MPU-6050 and MPU-6500
// variants. The register values come from the manufacturer register maps (RM-MPU-6000A-00 rev 4.0
// and RM-MPU-6500A-00 rev 2.1) and the MPU-6500 product specification rev 1.3; the frame fixture is
// a real frame captured on the bench. Nothing here qualifies the physical sensor.
#include "nodx/sensor.hpp"
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using namespace nodx;
namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
bool near(double a, double b, double eps) {
    return std::abs(a - b) <= eps;
}
// A register file with fault injection. Reads return what was written (or the preset), like a chip.
class FakeBus : public RegisterBus {
public:
    uint8_t regs[256] = {};
    std::vector<std::pair<uint8_t, uint8_t>> writes;
    std::map<uint8_t, uint8_t> stuck; // register -> value it always reads back, ignoring writes
    bool nackRead = false, nackWrite = false;
    int failWriteAfter = -1, failReadsFrom = -1; // counts
    int readCount = 0;
    explicit FakeBus(uint8_t who) {
        regs[0x75] = who;
        regs[0x6b] = who == 0x68 ? 0x40 : 0x01; // documented reset values (6050 asleep, 6500 awake)
    }
    bool write(uint8_t reg, uint8_t value) override {
        if (nackWrite || (failWriteAfter >= 0 && int(writes.size()) >= failWriteAfter)) {
            return false;
        }
        writes.emplace_back(reg, value);
        if (!stuck.count(reg)) {
            regs[reg] = value;
        }
        return true;
    }
    bool read(uint8_t reg, uint8_t* bytes, size_t count) override {
        ++readCount;
        if (nackRead || (failReadsFrom >= 0 && readCount > failReadsFrom)) {
            return false;
        }
        for (size_t i = 0; i < count; ++i) {
            const uint8_t at = uint8_t(reg + i);
            bytes[i] = stuck.count(at) ? stuck[at] : regs[at];
        }
        return true;
    }
    bool wrote(uint8_t reg) const {
        for (const auto& item : writes) {
            if (item.first == reg) {
                return true;
            }
        }
        return false;
    }
    uint8_t lastWrite(uint8_t reg) const {
        for (auto it = writes.rbegin(); it != writes.rend(); ++it) {
            if (it->first == reg) {
                return it->second;
            }
        }
        throw std::runtime_error("register was never written");
    }
    void setFrame(const uint8_t (&frame)[14]) {
        for (size_t i = 0; i < 14; ++i) {
            regs[0x3b + i] = frame[i];
        }
    }
};
} // namespace

int main() {
    unsigned passed = 0, failed = 0;
    auto test = [&](const char* label, const std::function<void()>& body) {
        try {
            body();
            ++passed;
            std::printf("PASS %s\n", label);
        } catch (const std::exception& e) {
            ++failed;
            std::printf("FAIL %s: %s\n", label, e.what());
        }
    };

    // ------------------------------------------------------------------ identity
    test("0x68 and 0x70 are distinct supported variants", [] {
        require(identifyImu(0x68) == ImuVariant::Mpu6050, "0x68 is the MPU-6000/6050");
        require(identifyImu(0x70) == ImuVariant::Mpu6500, "0x70 is the MPU-6500");
        require(identifyImu(0x68) != identifyImu(0x70), "variants must differ");
        require(std::string(name(ImuVariant::Mpu6050)) == "MPU-6050" &&
                    std::string(name(ImuVariant::Mpu6500)) == "MPU-6500" &&
                    std::string(name(ImuVariant::Unknown)) == "UNKNOWN",
                "names");
    });
    test("every other identity value is rejected", [] {
        for (unsigned who = 0; who < 256; ++who) {
            if (who == 0x68 || who == 0x70) {
                continue;
            }
            require(identifyImu(uint8_t(who)) == ImuVariant::Unknown,
                    "an unknown identity was accepted");
        }
    });
    test("an unknown identity is never configured, never ready, never supplies motion", [] {
        for (uint8_t who : {uint8_t(0x00), uint8_t(0xff), uint8_t(0x69), uint8_t(0x71),
                            uint8_t(0x72), uint8_t(0x98), uint8_t(0x75), uint8_t(0x19)}) {
            FakeBus bus(who);
            MPU6050Sensor sensor(bus);
            require(!sensor.begin(), "begin accepted an unknown part");
            require(bus.writes.empty(), "an unknown part was written to");
            require(sensor.variant() == ImuVariant::Unknown, "variant set");
            bus.regs[0x3a] = 1;
            require(!sensor.read(10).valid, "an unknown part supplied motion");
        }
    });
    test("the identity read failing, or the bus being absent, is not ready", [] {
        FakeBus bus(0x70);
        bus.nackRead = true;
        MPU6050Sensor sensor(bus);
        require(!sensor.begin() && bus.writes.empty(), "identity NACK");
        FakeBus none(0x70);
        none.nackWrite = true;
        MPU6050Sensor second(none);
        require(!second.begin() && !second.read(0).valid, "write NACK");
    });

    // --------------------------------------------------------------- initialisation
    test("MPU-6050 initialisation writes the documented values and not the 6500-only register", [] {
        FakeBus bus(0x68);
        MPU6050Sensor sensor(bus);
        require(sensor.begin() && sensor.variant() == ImuVariant::Mpu6050, "begin");
        require(bus.lastWrite(0x6b) == 0x01,
                "PWR_MGMT_1: SLEEP clear (reset value 0x40), CLKSEL=1");
        require(bus.lastWrite(0x1a) == 0x03, "CONFIG DLPF_CFG=3");
        require(bus.lastWrite(0x19) == 0x09, "SMPLRT_DIV 9 -> 100 Hz from the 1 kHz DLPF rate");
        require(bus.lastWrite(0x1b) == 0x00, "GYRO_CONFIG +-250 dps");
        require(bus.lastWrite(0x1c) == 0x00, "ACCEL_CONFIG +-2 g");
        require(bus.lastWrite(0x38) == 0x01, "INT_ENABLE data ready");
        require(!bus.wrote(0x1d), "0x1D is reserved on the MPU-6050 and must not be written");
        require(bus.writes.size() == 6, "exactly the six documented writes");
    });
    test("MPU-6500 initialisation adds the accelerometer filter register", [] {
        FakeBus bus(0x70);
        MPU6050Sensor sensor(bus);
        require(sensor.begin() && sensor.variant() == ImuVariant::Mpu6500, "begin");
        require(bus.lastWrite(0x6b) == 0x01 && bus.lastWrite(0x1a) == 0x03 &&
                    bus.lastWrite(0x19) == 0x09 && bus.lastWrite(0x1b) == 0x00 &&
                    bus.lastWrite(0x1c) == 0x00 && bus.lastWrite(0x38) == 0x01,
                "the shared registers");
        require(bus.lastWrite(0x1d) == 0x03, "ACCEL_CONFIG_2: ACCEL_FCHOICE_B=0, A_DLPF_CFG=3");
        require((bus.lastWrite(0x1b) & 0x03) == 0,
                "FCHOICE_B=00 keeps the DLPF the CONFIG value needs");
        require(bus.writes.size() == 7, "exactly seven writes");
    });
    test("no write to the chip happens before its identity is accepted and none resets it", [] {
        FakeBus bus(0x70);
        MPU6050Sensor sensor(bus);
        require(sensor.begin(), "begin");
        require(bus.writes.front().first == 0x6b && bus.writes.front().second == 0x01,
                "the first write is the wake");
        for (const auto& item : bus.writes) {
            require(!(item.first == 0x6b && (item.second & 0x80)), "DEVICE_RESET must not be set");
        }
    });
    test("re-probing after a loss re-initialises cleanly (the firmware calls begin() again)", [] {
        FakeBus bus(0x70);
        MPU6050Sensor sensor(bus);
        require(sensor.begin(), "first");
        bus.nackRead = true;
        require(!sensor.begin() && !sensor.read(1).valid, "lost");
        bus.nackRead = false;
        require(sensor.begin() && sensor.variant() == ImuVariant::Mpu6500, "recovered");
    });

    // -------------------------------------------------------------------- read-back
    test("a register that does not take its value fails begin() for every checked register", [] {
        for (uint8_t who : {uint8_t(0x68), uint8_t(0x70)}) {
            std::vector<uint8_t> checked = {0x6b, 0x1a, 0x19, 0x1b, 0x1c, 0x38};
            if (who == 0x70) {
                checked.push_back(0x1d);
            }
            for (uint8_t reg : checked) {
                FakeBus bus(who);
                bus.stuck[reg] = 0x5a; // reads back something else
                MPU6050Sensor sensor(bus);
                require(!sensor.begin(), "a stuck register was accepted");
                require(!sensor.read(10).valid && sensor.variant() == ImuVariant::Unknown,
                        "a half-configured sensor supplied motion");
            }
        }
    });
    test("a write that is acknowledged but silently ignored is caught by the read-back", [] {
        FakeBus bus(0x68); // the 6050 resets asleep (0x40); if the wake is ignored it stays asleep
        bus.stuck[0x6b] = 0x40;
        MPU6050Sensor sensor(bus);
        require(!sensor.begin(), "a sensor that stayed asleep was accepted");
    });
    test("a failure part-way through the writes leaves the sensor not ready", [] {
        for (int after = 0; after < 6; ++after) {
            FakeBus bus(0x70);
            bus.failWriteAfter = after;
            MPU6050Sensor sensor(bus);
            require(!sensor.begin() && !sensor.read(1).valid, "partial write accepted");
        }
        FakeBus bus(0x70);
        MPU6050Sensor sensor(bus);
        bus.failReadsFrom = 3; // identity + two read-backs succeed, then the bus fails
        require(!sensor.begin(), "read-back bus failure accepted");
    });

    // -------------------------------------------------------------------- conversions
    test("raw words convert to g and deg/s with the documented scale factors on both variants", [] {
        for (uint8_t who : {uint8_t(0x68), uint8_t(0x70)}) {
            FakeBus bus(who);
            MPU6050Sensor sensor(bus);
            require(sensor.begin(), "begin");
            bus.regs[0x3a] = 0x01;
            // ax=+16384 (1 g), ay=-16384 (-1 g), az=+8192 (0.5 g), temp=0, gx=+131, gy=-131,
            // gz=-262
            const uint8_t frame[14] = {0x40, 0x00, 0xc0, 0x00, 0x20, 0x00, 0x00,
                                       0x00, 0x00, 0x83, 0xff, 0x7d, 0xfe, 0xfa};
            bus.setFrame(frame);
            const MotionSample row = sensor.read(10);
            require(row.valid, "valid frame");
            require(near(row.accel[0], 1.0, 1e-6) && near(row.accel[1], -1.0, 1e-6) &&
                        near(row.accel[2], 0.5, 1e-6),
                    "16384 LSB/g");
            require(near(row.gyro[0], 1.0, 1e-6) && near(row.gyro[1], -1.0, 1e-6) &&
                        near(row.gyro[2], -2.0, 1e-6),
                    "131 LSB/(deg/s)");
        }
    });
    test("a frame captured on the bench decodes to the physically expected values", [] {
        // Real frame (still board, Z axis down): accel x/y/z, temp, gyro x/y/z, big-endian.
        const uint8_t frame[14] = {0xfe, 0x88, 0x00, 0xbc, 0xc1, 0x78, 0x0d,
                                   0x7f, 0xff, 0x50, 0xff, 0x96, 0xff, 0xee};
        FakeBus bus(0x70);
        MPU6050Sensor sensor(bus);
        require(sensor.begin(), "begin");
        bus.regs[0x3a] = 1;
        bus.setFrame(frame);
        const MotionSample row = sensor.read(10);
        require(row.valid, "valid");
        require(near(row.accel[0], -376 / 16384.0, 1e-5) &&
                    near(row.accel[1], 188 / 16384.0, 1e-5) &&
                    near(row.accel[2], -16008 / 16384.0, 1e-5),
                "accel");
        const double magnitude =
            std::sqrt(double(row.accel[0]) * row.accel[0] + double(row.accel[1]) * row.accel[1] +
                      double(row.accel[2]) * row.accel[2]);
        require(near(magnitude, 1.0, 0.03), "a still board reads about 1 g");
        require(near(row.gyro[0], -176 / 131.0, 1e-4) && near(row.gyro[1], -106 / 131.0, 1e-4) &&
                    near(row.gyro[2], -18 / 131.0, 1e-4),
                "gyro");
    });
    test("temperature uses the variant's own documented formula", [] {
        require(near(imuTemperatureC(ImuVariant::Mpu6500, 0), 21.0, 1e-4), "6500: 0 LSB is 21 C");
        require(near(imuTemperatureC(ImuVariant::Mpu6500, 3442), 3442 / 333.87 + 21, 1e-3),
                "6500 raw 3442");
        require(near(imuTemperatureC(ImuVariant::Mpu6050, 0), 36.53, 1e-4),
                "6050: 0 LSB is 36.53 C");
        require(near(imuTemperatureC(ImuVariant::Mpu6050, -3400), -3400 / 340.0 + 36.53, 1e-3),
                "6050 negative");
        require(std::isnan(imuTemperatureC(ImuVariant::Unknown, 0)),
                "unknown variant has no temperature");
        // The two formulas disagree at the bench value, which is why the variant matters.
        require(std::abs(imuTemperatureC(ImuVariant::Mpu6500, 3442) -
                         imuTemperatureC(ImuVariant::Mpu6050, 3442)) > 10,
                "formulas differ");
    });

    // -------------------------------------------------------------- reading and gating
    test("a still sensor is valid however many identical frames it produces", [] {
        FakeBus bus(0x70);
        MPU6050Sensor sensor(bus);
        require(sensor.begin(), "begin");
        const uint8_t frame[14] = {0, 0, 0, 0, 0x40, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        bus.setFrame(frame);
        for (unsigned i = 0; i < 1000; ++i) {
            bus.regs[0x3a] = 1;
            const MotionSample row = sensor.read(10 * (i + 1));
            require(row.valid && near(row.accel[2], 1.0, 1e-6) && row.gyro[0] == 0,
                    "identical frames rejected");
        }
    });
    test("data-ready gating is unchanged: no ready bit means no frame, not a fault", [] {
        FakeBus bus(0x70);
        MPU6050Sensor sensor(bus);
        require(sensor.begin(), "begin");
        bus.regs[0x3a] = 0;
        require(!sensor.read(10).valid, "no ready bit");
        bus.regs[0x3a] = 1;
        require(sensor.read(20).valid, "the sensor stayed ready after an empty poll");
    });
    test("a read error during a frame makes the sensor not ready until re-initialised", [] {
        FakeBus bus(0x68);
        MPU6050Sensor sensor(bus);
        require(sensor.begin(), "begin");
        bus.regs[0x3a] = 1;
        bus.failReadsFrom = bus.readCount + 1; // the status read works, the frame read fails
        require(!sensor.read(10).valid, "failing frame read");
        bus.failReadsFrom = -1;
        require(!sensor.read(20).valid, "the sensor must stay not ready until begin() succeeds");
        require(sensor.begin(), "re-initialised");
        bus.regs[0x3a] = 0;
        require(!sensor.read(30).valid, "no ready bit yet");
        bus.regs[0x3a] = 1;
        require(sensor.read(40).valid, "valid again after re-initialisation");
    });

    std::printf("%u passed, %u failed\n", passed, failed);
    return failed ? 1 : 0;
}
