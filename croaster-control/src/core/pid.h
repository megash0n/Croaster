#pragma once
// Fixed-sample-time PID. Replaces the PID half of the vendored AutoTunePID.
//  - u = Kp*e + Ki*integral(e dt) - Kd*d(PV)/dt   (same gain units as legacy, so
//    stored pidKp/pidKi/pidKd keep their meaning)
//  - derivative on measurement (no kick when the profile moves the setpoint),
//    with a first-order low-pass on the derivative,
//  - integrator held in output units and frozen while saturated (legacy clamped
//    the raw error-integral to 72 C*s, which capped the I term at Ki*72),
//  - bumpless transfer via reset(),
//  - optional feedforward `ff` (heater %) added to the output; the integrator
//    carries only what the feedforward does not predict.
struct PidGains {
    float kp = 0, ki = 0, kd = 0;
};

class Pid {
public:
    struct Config {
        float outMin = 0, outMax = 100;
        float dtSeconds = 0.25f;
        float derivativeAlpha = 0.25f; // 0..1, weight of the newest derivative sample
    };

    explicit Pid(Config c)
        : cfg_(c) {}

    void setGains(const PidGains& g) {
        g_ = g;
    }
    const PidGains& gains() const {
        return g_;
    }

    // Call exactly once per dtSeconds. Returns output clamped to [outMin, outMax].
    float update(float setpoint, float measurement, float ff = 0);

    // Prime state so the next update() continues smoothly from `currentOutput`
    // (pass the feedforward that update() will get).
    void reset(float setpoint, float measurement, float currentOutput, float ff = 0);

    float integralTerm() const {
        return iTerm_;
    }

private:
    float clamp(float v) const {
        return v < cfg_.outMin ? cfg_.outMin : (v > cfg_.outMax ? cfg_.outMax : v);
    }
    // The integrator's range shifts with the feedforward so their sum spans the output range.
    float clampI(float v, float ff) const {
        const float lo = cfg_.outMin - ff, hi = cfg_.outMax - ff;
        return v < lo ? lo : (v > hi ? hi : v);
    }
    Config cfg_;
    PidGains g_;
    float iTerm_ = 0, lastPv_ = 0, dFilt_ = 0;
    bool primed_ = false;
};
