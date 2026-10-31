#pragma once
// Autotune status text for telemetry and the OLED header (design D17).
// The controller records what happened (AutotuneStatus in the Snapshot); these
// turn it into words in the active display unit. Pure, so it is host-tested.
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "control_types.h"

namespace autotunetext {

// Telemetry and getControl: "idle", "running: heating to 350 F (2/3)",
// "running: cycle 3/6 at 350 F (2/3)", "running: fan test 60 % at 300 F (2/3)",
// "running: fan test shed 60 %", "results ready", "done", "discarded",
// "failed: no oscillation", "aborted", "aborted: setpoint changed",
// "need a valid probe", "autotune required".
inline void full(const Snapshot& s, char unit, char* out, size_t n) {
    const char* r = s.autotuneReason;
    const autotune::Progress& p = s.autotuneProgress;
    const float t = fromCelsius(p.targetC, unit);
    const int i = p.point + 1, k = p.pointCount;
    switch (s.autotune) {
    case AutotuneStatus::Running:
        switch (p.phase) {
        case autotune::Phase::Heating:
            snprintf(out, n, "running: heating to %.0f %c (%d/%d)", t, unit, i, k);
            break;
        case autotune::Phase::Settling:
            snprintf(out, n, "running: settling %d/%d at %.0f %c (%d/%d)", p.cycle, p.cycleOf, t, unit, i, k);
            break;
        case autotune::Phase::Oscillating:
            snprintf(out, n, "running: cycle %d/%d at %.0f %c (%d/%d)", p.cycle, p.cycleOf, t, unit, i, k);
            break;
        case autotune::Phase::Computing:
            snprintf(out, n, "running: computing (%d/%d)", i, k);
            break;
        case autotune::Phase::FanHold:
            snprintf(out, n, "running: fan test %.0f %% at %.0f %c (%d/%d)", p.fan, t, unit, p.fanLevel + 1,
                     p.fanLevelCount);
            break;
        case autotune::Phase::FanShed:
            snprintf(out, n, "running: fan test shed %.0f %%", p.fan);
            break;
        default:
            snprintf(out, n, "running");
            break;
        }
        break;
    case AutotuneStatus::Pending:
        snprintf(out, n, "results ready");
        break;
    case AutotuneStatus::Discarded:
        snprintf(out, n, "discarded");
        break;
    case AutotuneStatus::Done:
        snprintf(out, n, "done");
        break;
    case AutotuneStatus::Failed:
        snprintf(out, n, "failed: %s", r);
        break;
    case AutotuneStatus::Aborted:
        snprintf(out, n, "aborted%s%s", *r ? ": " : "", r);
        break;
    case AutotuneStatus::NeedSetpoint:
        snprintf(out, n, "need setpoint >= %.0f %c", fromCelsius(kAutotuneMinSetpointC, unit), unit);
        break;
    case AutotuneStatus::NeedProbe:
        snprintf(out, n, "need a valid probe");
        break;
    case AutotuneStatus::Required:
        snprintf(out, n, "autotune required");
        break;
    default:
        snprintf(out, n, "idle");
        break;
    }
}

// OLED header, at most 20 characters: "AT2/3 heat 350F",
// "AT2/3 cyc3/6 350F", "AT fan 60% 300F", "AT fan shed 60%", "AT results ready", "AT discarded", "AT done",
// "AT fail: timeout", "AT aborted", "AT: SV<212F", "AT: no probe",
// "Autotune first".
inline void brief(const Snapshot& s, char unit, char* out, size_t n) {
    // Short forms of RelayAutotune's and the controller's reasons.
    static const char* const kShort[][2] = {
        {"sensor fault", "sensor"},
        {"over temperature", "overtemp"},
        {"oscillation too small vs hysteresis", "small swing"},
        {"no oscillation", "no cycle"},
        {"no heat-up", "no heat"},
        {"no consistent oscillation", "inconsistent"},
        {"too hot to start", "too hot"},
        {"time limit", "timeout"},
        {"setpoint changed", "SV change"},
        {"correction changed", "offset"},
        {"input changed", "input"},
        {"fan changed", "fan"},
        {"heater changed", "heater"},
    };
    const char* r = s.autotuneReason;
    for (const auto& m : kShort)
        if (std::strcmp(r, m[0]) == 0)
            r = m[1];
    char line[48];
    const autotune::Progress& p = s.autotuneProgress;
    const float t = fromCelsius(p.targetC, unit);
    const int i = p.point + 1, k = p.pointCount;
    switch (s.autotune) {
    case AutotuneStatus::Running:
        switch (p.phase) {
        case autotune::Phase::Heating:
            snprintf(line, sizeof line, "AT%d/%d heat %.0f%c", i, k, t, unit);
            break;
        case autotune::Phase::Settling:
            snprintf(line, sizeof line, "AT%d/%d settle %.0f%c", i, k, t, unit);
            break;
        case autotune::Phase::Oscillating:
            snprintf(line, sizeof line, "AT%d/%d cyc%d/%d %.0f%c", i, k, p.cycle, p.cycleOf, t, unit);
            break;
        case autotune::Phase::Computing:
            snprintf(line, sizeof line, "AT%d/%d computing", i, k);
            break;
        case autotune::Phase::FanHold:
            snprintf(line, sizeof line, "AT fan %.0f%% %.0f%c", p.fan, t, unit);
            break;
        case autotune::Phase::FanShed:
            snprintf(line, sizeof line, "AT fan shed %.0f%%", p.fan);
            break;
        default:
            snprintf(line, sizeof line, "AT running");
            break;
        }
        break;
    case AutotuneStatus::Pending:
        snprintf(line, sizeof line, "AT results ready");
        break;
    case AutotuneStatus::Discarded:
        snprintf(line, sizeof line, "AT discarded");
        break;
    case AutotuneStatus::Done:
        snprintf(line, sizeof line, "AT done");
        break;
    case AutotuneStatus::Failed:
        snprintf(line, sizeof line, "AT fail: %s", r);
        break;
    case AutotuneStatus::Aborted:
        if (*r)
            snprintf(line, sizeof line, "AT abort: %s", r);
        else
            snprintf(line, sizeof line, "AT aborted");
        break;
    case AutotuneStatus::NeedSetpoint:
        snprintf(line, sizeof line, "AT: SV<%.0f%c", fromCelsius(kAutotuneMinSetpointC, unit), unit);
        break;
    case AutotuneStatus::NeedProbe:
        snprintf(line, sizeof line, "AT: no probe");
        break;
    case AutotuneStatus::Required:
        snprintf(line, sizeof line, "Autotune first");
        break;
    default:
        line[0] = '\0';
        break;
    }
    if (n == 0)
        return;
    size_t len = std::strlen(line);
    if (len > 20) // header limit
        len = 20;
    if (len > n - 1)
        len = n - 1;
    std::memcpy(out, line, len);
    out[len] = '\0';
}

} // namespace autotunetext
