#pragma once
// What autotune learned about this roaster, kept with the PID gains: per test
// point the heater-to-BT gain, time constant and holding heater, and from the
// fan test the holding heater, BT dip and heat-shed rate at each fan level.
// Used for fan feedforward, setpoint-ramp feedforward and fan assist on the
// board, and by the recipe editor for its achievable rate-of-rise warning
// (docs/autotune/fan-test.md). Measured with an empty drum: a bean charge adds
// thermal mass, so real rates of rise are lower than these estimates.
// Pure (no Arduino), so it is host-tested.
#include <cmath>
#include <cstdint>

#include "autotune_sequence.h"

namespace roastermodel {

constexpr uint8_t kVersion = 1;
constexpr int kMaxPoints = autotune::kMaxPoints;
constexpr int kMaxFanLevels = autotune::kMaxFanLevels;
constexpr int kMaxFanChannels = 2; // 0 = exhaust fan; 1 reserved for a second fan output

struct PointModel {
    float targetC = 0;
    float gainCPerPct = NAN; // steady BT rise over ambient per heater %
    float tauS = NAN;        // first-order time constant
    float holdPct = NAN;     // heater that held this temperature at the run's fan
};

struct FanLevel {
    float fanPct = 0;
    float holdPct = NAN, dipC = NAN, recoveryS = NAN, gainCPerPct = NAN, tauS = NAN, shedCpm = NAN;
};

struct FanModel {
    bool valid = false; // at least two levels held
    float testC = 0;
    int levelCount = 0;
    FanLevel levels[kMaxFanLevels];
};

struct RoasterModel {
    uint8_t version = kVersion;
    bool valid = false; // at least one point with a model
    float ambientC = 25;
    float runFanPct = NAN; // fan during the test points
    int pointCount = 0;
    PointModel points[kMaxPoints];
    FanModel fan[kMaxFanChannels];
};

// From a finished autotune report; `runFanPct` is the fan during its points.
RoasterModel fromReport(const autotune::Report& r, float runFanPct);

// The model saved by ApplyAutotune: `fresh` from the new run, keeping the
// previous fan test for a channel the new run did not measure (fan test off
// or failed), since airflow loss does not change with re-tuned gains.
RoasterModel merge(const RoasterModel& previous, const RoasterModel& fresh);

// True when `m` was saved by this firmware's layout (NVS blob check).
inline bool usable(const RoasterModel& m) {
    return m.version == kVersion && m.valid;
}

// Heater % that holds `tempC` at `fanPct`. Uses the fan test when there is one
// (interpolated between levels, held at the ends), otherwise the test points
// at the run's fan. Heat loss scales with BT - ambient. NaN without data.
float holdPct(const RoasterModel& m, float tempC, float fanPct, int channel = 0);

// Heater % to add for running the fan at `fanPct` instead of the lowest tested
// level, at `tempC`. 0 without a fan test. The PID integrator carries the rest.
float fanFeedforward(const RoasterModel& m, float fanPct, float tempC, int channel = 0);

// Heater % that keeps BT on a setpoint moving at `slopeCps` (C/s):
// tau * slope / K, K the heater gain at this fan, limited to +-kMaxRampFfPct.
// 0 without a model.
constexpr float kMaxRampFfPct = 40;
float rampFeedforward(const RoasterModel& m, float tempC, float fanPct, float slopeCps);

// Highest BT rate of rise (C/min) at `tempC` with the heater at `heaterMaxPct`:
// K * (heaterMax - hold) / tau. NaN without a model.
float maxRorCpm(const RoasterModel& m, float tempC, float fanPct, float heaterMaxPct);

// Rate of a moving setpoint (C/s) from its samples, for rampFeedforward. A
// recipe ramp moves the setpoint in small steps (every 0.5 s); a jump larger
// than stepC is a new target, not a ramp, so the estimate restarts at 0.
class SetpointSlope {
public:
    float update(float setpointC, float dtS);
    float value() const {
        return slope_;
    }
    void reset() {
        primed_ = false;
        slope_ = 0;
    }

private:
    static constexpr float kStepC = 2;    // larger single change: a step
    static constexpr float kFilterS = 4;  // first-order filter time constant
    static constexpr float kMaxCps = 1;   // 60 C/min: beyond any roast ramp
    bool primed_ = false;
    float last_ = 0, slope_ = 0;
};

} // namespace roastermodel
