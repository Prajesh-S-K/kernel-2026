#pragma once
#include "nodx/engine.hpp"
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <Wire.h>
#include <atomic>

// Disabled defaults prevent accidental GPIO use before board/pin qualification.
#ifndef NODX_SDA
#define NODX_SDA -1
#endif
#ifndef NODX_SCL
#define NODX_SCL -1
#endif
#ifndef NODX_SWITCH
#define NODX_SWITCH -1
#endif
#ifndef NODX_PAUSE
#define NODX_PAUSE -1
#endif
#ifndef NODX_CALIBRATE
#define NODX_CALIBRATE -1
#endif
#ifndef NODX_BUZZER
#define NODX_BUZZER -1
#endif
// Control-enable input to GND (active = LOW): a momentary push button (default setup) or a
// maintained switch, as stored in the hands-free configuration. Disabled by default: hands-free
// control then stays inhibited unless setup explicitly qualified a switchless configuration.
#ifndef NODX_ENABLE
#define NODX_ENABLE -1
#endif

using namespace nodx;
class NVSStorage : public ProfileStorage {
public:
    bool begin() {
        return prefs_.begin("nodx-v1", false);
    }
    std::vector<uint8_t> read(unsigned slot) override {
        const char* key = slot ? "profile1" : "profile0";
        size_t size = prefs_.getBytesLength(key);
        if (size != 84) {
            return {};
        }
        std::vector<uint8_t> bytes(size);
        if (prefs_.getBytes(key, bytes.data(), size) != size) {
            return {};
        }
        return bytes;
    }
    bool write(unsigned slot, const std::vector<uint8_t>& bytes) override {
        return prefs_.putBytes(slot ? "profile1" : "profile0", bytes.data(), bytes.size()) ==
               bytes.size();
    }

private:
    Preferences prefs_;
};
// Hands-free configuration record slots. Separate namespace from the 84-byte profile slots.
class NVSConfigStorage : public ConfigStorage {
public:
    bool begin() {
        return prefs_.begin("nodx-hf", false);
    }
    std::vector<uint8_t> read(unsigned slot) override {
        const char* key = slot ? "hf1" : "hf0";
        const size_t size = prefs_.getBytesLength(key);
        if (size == 0) {
            return {};
        }
        if (size > 512) {
            return {0xff}; // reported as a corrupt record, never silently ignored
        }
        std::vector<uint8_t> bytes(size);
        if (prefs_.getBytes(key, bytes.data(), size) != size) {
            return {0xff};
        }
        return bytes;
    }
    bool write(unsigned slot, const std::vector<uint8_t>& bytes) override {
        return prefs_.putBytes(slot ? "hf1" : "hf0", bytes.data(), bytes.size()) == bytes.size();
    }

private:
    Preferences prefs_;
};
class I2CBus : public RegisterBus {
public:
    bool enabled = false;
    bool write(uint8_t reg, uint8_t value) override {
        if (!enabled) {
            return false;
        }
        Wire.beginTransmission(0x68);
        Wire.write(reg);
        Wire.write(value);
        return Wire.endTransmission() == 0;
    }
    bool read(uint8_t reg, uint8_t* bytes, size_t count) override {
        if (!enabled) {
            return false;
        }
        Wire.beginTransmission(0x68);
        Wire.write(reg);
        if (Wire.endTransmission(false) != 0) {
            return false;
        }
        if (Wire.requestFrom(uint8_t(0x68), uint8_t(count)) != count) {
            return false;
        }
        for (size_t i = 0; i < count; ++i) {
            bytes[i] = Wire.read();
        }
        return true;
    }
};
// Standard BLE report-protocol mouse; no battery/Wi-Fi/cloud services.
// Physical host pairing, encryption, subscription and delivery need validation.
class BLEHID : public HIDTransport {
public:
    std::atomic<bool> secured{false}, subscribed{false};
    NimBLECharacteristic* input = nullptr;
    class Callbacks : public NimBLEServerCallbacks {
    public:
        explicit Callbacks(BLEHID& owner) : owner_(owner) {}
        void onConnect(NimBLEServer*, NimBLEConnInfo&) override {
            owner_.secured = false;
            owner_.subscribed = false;
        }
        void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) override {
            owner_.secured = false;
            owner_.subscribed = false;
            NimBLEDevice::startAdvertising();
        }
        void onAuthenticationComplete(NimBLEConnInfo& info) override {
            owner_.secured = info.isEncrypted();
        }

    private:
        BLEHID& owner_;
    } callbacks{*this};
    class Subscription : public NimBLECharacteristicCallbacks {
    public:
        explicit Subscription(BLEHID& owner) : owner_(owner) {}
        void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo&, uint16_t value) override {
            owner_.subscribed = (value & 1) != 0;
        }

    private:
        BLEHID& owner_;
    } subscription{*this};
    void begin() {
        NimBLEDevice::init("NodX Adapt");
        NimBLEDevice::setSecurityAuth(true, false, true);
        NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
        auto* server = NimBLEDevice::createServer();
        server->setCallbacks(&callbacks, false);
        auto* service = server->createService("1812");
        // Report ID 1: 3 buttons + padding, signed relative X/Y/wheel (-127..127).
        static const uint8_t map[] = {
            0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x85, 0x01, 0x09, 0x01, 0xa1, 0x00, 0x05, 0x09,
            0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
            0x95, 0x01, 0x75, 0x05, 0x81, 0x03, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38,
            0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06, 0xc0, 0xc0};
        auto* reportMap = service->createCharacteristic("2a4b", NIMBLE_PROPERTY::READ |
                                                                    NIMBLE_PROPERTY::READ_ENC);
        reportMap->setValue(map, sizeof(map));
        input = service->createCharacteristic(
            "2a4d", NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::NOTIFY);
        uint8_t reference[] = {1, 1};
        input->createDescriptor("2908", NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC)
            ->setValue(reference, 2);
        input->setCallbacks(&subscription);
        uint8_t empty[] = {0, 0, 0, 0};
        input->setValue(empty, 4);
        uint8_t info[] = {0x11, 0x01, 0, 0x02};
        service->createCharacteristic("2a4a", NIMBLE_PROPERTY::READ)->setValue(info, 4);
        service->createCharacteristic("2a4c", NIMBLE_PROPERTY::WRITE_NR);
        // Report protocol only: omit Protocol Mode and Boot reports.
        service->start();
        auto* ad = NimBLEDevice::getAdvertising();
        ad->setName("NodX Adapt");
        ad->setAppearance(0x03c2);
        ad->addServiceUUID("1812");
        ad->enableScanResponse(true);
        ad->start();
    }
    bool connected() const override {
        return secured && subscribed;
    }
    bool send(const Report& r) override {
        if (!connected()) {
            return false;
        }
        uint8_t bytes[] = {uint8_t(r.down ? 1 : 0), uint8_t(r.dx), uint8_t(r.dy), uint8_t(r.wheel)};
        return input->notify(bytes, sizeof(bytes));
    }
};
