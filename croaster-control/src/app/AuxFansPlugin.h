#pragma once
// Box temperature and the optional fans (core/aux_fans.h, docs/wiring.md).
//  - Data: adds "aux" to every data payload: box temperature and its source,
//    ESP32-S3 chip temperature, the box warning, and each fitted fan's state.
//  - Command: {"command":{"auxFans":{...}}}, any of
//      "scroll":true|false           Cooling Fan on/off (Manual mode, idle only)
//      "scrollEn":true|false         Cooling Fan enable (saved; default on)
//      "fan1"/"fan2":"auto"|"on"|"off"  override (not saved; Auto after boot)
//      "fan1On"/"fan2On":T           turn-on temperature (25..90 C), saved to NVS
//      "boxWarn":T                   box warning (30..100 C), saved to NVS
//    Temperatures are in Croaster's current unit. Reply {"ok":bool,"error":..}.
// Fans are fitted with config.h CPID_SCROLL_FAN, CPID_FAN1, CPID_FAN2.
#include <Arduino.h>
#include <ArduinoJson.h>

#include "controller.h"
#include "core/aux_fans.h"

class CroasterApp;
class ControlPlugin;

class AuxFansPlugin {
public:
    struct Config {
        int scrollPin = -1, fan1Pin = -1, fan2Pin = -1; // -1 = not fitted
        uint32_t pwmHz = 10000;
        float fan1OnC = 45, fan2OnC = 60, boxWarnC = 50; // first-boot defaults
    };
    AuxFansPlugin(Controller& controller, const Config& cfg)
        : controller_(controller)
        , cfg_(cfg) {}

    // Outputs off and saved settings loaded. Call early in setup(), after NVS is usable.
    void begin();
    // Registers the command. Call before app.begin().
    void attach(CroasterApp& app);
    // Adds the "aux" object; ControlPlugin::onData calls it.
    void writeData(JsonObject& data);
    // Call from loop().
    void loop();

private:
    String handle(const JsonObject& json);
    void setFan(int pin, bool on);
    char unit() const;

    Controller& controller_;
    Config cfg_;
    CroasterApp* app_ = nullptr;
    auxfans::ScrollFan scroll_;
    bool scrollEnabled_ = true; // NVS "scrEn"; off keeps the Cooling Fan output low
    auxfans::ThermoFan fan1_, fan2_;
    auxfans::Override fan1Mode_ = auxfans::Override::Auto, fan2Mode_ = auxfans::Override::Auto;
    auxfans::BoxTemp box_;
    float chipC_ = NAN;
    unsigned long lastMs_ = 0;
};
