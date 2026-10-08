#pragma once
// BLE stub: records nothing and delivers nothing. Real pairing, encryption and notification
// behaviour is NOT exercised by the host firmware tests.
#include <cstddef>
#include <cstdint>
#define BLE_HS_IO_NO_INPUT_OUTPUT 3
namespace NIMBLE_PROPERTY {
enum : unsigned { READ = 1, READ_ENC = 2, NOTIFY = 4, WRITE_NR = 8 };
}
struct NimBLEConnInfo {
    bool isEncrypted() const {
        return false;
    }
};
class NimBLEServer;
class NimBLEServerCallbacks {
public:
    virtual ~NimBLEServerCallbacks() = default;
    virtual void onConnect(NimBLEServer*, NimBLEConnInfo&) {}
    virtual void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) {}
    virtual void onAuthenticationComplete(NimBLEConnInfo&) {}
};
class NimBLECharacteristic;
class NimBLECharacteristicCallbacks {
public:
    virtual ~NimBLECharacteristicCallbacks() = default;
    virtual void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo&, uint16_t) {}
};
class NimBLEDescriptor {
public:
    void setValue(const uint8_t*, size_t) {}
};
class NimBLECharacteristic {
public:
    void setValue(const uint8_t*, size_t) {}
    void setCallbacks(NimBLECharacteristicCallbacks*) {}
    NimBLEDescriptor* createDescriptor(const char*, unsigned) {
        static NimBLEDescriptor descriptor;
        return &descriptor;
    }
    // Always accepts: delivery to a host is NOT modelled. Tests mark the link secured/subscribed
    // by setting the public flags on the firmware's own BLEHID object.
    bool notify(const uint8_t*, size_t) {
        return true;
    }
};
class NimBLEService {
public:
    NimBLECharacteristic* createCharacteristic(const char*, unsigned = 0) {
        static NimBLECharacteristic characteristic;
        return &characteristic;
    }
    void start() {}
};
class NimBLEServer {
public:
    void setCallbacks(NimBLEServerCallbacks*, bool) {}
    NimBLEService* createService(const char*) {
        static NimBLEService service;
        return &service;
    }
};
class NimBLEAdvertising {
public:
    void setName(const char*) {}
    void setAppearance(uint16_t) {}
    void addServiceUUID(const char*) {}
    void enableScanResponse(bool) {}
    void start() {}
};
class NimBLEDevice {
public:
    static void init(const char*) {}
    static void setSecurityAuth(bool, bool, bool) {}
    static void setSecurityIOCap(int) {}
    static NimBLEServer* createServer() {
        static NimBLEServer server;
        return &server;
    }
    static NimBLEAdvertising* getAdvertising() {
        static NimBLEAdvertising advertising;
        return &advertising;
    }
    static void startAdvertising() {}
};
