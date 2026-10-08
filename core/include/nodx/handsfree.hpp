#pragma once
#include "gesture.hpp"
#include <array>
#include <vector>

namespace nodx {
enum class InteractionMode { Legacy, HandsFree, ConfigInvalid };
const char* name(InteractionMode mode);
enum class ConfigState { Missing, Valid, Corrupt, Unsupported, OutOfBounds };
const char* name(ConfigState state);

// Separate versioned record. The 84-byte UserProfile is not involved in its encoding.
struct HandsFreeConfig {
    static constexpr uint32_t magic = 0x4846444e; // "NDFH" little endian
    static constexpr uint32_t version = 1;
    static constexpr size_t wireSize = 136;
    bool enabled = false;
    bool switchlessQualified = false; // setup explicitly allows control without a switch
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

// Maintained control-enable switch. ON permits; OFF inhibits. OFF is immediate, ON needs a
// stable debounce window, and an unknown or unconfigured input never permits control.
class EnableGate {
public:
    void configure(bool present, bool switchlessQualified);
    void setSwitchless(bool qualified) {
        switchless_ = qualified;
    }
    bool update(bool rawOn, uint32_t now);
    bool permitted() const {
        return present_ ? on_ : switchless_;
    }
    bool present() const {
        return present_;
    }
    bool on() const {
        return on_;
    }
    bool switchless() const {
        return switchless_;
    }
    const char* blocked() const; // nullptr when permitted

private:
    bool present_ = false, switchless_ = false, on_ = false, raw_ = false;
    uint32_t since_ = 0;
};

// Everything the companion needs about hands-free operation, with static strings only.
struct HandsFreeStatus {
    const char* mode = "LEGACY_SWITCH";
    const char* config = "MISSING";
    uint32_t configId = 0;
    bool switchPresent = false, switchOn = false, permitted = false, switchless = false;
    bool switchlessStaged = false;
    const char* recognizer = "WAIT_NEUTRAL";
    const char* lastGesture = "NONE";
    const char* lastReject = "NONE";
    uint32_t candidates = 0, rejected = 0, executed = 0, refused = 0;
    bool suppressing = false, dragging = false;
    const char* trainPhase = "IDLE";
    const char* trainGesture = "NONE";
    const char* trainReason = "idle";
    unsigned trainAccepted = 0, trainRequired = 0, trainRejects = 0;
    bool trainValidated = false;
    std::array<bool, gestureCount> staged{}, stored{};
    const char* blocked = "";
};
// Writes one JSON object (no trailing newline). Returns the length, or 0 if it does not fit.
size_t handsFreeJson(char* out, size_t capacity, const HandsFreeStatus& status);
} // namespace nodx
