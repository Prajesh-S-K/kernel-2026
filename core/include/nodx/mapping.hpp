#pragma once
// Guided movement mapping ("configured control"): the user teaches right, left, up and down; the
// firmware learns which gyro direction means horizontal and which means vertical, for ANY fixed
// mounting, then smooths with a timestamp-aware One Euro filter. All numeric defaults are START
// values until measured on hardware (docs/PARAMETERS.md).
#include "motion.hpp"
#include "profile.hpp"
#include "sensor.hpp"
#include <array>
#include <cstdint>
#include <vector>

namespace nodx {
using Vec3 = std::array<float, 3>;
float dot(const Vec3& a, const Vec3& b);
float norm(const Vec3& a);

// ---------------------------------------------------------------- One Euro filter
struct OneEuroParams {
    float minCutoffHz = start::oneEuroMinCutoffHz;
    float beta = start::oneEuroBeta; // cutoff increase per (deg/s^2) of filtered signal speed
    float derivativeCutoffHz = start::oneEuroDerivativeCutoffHz;
    bool valid() const;
};
class OneEuroFilter {
public:
    // dtSeconds comes from the sample timestamps; an unusable dt restarts the filter.
    float filter(float x, float dtSeconds, const OneEuroParams& params);
    void reset();

private:
    bool primed_ = false;
    float x_ = 0, dx_ = 0, prevRaw_ = 0;
};

// ---------------------------------------------------------------- learned settings record
struct LearnedControl {
    static constexpr uint32_t magic = 0x4c544e43; // "CNTL"
    static constexpr uint32_t schema = 1;
    static constexpr size_t recordBytes = 120;
    Vec3 horizontal{1, 0, 0}; // unit row, sensor coordinates: positive = pointer right
    Vec3 vertical{0, 1, 0};   // unit row, sensor coordinates: positive = pointer down
    Vec3 bias{};              // gyro bias at rest, deg/s, sensor coordinates
    Vec3 noise{1, 1, 1};      // gyro noise sigma at rest per sensor axis, deg/s
    Vec3 gravity{0, 0, 1};    // unit gravity direction while still (detects a changed mounting)
    std::array<float, 4> gain{start::mapGainFallback, start::mapGainFallback,
                              start::mapGainFallback, start::mapGainFallback}; // L R U D, px/deg
    OneEuroParams filter;
    std::array<float, 2> deadzoneEnter{1, 1}; // deg/s, horizontal then vertical
    std::array<float, 2> deadzoneExit{.6f, .6f};
    bool valid() const;
    Vec3 rollRow() const; // third orthogonal row (cross product): side tilt
};
std::vector<uint8_t> encode(const LearnedControl& control, uint32_t generation);
bool decode(const std::vector<uint8_t>& bytes, LearnedControl& control, uint32_t& generation);

enum class ControlRecordState { Missing, Valid, Corrupt };
const char* name(ControlRecordState state);
// Two alternating slots, same transactional rules as the profile repository. Separate keys and a
// separate format: the 84-byte profile and the hands-free record are untouched.
class ControlRepository {
public:
    explicit ControlRepository(ProfileStorage& storage) : storage_(storage) {}
    bool load(LearnedControl& control);
    bool save(const LearnedControl& control);
    ControlRecordState state() const {
        return state_;
    }

private:
    ProfileStorage& storage_;
    ControlRecordState state_ = ControlRecordState::Missing;
};

// ---------------------------------------------------------------- runtime processor
// Projects the bias-corrected raw gyro onto the learned rows, filters with One Euro, applies the
// noise-based deadzone with hysteresis, and hands the existing AdaptiveEngine a Motion.
class ControlProcessor {
public:
    Motion process(const MotionSample& rawSample, const LearnedControl& control);
    void reset();
    Vec3 lastRates() const { // bias-corrected, unfiltered (horizontal, vertical, side tilt) deg/s
        return rates_;
    }

private:
    OneEuroFilter h_, v_;
    bool activeH_ = false, activeV_ = false, primed_ = false;
    uint32_t lastMs_ = 0;
    Vec3 rates_{};
};

// ---------------------------------------------------------------- guided teaching
enum class MapPhase { Idle, Still, Example, Analyze, Preview, Done, Failed };
const char* name(MapPhase phase);
enum class MapCue { None, HoldStill, Countdown, Go, Recording, ReturnToCentre, Preview };
const char* name(MapCue cue);
const char* directionName(unsigned direction);

struct MappingStatus {
    MapPhase phase = MapPhase::Idle;
    MapCue cue = MapCue::None;
    unsigned step = 0, steps = 0; // current example (0-based) of all examples
    int direction = -1;           // 0 right, 1 left, 2 up, 3 down
    bool validation = false;
    unsigned example = 0, examplesPerDirection = 0, retries = 0, interruptions = 0;
    uint32_t cueMs = 0, stillMs = 0, windowMs = 0;
    float progress = 0;
    const char* reason = "idle"; // last rejection or failure, in plain words
    float previewX = 0, previewY = 0;      // filtered rates in the learned frame (deg/s)
    float previewAngleX = 0, previewAngleY = 0; // integrated preview angles (deg), bounded
    Vec3 bias{}, noise{};
    // Filled by System: what the learned settings are and where they live.
    bool learnedValid = false, unsaved = false;
    const char* stored = "MISSING";
    const char* mode = "OFF";
    const char* blocked = "";
    const char* saveResult = "";
    float mountingDeg = 0;      // angle between the gravity direction now and while teaching
    bool mountingWarning = false;
};
// Writes one JSON object (no trailing newline); 0 when it does not fit.
size_t mappingJson(char* out, size_t capacity, const MappingStatus& status);
constexpr size_t mappingJsonCapacity = 1280;

class MappingTeacher {
public:
    void begin(uint32_t now);
    void cancel();
    // Raw (sensor-coordinate) samples only; the caller has already checked sensor validity.
    void tick(const MotionSample& raw, uint32_t now);
    // Preview -> Done. Returns false in any other phase.
    bool accept();
    MappingStatus status(uint32_t now) const;
    bool active() const {
        return phase_ != MapPhase::Idle && phase_ != MapPhase::Done && phase_ != MapPhase::Failed;
    }
    MapPhase phase() const {
        return phase_;
    }
    const LearnedControl& candidate() const {
        return candidate_;
    }

private:
    enum class Sub { Countdown, WaitOnset, Moving, Settle };
    MapPhase phase_ = MapPhase::Idle;
    Sub sub_ = Sub::Countdown;
    uint32_t phaseStart_ = 0, subStart_ = 0, lastMs_ = 0;
    bool pendingAdvance_ = false;
    // stillness
    uint32_t runStart_ = 0;
    unsigned runCount_ = 0, interruptions_ = 0;
    Vec3 runMean_{}, runM2_{}, accelMean_{};
    const char* reason_ = "idle";
    // examples
    unsigned step_ = 0, retries_ = 0;
    Vec3 bias_{}, noise_{}, gravity_{0, 0, 1};
    std::array<std::vector<Vec3>, 4> taught_;
    // current movement
    Vec3 rotation_{};
    float peak_ = 0, path_ = 0;
    uint32_t moveStart_ = 0, calmSince_ = 0;
    unsigned onsetCount_ = 0;
    std::vector<std::pair<Vec3, float>> pre_;
    // results
    LearnedControl candidate_;
    std::array<float, 4> angle_{};
    Vec3 filtered_{};
    float angleX_ = 0, angleY_ = 0;

    void fail(const char* reason);
    void startStill(uint32_t now);
    void tickStill(const MotionSample& s, uint32_t now);
    void tickExample(const MotionSample& s, uint32_t now);
    void beginStep(uint32_t now);
    void finishMovement(uint32_t now);
    void rejectExample(const char* reason, uint32_t now);
    void acceptExample(uint32_t now);
    bool analyze();
    bool validateExample(unsigned direction, const Vec3& rotation, const char** why) const;
    void tickPreview(const MotionSample& s, uint32_t now);
    float onsetRate() const;
    float exitRate() const;
};
} // namespace nodx
