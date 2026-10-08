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
// Hands-free gestures and enable switch. All START, unvalidated on hardware (HANDS_FREE_SPEC.md).
constexpr float gestureAlpha = .5f;            // recognition EMA, dimensionless
constexpr uint32_t gestureNeutralHoldMs = 300; // neutral needed before (re)arming
constexpr uint32_t gestureRampMs = 120;        // slow motion longer than this disarms
constexpr float gestureExitRatio = .5f;        // stroke ends below this fraction of enterRate
constexpr float gestureCrossRatio = .6f;       // other axes may reach this fraction of the peak
constexpr float gestureMinPeak = 20.f;         // deg/s, training rejects weaker strokes
constexpr unsigned trainExamples = 4;          // accepted examples required
constexpr unsigned trainMaxRejects = 8;        // rejected attempts before training fails
constexpr uint32_t trainRestMs = 1000;         // rest capture, with at least trainRestSamples
constexpr unsigned trainRestSamples = 80;
constexpr uint32_t trainWaitMs = 6000;      // wait for an example to begin
constexpr uint32_t trainCaptureMs = 3000;   // longest example
constexpr uint32_t trainValidateMs = 20000; // time allowed for the validation repeat
constexpr uint32_t enableDebounceMs = 30;   // ON must be stable; OFF is immediate
} // namespace nodx::start
