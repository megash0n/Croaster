#pragma once
// SSR heater, time-proportioned (slow PWM over a window, e.g. 1 s = 100 mains
// half-cycles at 50 Hz, so a zero-cross SSR switches whole half-cycles).
// Interface: set() any time from the control task, tick() every <=10 ms.
#include <cstdint>

#include "core/time_proportional.h"

class HeaterOutput {
public:
    HeaterOutput(int pin, uint32_t windowMs, float maxPercent, bool activeHigh = true)
        : pin_(pin)
        , activeHigh_(activeHigh)
        , maxPct_(maxPercent)
        , tp_(windowMs) {}
    void begin();
    // percent 0..100 is a *request*; physical duty = percent * maxPercent / 100
    // (legacy MAX_HEATER_POWER=90 scaled manual output the same way).
    void set(float percent);
    float requested() const {
        return req_;
    }
    // True when this request would switch the element on (see TimeProportional::delivers).
    bool heats(float percent) const {
        return percent > 0 && tp_.delivers((percent > 100 ? 100 : percent) * maxPct_ / 100.0f);
    }
    void tick(uint32_t nowMs);
    void off() {
        set(0);
    }

private:
    int pin_;
    bool activeHigh_;
    float maxPct_, req_ = 0;
    TimeProportional tp_;
    bool level_ = false;
};
