#pragma once
// The controller. Owns sensors, PID/autotune/profile and both actuators, and runs
// them in one fixed-rate FreeRTOS task (default: core 1). Everything else talks to
// it through two calls:
//   submit(Command)  - thread-safe, non-blocking (queue)
//   snapshot()       - thread-safe copy of the latest state
// Safety interlocks live here, so no network path can bypass them:
// over-temperature, dead PID probe, no mains (AC dimmer), OTA in progress,
// fan to the trip speed when a trip starts, minimum fan while heating, heater ceiling (HeaterOutput maxPercent).
// The fan output is the operator's base fan plus fan assist, or the fan test's
// level during autotune; every value goes through the interlocks above.
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "core/control_types.h"
#include "core/mains_watch.h"
#include "core/pid.h"
#include "core/probe_correction.h"
#include "core/autotune_sequence.h"
#include "core/cooldown.h"
#include "core/fan_assist.h"
#include "core/roaster_model.h"
#include "core/roast_profile.h"
#include "hw/fan_output.h"
#include "hw/heater_output.h"
#include "hw/thermocouples.h"

// Over-temperature trips on the raw or the corrected reading, whichever is
// higher. Setpoints are on the corrected scale, so the clamp is too.
struct SafetyLimits {
    float overTempC = 275;     // any probe -> heater off, fan 100
    float maxSetpointC = 250;  // legacy clamp in Control::setSetpoint
    float minFanWithHeat = 30; // legacy: heater on with fan <= 10 forced fan to 30
    bool requireMains = true;  // refuse heat when the dimmer sees no zero-cross
    // requireMains: trip once mains is missing this long without a break while heating
    uint32_t mainsGraceMs = 2000;
    bool requireTunedGains = false; // refuse PID and Profile until gains are tuned (modeNeedsTuning)
};

class Controller {
public:
    Controller(Thermocouples& tc, HeaterOutput& heater, FanOutput& fan, SafetyLimits lim)
        : tc_(tc)
        , heater_(heater)
        , fan_(fan)
        , lim_(lim)
        , pid_(pidCfg()) {}

    // tripFanPct: fan % when a safety trip starts (CooldownFan::trip);
    // cooldownEndC: ET below which the cooldown ends (CooldownFan::endC).
    void begin(const PidGains& gains, bool gainsTuned, PidInput input, float tripFanPct, float cooldownEndC,
               int core, UBaseType_t priority);
    bool submit(const Command& c);
    Snapshot snapshot() const;
    // Copies a parsed profile in; takes effect on StartProfile. Network task only.
    bool loadProfile(const RoastProfile& p);
    // Autotune test points and limits (docs/autotune). Call before begin(); the
    // points are clamped to this controller's limits (autotune::makePlan).
    void configureAutotune(const autotune::Config& c);
    const autotune::Config& autotunePlan() const {
        return atCfg_; // fixed after configureAutotune()
    }
    // Thread-safe copy of the last autotune run's results and log.
    void autotuneReport(autotune::Report& out) const;

    // What the saved autotune learned (roaster_model.h), and how it is used.
    // Call setRoasterModel() and configureModelUse() before begin().
    struct ModelUse {
        bool fanFeedforward = true;  // CPID_FAN_FEEDFORWARD
        bool rampFeedforward = true; // CPID_RAMP_FEEDFORWARD
        bool fanAssist = true;       // option (NVS); acts only with a saved fan test
        fanassist::Settings assist;  // limits from config.h (CPID_FAN_ASSIST_*)
    };
    void configureModelUse(const ModelUse& u);
    void setRoasterModel(const roastermodel::RoasterModel& m);
    // Thread-safe copy of the model in use (updated by ApplyAutotune).
    void roasterModel(roastermodel::RoasterModel& out) const;

    static constexpr uint32_t kTickMs = 10;       // SSR timing resolution
    static constexpr uint32_t kControlEvery = 25; // => 250 ms sensor/PID period

private:
    static Pid::Config pidCfg() {
        Pid::Config c;
        c.dtSeconds = kTickMs * kControlEvery / 1000.0f;
        return c;
    }
    static void taskEntry(void* self);
    void run();
    void apply(const Command& c);
    void controlStep(uint32_t nowMs);
    // Selected PID input with the correction offsets applied.
    float pidInput(const ThermoSample& s) const;
    // abortReason: why a running autotune stops, if this ends one ("" = a command).
    void setMode(Mode m, float pv, const char* abortReason = "");
    void setAutotuneStatus(AutotuneStatus status, const char* reason = "");
    // The PV just stepped (PID input or correction change); see onPvStep().
    void handlePvStep(float newPv, const char* abortReason);
    // Heater % added to the PID output: fan and setpoint-ramp feedforward.
    float feedforward(float sp) const;
    void setBaseFan(float pct);

    Thermocouples& tc_;
    HeaterOutput& heater_;
    FanOutput& fan_;
    SafetyLimits lim_;
    Pid pid_;
    autotune::Sequence autotune_;
    autotune::Config atCfg_;
    autotune::Report atReport_; // published copy, under mux_
    autotune::Report scratchReport_; // control task; filled outside the critical section
    uint32_t atReportSeq_ = 0;
    RoastProfile profile_, pendingProfile_;
    volatile bool profilePending_ = false;
    QueueHandle_t queue_ = nullptr;
    mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;

    // control-task-owned state
    Snapshot st_;
    ThermoSample last_{}; // uncorrected
    ProbeCorrection correction_;
    float manualHeater_ = 0;
    float tripFan_ = 100;
    float cooldownEndC_ = 60;
    Cooldown cooldown_;
    bool tripFanApplied_ = false; // trip fan set for the current trip
    MainsWatch mainsWatch_;
    uint32_t profileStartMs_ = 0, sensorBadSinceMs_ = 0;
    float fanBase_ = 0; // operator's / recipe's / profile's fan
    bool testFanOut_ = false; // the fan test's level is on the output
    ModelUse use_;
    roastermodel::RoasterModel model_;          // control task
    roastermodel::RoasterModel modelPublished_; // copy under mux_
    fanassist::Assist assist_;
    roastermodel::SetpointSlope spSlope_;
    Snapshot published_;
};
