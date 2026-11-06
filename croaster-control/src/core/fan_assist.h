#pragma once
// Fan assist: lets PID shed heat with airflow (docs/autotune/fan-test.md).
// When BT runs above the setpoint path with the heater already at or near 0 %,
// the beans are carrying more heat than the curve wants (momentum, typically
// late in drying or approaching first crack) and the heater has nothing left
// to take away. More airflow removes heat from the beans by convection, so the
// fan rises above the user's or recipe's value, within a bound and at a
// limited rate, and returns to it once BT is back on the path.
// Never below the user's or recipe's fan; off in Manual (stop, cooldown,
// trips) and Autotune; off when the setpoint is off (0 is not a target of
// 0 C); off unless a fan test is saved and the option is on.
// Pure (no Arduino), so it is host-tested.
#include <cstdint>

#include "control_types.h"

namespace fanassist {

struct Settings {
    bool enabled = false;      // option on and a fan test saved
    float maxExtraPct = 20;    // CPID_FAN_ASSIST_MAX
    float bandC = 2;           // BT this far above the setpoint before assisting
    float pctPerC = 5;         // extra fan per C beyond the band
    float heaterNearZeroPct = 2;
    float riseRatePctPerS = 1; // rate limits
    float fallRatePctPerS = 2;
};

struct Inputs {
    Mode mode = Mode::Manual;
    SafetyTrip trip = SafetyTrip::None;
    float btC = NAN, setpointC = NAN;
    float heaterPct = 0; // controller output this step
    float baseFanPct = 0; // the user's or recipe's fan
};

class Assist {
public:
    // Call once per control step; returns the extra fan % (>= 0) to add to the base.
    float update(const Settings& s, const Inputs& in, float dtS);
    float extra() const {
        return extra_;
    }
    void reset() {
        extra_ = 0;
    }

private:
    float extra_ = 0;
};

} // namespace fanassist
