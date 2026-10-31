#include "AutotunePlugin.h"

#include <ArduinoJson.h>
#include <CroasterApp.h>

#include "core/probe_correction.h"

char AutotunePlugin::unit() const {
    const String u = app_ ? app_->core().temperatureUnit() : String("C");
    return u.length() ? u[0] : 'C';
}

void AutotunePlugin::attach(CroasterApp& app) {
    app_ = &app;
    statusSeq_ = controller_.snapshot().autotuneStatusSeq;
    app.commands().onCommand("getAutotune", [this](const JsonObject&) { return report(); });
    app.commands().onJsonCommand("autotune", [this](const JsonObject& json) {
        Command c;
        if (!autotuneproto::parseAction(json["autotune"], c))
            return String("");
        controller_.submit(c);
        JsonDocument doc;
        doc["command"] = "autotune";
        JsonObject r = doc["response"].to<JsonObject>();
        r["ok"] = controller_.snapshot().autotunePending; // applied on the next control step
        r["action"] = c.kind == Command::Kind::ApplyAutotune ? "apply" : "discard";
        String out;
        serializeJson(doc, out);
        return out;
    });
}

String AutotunePlugin::report() {
    const Snapshot s = controller_.snapshot();
    controller_.autotuneReport(report_);
    const autotune::Config& plan = controller_.autotunePlan();
    ThermoSample t{};
    t.et = s.et;
    t.bt = s.bt;
    const float pv = selectPidInput(s.pidInput, t); // uncorrected; close enough for an estimate
    controller_.roasterModel(model_);
    const float estimate = autotune::Sequence::estimateSeconds(plan, pv);
    autotune::Config noFan = plan;
    noFan.fanTest = false;
    const float fanPart = estimate - autotune::Sequence::estimateSeconds(noFan, pv);
    JsonDocument doc;
    doc["command"] = "getAutotune";
    autotuneproto::writeReport(s, report_, plan, estimate, unit(), doc["response"].to<JsonObject>(),
                               roastermodel::usable(model_) ? &model_ : nullptr, heaterMaxPct_, fanPart);
    String out;
    serializeJson(doc, out);
    return out;
}

void AutotunePlugin::loop() {
    const Snapshot s = controller_.snapshot();
    if (s.autotuneStatusSeq == statusSeq_)
        return;
    statusSeq_ = s.autotuneStatusSeq;
    // A run that ended on its own (results ready, or a failure inside the
    // sequence) leaves the heater off: give it the stop path's cooldown fan.
    // Stop and trips already set the fan themselves.
    if ((s.autotune == AutotuneStatus::Pending || s.autotune == AutotuneStatus::Failed) && s.mode == Mode::Manual)
        control_.stop();
}
