#pragma once
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>
// In-memory NVS stand-in. Not a model of real NVS atomicity or wear.
class Preferences {
public:
    bool begin(const char*, bool) {
        return true;
    }
    size_t getBytesLength(const char* key) {
        auto found = store_.find(key);
        return found == store_.end() ? 0 : found->second.size();
    }
    size_t getBytes(const char* key, void* out, size_t size) {
        auto found = store_.find(key);
        if (found == store_.end() || found->second.size() < size) {
            return 0;
        }
        memcpy(out, found->second.data(), size);
        return size;
    }
    size_t putBytes(const char* key, const void* data, size_t size) {
        auto* bytes = static_cast<const uint8_t*>(data);
        store_[key] = std::vector<uint8_t>(bytes, bytes + size);
        return size;
    }

private:
    std::map<std::string, std::vector<uint8_t>> store_;
};
