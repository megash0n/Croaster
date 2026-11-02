#pragma once
// Optional fans next to the roast fan (config.h: CPID_SCROLL_FAN, CPID_FAN1,
// CPID_FAN2) and the box temperature they follow. Pure logic, host-tested; the
// outputs and settings are in app/AuxFansPlugin.
//  - Scroll fan (relay): on while the cooldown runs; a manual on/off only in
//    Manual mode with the heater off and no cooldown.
//  - FAN1 (case fan): follows the box temperature. FAN2 (ESP fan): follows the
//    ESP32-S3 chip temperature. Each turns on at its set temperature, off 3 C
//    below it; the page can force it on or off.
#include <cmath>
#include <cstdint>
#include <cstring>

#include "control_types.h"

namespace auxfans {

enum class Override : uint8_t { Auto, On, Off };

inline const char* overrideName(Override o) {
    return o == Override::On ? "on" : (o == Override::Off ? "off" : "auto");
}
inline bool parseOverride(const char* s, Override& out) {
    if (!s)
        return false;
    if (!strcmp(s, "auto"))
        out = Override::Auto;
    else if (!strcmp(s, "on"))
        out = Override::On;
    else if (!strcmp(s, "off"))
        out = Override::Off;
    else
        return false;
    return true;
}

enum class BoxSource : uint8_t { None, Sensor, Chip };
inline const char* boxSourceName(BoxSource s) {
    return s == BoxSource::Sensor ? "sensor" : (s == BoxSource::Chip ? "chip" : "none");
}

struct BoxTemp {
    float c = NAN;
    BoxSource source = BoxSource::None;
};

// The thermocouple chips' temperature (max31855::boxTemperatureC), else the
// ESP32-S3 chip temperature (MAX6675 boards, or no chip answering).
inline BoxTemp boxTemperature(float sensorC, float chipC) {
    BoxTemp t;
    if (std::isfinite(sensorC)) {
        t.c = sensorC;
        t.source = BoxSource::Sensor;
    } else if (std::isfinite(chipC)) {
        t.c = chipC;
        t.source = BoxSource::Chip;
    }
    return t;
}

// On at onC or above, off at onC - kHysteresisC or below. With no reading the
// fan runs, so a failed sensor never leaves the electronics without air.
class ThermoFan {
public:
    static constexpr float kHysteresisC = 3;
    bool update(float tempC, float onC, Override o) {
        if (o == Override::On)
            on_ = true;
        else if (o == Override::Off)
            on_ = false;
        else if (!std::isfinite(tempC) || tempC >= onC)
            on_ = true;
        else if (tempC <= onC - kHysteresisC)
            on_ = false;
        return on_;
    }
    bool on() const {
        return on_;
    }

private:
    bool on_ = false;
};

// Scroll fan: runs while the cooldown runs (stop, emergency off and safety trips
// all start one) and stops when it ends. Off at boot. A manual request is taken
// only when idle, and ends when the heater comes on, the mode leaves Manual or a
// cooldown starts.
class ScrollFan {
public:
    static bool idle(Mode m, float heaterPct, bool cooling) {
        return m == Mode::Manual && !(heaterPct > 0) && !cooling;
    }
    // Returns false when refused (turning it on while not idle). Off is always taken.
    bool setManual(bool on, Mode m, float heaterPct, bool cooling) {
        if (on && !idle(m, heaterPct, cooling))
            return false;
        manual_ = on;
        return true;
    }
    bool update(Mode m, float heaterPct, bool cooling) {
        if ((cooling && !cooling_) || m != Mode::Manual || heaterPct > 0)
            manual_ = false;
        cooling_ = cooling;
        on_ = cooling || manual_;
        return on_;
    }
    bool on() const {
        return on_;
    }
    bool manual() const {
        return manual_;
    }

private:
    bool manual_ = false, cooling_ = false, on_ = false;
};

} // namespace auxfans
