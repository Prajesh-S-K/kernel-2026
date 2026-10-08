#pragma once
#include "gesture.hpp"
#include <array>
#include <vector>

namespace nodx {
enum class InteractionMode { Legacy, HandsFree, ConfigInvalid };
const char* name(InteractionMode mode);
enum class ConfigState { Missing, Valid, Corrupt, Unsupported, OutOfBounds };
const char* name(ConfigState state);

// How the one control-enable input is read. MAINTAINED: its level is the permission (the original
// slide switch). MOMENTARY: a push button whose debounced presses toggle a permission latch.
enum class EnableKind : uint8_t { Maintained, Momentary };
const char* name(EnableKind kind);

// Separate versioned record. The 84-byte UserProfile is not involved in its encoding.
struct HandsFreeConfig {
    static constexpr uint32_t magic = 0x4846444e; // "NDFH" little endian
    static constexpr uint32_t version = 1;
    static constexpr size_t wireSize = 136;
    bool enabled = false;
    bool switchlessQualified = false; // setup explicitly allows control without a switch
    // Stored in flag bit 2. A record written before the button existed has the bit clear and keeps
    // its original maintained-switch meaning; it is never silently reinterpreted.
    EnableKind enableKind = EnableKind::Maintained;
    GestureSet gestures;
    bool valid() const;
};
std::vector<uint8_t> encode(const HandsFreeConfig& config, uint32_t generation);
ConfigState decode(const std::vector<uint8_t>& bytes, HandsFreeConfig& config,
                   uint32_t& generation);
// Identity of the content (generation excluded), used to freeze Lab blocks.
uint32_t configId(const HandsFreeConfig& config);

class ConfigStorage {
public:
    virtual ~ConfigStorage() = default;
    virtual std::vector<uint8_t> read(unsigned slot) = 0;
    virtual bool write(unsigned slot, const std::vector<uint8_t>& bytes) = 0;
};
class HandsFreeRepository {
public:
    explicit HandsFreeRepository(ConfigStorage& storage) : storage_(storage) {}
    ConfigState load(HandsFreeConfig& config);
    bool save(const HandsFreeConfig& config);

private:
    ConfigStorage& storage_;
};
class MemoryConfigStorage : public ConfigStorage {
public:
    std::array<std::vector<uint8_t>, 2> slots;
    bool failWrite = false;
    bool tearWrite = false;
    std::vector<uint8_t> read(unsigned slot) override {
        return slots.at(slot);
    }
    bool write(unsigned slot, const std::vector<uint8_t>& bytes) override;
};

// Control-enable input. The gate only ever PERMITS or INHIBITS control; it never resumes it.
// MAINTAINED: ON permits, OFF inhibits; OFF is immediate and ON needs a stable debounce window.
// MOMENTARY (push button): the permission is a latch that is ALWAYS disabled at boot. It must see
// a stable release first (so a button held at boot enables nothing), then one debounced press
// enables; the next press disables at its first edge (fail safe, no sensor sample needed). Every
// accepted press toggles exactly once and a stable release is required before the next press.
// The latch is never persisted. An unknown or unconfigured input never permits control.
class EnableGate {
public:
    void configure(bool present, bool switchlessQualified,
                   EnableKind kind = EnableKind::Maintained);
    void setSwitchless(bool qualified) {
        switchless_ = qualified;
    }
    void setKind(EnableKind kind); // resets the latch and requires a fresh release
    // `active` is the raw input: switch ON / button pressed.
    bool update(bool active, uint32_t now);
    void clearLatch(); // momentary: drop permission (fault); a release and new press are required
    bool permitted() const {
        return present_ ? on_ : switchless_;
    }
    bool present() const {
        return present_;
    }
    // Maintained: the switch is ON (debounced). Momentary: the permission latch.
    bool on() const {
        return on_;
    }
    bool pressed() const { // raw input as last seen (not debounced, not a permission)
        return raw_;
    }
    bool armed() const { // momentary: a stable release has been seen since the last accepted press
        return armed_;
    }
    EnableKind kind() const {
        return kind_;
    }
    bool switchless() const {
        return switchless_;
    }
    const char* blocked() const; // nullptr when permitted

private:
    bool present_ = false, switchless_ = false, on_ = false, raw_ = false;
    EnableKind kind_ = EnableKind::Maintained;
    bool armed_ = false, pressTracked_ = false, releaseTracked_ = false;
    uint32_t since_ = 0, pressSince_ = 0, releaseSince_ = 0;
};

// Everything the companion needs about hands-free operation, with static strings only.
struct HandsFreeStatus {
    const char* mode = "LEGACY_SWITCH";
    const char* config = "MISSING";
    uint32_t configId = 0;
    bool switchPresent = false, switchOn = false, permitted = false, switchless = false;
    bool switchlessStaged = false;
    const char* switchKind = "MAINTAINED"; // how the enable input is read (stored)
    const char* switchKindStaged = "MAINTAINED";
    bool switchPressed = false, switchLatched = false, switchArmed = false; // raw vs permission
    const char* recognizer = "WAIT_NEUTRAL";
    const char* lastGesture = "NONE";
    const char* lastReject = "NONE";
    uint32_t candidates = 0, rejected = 0, executed = 0, refused = 0;
    bool suppressing = false, dragging = false, demoMovementOnly = false;
    const char* trainPhase = "IDLE";
    const char* trainGesture = "NONE";
    const char* trainReason = "idle";
    unsigned trainAccepted = 0, trainRequired = 0, trainRejects = 0;
    bool trainValidated = false;
    std::array<bool, gestureCount> staged{}, stored{};
    const char* blocked = "";
    // Temporary uncalibrated pointer demo (RAM only; never a calibration, never saved).
    bool uncalActive = false, uncalPermitted = false, uncalPresent = false;
    const char* uncalBlocked = "";
    const char* profileState = "MISSING";
    float uncalGain = 0, uncalDeadzone = 0, uncalMaxStep = 0;
};
// Writes one JSON object (no trailing newline). Returns the length, or 0 if it does not fit.
size_t handsFreeJson(char* out, size_t capacity, const HandsFreeStatus& status);
} // namespace nodx
