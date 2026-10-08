#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace nodx {
struct CommandEnvelope {
    uint32_t requestId = 0;
    std::string body;
    bool valid = false;
};
// Keep correlation even for an overlong command, but never execute its prefix.
inline CommandEnvelope parseEnvelope(const std::string& line, size_t maximumBytes,
                                     bool truncated = false) {
    CommandEnvelope result;
    result.body = line;
    if (!line.empty() && line.front() == '@') {
        const size_t space = line.find(' ');
        if (space == std::string::npos || space < 2 || space > 11) {
            return result;
        }
        uint64_t requestId = 0;
        for (size_t index = 1; index < space; ++index) {
            char digit = line[index];
            if (digit < '0' || digit > '9') {
                return result;
            }
            requestId = requestId * 10 + unsigned(digit - '0');
        }
        if (requestId == 0 || requestId > UINT32_MAX) {
            return result;
        }
        result.requestId = uint32_t(requestId);
        result.body = line.substr(space + 1);
    }
    if (truncated || line.size() > maximumBytes || result.body.empty()) {
        return result;
    }
    for (char character : line) {
        if (character < ' ' || character > '~') {
            return result;
        }
    }
    result.valid = true;
    return result;
}
} // namespace nodx
