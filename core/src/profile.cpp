#include "nodx/profile.hpp"
#include <cmath>
#include <cstring>
#include <limits>

namespace nodx {
namespace {
bool between(float v, float low, float high) {
    return std::isfinite(v) && v >= low && v <= high;
}
void put(std::vector<uint8_t>& b, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) {
        b.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
}
uint32_t get(const std::vector<uint8_t>& b, size_t& at) {
    uint32_t v = 0;
    for (unsigned i = 0; i < 4; ++i) {
        v |= uint32_t(b.at(at++)) << (8 * i);
    }
    return v;
}
void putFloat(std::vector<uint8_t>& b, float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    put(b, bits);
}
float getFloat(const std::vector<uint8_t>& b, size_t& at) {
    uint32_t bits = get(b, at);
    float v;
    std::memcpy(&v, &bits, 4);
    return v;
}
} // namespace
bool UserProfile::valid() const {
    for (float v : bias) {
        if (!between(v, -start::maxRestBias, start::maxRestBias)) {
            return false;
        }
    }
    for (float v : deadzone) {
        if (!between(v, .1f, 15.f)) {
            return false;
        }
    }
    for (float v : gain) {
        if (!between(v, 1.f, 120.f)) {
            return false;
        }
    }
    return between(alpha, .05f, 1.f) && between(precisionThreshold, 1.f, 20.f) &&
           between(fastThreshold, precisionThreshold + 1.f, 100.f) &&
           between(dwellTolerance, 2.f, 50.f) && dwellMs >= 500 && dwellMs <= 5000 &&
           between(scrollThreshold, 5.f, 45.f) && between(scrollGain, .05f, 3.f);
}
uint32_t checksum(const std::vector<uint8_t>& bytes) {
    uint32_t crc = 0xffffffffu;
    for (uint8_t b : bytes) {
        crc ^= b;
        for (unsigned i = 0; i < 8; ++i) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}
std::vector<uint8_t> encode(const UserProfile& p, uint32_t generation) {
    static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559,
                  "IEEE binary32 required");
    std::vector<uint8_t> b;
    put(b, UserProfile::magic);
    put(b, UserProfile::schema);
    put(b, generation);
    for (float v : p.bias) {
        putFloat(b, v);
    }
    for (float v : p.deadzone) {
        putFloat(b, v);
    }
    for (float v : p.gain) {
        putFloat(b, v);
    }
    for (float v : {p.alpha, p.precisionThreshold, p.fastThreshold, p.dwellTolerance}) {
        putFloat(b, v);
    }
    put(b, p.dwellMs);
    putFloat(b, p.scrollThreshold);
    putFloat(b, p.scrollGain);
    put(b, (p.scrollEnabled ? 1u : 0u) | (p.dwellEnabled ? 2u : 0u));
    put(b, checksum(b));
    return b;
}
bool decode(const std::vector<uint8_t>& b, UserProfile& p, uint32_t& generation) {
    if (b.size() != 84) {
        return false;
    }
    size_t end = b.size() - 4;
    size_t tail = end;
    if (checksum(std::vector<uint8_t>(b.begin(), b.begin() + end)) != get(b, tail)) {
        return false;
    }
    size_t at = 0;
    if (get(b, at) != UserProfile::magic || get(b, at) != UserProfile::schema) {
        return false;
    }
    uint32_t g = get(b, at);
    UserProfile candidate;
    for (float& v : candidate.bias) {
        v = getFloat(b, at);
    }
    for (float& v : candidate.deadzone) {
        v = getFloat(b, at);
    }
    for (float& v : candidate.gain) {
        v = getFloat(b, at);
    }
    candidate.alpha = getFloat(b, at);
    candidate.precisionThreshold = getFloat(b, at);
    candidate.fastThreshold = getFloat(b, at);
    candidate.dwellTolerance = getFloat(b, at);
    candidate.dwellMs = get(b, at);
    candidate.scrollThreshold = getFloat(b, at);
    candidate.scrollGain = getFloat(b, at);
    uint32_t flags = get(b, at);
    if (flags > 3) {
        return false;
    }
    candidate.scrollEnabled = flags & 1;
    candidate.dwellEnabled = flags & 2;
    if (!candidate.valid()) {
        return false;
    }
    p = candidate;
    generation = g;
    return true;
}
bool ProfileRepository::load(UserProfile& p) {
    UserProfile a, b;
    uint32_t ga = 0, gb = 0;
    bool va = decode(storage_.read(0), a, ga), vb = decode(storage_.read(1), b, gb);
    if (!va && !vb) {
        // Empty slots are a missing profile; anything else that fails to decode is corrupt.
        state_ = storage_.read(0).empty() && storage_.read(1).empty() ? ProfileState::Missing
                                                                      : ProfileState::Corrupt;
        return false;
    }
    state_ = ProfileState::Valid;
    p = (!va || (vb && gb > ga)) ? b : a;
    return true;
}
const char* name(ProfileState state) {
    switch (state) {
    case ProfileState::Missing:
        return "MISSING";
    case ProfileState::Valid:
        return "VALID";
    case ProfileState::Corrupt:
        return "CORRUPT";
    }
    return "CORRUPT";
}
bool ProfileRepository::save(const UserProfile& p) {
    if (!p.valid()) {
        return false;
    }
    UserProfile a, b;
    uint32_t ga = 0, gb = 0;
    bool va = decode(storage_.read(0), a, ga), vb = decode(storage_.read(1), b, gb);
    unsigned slot = (!va || (vb && ga <= gb)) ? 0 : 1;
    uint32_t g = std::max(va ? ga : 0, vb ? gb : 0);
    if (g == UINT32_MAX) {
        return false; // no ambiguous wraparound
    }
    auto bytes = encode(p, g + 1);
    if (!storage_.write(slot, bytes)) {
        return false;
    }
    // A failed/torn write cannot replace the last valid slot.
    if (storage_.read(slot) != bytes) {
        return false;
    }
    state_ = ProfileState::Valid;
    return true;
}
bool MemoryStorage::write(unsigned slot, const std::vector<uint8_t>& bytes) {
    if (failWrite) {
        return false;
    }
    slots.at(slot) = bytes;
    if (tearWrite) {
        slots.at(slot).resize(bytes.size() / 2);
    }
    return !tearWrite;
}
} // namespace nodx
