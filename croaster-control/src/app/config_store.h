#pragma once
// Control settings in NVS. WiFi credentials are not here: Croaster's WiFiManager
// stores them. Namespace "control" (yaeger used "preferences-v1"; keys differ, so a
// yaeger board starts from defaults; set PID gains once or run autotune).
#include "core/control_types.h"
#include "core/roaster_model.h"

struct ControlSettings {
    PidGains gains;                   // defaults from config.h (CPID_PID_KP/KI/KD)
    bool gainsTuned = false;          // gains saved by autotune or the pid command (NVS "tuned")
    PidInput pidInput = PidInput::BT; // default from config.h (CPID_PID_INPUT)
    CooldownFan cooldownFan;          // defaults from config.h (CPID_COOLDOWN_FAN_STOP/TRIP, CPID_COOLDOWN_END_C)
    WatchdogSettings watchdog;        // defaults from config.h (CPID_WATCHDOG_*)
    bool fanAssist = true;            // default from config.h (CPID_FAN_ASSIST)
    float fanAssistMaxPct = 20;       // CPID_FAN_ASSIST_MAX (not stored)
    bool mainsTrip = false;           // nomains trip; default from config.h (CPID_REQUIRE_MAINS)
    bool mainsTripAvailable = false;  // AC dimmer build: the setting applies (not stored)
};

namespace ConfigStore {
ControlSettings load(const ControlSettings& defaults);
void saveGains(const PidGains& g); // also marks them tuned
void savePidInput(PidInput t);
void saveCooldownFan(const CooldownFan& c);
void saveWatchdog(const WatchdogSettings& w);
void saveFanAssist(bool on);
void saveMainsTrip(bool on);
// Autotune's roaster model (saved with the gains). load returns false when none
// is saved or it was saved by a different layout (roastermodel::kVersion).
void saveModel(const roastermodel::RoasterModel& m);
bool loadModel(roastermodel::RoasterModel& out);
} // namespace ConfigStore
