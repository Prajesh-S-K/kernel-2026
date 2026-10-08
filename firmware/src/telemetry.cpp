#include "runtime.hpp"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {
constexpr size_t telemetryBytes = 2048;
constexpr size_t transmitBudgetBytes = 64;
struct Frame {
    char data[telemetryBytes]{};
    size_t size = 0;
};
Frame acknowledgements[2], snapshot, current;
unsigned head = 0, count = 0;
size_t sent = 0;
void append(char* buffer, size_t capacity, size_t& used, const char* format, ...) {
    if (used >= capacity) {
        return;
    }
    va_list arguments;
    va_start(arguments, format);
    int written = vsnprintf(buffer + used, capacity - used, format, arguments);
    va_end(arguments);
    if (written < 0) {
        used = capacity;
        return;
    }
    used += size_t(written);
}
void enqueue(const char* data, size_t size, bool acknowledgement) {
    Frame* target = &snapshot;
    if (acknowledgement) {
        if (count == 2) {
            return; // Parser stops accepting lines while the queue is full.
        }
        target = &acknowledgements[(head + count) % 2];
        ++count;
    }
    memcpy(target->data, data, size);
    target->size = size;
}
} // namespace
bool acknowledgementCapacity() {
    return count < 2;
}
void transmitTelemetry() {
    // Finish the current JSON line, then prefer acknowledgements over snapshots.
    if (current.size == 0) {
        if (count) {
            current = acknowledgements[head];
            head = (head + 1) % 2;
            --count;
        } else {
            current = snapshot;
            snapshot.size = 0;
        }
        sent = 0;
    }
    if (current.size == 0) {
        return;
    }
    size_t available = size_t(std::max(0, Serial.availableForWrite()));
    size_t bytes = std::min({available, transmitBudgetBytes, current.size - sent});
    if (bytes == 0) {
        return;
    }
    sent += Serial.write(reinterpret_cast<const uint8_t*>(current.data + sent), bytes);
    if (sent == current.size) {
        current.size = 0;
    }
}
void diagnostic(uint32_t now, bool ok, uint32_t requestId) {
    char buffer[2048];
    size_t used = 0;
    auto& s = *systemEngine;
#ifdef NODX_SIMULATED
    const char* source = "FIRMWARE_SIMULATED";
#else
    const char* source = "HARDWARE";
#endif
    append(buffer, sizeof(buffer), used,
           "{\"protocol\":1,\"protocolRevision\":4,\"requestId\":%lu,\"source\":\"%s\",\"ok\":%s,"
           "\"firmware\":\"0.2.0\",\"timeMs\":%lu,\"state\":\"%s\",\"calibration\":\"%s\","
           "\"reason\":\"%s\",\"faults\":%lu,\"dwell\":\"%s\",\"cancellations\":%lu,\"hasProfile\":"
           "%s,\"connected\":%s,",
           (unsigned long)requestId, source, ok ? "true" : "false", (unsigned long)now,
           name(s.state), name(s.calibration.phase), s.diagnostics.reason,
           (unsigned long)s.diagnostics.faults, name(s.selection.dwell),
           (unsigned long)s.selection.cancellations, s.hasProfile ? "true" : "false",
           ble.connected() ? "true" : "false");
    append(buffer, sizeof(buffer), used,
           "\"faultCode\":\"%s\",\"cursor\":\"%s\",\"calibrationReason\":\"%s\","
           "\"calibrationProgress\":%.5f,\"calibrationCueMs\":%lu,\"dwellProgress\":%.5f,\"stability\":%.5f,\"motion\":[%."
           "5f,%.5f,%.5f],\"reports\":[],\"profile\":{\"schema\":1,",
           name(s.diagnostics.faultCode), s.diagnostics.cursor, s.calibration.reason,
           s.calibration.progress(now), (unsigned long)s.calibration.cueRemainingMs(now),
           s.selection.progress(now, s.profile),
           s.diagnostics.motion.stability, s.diagnostics.motion.x, s.diagnostics.motion.y,
           s.diagnostics.motion.roll);
    auto& p = s.profile;
    append(buffer, sizeof(buffer), used,
           "\"bias\":[%.5f,%.5f,%.5f],\"deadzone\":[%.5f,%.5f],\"gain\":[%.5f,%.5f,%.5f,%.5f],"
           "\"alpha\":%.5f,\"precisionThreshold\":%.5f,\"fastThreshold\":%.5f,\"dwellTolerance\":%."
           "5f,\"dwellMs\":%lu,\"scrollThreshold\":%.5f,\"scrollGain\":%.5f,\"dwellEnabled\":%s,"
           "\"scrollEnabled\":%s}",
           p.bias[0], p.bias[1], p.bias[2], p.deadzone[0], p.deadzone[1], p.gain[0], p.gain[1],
           p.gain[2], p.gain[3], p.alpha, p.precisionThreshold, p.fastThreshold, p.dwellTolerance,
           (unsigned long)p.dwellMs, p.scrollThreshold, p.scrollGain,
           p.dwellEnabled ? "true" : "false", p.scrollEnabled ? "true" : "false");
    static char hands[1024]; // static: keeps the loop task stack small
    const size_t handsLength = handsFreeJson(hands, sizeof(hands), s.handsFreeStatus());
    append(buffer, sizeof(buffer), used, ",\"handsFree\":%s", handsLength ? hands : "{}");
#ifndef NODX_SIMULATED
    // Raw sensor view for bench sessions (sensor coordinates, before the axis mapping).
    const auto& snap = sensorSnapshot;
    append(buffer, sizeof(buffer), used,
           ",\"sensor\":{\"variant\":\"%s\",\"seen\":%s,\"frames\":%lu,\"ageMs\":%lu,"
           "\"gyro\":[%.2f,%.2f,%.2f],\"accel\":[%.4f,%.4f,%.4f],"
           "\"angle\":[%.3f,%.3f,%.3f]}",
           name(mpu.variant()), snap.seen ? "true" : "false", (unsigned long)snap.frames,
           (unsigned long)(snap.seen ? uint32_t(now - snap.lastAtMs) : 0), snap.last.gyro[0],
           snap.last.gyro[1], snap.last.gyro[2], snap.last.accel[0], snap.last.accel[1],
           snap.last.accel[2], snap.angle[0], snap.angle[1], snap.angle[2]);
#endif
    append(buffer, sizeof(buffer), used, "}\n");
    if (used >= sizeof(buffer)) {
        return;
    }
    enqueue(buffer, used, requestId != 0);
}
