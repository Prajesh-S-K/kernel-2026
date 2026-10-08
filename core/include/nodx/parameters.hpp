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
constexpr uint32_t calibrationCueMs = 3000; // guided calibration countdown before each phase
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
constexpr float demoMaxStep = 6.f;          // movement-only demo: pixels per report, both axes
// Uncalibrated pointer demo (RAM only, movement only). Conservative START values, not tuned.
constexpr float uncalDemoGain = 12.f;       // pixels per degree, all four directions
constexpr float uncalDemoDeadzone = 2.5f;   // deg/s; above the measured idle bias of the bench MPU
constexpr float uncalDemoAlpha = .3f;       // filter, dimensionless
constexpr float uncalDemoMaxStep = 4.f;     // pixels per report, both axes
// Dwell clicking in the uncalibrated demo (explicitly enabled, RAM only, adjustable). The distance unit
// is accumulated OUTGOING HID movement; host pointer acceleration means it is not verified screen
// pixels. The dwell duration is the progress phase; the selection manager's 250 ms arming precedes it.
constexpr uint32_t uncalDwellMs = 1200;
constexpr float uncalDwellTolerance = 8.f;
// One Euro smoothing for configured control. EXPERIMENTAL START values: chosen on synthetic recordings
// only (tests/test_mapping.cpp prints the comparison with the old EMA); not hardware-measured.
constexpr float oneEuroMinCutoffHz = 2.0f;
constexpr float oneEuroBeta = 0.02f;
constexpr float oneEuroDerivativeCutoffHz = 2.0f;
// Guided mapping (teaching). START values, not measured on users.
constexpr uint32_t mapStillMs = 2000;       // qualified stillness required (contiguous)
constexpr uint32_t mapStillWindowMs = 10000; // time allowed to get it
constexpr float mapStillGyroDeviation = 4.f;   // deg/s from the running mean while "still"
constexpr float mapStillAccelDeviation = .08f; // g from the running mean while "still"
constexpr uint32_t mapCountdownMs = 3000;   // 3-2-1 before each example
constexpr uint32_t mapOnsetWindowMs = 2000; // time to start moving after "go"
constexpr uint32_t mapMaxMoveMs = 2500;     // longest accepted movement
constexpr uint32_t mapSettleMs = 1500;      // return-to-centre cue (samples ignored)
constexpr uint32_t mapMinMoveMs = 120;
constexpr uint32_t mapCalmMs = 150;         // calm this long = movement finished
constexpr float mapOnsetFloor = 12.f;       // deg/s, also at least 8 sigma
constexpr float mapExitFloor = 6.f;         // deg/s, also at least 4 sigma
constexpr float mapMinAngle = 6.f;          // degrees of net rotation per example
constexpr float mapMinStraightness = .6f;   // net rotation / path length
constexpr float mapConsistency = .8f;       // cosine to the earlier examples of a direction
constexpr float mapOppositeMax = -.8f;      // right vs left and up vs down must be this opposed
constexpr float mapSeparationMax = .5f;     // |cos| of horizontal vs vertical
constexpr unsigned mapExamples = 3;         // per direction, plus one validation each
constexpr unsigned mapMaxRetries = 3;       // per example
constexpr float mapTravelPixels = 300.f;    // output units a comfortable movement should cover
constexpr float mapGainFallback = 12.f;
constexpr float mapDeadzoneSigmas = 4.f;
constexpr float mapDeadzoneFloor = .8f;
constexpr float mapDeadzoneCap = 10.f;
constexpr float mapHysteresis = .6f;        // exit threshold / enter threshold
constexpr float mapMountingToleranceDeg = 25.f; // gravity direction change that needs relearning
constexpr uint32_t mapPreviewTimeoutMs = 120000;
// Gesture click (optional, RAM only). START values, not measured on users.
constexpr uint32_t clickRestMs = 1500;        // qualified stillness before training
constexpr uint32_t clickRestWindowMs = 6000;
constexpr unsigned clickExamples = 5;
constexpr unsigned clickValidations = 2;
constexpr unsigned clickMaxRetries = 3;
constexpr float clickEnterFloor = 25.f;       // deg/s, also at least 8 sigma
constexpr float clickExitFloor = 8.f;         // deg/s, also at least 4 sigma
constexpr float clickMinPeak = 30.f;          // weaker patterns are rejected
constexpr uint32_t clickMinMs = 250;          // gesture duration bounds
constexpr uint32_t clickMaxMs = 1500;
constexpr uint32_t clickCalmMs = 150;         // calm this long = the gesture ended
constexpr uint32_t clickNeutralMs = 300;      // neutral return needed before rearming
constexpr float clickPrincipalShare = .6f;    // energy share of the principal axis
constexpr float clickDominance = 1.5f;        // principal axis vs the others to open a candidate
constexpr float clickConsistency = .6f;       // relative distance of an example to the earlier ones
constexpr float clickMaxExampleError = .45f;  // leave-one-out error that still yields a template
constexpr float clickThresholdMargin = 1.6f;
constexpr float clickThresholdMin = .25f;
constexpr float clickThresholdMax = .5f;
constexpr uint32_t clickConfusionWindowMs = 15000;
constexpr uint32_t clickConfusionActivityMs = 5000; // ordinary pointing observed with no false click
constexpr uint32_t enableDebounceMs = 30;   // ON must be stable; OFF is immediate
} // namespace nodx::start
