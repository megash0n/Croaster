#pragma once
// Vocabulary shared across the control seam: commands in, snapshots out.
#include <cmath>
#include <cstdint>

#include "autotune_sequence.h"
#include "pid.h"

enum class Mode : uint8_t { Manual, Pid, Autotune, Profile };
enum class PidInput : uint8_t { ET, BT, MAX }; // legacy "Target"

enum class SafetyTrip : uint8_t {
    None,
    SensorFault, // PID input unreadable outside Manual, or both probes while Manual heats
    OverTemp,    // any probe above limit
    NoMainsSync, // heater requested but dimmer sees no zero-cross (fan can't run)
    Firmware,    // an OTA update is writing flash
    CommsLost,   // remote manual control and no client traffic for the timeout
};

inline const char* tripName(SafetyTrip t) {
    switch (t) {
    case SafetyTrip::SensorFault:
        return "sensor";
    case SafetyTrip::OverTemp:
        return "overtemp";
    case SafetyTrip::NoMainsSync:
        return "nomains";
    case SafetyTrip::Firmware:
        return "ota";
    case SafetyTrip::CommsLost:
        return "comms";
    default:
        return "";
    }
}

// Comms watchdog: while heating outside a profile run, no client message for
// timeoutS seconds trips CommsLost. Defaults come from config.h; the
// {"command":{"watchdog":...}} command changes them and they are saved in NVS.
struct WatchdogSettings {
    static constexpr uint16_t kMinTimeoutS = 2;
    static constexpr uint16_t kMaxTimeoutS = 600;
    bool enabled = true;
    uint16_t timeoutS = 10;
};

// Fan % after a heater shutdown. Defaults come from config.h; the
// {"command":{"cooldownFan":...}} command changes them and they are saved in NVS.
struct CooldownFan {
    float stop = 65;  // after the stop command (yaeger's stored default)
    float trip = 100; // when a safety trip starts; over-temperature always uses 100
    float endC = 60;  // cooldown ends (fan off) once ET is below this (140 F); cooldown.h
    static constexpr float kMinEndC = 30, kMaxEndC = 150;
};

// Croaster reports temperatures in a user-selected unit ("C", "F" or "K"); the
// controller works in Celsius. These convert absolute temperatures.
inline float toCelsius(float v, char unit) {
    return unit == 'F' ? (v - 32.0f) * 5.0f / 9.0f : (unit == 'K' ? v - 273.15f : v);
}
inline float fromCelsius(float c, char unit) {
    return unit == 'F' ? c * 9.0f / 5.0f + 32.0f : (unit == 'K' ? c + 273.15f : c);
}

// Lowest autotune test point (100 C / 212 F); autotune::makePlan clamps to it.
// Before T57 autotune refused to start below this setpoint (NeedSetpoint).
constexpr float kAutotuneMinSetpointC = 100;

// Last autotune outcome, kept until the next one. Text: autotune_status.h.
enum class AutotuneStatus : uint8_t {
    Idle, // no autotune since boot
    Running,
    Done,         // new gains in use and saved (ApplyAutotune)
    Failed,       // autotuneReason: autotune::Sequence::failure()
    Aborted,      // left by a command or a trip; autotuneReason may say why
    NeedSetpoint, // refused: setpoint below kAutotuneMinSetpointC (before T57; no longer raised)
    NeedProbe,    // refused: PID input probe unreadable
    Required,     // PID or Profile refused: no tuned gains yet (modeNeedsTuning)
    Pending,      // finished; results held until ApplyAutotune or DiscardAutotune
    Discarded,    // results dropped, gains unchanged
};

struct Command {
    enum class Kind : uint8_t {
        SetHeater,   // a = 0..100 % (manual request)
        SetFan,      // a = 0..100 %
        SetSetpoint, // a = C; b = 1: restart the PID bumpless from its present output (recipe steps and preheat)
        SetMode,     // a = (float)Mode  (Manual or Pid; others via Start*)
        SetPidInput, // a = (float)PidInput
        SetGains,    // a,b,c = kp,ki,kd
        StartAutotune, // a = fan test 1/0, NaN = the configured default (CPID_AUTOTUNE_FAN_TEST)
        StartProfile, // profile must already be loaded via Controller::loadProfile
        Stop,         // heater 0, back to Manual; b = 1: fan to a % and start the cooldown (cooldown.h), else fan unchanged
        EmergencyOff, // heater 0, fan 100, Manual
        SetTripFan,   // a = fan % applied when a safety trip starts
        Trip,         // a = (float)SafetyTrip; raised by the network side (comms, OTA)
        ClearTrip,
        SetCorrection, // a, b = BT, ET correction offsets, C differences (CroasterCore::correctionBtCelsius)
        ApplyAutotune,   // pending autotune gains -> in use; autotuneSeq++ saves them
        DiscardAutotune, // drop pending autotune results
        SetFanAssist,    // a = 1/0; acts only with a saved fan test (fan_assist.h)
        SetCooldownEnd,  // a = C: ET below which the cooldown ends (CooldownFan::endC)
    };
    Kind kind;
    float a = 0, b = 0, c = 0;
};

// One control-task sample of both probes (C), before any user correction.
struct ThermoSample {
    float et, bt;       // filtered (moving average), NaN when faulted
    float etRaw, btRaw; // this sample, NaN when faulted
    float ambient;      // MAX31855 cold-junction temp, BT chip first (NaN on MAX6675)
    uint8_t etFault, btFault;
};

// Probe temperatures here are uncorrected: CroasterCore adds the user's
// correction when it reads them through RoasterProbe.
struct Snapshot {
    float et = NAN, bt = NAN, ambient = NAN; // filtered; NaN on fault / before the first sample
    float etRaw = NAN, btRaw = NAN;          // this sample, NaN on fault
    uint8_t etFault = 0, btFault = 0;        // converter fault bits (see max31855.h / max6675.h)
    float heaterPct = 0;                     // requested 0..100 (PID output or manual)
    float fanPct = 0;     // fan output now (base + assist, or the fan test's level)
    float fanBasePct = 0; // the user's, recipe's or profile's fan
    float fanAssistPct = 0; // extra fan from fan assist (0 when idle or off)
    bool fanAssistOn = false; // option set (saved in NVS)
    bool fanTested = false;   // a fan test is saved with the gains
    float setpoint = 0;
    Mode mode = Mode::Manual;
    PidInput pidInput = PidInput::BT;
    PidGains gains;
    bool gainsTuned = false; // gains from a completed autotune or the pid command
    SafetyTrip trip = SafetyTrip::None;
    bool mainsOk = false;
    float profileElapsedS = 0;
    uint32_t autotuneSeq = 0; // increments when new gains were produced
    AutotuneStatus autotune = AutotuneStatus::Idle;
    const char* autotuneReason = ""; // string literal; "" when there is none
    uint32_t autotuneStatusSeq = 0;  // increments on every autotune status update
    autotune::Progress autotuneProgress; // phase, point, cycle; valid while autotune runs
    bool autotunePending = false;        // results waiting for apply or discard
    bool cooling = false;                // cooldown running: fan on until ET is below CooldownFan::endC
    uint32_t uptimeMs = 0;
};

// Heater output a new mode starts from (bumpless transfer: keep the current
// output). The autotune relay's 0/100 % is not an operating point, so a mode
// entered from Autotune starts from 0 instead.
inline float startOutputOnModeChange(Mode from, Mode to, float currentOutput) {
    return from == Mode::Autotune && to != Mode::Autotune ? 0.0f : currentOutput;
}

// What a step in the process value does to the running mode. Changing the PID
// input or a correction offset moves the PV at once. Autotune would measure the
// step as part of a cycle, so it stops (heater off); PID and Profile restart the
// PID bumpless so the derivative term does not kick; Manual ignores it.
enum class PvStepAction : uint8_t { None, AbortAutotune, ResetPid };
inline PvStepAction onPvStep(Mode m) {
    switch (m) {
    case Mode::Autotune:
        return PvStepAction::AbortAutotune;
    case Mode::Pid:
    case Mode::Profile:
        return PvStepAction::ResetPid;
    default:
        return PvStepAction::None;
    }
}

// An operator fan or heater command while autotune runs. The relay owns the
// heater and measures the cycle at the fan speed it started with, so any heater
// command, or a fan command that changes the speed, stops autotune (heater off),
// like a setpoint or offset change. Returns the abort reason, or nullptr. The
// fan rule at autotune start (below 30 % -> 60 %) is set by the controller, not
// a command, so it never counts. `currentFan` is the operator's fan (the
// controller's base fan), not the fan test's level: the fan test's own steps
// are not commands, and a client re-sending the operator's value is no change.
inline const char* outputCommandStopsAutotune(Mode m, Command::Kind k, float requested, float currentFan) {
    if (m != Mode::Autotune)
        return nullptr;
    if (k == Command::Kind::SetHeater)
        return "heater changed";
    if (k == Command::Kind::SetFan) {
        const float fan = requested > 0 ? (requested < 100 ? requested : 100) : 0; // as the fan drivers clamp
        return fan != currentFan ? "fan changed" : nullptr;
    }
    return nullptr;
}

// CPID_REQUIRE_AUTOTUNE: PID and Profile run on the gains, so with untuned
// (default) gains they are refused. Manual and Autotune never need tuning.
inline bool modeNeedsTuning(Mode m, bool requireTuned, bool tuned) {
    return requireTuned && !tuned && (m == Mode::Pid || m == Mode::Profile);
}

// The readings can't support the current heat; the controller trips
// SafetyTrip::SensorFault once this has held for 2 s. PID, Profile and Autotune
// need their input (pv). Manual needs at least one readable probe while the
// heater is on; with neither, the over-temperature trip is blind.
inline bool sensorFault(Mode m, float pv, const ThermoSample& s, float heaterPct) {
    if (m != Mode::Manual)
        return !std::isfinite(pv);
    return heaterPct > 0 && !std::isfinite(s.et) && !std::isfinite(s.bt);
}

inline const char* modeName(Mode m) {
    switch (m) {
    case Mode::Pid:
        return "PID"; // legacy modeToChar() strings
    case Mode::Autotune:
        return "Tuning";
    case Mode::Profile:
        return "Profile";
    default:
        return "Manual";
    }
}
inline const char* pidInputName(PidInput t) {
    return t == PidInput::BT ? "BT" : (t == PidInput::MAX ? "MAX" : "ET");
}
