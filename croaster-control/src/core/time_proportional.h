#pragma once
// Time-proportioning ("slow PWM") for a zero-cross SSR.
// Duty is latched at the start of each window so a window is never cut short.
// Pulses shorter than minPulseMs are dropped/extended: a zero-cross SSR can only
// switch on whole mains half-cycles (8.3 ms @60 Hz, 10 ms @50 Hz).
#include <cstdint>

class TimeProportional {
public:
    explicit TimeProportional(uint32_t windowMs = 1000, uint32_t minPulseMs = 20)
        : windowMs_(windowMs)
        , minPulseMs_(minPulseMs) {}

    void setDuty(float percent) {
        if (!(percent > 0))
            percent = 0; // also catches NaN
        if (percent > 100)
            percent = 100;
        pending_ = percent;
    }
    float duty() const {
        return pending_;
    }
    // On-time per window for a duty, after the minimum-pulse rule.
    uint32_t onMsFor(float percent) const {
        if (!(percent > 0))
            return 0;
        if (percent > 100)
            percent = 100;
        uint32_t on = static_cast<uint32_t>(percent * windowMs_ / 100.0f + 0.5f);
        if (on < minPulseMs_)
            on = 0;
        if (windowMs_ - on < minPulseMs_)
            on = windowMs_;
        return on;
    }
    // True when the duty switches the SSR on at all (a pulse survives the minimum).
    bool delivers(float percent) const {
        return onMsFor(percent) > 0;
    }

    // Call at least every few ms. Returns the desired output state.
    bool isOn(uint32_t nowMs) {
        if (!started_ || nowMs - windowStart_ >= windowMs_) {
            // resync if we fell more than one window behind
            windowStart_ = (started_ && nowMs - windowStart_ < 2 * windowMs_) ? windowStart_ + windowMs_ : nowMs;
            started_ = true;
            onMs_ = onMsFor(pending_);
        }
        return (nowMs - windowStart_) < onMs_;
    }

private:
    uint32_t windowMs_, minPulseMs_;
    uint32_t windowStart_ = 0, onMs_ = 0;
    float pending_ = 0;
    bool started_ = false;
};
