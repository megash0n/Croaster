#include "config_store.h"

#include <Preferences.h>

namespace {
constexpr const char* kNs = "control";
}

ControlSettings ConfigStore::load(const ControlSettings& d) {
    Preferences p;
    ControlSettings s = d;
    if (!p.begin(kNs, true))
        return s; // namespace absent on first boot
    s.gains.kp = p.getFloat("kp", d.gains.kp);
    s.gains.ki = p.getFloat("ki", d.gains.ki);
    s.gains.kd = p.getFloat("kd", d.gains.kd);
    s.gainsTuned = p.getBool("tuned", p.isKey("kp")); // gains saved before the flag existed count too
    s.pidInput = static_cast<PidInput>(p.getUChar("in", static_cast<uint8_t>(d.pidInput)));
    s.cooldownFan.stop = p.getFloat("cool", d.cooldownFan.stop);
    s.cooldownFan.trip = p.getFloat("coolTrip", d.cooldownFan.trip);
    s.watchdog.enabled = p.getBool("wdOn", d.watchdog.enabled);
    s.watchdog.timeoutS = p.getUShort("wdS", d.watchdog.timeoutS);
    s.fanAssist = p.getBool("fanAs", d.fanAssist);
    p.end();
    return s;
}

void ConfigStore::saveGains(const PidGains& g) {
    Preferences p;
    p.begin(kNs, false);
    p.putFloat("kp", g.kp);
    p.putFloat("ki", g.ki);
    p.putFloat("kd", g.kd);
    p.putBool("tuned", true);
    p.end();
}

void ConfigStore::savePidInput(PidInput t) {
    Preferences p;
    p.begin(kNs, false);
    p.putUChar("in", static_cast<uint8_t>(t));
    p.end();
}

void ConfigStore::saveCooldownFan(const CooldownFan& c) {
    Preferences p;
    p.begin(kNs, false);
    p.putFloat("cool", c.stop);
    p.putFloat("coolTrip", c.trip);
    p.end();
}

void ConfigStore::saveWatchdog(const WatchdogSettings& w) {
    Preferences p;
    p.begin(kNs, false);
    p.putBool("wdOn", w.enabled);
    p.putUShort("wdS", w.timeoutS);
    p.end();
}

void ConfigStore::saveFanAssist(bool on) {
    Preferences p;
    p.begin(kNs, false);
    p.putBool("fanAs", on);
    p.end();
}

void ConfigStore::saveModel(const roastermodel::RoasterModel& m) {
    Preferences p;
    p.begin(kNs, false);
    p.putBytes("model", &m, sizeof m);
    p.end();
}

bool ConfigStore::loadModel(roastermodel::RoasterModel& out) {
    Preferences p;
    if (!p.begin(kNs, true))
        return false;
    const bool ok = p.isKey("model") && p.getBytesLength("model") == sizeof out && p.getBytes("model", &out, sizeof out) == sizeof out &&
                    roastermodel::usable(out);
    p.end();
    return ok;
}
