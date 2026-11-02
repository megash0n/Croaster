#include "control_protocol.h"

#include "autotune_status.h"

#include <cmath>
#include <cstring>

namespace controlproto {

const char* const kNestedKeys[] = {"burner", "fan",         "setpoint", "mode",    "pidInput",
                                   "pid",    "cooldownFan", "profile",  "watchdog", "fanAssist", "mainsTrip"};
const size_t kNestedKeyCount = sizeof(kNestedKeys) / sizeof(kNestedKeys[0]);
const char* const kBasicNames[] = {"startProfile", "stop", "autotune", "clearTrip", "getControl"};
const size_t kBasicNameCount = sizeof(kBasicNames) / sizeof(kBasicNames[0]);
const char* const kLegacyBasicNames[] = {"setBurner", "setFan", "setPreferences", "getPreferences"};
const size_t kLegacyBasicNameCount = sizeof(kLegacyBasicNames) / sizeof(kLegacyBasicNames[0]);

namespace {
void add(Parsed& p, Command::Kind k, float a = 0, float b = 0, float c = 0) {
    if (p.count >= Parsed::kMax)
        return;
    Command cmd{k};
    cmd.a = a;
    cmd.b = b;
    cmd.c = c;
    p.commands[p.count++] = cmd;
}
bool eq(const char* a, const char* b) {
    if (!a)
        return false;
    while (*a && *b) {
        char x = *a++, y = *b++;
        if (x >= 'A' && x <= 'Z')
            x += 32;
        if (y >= 'A' && y <= 'Z')
            y += 32;
        if (x != y)
            return false;
    }
    return *a == *b;
}
template <typename T>
void putNum(T&& v, float x, float scale = 10.0f) { // JSON has no NaN
    if (std::isfinite(x))
        v = std::round(x * scale) / scale;
    else
        v = nullptr;
}
// Setpoint 0 C means off (no target, heater held off); it reads 0 in every unit, never 32 F.
float setpointIn(float c, char unit) {
    return c <= 0 ? 0.0f : fromCelsius(c, unit); // NaN stays NaN (null)
}
} // namespace

namespace {
bool fanPercent(JsonVariantConst v, float& out) {
    if (!v.is<float>())
        return false;
    const float x = v.as<float>();
    if (!(x >= 0 && x <= 100))
        return false;
    out = x;
    return true;
}
bool parsePidInput(const char* t, PidInput& out) {
    if (!(eq(t, "ET") || eq(t, "BT") || eq(t, "MAX")))
        return false;
    out = eq(t, "BT") ? PidInput::BT : (eq(t, "MAX") ? PidInput::MAX : PidInput::ET);
    return true;
}
} // namespace

Parsed parseNested(JsonObjectConst c, char unit, const char* only) {
    Parsed p;
    auto want = [only](const char* k) {
        return !only || std::strcmp(only, k) == 0;
    };
    if (want("mode") && c["mode"].is<const char*>()) { // mode before values so {"mode":"pid","setpoint":200} works
        const char* m = c["mode"];
        if (eq(m, "pid"))
            add(p, Command::Kind::SetMode, (float)Mode::Pid);
        if (eq(m, "manual"))
            add(p, Command::Kind::SetMode, (float)Mode::Manual);
    }
    if (want("pidInput") && c["pidInput"].is<const char*>()) {
        p.hasPidInput = parsePidInput(c["pidInput"], p.pidInput);
        if (p.hasPidInput)
            add(p, Command::Kind::SetPidInput, (float)p.pidInput);
    }
    if (want("pid") && c["pid"].is<JsonObjectConst>()) {
        JsonObjectConst g = c["pid"];
        if (g["kp"].is<float>() && g["ki"].is<float>() && g["kd"].is<float>()) {
            p.hasGains = true;
            p.gains = {g["kp"].as<float>(), g["ki"].as<float>(), g["kd"].as<float>()};
            add(p, Command::Kind::SetGains, p.gains.kp, p.gains.ki, p.gains.kd);
        }
    }
    if (want("setpoint") && c["setpoint"].is<float>())
        add(p, Command::Kind::SetSetpoint, toCelsius(c["setpoint"].as<float>(), unit));
    if (want("fan") && c["fan"].is<float>())
        add(p, Command::Kind::SetFan, c["fan"].as<float>());
    if (want("burner") && c["burner"].is<float>())
        add(p, Command::Kind::SetHeater, c["burner"].as<float>());
    if (want("cooldownFan")) {
        JsonVariantConst v = c["cooldownFan"];
        if (v.is<JsonObjectConst>()) {
            p.hasCooldownStop = fanPercent(v["stop"], p.cooldownFan.stop);
            p.hasCooldownTrip = fanPercent(v["trip"], p.cooldownFan.trip);
            if (v["end"].is<float>()) {
                const float endC = toCelsius(v["end"].as<float>(), unit);
                p.hasCooldownEnd = endC >= CooldownFan::kMinEndC && endC <= CooldownFan::kMaxEndC;
                if (p.hasCooldownEnd)
                    p.cooldownFan.endC = endC;
            }
        } else {
            p.hasCooldownStop = fanPercent(v, p.cooldownFan.stop);
        }
    }
    if (want("watchdog") && c["watchdog"].is<JsonObjectConst>()) {
        JsonObjectConst w = c["watchdog"];
        if (w["enabled"].is<bool>()) {
            p.hasWatchdogEnabled = true;
            p.watchdogEnabled = w["enabled"].as<bool>();
        }
        if (w["timeout"].is<float>()) {
            const float t = w["timeout"].as<float>();
            if (t >= WatchdogSettings::kMinTimeoutS && t <= WatchdogSettings::kMaxTimeoutS) {
                p.hasWatchdogTimeout = true;
                p.watchdogTimeoutS = static_cast<uint16_t>(std::lround(t));
            }
        }
    }
    if (want("fanAssist") && c["fanAssist"].is<bool>()) {
        p.hasFanAssist = true;
        p.fanAssist = c["fanAssist"].as<bool>();
        add(p, Command::Kind::SetFanAssist, p.fanAssist ? 1.0f : 0.0f);
    }
    if (want("mainsTrip") && c["mainsTrip"].is<bool>()) {
        p.hasMainsTrip = true;
        p.mainsTrip = c["mainsTrip"].as<bool>();
    }
    if (want("profile") && !c["profile"].isNull()) {
        p.hasProfile = p.profile.loadJson(c["profile"].as<JsonArrayConst>(), unit);
        p.profileError = !p.hasProfile;
    }
    return p;
}

Parsed parseLegacy(JsonObjectConst m) {
    Parsed p;
    const char* cmd = m["command"].is<const char*>() ? m["command"].as<const char*>() : nullptr;
    if (eq(cmd, "setBurner") && m["value"].is<float>())
        add(p, Command::Kind::SetHeater, m["value"].as<float>());
    if (eq(cmd, "setFan") && m["value"].is<float>())
        add(p, Command::Kind::SetFan, m["value"].as<float>());
    if (eq(cmd, "setPreferences")) {
        if (m["pidKp"].is<float>() && m["pidKi"].is<float>() && m["pidKd"].is<float>()) {
            p.hasGains = true;
            p.gains = {m["pidKp"].as<float>(), m["pidKi"].as<float>(), m["pidKd"].as<float>()};
            add(p, Command::Kind::SetGains, p.gains.kp, p.gains.ki, p.gains.kd);
        }
        p.hasCooldownStop = fanPercent(m["cooldownFanSpeed"], p.cooldownFan.stop);
    }
    return p;
}

bool basicCommand(const char* name, Command& out, JsonObjectConst msg) {
    if (eq(name, "startProfile"))
        out = Command{Command::Kind::StartProfile};
    else if (eq(name, "stop"))
        out = Command{Command::Kind::Stop};
    else if (eq(name, "autotune")) {
        out = Command{Command::Kind::StartAutotune};
        out.a = msg["fanTest"].is<bool>() ? (msg["fanTest"].as<bool>() ? 1.0f : 0.0f) : NAN;
    }
    else if (eq(name, "clearTrip"))
        out = Command{Command::Kind::ClearTrip};
    else
        return false;
    return true;
}

void writeDataFields(const Snapshot& s, char unit, JsonObject d) {
    putNum(d["BurnerVal"], s.heaterPct);
    putNum(d["FanVal"], s.fanPct);
    putNum(d["FanAssist"], s.fanAssistPct); // part of FanVal added by fan assist
    putNum(d["SV"], setpointIn(s.setpoint, unit));
    d["Mode"] = modeName(s.mode);
    d["trip"] = tripName(s.trip);
    d["cooling"] = s.cooling;
    putNum(d["Amb"], fromCelsius(s.ambient, unit));
    d["Target"] = pidInputName(s.pidInput);
    char at[48];
    autotunetext::full(s, unit, at, sizeof at);
    d["autotune"] = at;
    if (s.mode == Mode::Autotune) // the relay's target, for the chart overlay
        putNum(d["atTarget"], fromCelsius(s.autotuneProgress.targetC, unit));
    d["pidKp"] = s.gains.kp;
    d["pidKi"] = s.gains.ki;
    d["pidKd"] = s.gains.kd;
}

void writePreferences(const PidGains& g, const CooldownFan& cooldownFan, JsonObject d) {
    d["type"] = "preferences";
    d["pidKp"] = g.kp;
    d["pidKi"] = g.ki;
    d["pidKd"] = g.kd;
    d["cooldownFanSpeed"] = cooldownFan.stop;
}

void writeControlInfo(const Snapshot& s, const CooldownFan& cooldownFan, const WatchdogSettings& wd, char unit,
                      JsonObject o) {
    o["mode"] = modeName(s.mode);
    o["pidInput"] = pidInputName(s.pidInput);
    putNum(o["setpoint"], setpointIn(s.setpoint, unit));
    putNum(o["burner"], s.heaterPct);
    putNum(o["fan"], s.fanPct);
    JsonObject cf = o["cooldownFan"].to<JsonObject>();
    cf["stop"] = cooldownFan.stop;
    cf["trip"] = cooldownFan.trip;
    putNum(cf["end"], fromCelsius(cooldownFan.endC, unit));
    o["cooling"] = s.cooling;
    JsonObject w = o["watchdog"].to<JsonObject>();
    w["enabled"] = wd.enabled;
    w["timeout"] = wd.timeoutS;
    JsonObject g = o["pid"].to<JsonObject>();
    g["kp"] = s.gains.kp;
    g["ki"] = s.gains.ki;
    g["kd"] = s.gains.kd;
    char at[48];
    autotunetext::full(s, unit, at, sizeof at);
    o["autotune"] = at;
    o["tuned"] = s.gainsTuned;
    o["trip"] = tripName(s.trip);
    o["mainsOk"] = s.mainsOk;
    JsonObject fa = o["fanAssist"].to<JsonObject>();
    fa["enabled"] = s.fanAssistOn;
    fa["available"] = s.fanTested; // acts only with a saved fan test
    putNum(fa["extra"], s.fanAssistPct);
    putNum(o["profileElapsed"], s.profileElapsedS, 1.0f);
    putNum(o["ambient"], fromCelsius(s.ambient, unit));
}

} // namespace controlproto
