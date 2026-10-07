#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <NimBLEDevice.h>
#include "nodx/engine.hpp"
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

using namespace nodx;
class NVSStorage : public ProfileStorage {
public:
    bool begin() { return prefs_.begin("nodx-v1",false); }
    std::vector<uint8_t> read(unsigned slot) override {
        const char* key=slot ? "profile1" : "profile0";
        size_t size=prefs_.getBytesLength(key);
        if(size!=84) return {};
        std::vector<uint8_t> bytes(size);
        if(prefs_.getBytes(key,bytes.data(),size)!=size) return {};
        return bytes;
    }
    bool write(unsigned slot,const std::vector<uint8_t>& bytes) override {
        return prefs_.putBytes(slot?"profile1":"profile0",bytes.data(),bytes.size())==bytes.size();
    }
private:
    Preferences prefs_;
};
class I2CBus : public RegisterBus {
public:
    bool enabled=false;
    bool write(uint8_t reg,uint8_t value) override {
        if(!enabled) return false;
        Wire.beginTransmission(0x68); Wire.write(reg); Wire.write(value);
        return Wire.endTransmission()==0;
    }
    bool read(uint8_t reg,uint8_t* bytes,size_t count) override {
        if(!enabled) return false;
        Wire.beginTransmission(0x68); Wire.write(reg);
        if(Wire.endTransmission(false)!=0) return false;
        if(Wire.requestFrom(uint8_t(0x68),uint8_t(count))!=count) return false;
        for(size_t i=0;i<count;++i) bytes[i]=Wire.read();
        return true;
    }
};
// Standard BLE report-protocol mouse; no battery/Wi-Fi/cloud services.
// Physical host pairing, encryption, subscription and delivery need validation.
class BLEHID : public HIDTransport {
public:
    std::atomic<bool> secured{false}, subscribed{false};
    NimBLECharacteristic* input=nullptr;
    class Callbacks : public NimBLEServerCallbacks {
    public:
        explicit Callbacks(BLEHID& owner):owner_(owner){}
        void onConnect(NimBLEServer*,NimBLEConnInfo&) override { owner_.secured=false; owner_.subscribed=false; }
        void onDisconnect(NimBLEServer*,NimBLEConnInfo&,int) override {
            owner_.secured=false;owner_.subscribed=false;NimBLEDevice::startAdvertising();
        }
        void onAuthenticationComplete(NimBLEConnInfo& info) override { owner_.secured=info.isEncrypted(); }
    private: BLEHID& owner_;
    } callbacks{*this};
    class Subscription : public NimBLECharacteristicCallbacks {
    public:
        explicit Subscription(BLEHID& owner):owner_(owner){}
        void onSubscribe(NimBLECharacteristic*,NimBLEConnInfo&,uint16_t value) override { owner_.subscribed=(value&1)!=0; }
    private: BLEHID& owner_;
    } subscription{*this};
    void begin() {
        NimBLEDevice::init("NodX Adapt");
        NimBLEDevice::setSecurityAuth(true,false,true);
        NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
        auto* server=NimBLEDevice::createServer();server->setCallbacks(&callbacks,false);
        auto* service=server->createService("1812");
        // Report ID 1: 3 buttons + padding, signed relative X/Y/wheel (-127..127).
        static const uint8_t map[]={
            0x05,0x01,0x09,0x02,0xa1,0x01,0x85,0x01,0x09,0x01,0xa1,0x00,
            0x05,0x09,0x19,0x01,0x29,0x03,0x15,0x00,0x25,0x01,0x95,0x03,
            0x75,0x01,0x81,0x02,0x95,0x01,0x75,0x05,0x81,0x03,0x05,0x01,
            0x09,0x30,0x09,0x31,0x09,0x38,0x15,0x81,0x25,0x7f,0x75,0x08,
            0x95,0x03,0x81,0x06,0xc0,0xc0};
        auto* reportMap=service->createCharacteristic("2a4b",NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC);
        reportMap->setValue(map,sizeof(map));
        input=service->createCharacteristic("2a4d",NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::NOTIFY);
        uint8_t reference[]={1,1};
        input->createDescriptor("2908",NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC)->setValue(reference,2);
        input->setCallbacks(&subscription);
        uint8_t empty[]={0,0,0,0};input->setValue(empty,4);
        uint8_t info[]={0x11,0x01,0,0x02};
        service->createCharacteristic("2a4a",NIMBLE_PROPERTY::READ)->setValue(info,4);
        service->createCharacteristic("2a4c",NIMBLE_PROPERTY::WRITE_NR);
        // Report protocol only: omit Protocol Mode and Boot reports.
        service->start();
        auto* ad=NimBLEDevice::getAdvertising();ad->setName("NodX Adapt");
        ad->setAppearance(0x03c2);ad->addServiceUUID("1812");ad->enableScanResponse(true);ad->start();
    }
    bool connected() const override { return secured && subscribed; }
    bool send(const Report& r) override {
        if(!connected()) return false;
        uint8_t bytes[]={uint8_t(r.down?1:0),uint8_t(r.dx),uint8_t(r.dy),uint8_t(r.wheel)};
        return input->notify(bytes,sizeof(bytes));
    }
};

NVSStorage storage;I2CBus bus;MPU6050Sensor mpu(bus);SimulatedSensor simulator;BLEHID ble;
ProfileRepository repository(storage);System* systemEngine=nullptr;
DebouncedSwitch pauseSwitch,calSwitch;
uint32_t lastPoll=0,lastSample=0,lastProbe=0,lastDiagnostic=0;
bool previousPause=false,previousCal=false;
String serialLine;
bool pressed(int pin) { return pin>=0 && digitalRead(pin)==LOW; }
void diagnostic(uint32_t now,bool ok=true,uint32_t requestId=0) {
    auto& s=*systemEngine;
#ifdef NODX_SIMULATED
    const char* source="FIRMWARE_SIMULATED";
#else
    const char* source="HARDWARE";
#endif
    Serial.printf("{\"protocol\":1,\"requestId\":%lu,\"source\":\"%s\",\"ok\":%s,\"firmware\":\"0.1.0\",\"timeMs\":%lu,\"state\":\"%s\",\"calibration\":\"%s\",\"reason\":\"%s\",\"faults\":%lu,\"dwell\":\"%s\",\"cancellations\":%lu,\"hasProfile\":%s,\"connected\":%s,",
        (unsigned long)requestId,source,ok?"true":"false",(unsigned long)now,name(s.state),name(s.calibration.phase),s.diagnostics.reason,(unsigned long)s.diagnostics.faults,
        name(s.selection.dwell),(unsigned long)s.selection.cancellations,s.hasProfile?"true":"false",ble.connected()?"true":"false");
    Serial.printf("\"cursor\":\"%s\",\"calibrationReason\":\"%s\",\"calibrationProgress\":%.5f,\"dwellProgress\":%.5f,\"stability\":%.5f,\"motion\":[%.5f,%.5f,%.5f],\"reports\":[],\"profile\":{\"schema\":1,",
        s.diagnostics.cursor,s.calibration.reason,s.calibration.progress(now),s.selection.progress(now,s.profile),s.diagnostics.motion.stability,
        s.diagnostics.motion.x,s.diagnostics.motion.y,s.diagnostics.motion.roll);
    auto& p=s.profile;
    Serial.printf("\"bias\":[%.5f,%.5f,%.5f],\"deadzone\":[%.5f,%.5f],\"gain\":[%.5f,%.5f,%.5f,%.5f],\"alpha\":%.5f,\"precisionThreshold\":%.5f,\"fastThreshold\":%.5f,\"dwellTolerance\":%.5f,\"dwellMs\":%lu,\"scrollThreshold\":%.5f,\"scrollGain\":%.5f,\"dwellEnabled\":%s,\"scrollEnabled\":%s}}\n",
        p.bias[0],p.bias[1],p.bias[2],p.deadzone[0],p.deadzone[1],p.gain[0],p.gain[1],p.gain[2],p.gain[3],p.alpha,p.precisionThreshold,
        p.fastThreshold,p.dwellTolerance,(unsigned long)p.dwellMs,p.scrollThreshold,p.scrollGain,p.dwellEnabled?"true":"false",p.scrollEnabled?"true":"false");
}
void command(String line,uint32_t now) {
    auto& s=*systemEngine;
    uint32_t requestId=0;
    if(line.startsWith("@")){int space=line.indexOf(' ');if(space<2)return;requestId=strtoul(line.substring(1,space).c_str(),nullptr,10);line=line.substring(space+1);}
    bool ok=true;
    if(line=="calibrate") s.calibrate(now);
    else if(line=="cancel") s.cancelCalibration();
    else if(line=="resume") ok=s.resume();
    else if(line=="pause") s.pause();
    else if(line=="load") {UserProfile p;ok=repository.load(p) && s.setProfile(p,false);}
    else if(line=="generic") ok=s.setProfile(UserProfile{},false);
    else if(line=="dwell on" || line=="dwell off") { auto p=s.profile;p.dwellEnabled=line.endsWith("on");if(s.hasProfile)s.setProfile(p); }
    else if(line=="scroll on" || line=="scroll off") { auto p=s.profile;p.scrollEnabled=line.endsWith("on");if(s.hasProfile)s.setProfile(p); }
#ifdef NODX_SIMULATED
    else if(line.startsWith("motion ")) { float x,y,z;if(sscanf(line.c_str(),"motion %f %f %f",&x,&y,&z)==3)simulator.gyro={x,y,z}; }
    else if(line.startsWith("fault ")) { int value=line.substring(6).toInt();if(value>=0 && value<=5)simulator.fault=static_cast<Fault>(value); }
#endif
    else if(line!="status") ok=false;
    diagnostic(now,ok,requestId);
}
void setup() {
    Serial.begin(115200);
    Serial.println("[NODX] 0.1.0 pre-hardware; START parameters; ESP32-S3");
    bool storageOK=storage.begin();
    Serial.println(storageOK?"[STORAGE] initialized":"[STORAGE] failed");
    for(int pin:{NODX_SWITCH,NODX_PAUSE,NODX_CALIBRATE}) if(pin>=0)pinMode(pin,INPUT_PULLUP);
    if(NODX_BUZZER>=0){pinMode(NODX_BUZZER,OUTPUT);digitalWrite(NODX_BUZZER,LOW);}
    if(NODX_SDA>=0 && NODX_SCL>=0) {bus.enabled=Wire.begin(NODX_SDA,NODX_SCL,100000);Wire.setTimeOut(20);}
    bool imuOK=mpu.begin();Serial.println(imuOK?"[IMU] detected":"[IMU] unavailable; outputs inhibited");
    ble.begin();
    systemEngine=new System(ble,repository);
#ifdef NODX_SIMULATED
    systemEngine->axes.axes={0,1,2};
    systemEngine->axes.accelAxes={0,1,2};systemEngine->axes.accelSigns={1,1,1};
    Serial.println("[INPUT] simulated; motion and fault serial commands enabled");
#endif
    diagnostic(millis());
}
void loop() {
    uint32_t now=millis();auto& s=*systemEngine;
    // Bounded nonblocking serial parser.
    for(unsigned i=0;i<64 && Serial.available();++i) {
        char c=Serial.read();
        if(c=='\n'){command(serialLine,now);serialLine="";}
        else if(c!='\r' && serialLine.length()<80)serialLine+=c;
    }
    bool pauseNow=pauseSwitch.update(pressed(NODX_PAUSE),now);
    bool calNow=calSwitch.update(pressed(NODX_CALIBRATE),now);
    if(pauseNow && !previousPause){if(s.state==SystemState::Active)s.pause();else s.resume();}
    if(calNow && !previousCal)s.calibrate(now);
    previousPause=pauseNow;previousCal=calNow;
    // Poll faster than data-ready cadence; tick only on fresh 100Hz sensor frames.
#ifdef NODX_SIMULATED
    constexpr uint32_t pollMs=start::sampleMs;
#else
    constexpr uint32_t pollMs=2;
#endif
    if(uint32_t(now-lastPoll)>=pollMs) {
        lastPoll=now;
#ifdef NODX_SIMULATED
        s.tick(simulator.read(now),now,pressed(NODX_SWITCH));
#else
        MotionSample sample=mpu.read(now);
        if(sample.valid){lastSample=now;s.tick(sample,now,pressed(NODX_SWITCH));}
        else if(uint32_t(now-lastSample)>start::timeoutMs)s.tick(sample,now,pressed(NODX_SWITCH));
        if(uint32_t(now-lastSample)>start::timeoutMs && uint32_t(now-lastProbe)>=1000){lastProbe=now;mpu.begin();}
#endif
    }
    if(NODX_BUZZER>=0)digitalWrite(NODX_BUZZER,s.feedback.buzzer?HIGH:LOW);
    if(uint32_t(now-lastDiagnostic)>=500){lastDiagnostic=now;diagnostic(now);}
    delay(1);
}
