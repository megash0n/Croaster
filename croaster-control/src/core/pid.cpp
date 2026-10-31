#include "pid.h"

#include <cmath>

float Pid::update(float sp, float pv, float ff) {
    if (!std::isfinite(pv) || !std::isfinite(sp))
        return cfg_.outMin;
    if (!std::isfinite(ff))
        ff = 0;
    const float dt = cfg_.dtSeconds;
    const float e = sp - pv;
    const float dPv = primed_ ? (pv - lastPv_) / dt : 0.0f;
    lastPv_ = pv;
    primed_ = true;
    dFilt_ += cfg_.derivativeAlpha * (dPv - dFilt_);

    const float p = g_.kp * e;
    const float d = -g_.kd * dFilt_;
    const float iNext = iTerm_ + g_.ki * e * dt;
    const float uNext = p + iNext + d + ff;
    // conditional integration: don't wind further into saturation
    const bool windingUp = uNext > cfg_.outMax && e > 0;
    const bool windingDown = uNext < cfg_.outMin && e < 0;
    if (!windingUp && !windingDown)
        iTerm_ = clampI(iNext, ff);
    return clamp(p + iTerm_ + d + ff);
}

void Pid::reset(float sp, float pv, float out, float ff) {
    if (!std::isfinite(ff))
        ff = 0;
    lastPv_ = pv;
    primed_ = std::isfinite(pv);
    dFilt_ = 0;
    const float e = std::isfinite(sp) && std::isfinite(pv) ? sp - pv : 0;
    iTerm_ = clampI(out - g_.kp * e - ff, ff);
}
