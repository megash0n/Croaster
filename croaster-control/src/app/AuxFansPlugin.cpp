#include "AuxFansPlugin.h"

#include <CroasterApp.h>
#include <Preferences.h>

#include <cmath>
#include <initializer_list>

namespace {
constexpr const char* kNs = "aux";
constexpr uint32_t kPwmBits = 8;
constexpr unsigned long kPeriodMs = 500;

float rounded(float v) {
    return std::isfinite(v) ? roundf(v * 10) / 10 : v;
}

// A temperature from a command, in C; NaN when absent or outside lo..hi.
float tempArg(JsonVariantConst v, char unit, float lo, float hi) {
    if (!v.is<float>())
        return NAN;
    const float c = toCelsius(v.as<float>(), unit);
    return c >= lo && c <= hi ? c : NAN;
}

void saveFloat(const char* key, float v) {
    Preferences p;
    if (p.begin(kNs, false)) {
        p.putFloat(key, v);
        p.end();
    }
}
} // namespace

char AuxFansPlugin::unit() const {
    const String u = app_ ? app_->core().temperatureUnit() : String("C");
    return u.length() ? u[0] : 'C';
}

void AuxFansPlugin::setFan(int pin, bool on) {
    if (pin >= 0)
        ledcWrite(pin, on ? (1u << kPwmBits) - 1 : 0);
}

void AuxFansPlugin::begin() {
    if (cfg_.scrollPin >= 0) {
        digitalWrite(cfg_.scrollPin, LOW); // relay off before the pin drives
        pinMode(cfg_.scrollPin, OUTPUT);
    }
    for (int pin : {cfg_.fan1Pin, cfg_.fan2Pin}) {
        if (pin >= 0) {
            ledcAttach(pin, cfg_.pwmHz, kPwmBits);
            setFan(pin, false);
        }
    }
    Preferences p;
    if (p.begin(kNs, true)) {
        cfg_.fan1OnC = p.getFloat("f1On", cfg_.fan1OnC);
        cfg_.fan2OnC = p.getFloat("f2On", cfg_.fan2OnC);
        cfg_.boxWarnC = p.getFloat("boxW", cfg_.boxWarnC);
        p.end();
    }
}

void AuxFansPlugin::attach(CroasterApp& app) {
    app_ = &app;
    app.commands().onJsonCommand("auxFans", [this](const JsonObject& json) {
        return handle(json);
    });
}

void AuxFansPlugin::loop() {
    const unsigned long now = millis();
    if (now - lastMs_ < kPeriodMs)
        return;
    lastMs_ = now;
    const Snapshot s = controller_.snapshot();
    chipC_ = temperatureRead();
    box_ = auxfans::boxTemperature(s.box, chipC_);
    const bool scroll = scroll_.update(s.mode, s.heaterPct, s.cooling);
    if (cfg_.scrollPin >= 0)
        digitalWrite(cfg_.scrollPin, scroll ? HIGH : LOW);
    if (cfg_.fan1Pin >= 0)
        setFan(cfg_.fan1Pin, fan1_.update(box_.c, cfg_.fan1OnC, fan1Mode_));
    if (cfg_.fan2Pin >= 0)
        setFan(cfg_.fan2Pin, fan2_.update(chipC_, cfg_.fan2OnC, fan2Mode_));
}

String AuxFansPlugin::handle(const JsonObject& json) {
    JsonObjectConst a = json["auxFans"];
    if (a.isNull())
        return "";
    const char u = unit();
    const char* error = nullptr;
    if (a["scroll"].is<bool>()) {
        const Snapshot s = controller_.snapshot();
        if (cfg_.scrollPin < 0)
            error = "no scroll fan";
        else if (!scroll_.setManual(a["scroll"].as<bool>(), s.mode, s.heaterPct, s.cooling))
            error = "not idle";
    }
    auxfans::parseOverride(a["fan1"] | "", fan1Mode_);
    auxfans::parseOverride(a["fan2"] | "", fan2Mode_);
    struct {
        const char* key;
        const char* nvs;
        float* value;
        float lo, hi;
    } temps[] = {{"fan1On", "f1On", &cfg_.fan1OnC, 25, 90},
                 {"fan2On", "f2On", &cfg_.fan2OnC, 25, 90},
                 {"boxWarn", "boxW", &cfg_.boxWarnC, 30, 100}};
    for (auto& t : temps) {
        if (a[t.key].isNull())
            continue;
        const float c = tempArg(a[t.key], u, t.lo, t.hi);
        if (std::isnan(c)) {
            error = "out of range";
        } else if (c != *t.value) {
            *t.value = c;
            saveFloat(t.nvs, c);
        }
    }
    lastMs_ = millis() - kPeriodMs; // apply on the next loop()
    JsonDocument doc;
    doc["command"] = "auxFans";
    JsonObject r = doc["response"].to<JsonObject>();
    r["ok"] = error == nullptr;
    if (error)
        r["error"] = error;
    String out;
    serializeJson(doc, out);
    return out;
}

void AuxFansPlugin::writeData(JsonObject& data) {
    const char u = unit();
    JsonObject a = data["aux"].to<JsonObject>();
    if (std::isfinite(box_.c))
        a["box"] = rounded(fromCelsius(box_.c, u));
    a["boxSrc"] = auxfans::boxSourceName(box_.source);
    if (std::isfinite(chipC_))
        a["chip"] = rounded(fromCelsius(chipC_, u));
    a["warn"] = rounded(fromCelsius(cfg_.boxWarnC, u));
    if (cfg_.scrollPin >= 0)
        a["scroll"] = scroll_.on();
    struct {
        const char* key;
        int pin;
        const auxfans::ThermoFan& fan;
        auxfans::Override mode;
        float onC;
    } fans[] = {{"fan1", cfg_.fan1Pin, fan1_, fan1Mode_, cfg_.fan1OnC}, {"fan2", cfg_.fan2Pin, fan2_, fan2Mode_, cfg_.fan2OnC}};
    for (const auto& f : fans) {
        if (f.pin < 0)
            continue;
        JsonObject o = a[f.key].to<JsonObject>();
        o["on"] = f.fan.on();
        o["mode"] = auxfans::overrideName(f.mode);
        o["at"] = rounded(fromCelsius(f.onC, u));
    }
}
