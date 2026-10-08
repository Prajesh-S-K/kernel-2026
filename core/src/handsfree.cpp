#include "nodx/handsfree.hpp"
#include "nodx/profile.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace nodx {
namespace {
void put(std::vector<uint8_t>& bytes, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) {
        bytes.push_back(static_cast<uint8_t>(value >> (8 * i)));
    }
}
void putFloat(std::vector<uint8_t>& bytes, float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    put(bytes, bits);
}
uint32_t get(const std::vector<uint8_t>& bytes, size_t& at) {
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) {
        value |= uint32_t(bytes.at(at++)) << (8 * i);
    }
    return value;
}
float getFloat(const std::vector<uint8_t>& bytes, size_t& at) {
    const uint32_t bits = get(bytes, at);
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
}
void putTemplate(std::vector<uint8_t>& bytes, const GestureTemplate& item) {
    put(bytes, item.strokes);
    for (unsigned i = 0; i < maxStrokes; ++i) {
        const bool used = i < item.strokes;
        put(bytes, used ? (item.axis[i] | (item.sign[i] > 0 ? 0x100u : 0u)) : 0u);
    }
    putFloat(bytes, item.enterRate);
    putFloat(bytes, item.peakMin);
    putFloat(bytes, item.peakMax);
    put(bytes, item.strokeMinMs);
    put(bytes, item.strokeMaxMs);
    put(bytes, item.gapMaxMs);
    put(bytes, item.totalMaxMs);
}
// False when a field cannot be represented (the record is then out of bounds).
bool getTemplate(const std::vector<uint8_t>& bytes, size_t& at, GestureTemplate& item) {
    item = GestureTemplate{};
    const uint32_t strokes = get(bytes, at);
    bool ok = strokes <= maxStrokes;
    for (unsigned i = 0; i < maxStrokes; ++i) {
        const uint32_t word = get(bytes, at);
        if (i < strokes && ok) {
            ok = (word & ~0x103u) == 0 && (word & 0xff) <= 2;
            item.axis[i] = uint8_t(word & 0xff);
            item.sign[i] = (word & 0x100) ? 1 : -1;
        } else if (word != 0) {
            ok = false; // unused stroke slots must be zero
        }
    }
    item.enterRate = getFloat(bytes, at);
    item.peakMin = getFloat(bytes, at);
    item.peakMax = getFloat(bytes, at);
    const uint32_t minMs = get(bytes, at), maxMs = get(bytes, at), gap = get(bytes, at),
                   total = get(bytes, at);
    ok = ok && minMs <= 0xffff && maxMs <= 0xffff && gap <= 0xffff && total <= 0xffff;
    item.strokes = uint8_t(strokes);
    item.strokeMinMs = uint16_t(minMs);
    item.strokeMaxMs = uint16_t(maxMs);
    item.gapMaxMs = uint16_t(gap);
    item.totalMaxMs = uint16_t(total);
    if (strokes == 0) {
        // An unconfigured template must be entirely zero.
        ok = ok && item.enterRate == 0 && item.peakMin == 0 && item.peakMax == 0 && minMs == 0 &&
             maxMs == 0 && gap == 0 && total == 0;
    }
    return ok;
}
} // namespace

const char* name(InteractionMode mode) {
    switch (mode) {
    case InteractionMode::Legacy:
        return "LEGACY_SWITCH";
    case InteractionMode::HandsFree:
        return "HANDS_FREE";
    case InteractionMode::ConfigInvalid:
        return "CONFIG_INVALID";
    }
    return "CONFIG_INVALID";
}
const char* name(EnableKind kind) {
    return kind == EnableKind::Momentary ? "MOMENTARY" : "MAINTAINED";
}
const char* name(ConfigState state) {
    switch (state) {
    case ConfigState::Missing:
        return "MISSING";
    case ConfigState::Valid:
        return "VALID";
    case ConfigState::Corrupt:
        return "CORRUPT";
    case ConfigState::Unsupported:
        return "UNSUPPORTED";
    case ConfigState::OutOfBounds:
        return "OUT_OF_BOUNDS";
    }
    return "CORRUPT";
}

bool HandsFreeConfig::valid() const {
    return gestures.valid(enabled);
}
std::vector<uint8_t> encode(const HandsFreeConfig& config, uint32_t generation) {
    std::vector<uint8_t> bytes;
    put(bytes, HandsFreeConfig::magic);
    put(bytes, HandsFreeConfig::version);
    put(bytes, generation);
    put(bytes, (config.enabled ? 1u : 0u) | (config.switchlessQualified ? 2u : 0u) |
                   (config.enableKind == EnableKind::Momentary ? 4u : 0u));
    putFloat(bytes, config.gestures.neutralRate);
    for (const auto& item : config.gestures.templates) {
        putTemplate(bytes, item);
    }
    put(bytes, checksum(bytes));
    return bytes;
}
ConfigState decode(const std::vector<uint8_t>& bytes, HandsFreeConfig& config,
                   uint32_t& generation) {
    if (bytes.size() != HandsFreeConfig::wireSize) {
        return ConfigState::Corrupt;
    }
    const size_t end = bytes.size() - 4;
    size_t tail = end;
    if (checksum(std::vector<uint8_t>(bytes.begin(), bytes.begin() + end)) != get(bytes, tail)) {
        return ConfigState::Corrupt;
    }
    size_t at = 0;
    if (get(bytes, at) != HandsFreeConfig::magic) {
        return ConfigState::Corrupt;
    }
    if (get(bytes, at) != HandsFreeConfig::version) {
        return ConfigState::Unsupported;
    }
    const uint32_t stored = get(bytes, at);
    const uint32_t flags = get(bytes, at);
    HandsFreeConfig candidate;
    candidate.gestures.neutralRate = getFloat(bytes, at);
    bool shaped = flags <= 7;
    for (auto& item : candidate.gestures.templates) {
        shaped = getTemplate(bytes, at, item) && shaped;
    }
    candidate.enabled = flags & 1;
    candidate.switchlessQualified = flags & 2;
    candidate.enableKind = (flags & 4) ? EnableKind::Momentary : EnableKind::Maintained;
    if (!shaped || !candidate.valid()) {
        return ConfigState::OutOfBounds;
    }
    config = candidate;
    generation = stored;
    return ConfigState::Valid;
}
uint32_t configId(const HandsFreeConfig& config) {
    auto bytes = encode(config, 0);
    bytes.resize(bytes.size() - 4); // hash the content, not the stored checksum
    return checksum(bytes);
}

ConfigState HandsFreeRepository::load(HandsFreeConfig& config) {
    std::array<HandsFreeConfig, 2> found;
    std::array<uint32_t, 2> generation{};
    std::array<ConfigState, 2> state{ConfigState::Missing, ConfigState::Missing};
    bool any = false;
    for (unsigned slot = 0; slot < 2; ++slot) {
        const auto bytes = storage_.read(slot);
        if (bytes.empty()) {
            continue;
        }
        any = true;
        state[slot] = decode(bytes, found[slot], generation[slot]);
    }
    const bool valid0 = state[0] == ConfigState::Valid, valid1 = state[1] == ConfigState::Valid;
    if (valid0 || valid1) {
        config = (!valid0 || (valid1 && generation[1] > generation[0])) ? found[1] : found[0];
        return ConfigState::Valid;
    }
    if (!any) {
        return ConfigState::Missing;
    }
    for (ConfigState worst : {ConfigState::Unsupported, ConfigState::OutOfBounds}) {
        if (state[0] == worst || state[1] == worst) {
            return worst;
        }
    }
    return ConfigState::Corrupt;
}
bool HandsFreeRepository::save(const HandsFreeConfig& config) {
    if (!config.valid()) {
        return false;
    }
    HandsFreeConfig a, b;
    uint32_t ga = 0, gb = 0;
    const bool va = decode(storage_.read(0), a, ga) == ConfigState::Valid;
    const bool vb = decode(storage_.read(1), b, gb) == ConfigState::Valid;
    const unsigned slot = (!va || (vb && ga <= gb)) ? 0 : 1;
    const uint32_t newest = std::max(va ? ga : 0, vb ? gb : 0);
    if (newest == UINT32_MAX) {
        return false;
    }
    const auto bytes = encode(config, newest + 1);
    // A failed or torn write cannot replace the newest valid slot.
    return storage_.write(slot, bytes) && storage_.read(slot) == bytes;
}
bool MemoryConfigStorage::write(unsigned slot, const std::vector<uint8_t>& bytes) {
    if (failWrite) {
        return false;
    }
    slots.at(slot) = bytes;
    if (tearWrite) {
        slots.at(slot).resize(bytes.size() / 2);
    }
    return !tearWrite;
}

void EnableGate::configure(bool present, bool switchlessQualified, EnableKind kind) {
    present_ = present;
    switchless_ = switchlessQualified;
    kind_ = kind;
    on_ = raw_ = false; // unknown input is OFF / disabled until proven otherwise
    armed_ = pressTracked_ = releaseTracked_ = false;
}
void EnableGate::setKind(EnableKind kind) {
    configure(present_, switchless_, kind);
}
void EnableGate::clearLatch() {
    if (kind_ == EnableKind::Momentary) {
        on_ = false;
        armed_ = pressTracked_ = releaseTracked_ = false; // release and a new press are required
    }
}
bool EnableGate::update(bool active, uint32_t now) {
    if (!present_) {
        return permitted();
    }
    raw_ = active;
    if (kind_ == EnableKind::Maintained) {
        if (!active) {
            on_ = pressTracked_ = false;
            return false;
        }
        if (!pressTracked_) { // start of an ON run: it must stay stable for the debounce window
            pressTracked_ = true;
            since_ = now;
        }
        if (uint32_t(now - since_) >= start::enableDebounceMs) {
            on_ = true;
        }
        return on_;
    }
    // Momentary push button.
    if (!active) {
        pressTracked_ = false;
        if (!releaseTracked_) {
            releaseTracked_ = true;
            releaseSince_ = now;
        }
        if (!armed_ && uint32_t(now - releaseSince_) >= start::enableDebounceMs) {
            armed_ = true; // stable release seen: the next press counts
        }
        return on_;
    }
    releaseTracked_ = false;
    if (!armed_) {
        return on_; // held at boot, or still the same press, or bouncing: nothing to accept
    }
    if (on_) {
        on_ = false; // disable at the first press edge; bounce cannot re-enable (needs a release)
        armed_ = false;
        pressTracked_ = false;
        return false;
    }
    if (!pressTracked_) {
        pressTracked_ = true;
        pressSince_ = now;
    }
    if (uint32_t(now - pressSince_) >= start::enableDebounceMs) {
        on_ = true; // one debounced press: enable, exactly once
        armed_ = false;
        pressTracked_ = false;
    }
    return on_;
}
const char* EnableGate::blocked() const {
    if (permitted()) {
        return nullptr;
    }
    if (!present_) {
        return "enable switch not configured";
    }
    return kind_ == EnableKind::Momentary ? "control disabled: press the enable button"
                                          : "control switch is OFF";
}

size_t handsFreeJson(char* out, size_t capacity, const HandsFreeStatus& s) {
    const int written = std::snprintf(
        out, capacity,
        "{\"mode\":\"%s\",\"config\":\"%s\",\"configId\":\"%08lx\","
        "\"switch\":{\"present\":%s,\"on\":%s,\"permitted\":%s,\"switchless\":%s,"
        "\"switchlessStaged\":%s,\"kind\":\"%s\",\"kindStaged\":\"%s\",\"pressed\":%s,"
        "\"latched\":%s,\"armed\":%s},"
        "\"gesture\":{\"state\":\"%s\",\"last\":\"%s\",\"lastReject\":\"%s\",\"candidates\":%lu,"
        "\"rejected\":%lu,\"executed\":%lu,\"refused\":%lu,\"suppressing\":%s},"
        "\"drag\":%s,\"demoMovementOnly\":%s,"
        "\"training\":{\"phase\":\"%s\",\"gesture\":\"%s\",\"accepted\":%u,\"required\":%u,"
        "\"rejects\":%u,\"validated\":%s,\"reason\":\"%s\"},"
        "\"staged\":[%s,%s],\"stored\":[%s,%s],\"blocked\":\"%s\","
        "\"uncalDemo\":{\"active\":%s,\"blocked\":\"%s\","
        "\"profileState\":\"%s\",\"gain\":%.2f,\"deadzone\":%.2f,\"maxStep\":%.2f,"
        "\"dwell\":{\"enabled\":%s,\"ms\":%lu,\"tolerance\":%.1f,\"state\":\"%s\",\"progress\":%.3f,"
        "\"clicks\":%lu}}}",
        s.mode, s.config, static_cast<unsigned long>(s.configId),
        s.switchPresent ? "true" : "false", s.switchOn ? "true" : "false",
        s.permitted ? "true" : "false", s.switchless ? "true" : "false",
        s.switchlessStaged ? "true" : "false", s.switchKind, s.switchKindStaged,
        s.switchPressed ? "true" : "false", s.switchLatched ? "true" : "false",
        s.switchArmed ? "true" : "false", s.recognizer, s.lastGesture, s.lastReject,
        static_cast<unsigned long>(s.candidates), static_cast<unsigned long>(s.rejected),
        static_cast<unsigned long>(s.executed), static_cast<unsigned long>(s.refused),
        s.suppressing ? "true" : "false", s.dragging ? "true" : "false",
        s.demoMovementOnly ? "true" : "false", s.trainPhase, s.trainGesture, s.trainAccepted,
        s.trainRequired, s.trainRejects, s.trainValidated ? "true" : "false", s.trainReason,
        s.staged[0] ? "true" : "false", s.staged[1] ? "true" : "false",
        s.stored[0] ? "true" : "false", s.stored[1] ? "true" : "false", s.blocked,
        s.uncalActive ? "true" : "false", s.uncalBlocked, s.profileState, s.uncalGain,
        s.uncalDeadzone, s.uncalMaxStep, s.uncalDwellEnabled ? "true" : "false",
        static_cast<unsigned long>(s.uncalDwellMs), s.uncalDwellTolerance, s.uncalDwellState,
        s.uncalDwellProgress, static_cast<unsigned long>(s.uncalClicks));
    return (written > 0 && size_t(written) < capacity) ? size_t(written) : 0;
}
} // namespace nodx
