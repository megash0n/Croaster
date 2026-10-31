#pragma once
// Autotune results on Croaster PID (docs/autotune). The controller runs the
// multi-point sequence; this plugin serves its report, applies or discards the
// results, and sets the stop cooldown fan when a run ends on its own.
// Registered with Croaster's public onCommand()/onJsonCommand() hooks (no change
// to croaster/). Commands: core/autotune_protocol.h.
#include <Arduino.h>

#include "ControlPlugin.h"
#include "controller.h"
#include "core/autotune_protocol.h"

class CroasterApp;

class AutotunePlugin {
public:
    // heaterMaxPct: the heater ceiling (CPID_HEATER_MAX_PERCENT), for the
    // recipe editor's rate-of-rise check.
    AutotunePlugin(Controller& controller, ControlPlugin& control, float heaterMaxPct)
        : controller_(controller)
        , control_(control)
        , heaterMaxPct_(heaterMaxPct) {}

    // Registers getAutotune and {"autotune":"apply"|"discard"}. Call before app.begin().
    void attach(CroasterApp& app);
    // Call from loop().
    void loop();

private:
    String report();
    char unit() const;

    Controller& controller_;
    ControlPlugin& control_;
    CroasterApp* app_ = nullptr;
    float heaterMaxPct_;
    autotune::Report report_; // static storage: about 1 KB, kept off the loop task stack
    roastermodel::RoasterModel model_;
    uint32_t statusSeq_ = 0;
};
