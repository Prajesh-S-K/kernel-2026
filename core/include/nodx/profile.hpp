#pragma once
#include "parameters.hpp"
#include <array>
#include <cstdint>
#include <vector>

namespace nodx {
struct UserProfile {
    static constexpr uint32_t magic = 0x58444f4e;
    static constexpr uint32_t schema = 1;
    std::array<float, 3> bias{};
    std::array<float, 2> deadzone{start::deadzoneBase, start::deadzoneBase};
    std::array<float, 4> gain{start::gain, start::gain, start::gain, start::gain}; // L R U D
    float alpha = start::alpha;
    float precisionThreshold = start::precision;
    float fastThreshold = start::fast;
    float dwellTolerance = start::dwellTolerance;
    uint32_t dwellMs = start::dwellMs;
    float scrollThreshold = start::scrollThreshold;
    float scrollGain = start::scrollGain;
    bool scrollEnabled = true;
    bool dwellEnabled = false; // explicit opt-in
    bool valid() const;
};
uint32_t checksum(const std::vector<uint8_t>& bytes);
std::vector<uint8_t> encode(const UserProfile& p, uint32_t generation);
bool decode(const std::vector<uint8_t>& bytes, UserProfile& p, uint32_t& generation);

// The adapter must durably replace ONE slot. The other slot remains untouched.
class ProfileStorage {
public:
    virtual ~ProfileStorage() = default;
    virtual std::vector<uint8_t> read(unsigned slot) = 0;
    virtual bool write(unsigned slot, const std::vector<uint8_t>& bytes) = 0;
};
class ProfileRepository {
public:
    explicit ProfileRepository(ProfileStorage& storage) : storage_(storage) {}
    bool load(UserProfile& profile);
    bool save(const UserProfile& profile);
private:
    ProfileStorage& storage_;
};
class MemoryStorage : public ProfileStorage {
public:
    std::array<std::vector<uint8_t>, 2> slots;
    bool failWrite = false;
    bool tearWrite = false;
    std::vector<uint8_t> read(unsigned slot) override { return slots.at(slot); }
    bool write(unsigned slot, const std::vector<uint8_t>& bytes) override;
};
}
