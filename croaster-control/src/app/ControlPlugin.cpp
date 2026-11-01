#include "ControlPlugin.h"

#include <ArduinoJson.h>
#include <CroasterApp.h>
#include <WiFi.h>

#include <cmath>
#include <cstring>

namespace {
// Replies use the same envelope as Croaster's genResponseCommand():
// {"command":name,"response":{...}}. The response is written straight into the
// envelope. Copying a finished JsonDocument into it (doc["response"] = other)
// recurses through ArduinoJson's copy code; on top of the WebSockets library's
// nested read callbacks that overflowed the 8 KB loop task stack (T54).
JsonObject beginReply(JsonDocument& doc, const char* command) {
    doc["command"] = command;
    return doc["response"].to<JsonObject>();
}
String finishReply(const JsonDocument& doc) {
    String out;
    serializeJson(doc, out);
    return out;
}
} // namespace

char ControlPlugin::unit() const {
    const String u = app_ ? app_->core().temperatureUnit() : String("C");
    return u.length() ? u[0] : 'C';
}

void ControlPlugin::attach(CroasterApp& app, const ControlSettings& settings) {
    app_ = &app;
    settings_ = settings;
    for (size_t i = 0; i < controlproto::kNestedKeyCount; i++) {
        const char* key = controlproto::kNestedKeys[i];
        app.commands().onJsonCommand(key, [this, key](const JsonObject& json) {
            return handleNested(json, key);
        });
    }
    for (size_t i = 0; i < controlproto::kBasicNameCount; i++) {
        const char* name = controlproto::kBasicNames[i];
        app.commands().onCommand(name, [this, name](const JsonObject& json) {
            return handleBasic(name, json);
        });
    }
    for (size_t i = 0; i < controlproto::kLegacyBasicNameCount; i++) {
        const char* name = controlproto::kLegacyBasicNames[i];
        app.commands().onCommand(name, [this, name](const JsonObject& json) {
            return handleLegacy(json, name);
        });
    }
    app.core().onJsonData([this](JsonObject& data) {
        controlproto::writeDataFields(controller_.snapshot(), unit(), data);
        if (WiFi.status() == WL_CONNECTED)
            data["wifiStrength"] = WiFi.RSSI();
        // Fake readings state for the page's button (Croaster's useDummyData).
        data["dummy"] = app_->core().dummyDataEnabled();
        if (dataWriter_)
            dataWriter_(data);
    });
}

String ControlPlugin::handleNested(const JsonObject& json, const char* key) {
    if (json[key].isNull())
        return "";
    controlproto::Parsed p = controlproto::parseNested(json, unit(), key);
    apply(p);
    if (p.hasProfile || p.profileError) {
        JsonDocument doc;
        JsonObject r = beginReply(doc, "profile");
        r["loaded"] = p.hasProfile && controller_.loadProfile(p.profile);
        r["steps"] = p.profile.size();
        return finishReply(doc);
    }
    return "";
}

String ControlPlugin::handleLegacy(const JsonObject& json, const char* name) {
    apply(controlproto::parseLegacy(json));
    if (std::strcmp(name, "setFan") == 0 || std::strcmp(name, "setBurner") == 0)
        return "";
    JsonDocument r; // yaeger answers set/getPreferences with the stored values
    r["id"] = json["id"] | 0;
    controlproto::writePreferences(settings_.gains, settings_.cooldownFan, r["data"].to<JsonObject>());
    String out;
    serializeJson(r, out);
    return out;
}

void ControlPlugin::apply(const controlproto::Parsed& p) {
    bool stopped = false;
    for (size_t i = 0; i < p.count; i++) {
        controller_.submit(p.commands[i]);
        stopped = stopped || p.commands[i].kind == Command::Kind::Stop ||
                  p.commands[i].kind == Command::Kind::EmergencyOff;
    }
    if (stopped && stopListener_)
        stopListener_();
    if (p.hasGains) {
        settings_.gains = p.gains;
        ConfigStore::saveGains(p.gains);
    }
    if (p.hasPidInput) {
        settings_.pidInput = p.pidInput;
        ConfigStore::savePidInput(p.pidInput);
    }
    if (p.hasCooldownEnd) {
        settings_.cooldownFan.endC = p.cooldownFan.endC;
        Command c{Command::Kind::SetCooldownEnd};
        c.a = settings_.cooldownFan.endC;
        controller_.submit(c);
    }
    if (p.hasCooldownStop || p.hasCooldownTrip || p.hasCooldownEnd) {
        if (p.hasCooldownStop)
            settings_.cooldownFan.stop = p.cooldownFan.stop;
        if (p.hasCooldownTrip) {
            settings_.cooldownFan.trip = p.cooldownFan.trip;
            Command c{Command::Kind::SetTripFan};
            c.a = settings_.cooldownFan.trip;
            controller_.submit(c);
        }
        ConfigStore::saveCooldownFan(settings_.cooldownFan);
    }
    if (p.hasWatchdogEnabled || p.hasWatchdogTimeout) {
        if (p.hasWatchdogEnabled)
            settings_.watchdog.enabled = p.watchdogEnabled;
        if (p.hasWatchdogTimeout)
            settings_.watchdog.timeoutS = p.watchdogTimeoutS;
        ConfigStore::saveWatchdog(settings_.watchdog);
        watchdogArmedMs_ = millis();
    }
    if (p.hasFanAssist) {
        settings_.fanAssist = p.fanAssist;
        ConfigStore::saveFanAssist(p.fanAssist);
    }
}

String ControlPlugin::handleBasic(const char* name, const JsonObject& json) {
    Command c;
    if (controlproto::basicCommand(name, c, json)) {
        if (c.kind == Command::Kind::Stop) {
            stop();
            if (stopListener_)
                stopListener_();
        } else if (const char* why = c.kind == Command::Kind::StartAutotune && autotuneGuard_ ? autotuneGuard_()
                                                                                            : nullptr) {
            JsonDocument doc;
            JsonObject r = beginReply(doc, name);
            r["ok"] = false;
            r["error"] = why;
            return finishReply(doc);
        } else {
            controller_.submit(c);
        }
        return "";
    }
    JsonDocument doc; // getControl
    JsonObject r = beginReply(doc, name);
    controlproto::writeControlInfo(controller_.snapshot(), settings_.cooldownFan, settings_.watchdog, unit(), r);
    r["fanAssist"]["max"] = settings_.fanAssistMaxPct;
    // Smallest free loop task stack since boot, in bytes (ESP-IDF counts bytes).
    r["loopStackFree"] = uxTaskGetStackHighWaterMark(nullptr);
    return finishReply(doc);
}

void ControlPlugin::stop() {
    Command c{Command::Kind::Stop}; // heater off, Manual, cooldown fan until ET is below the end temperature
    c.a = settings_.cooldownFan.stop;
    c.b = 1;
    controller_.submit(c);
}

void ControlPlugin::begin() {
    autotuneSeq_ = controller_.snapshot().autotuneSeq;
    watchdogArmedMs_ = millis();
}

void ControlPlugin::loop() {
    const Snapshot s = controller_.snapshot();

    if (s.autotuneSeq != autotuneSeq_) { // autotune finished: keep the new gains
        autotuneSeq_ = s.autotuneSeq;
        settings_.gains = s.gains;
        ConfigStore::saveGains(s.gains);
        // The model learned with these gains (fan test included) is saved with them.
        static roastermodel::RoasterModel model; // about 300 B: off the loop task stack
        controller_.roasterModel(model);
        if (roastermodel::usable(model))
            ConfigStore::saveModel(model);
    }

    // The offsets change with correctionBt/correctionEt (a unit change keeps them); poll them.
    // A non-finite offset would make every PID input NaN; treat it as no offset.
    const float btCore = app_->core().correctionBtCelsius();
    const float etCore = app_->core().correctionEtCelsius();
    const float btC = std::isfinite(btCore) ? btCore : 0;
    const float etC = std::isfinite(etCore) ? etCore : 0;
    if (!correctionSent_ || btC != correctionBtC_ || etC != correctionEtC_) {
        Command c{Command::Kind::SetCorrection};
        c.a = btC;
        c.b = etC;
        if (controller_.submit(c)) { // queue full: retry on the next loop
            correctionSent_ = true;
            correctionBtC_ = btC;
            correctionEtC_ = etC;
        }
    }

    // Quiet time = since the last message Croaster's command handler accepted
    // (WebSocket or BLE, any command) or since the watchdog was (re)armed.
    const unsigned long now = millis();
    const unsigned long sinceCommand = now - app_->commands().lastCommandMillis();
    const unsigned long sinceArmed = now - watchdogArmedMs_;
    const unsigned long quietMs = sinceCommand < sinceArmed ? sinceCommand : sinceArmed;
    const bool heating = s.heaterPct > 0 || s.mode == Mode::Pid || s.mode == Mode::Autotune;
    if (settings_.watchdog.enabled && heating && s.mode != Mode::Profile && s.trip == SafetyTrip::None &&
        quietMs > settings_.watchdog.timeoutS * 1000UL) {
        Command trip{Command::Kind::Trip};
        trip.a = static_cast<float>(SafetyTrip::CommsLost);
        controller_.submit(trip); // the controller sets the trip fan speed
        watchdogArmedMs_ = now;   // one trip per timeout, not one per loop
    }
}
