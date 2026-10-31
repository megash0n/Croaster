#pragma once
// Autotune commands and the getAutotune reply (docs/autotune/implementation.md).
//   {"command":"autotune"}                        start (control_protocol.h)
//   {"command":{"autotune":"apply"|"discard"}}    pending results -> gains, or drop them
//   {"command":"getAutotune"}                     plan, progress, results and log
// Temperatures and swings are in Croaster's current unit; Ku and the gains stay
// in the controller's units (% per C), the same as the pid command.
// Pure (ArduinoJson only), so it is host-tested.
#include <ArduinoJson.h>

#include "control_types.h"
#include "roaster_model.h"

namespace autotuneproto {

// "apply" -> ApplyAutotune, "discard" -> DiscardAutotune; false otherwise.
bool parseAction(JsonVariantConst v, Command& out);

// One log entry as text: "heating to 350 F", "cycle 2: swing 6.1 F, period 131 s".
void logText(const autotune::LogEntry& e, const autotune::Report& r, char unit, char* out, size_t n);

// Body of the getAutotune reply. `plan` is the configured run (points, cycle
// counts, rule, fan test); `estimateS` the expected length from the current PV;
// `saved` the model kept with the gains (null: none saved), written as "saved"
// for the recipe editor's rate-of-rise check; `fanEstimateS` the part of the
// estimate the fan test takes (plan "fanEstimate", for the start dialog).
//   "fanTest": {"status","failure","channel":"exhaust","target","duration",
//               "levels":[{"fan","hold","dip","recovery","gain","tau","shed"}]}
//     hold: heater %; dip: BT drop after the step (unit); recovery, tau: s;
//     gain: BT per % fan (unit); shed: heater-off BT fall at the target (unit/min)
//   "saved": {"ambient","runFan","heaterMax","points":[{"target","gain","tau","hold"}],
//             "fan":{"target","levels":[{"fan","hold","shed"}]}|null}
void writeReport(const Snapshot& s, const autotune::Report& r, const autotune::Config& plan, float estimateS,
                 char unit, JsonObject o, const roastermodel::RoasterModel* saved = nullptr, float heaterMaxPct = 100,
                 float fanEstimateS = NAN);

// The "saved" object alone (also used by getControl).
void writeSaved(const roastermodel::RoasterModel& m, float heaterMaxPct, char unit, JsonObject o);

} // namespace autotuneproto
