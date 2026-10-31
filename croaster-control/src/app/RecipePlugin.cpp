#include "RecipePlugin.h"

#include <CroasterApp.h>
#include <Update.h>

#include <cmath>

#include "core/profile_library.h"

namespace {
JsonObject beginReply(JsonDocument& doc, const char* command) {
    doc["command"] = command;
    return doc["response"].to<JsonObject>();
}
String finishReply(const JsonDocument& doc) {
    String out;
    serializeJson(doc, out);
    return out;
}
// NaN as null.
void putSeconds(JsonObject o, const char* key, float s) {
    if (std::isfinite(s))
        o[key] = std::round(s * 10) / 10;
    else
        o[key] = nullptr;
}
} // namespace

void RecipePlugin::attach(CroasterApp& app) {
    app_ = &app;
    CroasterCommandHandler& c = app.commands();
    c.onCommand("roastEvent", [this](const JsonObject& json) { return event(json); });
    c.onCommand("recipeLoad", [this](const JsonObject& json) { return loadCommand(json); });
    c.onCommand("recipeStart", [this](const JsonObject&) {
        if (controller_.snapshot().mode == Mode::Autotune) // T57: autotune and a recipe never run together
            return simple("recipeStart", false, "autotuneRunning");
        const char* why = runner_.state() == recipe::State::Empty ? "empty" : "running";
        const bool ok = runner_.start();
        return simple("recipeStart", ok, why);
    });
    c.onCommand("recipeAbort", [this](const JsonObject&) {
        const bool was = running();
        runner_.abort("user");
        return simple("recipeAbort", was, "notRunning");
    });
    c.onCommand("recipeConfirm", [this](const JsonObject& json) {
        recipe::Output out;
        const bool ok = json["step"].is<int>() && runner_.confirm(json["step"].as<int>(), inputs(), out);
        submit(out);
        return simple("recipeConfirm", ok, "noPrompt");
    });
    c.onCommand("recipeSkip", [this](const JsonObject& json) {
        bool ok;
        if (json["step"].is<int>() && runner_.prompt() == json["step"].as<int>())
            ok = runner_.dismiss(json["step"].as<int>());
        else
            ok = runner_.skipNext(clock_, millis());
        return simple("recipeSkip", ok, "notRunning");
    });
    c.onCommand("getRecipe", [this](const JsonObject&) { return status(); });
    control_.onStop([this]() { runner_.abort("stop"); });
    control_.onAutotuneStart([this]() -> const char* { return running() ? "recipeRunning" : nullptr; });
    control_.onData([this](JsonObject& data) { writeData(data); });
}

const char* RecipePlugin::load(const recipe::Recipe& r, const std::string& text, const char* name, char unit) {
    if (!recipe::unitName(unit))
        return "unit must be C, F or K";
    if (!runner_.load(r))
        return "recipe running";
    text_ = text;
    recipe::copyText(name_, sizeof(name_), name);
    unit_ = unit;
    return nullptr;
}

recipe::Inputs RecipePlugin::inputs() const {
    recipe::Inputs in;
    in.nowMs = millis();
    const Snapshot s = controller_.snapshot();
    in.trip = s.trip;
    in.setpointC = s.setpoint;
    in.heaterPct = s.heaterPct;
    in.pidInput = s.pidInput;
    if (!app_)
        return in;
    CroasterCore& core = app_->core();
    const String u = core.temperatureUnit();
    const char unit = u.length() ? u[0] : 'C';
    const float bt = core.tempBt, et = core.tempEt, ror = core.rorBt;
    in.btC = std::isfinite(bt) ? toCelsius(bt, unit) : NAN;
    in.etC = std::isfinite(et) ? toCelsius(et, unit) : NAN;
    // Croaster reports 0 RoR when BT is invalid: no reading then.
    in.rorC = std::isfinite(bt) && std::isfinite(ror) ? (unit == 'F' ? ror * 5.0f / 9.0f : ror) : NAN;
    return in;
}

void RecipePlugin::submit(const recipe::Output& out) {
    for (size_t i = 0; i < out.count; i++)
        controller_.submit(out.commands[i]);
    if (out.stop)
        control_.stop();
}

void RecipePlugin::loop() {
    const uint32_t now = millis();
    if (!running() || now - lastTickMs_ < kTickMs)
        return;
    lastTickMs_ = now;
    if (Update.isRunning()) {
        runner_.abort("ota");
        return;
    }
    recipe::Output out;
    runner_.tick(inputs(), clock_, out);
    submit(out);
}

String RecipePlugin::simple(const char* command, bool ok, const char* error) {
    JsonDocument doc;
    JsonObject r = beginReply(doc, command);
    r["ok"] = ok;
    if (!ok && error)
        r["error"] = error;
    return finishReply(doc);
}

String RecipePlugin::event(const JsonObject& json) {
    recipe::Event e;
    if (!recipe::parseEvent(json["event"].as<const char*>(), e))
        return simple("roastEvent", false, "badEvent");
    const uint32_t now = millis();
    if (json["undo"] | false)
        clock_.undo(e);
    else
        clock_.mark(e, now);
    JsonDocument doc;
    JsonObject r = beginReply(doc, "roastEvent");
    r["ok"] = true;
    r["event"] = recipe::eventName(e);
    putSeconds(r, "sinceCharge", clock_.sinceCharge(now));
    return finishReply(doc);
}

String RecipePlugin::loadCommand(const JsonObject& json) {
    if (running())
        return simple("recipeLoad", false, "busy");
    JsonObjectConst p = json["profile"].as<JsonObjectConst>();
    if (p.isNull())
        return simple("recipeLoad", false, "badRequest");
    std::unique_ptr<RoastProfile> schedule(new RoastProfile()); // heap: keep the loop stack free
    std::unique_ptr<recipe::Recipe> rec(new recipe::Recipe());
    proflib::Loaded what;
    const char* reason = nullptr;
    const proflib::Error e = proflib::loadParts(p, *schedule, *rec, what, &reason);
    JsonDocument doc;
    JsonObject r = beginReply(doc, "recipeLoad");
    if (e != proflib::Error::None) {
        r["ok"] = false;
        r["error"] = proflib::errorCode(e);
        if (reason)
            r["reason"] = reason;
        return finishReply(doc);
    }
    if (what.recipe) {
        // Load the recipe first so a refusal leaves the schedule untouched.
        std::string text;
        serializeJson(p["recipe"], text);
        if (const char* why = load(*rec, text, p["name"] | "", (p["unit"] | "C")[0])) {
            r["ok"] = false;
            r["error"] = proflib::errorCode(proflib::Error::BadRecipe);
            r["reason"] = why;
            return finishReply(doc);
        }
    }
    bool scheduled = false;
    if (what.schedule)
        scheduled = controller_.loadProfile(*schedule);
    r["ok"] = true;
    r["recipe"] = what.recipe;
    r["schedule"] = scheduled;
    return finishReply(doc);
}

void RecipePlugin::writeData(JsonObject& data) {
    if (runner_.state() == recipe::State::Empty)
        return;
    JsonObject o = data["recipe"].to<JsonObject>();
    o["state"] = recipe::stateName(runner_.state());
    o["next"] = runner_.next();
    o["prompt"] = runner_.prompt();
    o["fired"] = runner_.firedCount();
    o["seq"] = runner_.seq();
    o["follow"] = running() && runner_.followingSetpoint();
}

String RecipePlugin::status() {
    const uint32_t now = millis();
    JsonDocument doc;
    JsonObject r = beginReply(doc, "getRecipe");
    r["ok"] = true;
    recipe::writeStatus(r, runner_, clock_, now, name_, unit_);
    const bool loaded = runner_.state() != recipe::State::Empty;
    String out;
    serializeJson(doc, out);
    if (loaded && !text_.empty()) {
        // Splice the recipe text in rather than parsing it into the document
        // (it was validated when loaded). out ends with "}}".
        out.remove(out.length() - 2);
        out.reserve(out.length() + text_.size() + 16);
        out += ",\"recipe\":";
        out += text_.c_str();
        out += "}}";
    }
    return out;
}
