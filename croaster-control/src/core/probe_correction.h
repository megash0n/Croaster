#pragma once
// User correction offsets on the control side. Croaster adds correctionBt/
// correctionEt to every temperature it shows, and clients send setpoints on that
// corrected scale. The controller reads the probes itself, so it adds the same
// offsets (as C differences from CroasterCore::correctionBtCelsius/EtCelsius)
// before PID, autotune and the setpoint compare. Over-temperature checks both the
// raw and the corrected reading, so a negative offset cannot hide a hot probe.
// The snapshot keeps raw values; CroasterCore corrects them itself.
#include <cmath>

#include "control_types.h"

struct ProbeCorrection {
    float btC = 0, etC = 0; // offsets, C differences
};

// The sample on the displayed (corrected) scale. NaN stays NaN; ambient and
// fault bits are unchanged.
inline ThermoSample corrected(const ThermoSample& s, const ProbeCorrection& k) {
    ThermoSample c = s;
    c.et += k.etC;
    c.bt += k.btC;
    c.etRaw += k.etC;
    c.btRaw += k.btC;
    return c;
}

// The process value for the selected PID input; NaN when that probe is unreadable.
inline float selectPidInput(PidInput in, const ThermoSample& s) {
    switch (in) {
    case PidInput::BT:
        return s.bt;
    case PidInput::MAX:
        return std::isfinite(s.et) && std::isfinite(s.bt) ? fmaxf(s.et, s.bt) : NAN;
    default:
        return s.et;
    }
}

// True when either probe's raw or corrected reading is above limitC.
inline bool overTemperature(const ThermoSample& raw, const ProbeCorrection& k, float limitC) {
    const auto hot = [limitC](float t, float offset) {
        return std::isfinite(t) && fmaxf(t, t + offset) > limitC;
    };
    return hot(raw.etRaw, k.etC) || hot(raw.btRaw, k.btC);
}

// Copies the uncorrected probe values into the snapshot.
inline void publishProbes(const ThermoSample& raw, Snapshot& out) {
    out.et = raw.et;
    out.bt = raw.bt;
    out.etRaw = raw.etRaw;
    out.btRaw = raw.btRaw;
    out.ambient = raw.ambient;
    out.box = raw.box;
    out.etFault = raw.etFault;
    out.btFault = raw.btFault;
}
