#include "fan_assist.h"

#include <algorithm>
#include <cmath>

namespace fanassist {

float Assist::update(const Settings& s, const Inputs& in, float dtS) {
    const bool controlled = in.mode == Mode::Pid || in.mode == Mode::Profile;
    // Safety paths own the fan: clear at once, no ramp.
    if (!s.enabled || !controlled || in.trip != SafetyTrip::None || !std::isfinite(in.btC) ||
        !std::isfinite(in.setpointC) || !(s.maxExtraPct > 0)) {
        extra_ = 0;
        return 0;
    }
    const float room = std::max(0.0f, 100 - in.baseFanPct);
    const float over = in.btC - in.setpointC - s.bandC;
    float target = 0;
    if (over > 0 && in.heaterPct <= s.heaterNearZeroPct)
        target = std::min(std::min(s.maxExtraPct, room), over * s.pctPerC);
    else if (extra_ > 0 && in.btC > in.setpointC && in.heaterPct <= s.heaterNearZeroPct)
        target = extra_; // inside the band: hold until BT reaches the path
    const float up = s.riseRatePctPerS * dtS, down = s.fallRatePctPerS * dtS;
    if (target > extra_)
        extra_ = std::min(target, extra_ + up);
    else
        extra_ = std::max(target, extra_ - down);
    extra_ = std::min(extra_, std::min(s.maxExtraPct, room));
    return extra_;
}

} // namespace fanassist
