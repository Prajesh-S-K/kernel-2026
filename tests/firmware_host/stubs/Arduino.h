// Host stub of the Arduino core: just enough for firmware/src to compile and run natively.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#define LOW 0
#define HIGH 1
#define INPUT_PULLUP 2
#define OUTPUT 1
uint32_t millis();
inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}
inline int digitalRead(int) {
    return HIGH;
}
// Scripted serial port: tests push bytes into `input` and read what the firmware wrote from
// `output`.
struct HostSerial {
    std::string input;
    size_t position = 0;
    std::string output;
    size_t writeCapacity = 4096;
    void begin(unsigned long) {}
    void println(const char* text) {
        output += text;
        output += '\n';
    }
    int available() const {
        return int(input.size() - position);
    }
    int read() {
        return position < input.size() ? (unsigned char)input[position++] : -1;
    }
    int availableForWrite() const {
        return int(writeCapacity);
    }
    size_t write(const uint8_t* data, size_t size) {
        output.append(reinterpret_cast<const char*>(data), size);
        return size;
    }
};
extern HostSerial Serial;
