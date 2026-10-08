#pragma once
#include <cstdint>

namespace nodx::start {
// Experimental START values. Units and hardware acceptance: docs/PARAMETERS.md.
constexpr uint32_t sampleMs = 10;
constexpr uint32_t timeoutMs = 100;
constexpr uint32_t recoverySamples = 20;
constexpr float maxGyro = 240.f;
constexpr float maxAccel = 4.f;
constexpr float alpha = .35f;
constexpr float deadzoneBase = .6f;
constexpr float noiseMultiplier = 3.f;
constexpr float gain = 30.f; // pixels per degree
constexpr float precision = 5.f;
constexpr float fast = 35.f;
constexpr float dwellTolerance = 8.f; // estimated output pixels, NOT OS position
constexpr uint32_t dwellMs = 1000;
constexpr uint32_t armMs = 250;
constexpr uint32_t debounceMs = 30;
constexpr float scrollThreshold = 12.f; // roll angle degrees
constexpr float scrollGain = .8f;       // wheel units/second/degree beyond threshold
constexpr uint32_t phaseMs = 1500;
constexpr uint32_t minPhaseSamples = 100;
constexpr float minimumRange = 3.f;
constexpr float maxRestSigma = 4.f;
constexpr float maxRestBias = 10.f;
constexpr int maxPointer = 40;
constexpr int maxWheel = 5;
} // namespace nodx::start
