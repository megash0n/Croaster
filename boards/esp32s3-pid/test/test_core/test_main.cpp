// Host tests for croaster-control/src/core. Run: pio test -e native
#include <unity.h>

#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <string>

#include <ArduinoJson.h>

#include "core/autotune_status.h"
#include "core/control_protocol.h"
#include "core/cooldown.h"
#include "core/mains_watch.h"
#include "core/max31855.h"
#include "core/max6675.h"
#include "core/moving_average.h"
#include "core/pid.h"
#include "core/probe_channel.h"
#include "core/probe_correction.h"
#include "core/profile_library.h"
#include "core/recipe.h"
#include "core/relay_autotune.h"
#include "core/roast_profile.h"
#include "core/temp_unit.h"
#include "core/time_proportional.h"
#include "core/zero_cross.h"

void setUp() {}
void tearDown() {}

// ---------- MovingAverage ----------
void test_moving_average_seeds_with_first_sample() {
    MovingAverage<4> f;
    TEST_ASSERT_TRUE(std::isnan(f.value()));
    TEST_ASSERT_EQUAL_FLOAT(200.0f, f.push(200.0f)); // legacy filter would return 40
    f.push(NAN);                                     // ignored
    TEST_ASSERT_EQUAL_FLOAT(200.0f, f.value());
    f.push(204);
    f.push(204);
    f.push(204);
    f.push(204);
    TEST_ASSERT_EQUAL_FLOAT(204.0f, f.value());
}

// ---------- MAX31855 ----------
void test_max31855_decode() {
    // +100.25 C thermocouple (401 LSB), +25.0625 C internal (401 LSB)
    uint32_t raw = (401u << 18) | (401u << 4);
    auto r = max31855::decode(raw);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_FLOAT(100.25f, r.thermocoupleC);
    TEST_ASSERT_EQUAL_FLOAT(25.0625f, r.internalC);
    // -0.25 C thermocouple: 14-bit two's complement 0x3FFF
    r = max31855::decode((0x3FFFu << 18) | (401u << 4));
    TEST_ASSERT_EQUAL_FLOAT(-0.25f, r.thermocoupleC);
    // open thermocouple: D16 + D0
    r = max31855::decode((401u << 4) | 0x10000u | 0x1u);
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_TRUE(std::isnan(r.thermocoupleC));
    TEST_ASSERT_EQUAL_UINT8(max31855::kFaultOpen, r.fault);
    // nothing connected
    TEST_ASSERT_FALSE(max31855::decode(0).ok);
    TEST_ASSERT_FALSE(max31855::decode(0xFFFFFFFFu).ok);
}

void test_max31855_fault_bits() {
    const uint32_t room = (100u << 18) | (400u << 4);  // 25 C, 25 C
    auto r = max31855::decode(room | 0x10000u | 0x2u); // short to GND
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_EQUAL_UINT8(max31855::kFaultShortGnd, r.fault);
    TEST_ASSERT_EQUAL_FLOAT(25.0f, r.internalC);  // cold junction still valid
    r = max31855::decode(room | 0x10000u | 0x4u); // short to VCC
    TEST_ASSERT_EQUAL_UINT8(max31855::kFaultShortVcc, r.fault);
    r = max31855::decode(room | 0x10000u); // D16 alone still means "fault"
    TEST_ASSERT_FALSE(r.ok);
    r = max31855::decode((4000u << 18) | (400u << 4)); // +1000 C, top of range
    TEST_ASSERT_EQUAL_FLOAT(1000.0f, r.thermocoupleC);
    r = max31855::decode((100u << 18) | (0xF00u << 4)); // internal -16 C
    TEST_ASSERT_EQUAL_FLOAT(-16.0f, r.internalC);
}

void test_board_temperature_skips_missing_chip() {
    const uint32_t room = (100u << 18) | (400u << 4); // 25 C, 25 C
    const ThermoReading absent = max31855::decode(0); // no chip: MISO stuck low
    const ThermoReading present = max31855::decode(room);
    const ThermoReading noProbe = max31855::decode((400u << 4) | 0x10000u | max31855::kFaultOpen);
    TEST_ASSERT_EQUAL_FLOAT(25.0f, max31855::boardTemperatureC(absent, present)); // not 0 C
    TEST_ASSERT_EQUAL_FLOAT(25.0f, max31855::boardTemperatureC(noProbe, absent)); // open probe, chip fine
    TEST_ASSERT_TRUE(std::isnan(max31855::boardTemperatureC(absent, absent)));
    TEST_ASSERT_TRUE(std::isnan(max31855::boardTemperatureC(max6675::decode(0x0640), absent)));
}

// ---------- Two probes (ET + BT on separate CS pins) ----------
static ThermoReading reading(float c) {
    return max31855::decode((static_cast<uint32_t>(c * 4) << 18) | (400u << 4));
}
static ThermoReading openProbe() {
    return max31855::decode((400u << 4) | 0x10000u | max31855::kFaultOpen);
}

void test_probe_channels_independent_faults() {
    ProbeChannel et, bt;
    TEST_ASSERT_EQUAL_FLOAT(150.0f, et.push(reading(150)));
    TEST_ASSERT_EQUAL_FLOAT(120.0f, bt.push(reading(120)));
    // ET glitches twice: it holds its last average; BT is untouched
    TEST_ASSERT_EQUAL_FLOAT(150.0f, et.push(openProbe()));
    TEST_ASSERT_EQUAL_FLOAT(150.0f, et.push(openProbe()));
    TEST_ASSERT_EQUAL_FLOAT(120.0f, bt.push(reading(120)));
    // third consecutive fault: ET reports NaN (Croaster shows it as missing,
    // the controller trips if ET is the PID input)
    TEST_ASSERT_TRUE(std::isnan(et.push(openProbe())));
    TEST_ASSERT_EQUAL_FLOAT(120.0f, bt.push(reading(120)));
    // probe reconnected: starts from the new value, not the stale 150
    TEST_ASSERT_EQUAL_FLOAT(160.0f, et.push(reading(160)));
    // one good read between glitches resets the count
    et.push(openProbe());
    et.push(openProbe());
    et.push(reading(160));
    TEST_ASSERT_FALSE(std::isnan(et.push(openProbe())));
}

// ---------- TimeProportional ----------
void test_time_proportional_window() {
    TimeProportional tp(1000, 20);
    tp.setDuty(25);
    int on = 0;
    for (uint32_t t = 0; t < 1000; t++)
        on += tp.isOn(t);
    TEST_ASSERT_EQUAL_INT(250, on);
    tp.setDuty(1); // 10 ms < min pulse -> off
    on = 0;
    for (uint32_t t = 1000; t < 2000; t++)
        on += tp.isOn(t);
    TEST_ASSERT_EQUAL_INT(0, on);
    tp.setDuty(99); // 990 ms, 10 ms off < min pulse -> full on
    on = 0;
    for (uint32_t t = 2000; t < 3000; t++)
        on += tp.isOn(t);
    TEST_ASSERT_EQUAL_INT(1000, on);
}

// ---------- PID ----------
void test_pid_integrator_does_not_wind_up() {
    Pid::Config c;
    c.dtSeconds = 0.25f;
    Pid pid(c);
    pid.setGains({2.0f, 0.5f, 0.0f});
    for (int i = 0; i < 4000; i++)
        pid.update(200, 20); // huge error, saturated for 1000 s
    TEST_ASSERT_TRUE(pid.integralTerm() <= 100.0f);
    // once PV passes setpoint the output must come off the rail quickly
    float u = 100;
    for (int i = 0; i < 8 && u > 0; i++)
        u = pid.update(200, 210);
    TEST_ASSERT_TRUE(u < 100.0f);
}

void test_pid_bumpless_reset() {
    Pid pid(Pid::Config{});
    pid.setGains({3.0f, 0.2f, 1.0f});
    pid.reset(180, 175, 42.0f);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 42.0f, pid.update(180, 175));
}

// Controller::setMode() seeds the next mode with startOutputOnModeChange().
// Autotune at 180 C with PV exactly at setpoint: the relay is still high (100 %),
// and a PID at zero error should then output ~0, not the relay's 100 %.
void test_leaving_autotune_starts_from_zero() {
    RelayAutotune at;
    RelayAutotune::Config cfg;
    cfg.setpoint = 180;
    at.start(cfg);
    const float relay = at.update(180);
    TEST_ASSERT_EQUAL_FLOAT(100.0f, relay);

    const PidGains gains{1.0f, 0.1f, 0.01f}; // CPID_PID_* defaults
    const float toPid = startOutputOnModeChange(Mode::Autotune, Mode::Pid, relay);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, toPid);
    Pid pid(Pid::Config{});
    pid.setGains(gains);
    pid.reset(180, 180, toPid);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 0.0f, pid.update(180, 180));

    // Seeding with the relay output (the behaviour before 067e316) holds 100 %.
    Pid carried(Pid::Config{});
    carried.setGains(gains);
    carried.reset(180, 180, relay);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 100.0f, carried.update(180, 180));

    // Manual takes the start value as its heater %.
    TEST_ASSERT_EQUAL_FLOAT(0.0f, startOutputOnModeChange(Mode::Autotune, Mode::Manual, relay));
    // Other transitions stay bumpless.
    TEST_ASSERT_EQUAL_FLOAT(60.0f, startOutputOnModeChange(Mode::Manual, Mode::Pid, 60.0f));
    TEST_ASSERT_EQUAL_FLOAT(60.0f, startOutputOnModeChange(Mode::Pid, Mode::Manual, 60.0f));
}

// ---------- Relay autotune on a simulated roaster ----------
// First-order-plus-dead-time plant: tau dT/dt = K*u - (T - Tamb), dead time L.
struct Fopdt {
    float T = 25, amb = 25, K = 2.6f /*C per % at steady state*/, tau = 90, L = 6, dt = 0.25f;
    float hist[256] = {};
    int head = 0;
    float step(float u) {
        const int n = (int)(L / dt);
        hist[head % 256] = u;
        const float delayed = head >= n ? hist[(head - n) % 256] : 0;
        head++;
        T += dt / tau * (K * delayed - (T - amb));
        return T;
    }
};

void test_relay_autotune_identifies_and_controls() {
    Fopdt plant;
    RelayAutotune at;
    RelayAutotune::Config c;
    c.setpoint = 180;
    c.outputHigh = 100;
    c.outputLow = 0;
    c.hysteresis = 0.5f;
    at.start(c);
    float pv = plant.T;
    for (int i = 0; i < 40000 && at.state() == RelayAutotune::State::Running; i++)
        pv = plant.step(at.update(pv));
    TEST_ASSERT_EQUAL(RelayAutotune::State::Done, at.state());
    TEST_ASSERT_TRUE(at.ku() > 0 && at.tuSeconds() > 5 && at.tuSeconds() < 120);

    // closed loop with the tuned gains: step from 150 to 200 C
    Fopdt p2;
    p2.T = 150;
    Pid::Config pc;
    pc.dtSeconds = 0.25f;
    Pid pid(pc);
    pid.setGains(at.result());
    pid.reset(150, 150, (150 - 25) / p2.K);
    float maxT = 0, T = p2.T;
    for (int i = 0; i < 4 * 900; i++) { // 15 min
        T = p2.step(pid.update(200, T));
        if (T > maxT)
            maxT = T;
    }
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 200.0f, T); // settled
    TEST_ASSERT_TRUE(maxT < 208.0f);           // modest overshoot
}

void test_legacy_autotune_math_is_plant_independent() {
    // Documents the vendored AutoTunePID result: Tu=18s/10, Ku=4/pi, ZN with Ti=0.34Tu, Td=0.085Tu
    const float tu = 18.0f / 10.0f, ku = 4.0f / 3.14159265f;
    const float kp = 0.6f * ku, ki = kp / (2 * 0.17f * tu), kd = kp * (0.17f * tu / 2);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.764f, kp);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.248f, ki);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.117f, kd);
}

// ---------- MAX6675 ----------
void test_max6675_decode() {
    auto r = max6675::decode(static_cast<uint16_t>(801u << 3)); // 200.25 C
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_FLOAT(200.25f, r.thermocoupleC);
    r = max6675::decode(static_cast<uint16_t>((801u << 3) | 0x4)); // open probe
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_TRUE(std::isnan(r.thermocoupleC));
    TEST_ASSERT_FALSE(max6675::decode(0xFFFF).ok); // no chip / MISO floating high
}

// ---------- RoastProfile ----------
static bool loadProfile(RoastProfile& p, const char* json, char unit = 'C') {
    JsonDocument d;
    if (deserializeJson(d, json))
        return false;
    return p.loadJson(d.as<JsonArrayConst>(), unit);
}

void test_profile_matches_readme_example() {
    RoastProfile p;
    TEST_ASSERT_TRUE(loadProfile(p, R"([{"duration":10,"setpoint":40,"interpolation":"linear"},
                                       {"duration":360,"setpoint":217,"interpolation":"ease-out"}])"));
    TEST_ASSERT_EQUAL_FLOAT(40.0f, p.at(5).setpointC);
    TEST_ASSERT_EQUAL_FLOAT(40.0f, p.at(10).setpointC);
    // halfway through ease-out: 40 + 177 * 0.75 = 172.75
    TEST_ASSERT_FLOAT_WITHIN(0.11f, 172.7f, p.at(190).setpointC);
    TEST_ASSERT_TRUE(p.at(400).finished);
    TEST_ASSERT_EQUAL_FLOAT(217.0f, p.at(400).setpointC);
}

void test_profile_in_fahrenheit() {
    RoastProfile p;
    TEST_ASSERT_TRUE(loadProfile(p, R"([{"duration":60,"setpoint":392}])", 'F'));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 200.0f, p.at(60).setpointC);
}

// ---------- Controller protocol (Croaster nested commands) ----------
static controlproto::Parsed parse(const char* json, char unit = 'C', const char* only = nullptr) {
    JsonDocument d;
    deserializeJson(d, json);
    return controlproto::parseNested(d["command"].as<JsonObjectConst>(), unit, only);
}

void test_protocol_values() {
    auto p = parse(R"({"command":{"burner":65}})");
    TEST_ASSERT_EQUAL(1, (int)p.count);
    TEST_ASSERT_TRUE(p.commands[0].kind == Command::Kind::SetHeater);
    TEST_ASSERT_EQUAL_FLOAT(65.0f, p.commands[0].a);

    p = parse(R"({"command":{"setpoint":428,"mode":"pid"}})", 'F'); // mode applied first
    TEST_ASSERT_EQUAL(2, (int)p.count);
    TEST_ASSERT_TRUE(p.commands[0].kind == Command::Kind::SetMode);
    TEST_ASSERT_EQUAL_FLOAT((float)Mode::Pid, p.commands[0].a);
    TEST_ASSERT_TRUE(p.commands[1].kind == Command::Kind::SetSetpoint);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 220.0f, p.commands[1].a); // 428 F -> 220 C

    p = parse(R"({"command":{"pid":{"kp":5,"ki":0.1,"kd":2},"pidInput":"BT","cooldownFan":80}})");
    TEST_ASSERT_TRUE(p.hasGains && p.hasPidInput && p.hasCooldownStop);
    TEST_ASSERT_FALSE(p.hasCooldownTrip); // a plain number sets the stop speed only
    TEST_ASSERT_EQUAL_FLOAT(5.0f, p.gains.kp);
    TEST_ASSERT_TRUE(p.pidInput == PidInput::BT);
    TEST_ASSERT_EQUAL_FLOAT(80.0f, p.cooldownFan.stop);
}

void test_protocol_cooldown_fan() {
    auto p = parse(R"({"command":{"cooldownFan":{"stop":65,"trip":100}}})");
    TEST_ASSERT_TRUE(p.hasCooldownStop && p.hasCooldownTrip);
    TEST_ASSERT_EQUAL_FLOAT(65.0f, p.cooldownFan.stop);
    TEST_ASSERT_EQUAL_FLOAT(100.0f, p.cooldownFan.trip);
    TEST_ASSERT_EQUAL(0, (int)p.count); // a setting, not a control command

    p = parse(R"({"command":{"cooldownFan":{"trip":90}}})");
    TEST_ASSERT_FALSE(p.hasCooldownStop);
    TEST_ASSERT_TRUE(p.hasCooldownTrip);
    TEST_ASSERT_EQUAL_FLOAT(90.0f, p.cooldownFan.trip);

    p = parse(R"({"command":{"cooldownFan":{"stop":-1,"trip":101}}})"); // out of range: ignored
    TEST_ASSERT_FALSE(p.hasCooldownStop || p.hasCooldownTrip);
    p = parse(R"({"command":{"cooldownFan":150}})");
    TEST_ASSERT_FALSE(p.hasCooldownStop);

    CooldownFan defaults;
    TEST_ASSERT_EQUAL_FLOAT(65.0f, defaults.stop);
    TEST_ASSERT_EQUAL_FLOAT(100.0f, defaults.trip);

    Snapshot s;
    JsonDocument d;
    controlproto::writeControlInfo(s, CooldownFan{60, 95}, WatchdogSettings{}, 'C', d.to<JsonObject>());
    TEST_ASSERT_EQUAL(60, d["cooldownFan"]["stop"].as<int>());
    TEST_ASSERT_EQUAL(95, d["cooldownFan"]["trip"].as<int>());
}

void test_protocol_cooldown_end() {
    auto p = parse(R"({"command":{"cooldownFan":{"end":140}}})", 'F');
    TEST_ASSERT_TRUE(p.hasCooldownEnd);
    TEST_ASSERT_FALSE(p.hasCooldownStop || p.hasCooldownTrip);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 60.0f, p.cooldownFan.endC);
    p = parse(R"({"command":{"cooldownFan":{"end":20}}})"); // below 30 C: ignored
    TEST_ASSERT_FALSE(p.hasCooldownEnd);
    p = parse(R"({"command":{"cooldownFan":{"end":400}}})", 'F'); // 204 C: ignored
    TEST_ASSERT_FALSE(p.hasCooldownEnd);
    TEST_ASSERT_EQUAL_FLOAT(60.0f, CooldownFan{}.endC);

    Snapshot s;
    s.cooling = true;
    JsonDocument d;
    controlproto::writeControlInfo(s, CooldownFan{60, 95, 60}, WatchdogSettings{}, 'F', d.to<JsonObject>());
    TEST_ASSERT_EQUAL_FLOAT(140.0f, d["cooldownFan"]["end"].as<float>());
    TEST_ASSERT_TRUE(d["cooling"].as<bool>());
    JsonDocument data;
    controlproto::writeDataFields(s, 'F', data.to<JsonObject>());
    TEST_ASSERT_TRUE(data["cooling"].as<bool>());
}

void test_cooldown_ends_below_et() {
    Cooldown c;
    TEST_ASSERT_FALSE(c.update(20, 60, false)); // not started
    c.start();
    TEST_ASSERT_FALSE(c.update(180, 60, false));
    TEST_ASSERT_FALSE(c.update(NAN, 60, false)); // no ET: keeps running
    TEST_ASSERT_TRUE(c.active());
    TEST_ASSERT_FALSE(c.update(59, 60, true)); // over-temperature: fan stays at 100
    TEST_ASSERT_TRUE(c.update(59.9f, 60, false));
    TEST_ASSERT_FALSE(c.active());
    TEST_ASSERT_FALSE(c.update(20, 60, false)); // ends once
    c.start();
    c.cancel(); // operator command
    TEST_ASSERT_FALSE(c.update(20, 60, false));
}

void test_protocol_watchdog() {
    auto p = parse(R"({"command":{"watchdog":{"enabled":false}}})");
    TEST_ASSERT_TRUE(p.hasWatchdogEnabled);
    TEST_ASSERT_FALSE(p.watchdogEnabled);
    TEST_ASSERT_FALSE(p.hasWatchdogTimeout);
    TEST_ASSERT_EQUAL(0, (int)p.count); // a setting, not a control command

    p = parse(R"({"command":{"watchdog":{"enabled":true,"timeout":20}}})", 'C', "watchdog");
    TEST_ASSERT_TRUE(p.hasWatchdogEnabled && p.watchdogEnabled);
    TEST_ASSERT_TRUE(p.hasWatchdogTimeout);
    TEST_ASSERT_EQUAL_UINT16(20, p.watchdogTimeoutS);

    p = parse(R"({"command":{"watchdog":{"timeout":0}}})"); // out of range: ignored
    TEST_ASSERT_FALSE(p.hasWatchdogTimeout);
    p = parse(R"({"command":{"watchdog":{"timeout":601}}})");
    TEST_ASSERT_FALSE(p.hasWatchdogTimeout);

    Snapshot s;
    WatchdogSettings wd{false, 30};
    JsonDocument d;
    controlproto::writeControlInfo(s, CooldownFan{}, wd, 'C', d.to<JsonObject>());
    TEST_ASSERT_FALSE(d["watchdog"]["enabled"].as<bool>());
    TEST_ASSERT_EQUAL(30, d["watchdog"]["timeout"].as<int>());
}

void test_protocol_only_key() {
    // Croaster calls each onJsonCommand(key) callback with the whole object:
    // each callback must handle only its own key, or values apply twice.
    auto p = parse(R"({"command":{"burner":40,"fan":70}})", 'C', "fan");
    TEST_ASSERT_EQUAL(1, (int)p.count);
    TEST_ASSERT_TRUE(p.commands[0].kind == Command::Kind::SetFan);
    p = parse(R"({"command":{"tempUnit":"F"}})", 'C', "burner"); // not ours
    TEST_ASSERT_EQUAL(0, (int)p.count);
}

void test_protocol_profile() {
    auto p = parse(
        R"({"command":{"profile":[{"duration":60,"setpoint":150},{"duration":120,"setpoint":200,"fanValue":70}]}})");
    TEST_ASSERT_TRUE(p.hasProfile);
    TEST_ASSERT_EQUAL(2, (int)p.profile.size());
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 180.0f, p.profile.totalDurationS());
    p = parse(R"({"command":{"profile":"nope"}})");
    TEST_ASSERT_TRUE(p.profileError);
}

// T54: the page's "Load to device" message (the board reset while replying to it).
void test_protocol_profile_from_page() {
    auto p = parse(R"({"command":{"profile":[{"duration":69,"setpoint":300,"interpolation":"linear"},)"
                   R"({"duration":60,"setpoint":300,"interpolation":"linear"}]}})",
                   'F', "profile");
    TEST_ASSERT_TRUE(p.hasProfile);
    TEST_ASSERT_FALSE(p.profileError);
    TEST_ASSERT_EQUAL(2, (int)p.profile.size());
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 129.0f, p.profile.totalDurationS());
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 148.8f, p.profile.at(30).setpointC); // 300 F
    TEST_ASSERT_TRUE(std::isnan(p.profile.at(30).fanPct));
}

void test_protocol_profile_rejects_malformed() {
    const char* bad[] = {
        R"({"command":{"profile":[]}})",
        R"({"command":{"profile":{}}})",
        R"({"command":{"profile":true}})",
        R"({"command":{"profile":42}})",
        R"({"command":{"profile":[1,"x",null,[]]}})",
        R"({"command":{"profile":[{"setpoint":200}]}})",                          // no duration
        R"({"command":{"profile":[{"duration":60}]}})",                           // no setpoint
        R"({"command":{"profile":[{"duration":"60","setpoint":200}]}})",          // string
        R"({"command":{"profile":[{"duration":60,"setpoint":"hot"}]}})",
        R"({"command":{"profile":[{"duration":60,"setpoint":{}}]}})",
        R"({"command":{"profile":[{"duration":0,"setpoint":200}]}})",
        R"({"command":{"profile":[{"duration":-5,"setpoint":200}]}})",
        R"({"command":{"profile":[{"duration":3601,"setpoint":200}]}})",
        R"({"command":{"profile":[{"duration":1e308,"setpoint":200}]}})",
        R"({"command":{"profile":[{"duration":60,"setpoint":1e308}]}})",
        R"({"command":{"profile":[{"duration":60,"setpoint":-1e308}]}})",
        R"({"command":{"profile":[{"duration":60,"setpoint":2000}]}})",
        R"({"command":{"profile":[{"duration":60,"setpoint":200,"fanValue":101}]}})",
        R"({"command":{"profile":[{"duration":60,"setpoint":200,"fanValue":-1}]}})",
        R"({"command":{"profile":[{"duration":60,"setpoint":200,"fanValue":"max"}]}})",
        R"({"command":{"profile":[{"duration":60,"setpoint":200,"fanValue":1e308}]}})",
        R"({"command":{"profile":[{"duration":60,"setpoint":200},{"duration":60}]}})", // one bad step
    };
    for (const char* json : bad) {
        auto p = parse(json, 'C', "profile");
        TEST_ASSERT_FALSE_MESSAGE(p.hasProfile, json);
        TEST_ASSERT_TRUE_MESSAGE(p.profileError, json);
        TEST_ASSERT_EQUAL_MESSAGE(0, (int)p.profile.size(), json);
    }
    // 33 steps: one over the limit; 32 are fine.
    auto steps = [](int n) {
        std::string m = "{\"command\":{\"profile\":[";
        for (int i = 0; i < n; i++)
            m += std::string(i ? "," : "") + "{\"duration\":10,\"setpoint\":100}";
        return m + "]}}";
    };
    auto p = parse(steps(33).c_str(), 'C', "profile");
    TEST_ASSERT_TRUE(p.profileError);
    p = parse(steps(32).c_str(), 'C', "profile");
    TEST_ASSERT_TRUE(p.hasProfile);
    TEST_ASSERT_EQUAL(32, (int)p.profile.size());
    // An unknown or non-string interpolation is linear.
    p = parse(R"({"command":{"profile":[{"duration":60,"setpoint":200,"interpolation":7}]}})", 'C', "profile");
    TEST_ASSERT_TRUE(p.hasProfile);
}

void test_profile_failed_load_keeps_previous() {
    RoastProfile p;
    TEST_ASSERT_TRUE(loadProfile(p, R"([{"duration":60,"setpoint":150}])"));
    TEST_ASSERT_FALSE(loadProfile(p, R"([{"duration":60,"setpoint":150},{"duration":"x","setpoint":1}])"));
    TEST_ASSERT_EQUAL(1, (int)p.size());
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 150.0f, p.at(10).setpointC);
}

void test_protocol_basic_and_data() {
    Command c;
    TEST_ASSERT_TRUE(controlproto::basicCommand("startProfile", c));
    TEST_ASSERT_TRUE(c.kind == Command::Kind::StartProfile);
    TEST_ASSERT_TRUE(controlproto::basicCommand("STOP", c));
    TEST_ASSERT_TRUE(c.kind == Command::Kind::Stop);
    TEST_ASSERT_FALSE(controlproto::basicCommand("getControl", c));

    Snapshot s;
    s.heaterPct = 55;
    s.fanPct = 70;
    s.setpoint = 200;
    s.mode = Mode::Pid;
    JsonDocument d;
    JsonObject data = d["data"].to<JsonObject>();
    controlproto::writeDataFields(s, 'F', data);
    char buf[384];
    serializeJson(d, buf, sizeof buf);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"Target\":\"BT\"")); // default PID input (D18)
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"pidKp\":0"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"Amb\":null"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"BurnerVal\":55"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"FanVal\":70"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"SV\":392"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"Mode\":\"PID\""));
    s.setpoint = NAN;
    controlproto::writeDataFields(s, 'C', data);
    serializeJson(d, buf, sizeof buf);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"SV\":null"));
}

static controlproto::Parsed parseLegacy(const char* json) {
    JsonDocument d;
    deserializeJson(d, json);
    return controlproto::parseLegacy(d.as<JsonObjectConst>());
}

void test_protocol_yaeger_compat() {
    auto p = parseLegacy(R"({"command":"setBurner","value":60,"id":123,"roasterID":0})");
    TEST_ASSERT_EQUAL(1, (int)p.count);
    TEST_ASSERT_TRUE(p.commands[0].kind == Command::Kind::SetHeater);
    TEST_ASSERT_EQUAL_FLOAT(60.0f, p.commands[0].a);

    p = parseLegacy(R"({"command":"setFan","value":80})");
    TEST_ASSERT_EQUAL(1, (int)p.count);
    TEST_ASSERT_TRUE(p.commands[0].kind == Command::Kind::SetFan);

    // Flat keys are not supported (D8): no "command" member, nothing parsed.
    p = parseLegacy(R"({"BurnerVal":60,"Mode":"PID","Setpoint":428,"Target":"MAX"})");
    TEST_ASSERT_EQUAL(0, (int)p.count);
    TEST_ASSERT_FALSE(p.hasPidInput);

    p = parseLegacy(R"({"command":"setPreferences","pidKp":4,"pidKi":0.2,"pidKd":1,"cooldownFanSpeed":70})");
    TEST_ASSERT_TRUE(p.hasGains && p.hasCooldownStop);
    TEST_ASSERT_FALSE(p.hasCooldownTrip);
    TEST_ASSERT_EQUAL_FLOAT(0.2f, p.gains.ki);
    TEST_ASSERT_EQUAL_FLOAT(70.0f, p.cooldownFan.stop);

    p = parseLegacy(R"({"command":"getData","id":5})"); // not a yaeger control message
    TEST_ASSERT_EQUAL(0, (int)p.count);
    TEST_ASSERT_FALSE(p.hasGains || p.hasCooldownStop || p.hasPidInput);

    JsonDocument d;
    controlproto::writePreferences(PidGains{4, 0.5f, 1}, CooldownFan{70, 100}, d.to<JsonObject>());
    char buf[160];
    serializeJson(d, buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING(R"({"type":"preferences","pidKp":4,"pidKi":0.5,"pidKd":1,"cooldownFanSpeed":70})", buf);
}

void test_autotune_status_text() {
    Snapshot s;
    char full[48], brief[48];
    autotunetext::full(s, 'F', full, sizeof full);
    TEST_ASSERT_EQUAL_STRING("idle", full);
    autotunetext::brief(s, 'F', brief, sizeof brief);
    TEST_ASSERT_EQUAL_STRING("", brief);

    s.autotune = AutotuneStatus::NeedSetpoint; // refusal in the display unit
    autotunetext::full(s, 'F', full, sizeof full);
    TEST_ASSERT_EQUAL_STRING("need setpoint >= 212 F", full);
    autotunetext::full(s, 'C', full, sizeof full);
    TEST_ASSERT_EQUAL_STRING("need setpoint >= 100 C", full);
    autotunetext::brief(s, 'F', brief, sizeof brief);
    TEST_ASSERT_EQUAL_STRING("AT: SV<212F", brief);

    s.autotune = AutotuneStatus::Failed;
    s.autotuneReason = "oscillation too small vs hysteresis";
    autotunetext::full(s, 'F', full, sizeof full);
    TEST_ASSERT_EQUAL_STRING("failed: oscillation too small vs hysteresis", full);
    autotunetext::brief(s, 'F', brief, sizeof brief);
    TEST_ASSERT_EQUAL_STRING("AT fail: small swing", brief);

    s.autotune = AutotuneStatus::Aborted;
    s.autotuneReason = "";
    autotunetext::full(s, 'F', full, sizeof full);
    TEST_ASSERT_EQUAL_STRING("aborted", full);
    s.autotuneReason = "setpoint changed";
    autotunetext::full(s, 'F', full, sizeof full);
    TEST_ASSERT_EQUAL_STRING("aborted: setpoint changed", full);
    autotunetext::brief(s, 'F', brief, sizeof brief);
    TEST_ASSERT_EQUAL_STRING("AT abort: SV change", brief);
    s.autotuneReason = "correction changed";
    autotunetext::brief(s, 'F', brief, sizeof brief);
    TEST_ASSERT_EQUAL_STRING("AT abort: offset", brief);

    s.autotuneReason = "input changed";
    autotunetext::brief(s, 'F', brief, sizeof brief);
    TEST_ASSERT_EQUAL_STRING("AT abort: input", brief);

    s.autotuneReason = "fan changed";
    autotunetext::full(s, 'F', full, sizeof full);
    TEST_ASSERT_EQUAL_STRING("aborted: fan changed", full);
    autotunetext::brief(s, 'F', brief, sizeof brief);
    TEST_ASSERT_EQUAL_STRING("AT abort: fan", brief);
    s.autotuneReason = "heater changed";
    autotunetext::full(s, 'F', full, sizeof full);
    TEST_ASSERT_EQUAL_STRING("aborted: heater changed", full);
    autotunetext::brief(s, 'F', brief, sizeof brief);
    TEST_ASSERT_EQUAL_STRING("AT abort: heater", brief);

    s.autotuneReason = "an unexpectedly long reason text"; // header stays within 20 characters
    autotunetext::brief(s, 'F', brief, sizeof brief);
    TEST_ASSERT_EQUAL(20, (int)strlen(brief));

    s.autotune = AutotuneStatus::Done; // telemetry carries the same text
    JsonDocument d;
    controlproto::writeDataFields(s, 'F', d.to<JsonObject>());
    TEST_ASSERT_EQUAL_STRING("done", d["autotune"].as<const char*>());
}

void test_unit_conversion() {
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 100.0f, toCelsius(212.0f, 'F'));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 195.0f, toCelsius(383.0f, 'F')); // Artisan setpoint in F, under the 250 C cap
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, toCelsius(273.15f, 'K'));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 212.0f, fromCelsius(100.0f, 'F'));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 50.0f, fromCelsius(toCelsius(50.0f, 'C'), 'C'));
}

// ---------- Correction offsets (CroasterCore correctionBt/correctionEt) ----------
// ThermoSample fields: et, bt, etRaw, btRaw, ambient, etFault, btFault.

void test_correction_shifts_pid_input() {
    const ThermoSample raw{230, 190, 231, 191, 30, 0, 0};
    const ProbeCorrection k{5, -3}; // BT +5 C, ET -3 C
    const ThermoSample c = corrected(raw, k);
    TEST_ASSERT_EQUAL_FLOAT(195, selectPidInput(PidInput::BT, c));
    TEST_ASSERT_EQUAL_FLOAT(227, selectPidInput(PidInput::ET, c));
    TEST_ASSERT_EQUAL_FLOAT(227, selectPidInput(PidInput::MAX, c));
    TEST_ASSERT_EQUAL_FLOAT(30, c.ambient); // cold junction is not a probe reading

    // The display shows 195 and the client sends SV 195: no error, no heat.
    Pid pid(Pid::Config{});
    pid.setGains({1, 0, 0});
    TEST_ASSERT_EQUAL_FLOAT(0, pid.update(195, selectPidInput(PidInput::BT, c)));
}

void test_correction_in_fahrenheit() {
    // correctionBt +18 in F mode; CroasterCore::correctionBtCelsius() returns 18 * 5 / 9.
    const ProbeCorrection k{18.0f * 5.0f / 9.0f, 0};
    const ThermoSample raw{150, 200, 150, 200, NAN, 0, 0};
    const float displayedF = fromCelsius(raw.bt, 'F') + 18; // what Croaster shows: 410 F
    const float setpointC = toCelsius(displayedF, 'F');     // the client's SV at the shown value
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 210, setpointC);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, setpointC, selectPidInput(PidInput::BT, corrected(raw, k)));
}

void test_correction_keeps_nan() {
    const ThermoSample raw{200, NAN, 200, NAN, NAN, 0, 1};
    const ThermoSample c = corrected(raw, {5, 5});
    TEST_ASSERT_TRUE(std::isnan(c.bt));
    TEST_ASSERT_TRUE(std::isnan(c.btRaw));
    TEST_ASSERT_TRUE(std::isnan(selectPidInput(PidInput::BT, c)));
    TEST_ASSERT_TRUE(std::isnan(selectPidInput(PidInput::MAX, c)));
    TEST_ASSERT_EQUAL_FLOAT(205, selectPidInput(PidInput::ET, c));
    TEST_ASSERT_FALSE(overTemperature(raw, {5, 5}, 275)); // a dead probe is a sensor fault, not over-temperature
}

void test_overtemp_uses_raw_and_corrected() {
    const float limit = 275;
    // A negative offset shows 270, but the probe reads 280: trip.
    TEST_ASSERT_TRUE(overTemperature(ThermoSample{100, 100, 100, 280, NAN, 0, 0}, {-10, 0}, limit));
    TEST_ASSERT_TRUE(overTemperature(ThermoSample{100, 100, 280, 100, NAN, 0, 0}, {0, -10}, limit));
    // A positive offset shows 280 while the probe reads 270: trip.
    TEST_ASSERT_TRUE(overTemperature(ThermoSample{100, 100, 100, 270, NAN, 0, 0}, {10, 0}, limit));
    // Both below the limit.
    TEST_ASSERT_FALSE(overTemperature(ThermoSample{100, 100, 100, 270, NAN, 0, 0}, {-10, 0}, limit));
}

void test_snapshot_stays_uncorrected() {
    // CroasterCore adds the correction to what RoasterProbe returns, so the
    // snapshot must carry raw values or the offset would count twice.
    const ThermoSample raw{230, 190, 231, 191, 30, 0, 2};
    const ThermoSample c = corrected(raw, {10, 10});
    TEST_ASSERT_EQUAL_FLOAT(200, c.bt);
    Snapshot s;
    publishProbes(raw, s);
    TEST_ASSERT_EQUAL_FLOAT(230, s.et);
    TEST_ASSERT_EQUAL_FLOAT(190, s.bt);
    TEST_ASSERT_EQUAL_FLOAT(231, s.etRaw);
    TEST_ASSERT_EQUAL_FLOAT(191, s.btRaw);
    TEST_ASSERT_EQUAL_FLOAT(30, s.ambient);
    TEST_ASSERT_EQUAL(2, s.btFault);
}

// Controller::handlePvStep(): a pidInput or correction change steps the PV.
// BT 195 C -> ET 205 C at SV 200 in PID with 50 % heater out.
void test_autotune_required_gate() {
    // CPID_REQUIRE_AUTOTUNE 1, default gains: PID and Profile refused.
    TEST_ASSERT_TRUE(modeNeedsTuning(Mode::Pid, true, false));
    TEST_ASSERT_TRUE(modeNeedsTuning(Mode::Profile, true, false));
    TEST_ASSERT_FALSE(modeNeedsTuning(Mode::Manual, true, false));
    TEST_ASSERT_FALSE(modeNeedsTuning(Mode::Autotune, true, false));
    // Tuned (autotune done or pid command): everything allowed.
    for (Mode m : {Mode::Manual, Mode::Pid, Mode::Profile, Mode::Autotune})
        TEST_ASSERT_FALSE(modeNeedsTuning(m, true, true));
    // CPID_REQUIRE_AUTOTUNE 0: default gains usable.
    for (Mode m : {Mode::Manual, Mode::Pid, Mode::Profile, Mode::Autotune})
        TEST_ASSERT_FALSE(modeNeedsTuning(m, false, false));

    // Setting gains with the pid command or yaeger's setPreferences is how a
    // board counts as tuned without autotune (Controller: SetGains -> tuned).
    auto p = parse(R"({"command":{"pid":{"kp":2,"ki":0.02,"kd":5}}})", 'C', "pid");
    TEST_ASSERT_EQUAL(1, (int)p.count);
    TEST_ASSERT_TRUE(p.commands[0].kind == Command::Kind::SetGains);

    Snapshot s;
    s.autotune = AutotuneStatus::Required;
    char full[48], brief[24];
    autotunetext::full(s, 'F', full, sizeof full);
    TEST_ASSERT_EQUAL_STRING("autotune required", full);
    autotunetext::brief(s, 'F', brief, sizeof brief);
    TEST_ASSERT_EQUAL_STRING("Autotune first", brief);

    JsonDocument d;
    s.gainsTuned = true;
    controlproto::writeControlInfo(s, CooldownFan{}, WatchdogSettings{}, 'C', d.to<JsonObject>());
    TEST_ASSERT_TRUE(d["tuned"].as<bool>());
}

void test_output_command_stops_autotune() {
    using K = Command::Kind;
    // Autotune at fan 60 % (the start rule may have raised it from below 30 %).
    TEST_ASSERT_EQUAL_STRING("fan changed", outputCommandStopsAutotune(Mode::Autotune, K::SetFan, 80, 60));
    TEST_ASSERT_EQUAL_STRING("fan changed", outputCommandStopsAutotune(Mode::Autotune, K::SetFan, 0, 60));
    TEST_ASSERT_NULL(outputCommandStopsAutotune(Mode::Autotune, K::SetFan, 60, 60));   // same speed: no change
    TEST_ASSERT_NULL(outputCommandStopsAutotune(Mode::Autotune, K::SetFan, 120, 100)); // clamps to the current 100
    TEST_ASSERT_EQUAL_STRING("heater changed", outputCommandStopsAutotune(Mode::Autotune, K::SetHeater, 50, 60));
    TEST_ASSERT_EQUAL_STRING("heater changed", outputCommandStopsAutotune(Mode::Autotune, K::SetHeater, 0, 60));
    // Other commands go through their own rules; other modes are unaffected.
    TEST_ASSERT_NULL(outputCommandStopsAutotune(Mode::Autotune, K::SetTripFan, 80, 60));
    TEST_ASSERT_NULL(outputCommandStopsAutotune(Mode::Autotune, K::StartAutotune, 0, 60));
    TEST_ASSERT_NULL(outputCommandStopsAutotune(Mode::Manual, K::SetFan, 80, 60));
    TEST_ASSERT_NULL(outputCommandStopsAutotune(Mode::Pid, K::SetHeater, 50, 60));
    TEST_ASSERT_NULL(outputCommandStopsAutotune(Mode::Profile, K::SetFan, 80, 60));
}

void test_pv_step_stops_autotune_and_resets_pid() {
    TEST_ASSERT_TRUE(onPvStep(Mode::Autotune) == PvStepAction::AbortAutotune);
    TEST_ASSERT_TRUE(onPvStep(Mode::Pid) == PvStepAction::ResetPid);
    TEST_ASSERT_TRUE(onPvStep(Mode::Profile) == PvStepAction::ResetPid);
    TEST_ASSERT_TRUE(onPvStep(Mode::Manual) == PvStepAction::None);

    const PidGains gains{3.0f, 0.05f, 20.0f};
    Pid kicked(Pid::Config{});
    kicked.setGains(gains);
    kicked.reset(200, 195, 50);
    kicked.update(200, 195);
    // Without a reset the derivative sees a 10 C jump in one 0.25 s sample.
    TEST_ASSERT_EQUAL_FLOAT(0.0f, kicked.update(200, 205));

    Pid reset(Pid::Config{});
    reset.setGains(gains);
    reset.reset(200, 195, 50);
    reset.update(200, 195);
    reset.reset(200, 205, 50); // what handlePvStep does in PID and Profile
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 50.0f, reset.update(200, 205));
}

// Controller::controlStep() trips SensorFault after 2 s of sensorFault().
void test_sensor_fault_rules() {
    const ThermoSample none{NAN, NAN, NAN, NAN, NAN, 1, 1};
    const ThermoSample etOnly{200, NAN, 200, NAN, 30, 0, 1};
    // PID, Profile and Autotune need their input; the other probe doesn't matter.
    TEST_ASSERT_TRUE(sensorFault(Mode::Pid, NAN, etOnly, 50));
    TEST_ASSERT_TRUE(sensorFault(Mode::Autotune, NAN, etOnly, 100));
    TEST_ASSERT_TRUE(sensorFault(Mode::Profile, NAN, etOnly, 0));
    TEST_ASSERT_FALSE(sensorFault(Mode::Pid, 200, etOnly, 50));
    // Manual: heating with no readable probe trips; one probe or no heat doesn't.
    TEST_ASSERT_TRUE(sensorFault(Mode::Manual, NAN, none, 40));
    TEST_ASSERT_FALSE(sensorFault(Mode::Manual, NAN, none, 0));
    TEST_ASSERT_FALSE(sensorFault(Mode::Manual, NAN, etOnly, 40));
}

// FanDimmer::mainsDetected(): a crossing stored after the control task read the
// timer used to wrap to a huge age and report "no mains", which latches the
// nomains trip while heating with CPID_REQUIRE_MAINS 1.
void test_zero_cross_age() {
    TEST_ASSERT_TRUE(crossedWithin(1000000, 1050000, 100000));
    TEST_ASSERT_FALSE(crossedWithin(1000000, 1100000, 100000));
    TEST_ASSERT_TRUE(crossedWithin(1000005, 1000000, 100000)); // ISR fired between the reads
    TEST_ASSERT_TRUE(crossedWithin(1000000, 1000000, 100000));
}

// CPID_REQUIRE_MAINS grace period (2 s default).
void test_mains_dropout_under_grace_does_not_trip() {
    MainsWatch w;
    TEST_ASSERT_FALSE(w.update(true, true, 1000, 2000));
    TEST_ASSERT_FALSE(w.update(true, false, 1010, 2000)); // mains lost
    TEST_ASSERT_FALSE(w.update(true, false, 3009, 2000)); // 1999 ms
    TEST_ASSERT_FALSE(w.update(true, true, 3010, 2000));  // back before 2 s
    TEST_ASSERT_FALSE(w.update(true, false, 3020, 2000)); // a new dropout starts from 0
    TEST_ASSERT_FALSE(w.update(true, false, 5019, 2000));
}

void test_mains_absent_past_grace_trips() {
    MainsWatch w;
    TEST_ASSERT_FALSE(w.update(true, false, 0, 2000)); // lost at millis() == 0
    TEST_ASSERT_FALSE(w.update(true, false, 1999, 2000));
    TEST_ASSERT_TRUE(w.update(true, false, 2000, 2000));
    TEST_ASSERT_TRUE(w.update(true, false, 2500, 2000));

    MainsWatch wrap; // across the millis() wrap
    TEST_ASSERT_FALSE(wrap.update(true, false, 0xFFFFFF00u, 2000));
    TEST_ASSERT_TRUE(wrap.update(true, false, 0xFFFFFF00u + 2000u, 2000));

    MainsWatch now; // grace 0: trips on the first missed check, as before the grace period
    TEST_ASSERT_TRUE(now.update(true, false, 5000, 0));
}

void test_mains_heater_off_resets_grace() {
    MainsWatch w;
    TEST_ASSERT_FALSE(w.update(true, false, 1000, 2000));
    TEST_ASSERT_FALSE(w.update(false, false, 2500, 2000)); // heater 0: no trip, count cleared
    TEST_ASSERT_FALSE(w.update(true, false, 2600, 2000));  // heating again, mains still absent
    TEST_ASSERT_FALSE(w.update(true, false, 4599, 2000));
    TEST_ASSERT_TRUE(w.update(true, false, 4600, 2000));
    TEST_ASSERT_FALSE(w.update(false, false, 9000, 2000)); // never trips with the heater off
}

// ---------- Temperature unit kept across reboots ----------
void test_temp_unit_boot_uses_saved_or_default() {
    TEST_ASSERT_EQUAL_STRING("C", tempunit::atBoot("C", "F"));
    TEST_ASSERT_EQUAL_STRING("K", tempunit::atBoot("K", "F"));
    TEST_ASSERT_EQUAL_STRING("F", tempunit::atBoot("", "F"));      // nothing saved yet
    TEST_ASSERT_EQUAL_STRING("F", tempunit::atBoot(nullptr, "F")); // namespace missing
    TEST_ASSERT_EQUAL_STRING("F", tempunit::atBoot("c", "F"));     // only C, F, K are units
    TEST_ASSERT_EQUAL_STRING("F", tempunit::atBoot("CF", "F"));
}

void test_temp_unit_saves_only_changes() {
    TEST_ASSERT_TRUE(tempunit::needsSave("", "C")); // first change
    TEST_ASSERT_TRUE(tempunit::needsSave("F", "C"));
    TEST_ASSERT_FALSE(tempunit::needsSave("C", "C")); // same unit: no flash write
    TEST_ASSERT_FALSE(tempunit::needsSave("C", "X")); // never store an invalid unit
    TEST_ASSERT_FALSE(tempunit::needsSave("", ""));
}


// ---------- Profile library ----------
namespace {
// In-memory LibraryStorage. failWrites makes write() fail (a full or failing flash).
struct FakeStorage : proflib::LibraryStorage {
    std::map<std::string, std::string> files;
    bool failWrites = false;
    int writes = 0;
    bool list(std::vector<std::string>& names) override {
        for (auto& f : files)
            names.push_back(f.first);
        return true;
    }
    bool read(const std::string& n, std::string& out) override {
        auto it = files.find(n);
        if (it == files.end())
            return false;
        out = it->second;
        return true;
    }
    bool write(const std::string& n, const std::string& d) override {
        writes++;
        if (failWrites) {
            files[n] = d.substr(0, d.size() / 2); // a torn write
            return false;
        }
        files[n] = d;
        return true;
    }
    bool rename(const std::string& a, const std::string& b) override {
        auto it = files.find(a);
        if (it == files.end())
            return false;
        files[b] = it->second;
        files.erase(it);
        return true;
    }
    bool remove(const std::string& n) override {
        return files.erase(n) == 1;
    }
};

const char* const kGoodProfile =
    R"({"version":1,"name":"City roast","notes":"Kenya AA","unit":"F","steps":[)"
    R"({"duration":60,"setpoint":300,"interpolation":"linear"},{"duration":120,"setpoint":400,"fanValue":60}]})";

proflib::Error validateText(const std::string& text) {
    JsonDocument d;
    deserializeJson(d, text);
    return proflib::validate(d.as<JsonObjectConst>());
}
std::string withField(const char* field) { // kGoodProfile with one more (or a replaced) member
    JsonDocument d;
    deserializeJson(d, kGoodProfile);
    JsonDocument f;
    deserializeJson(f, field);
    for (JsonPairConst kv : f.as<JsonObjectConst>())
        d[kv.key()] = kv.value();
    std::string out;
    serializeJson(d, out);
    return out;
}
} // namespace

void test_library_validate_format() {
    using proflib::Error;
    TEST_ASSERT_EQUAL(Error::None, validateText(kGoodProfile));
    TEST_ASSERT_EQUAL(Error::BadVersion, validateText(R"({"name":"x","unit":"C","steps":[{"duration":1,"setpoint":1}]})"));
    TEST_ASSERT_EQUAL(Error::BadVersion, validateText(withField(R"({"version":0})")));
    TEST_ASSERT_EQUAL(Error::BadVersion, validateText(withField(R"({"version":"1"})")));
    TEST_ASSERT_EQUAL(Error::BadVersion, validateText(withField(R"({"version":1.5})")));
    TEST_ASSERT_EQUAL(Error::NewerVersion, validateText(withField(R"({"version":2})")));
    TEST_ASSERT_EQUAL(Error::BadName, validateText(withField(R"({"name":""})")));
    TEST_ASSERT_EQUAL(Error::BadName, validateText(withField(R"({"name":"   "})")));
    TEST_ASSERT_EQUAL(Error::BadName, validateText(withField(R"({"name":7})")));
    TEST_ASSERT_EQUAL(Error::BadName, validateText(withField(R"({"name":"tab\there"})")));
    TEST_ASSERT_EQUAL(Error::None, validateText(withField(R"({"name":"0123456789012345678901234567890123456789"})")));
    TEST_ASSERT_EQUAL(Error::BadName, validateText(withField(R"({"name":"01234567890123456789012345678901234567890"})")));
    // 40 code points of two bytes each still fit: the limit counts characters, not bytes
    std::string accented = R"({"name":")";
    for (int i = 0; i < 40; i++)
        accented += "\xC3\xA9";
    TEST_ASSERT_EQUAL(Error::None, validateText(withField((accented + "\"}").c_str())));
    TEST_ASSERT_EQUAL(Error::BadNotes, validateText(withField(R"({"notes":5})")));
    TEST_ASSERT_EQUAL(Error::None, validateText(withField(("{\"notes\":\"" + std::string(1024, 'n') + "\"}").c_str())));
    TEST_ASSERT_EQUAL(Error::BadNotes, validateText(withField(("{\"notes\":\"" + std::string(1025, 'n') + "\"}").c_str())));
    TEST_ASSERT_EQUAL(Error::BadUnit, validateText(withField(R"({"unit":"X"})")));
    TEST_ASSERT_EQUAL(Error::BadUnit, validateText(withField(R"({"unit":"FF"})")));
    // steps follow the T54 limits of RoastProfile::loadJson
    TEST_ASSERT_EQUAL(Error::BadSteps, validateText(withField(R"({"steps":[]})")));
    TEST_ASSERT_EQUAL(Error::BadSteps, validateText(withField(R"({"steps":[{"duration":3601,"setpoint":300}]})")));
    TEST_ASSERT_EQUAL(Error::BadSteps, validateText(withField(R"({"steps":[{"duration":60,"setpoint":300,"fanValue":101}]})")));
    TEST_ASSERT_EQUAL(Error::BadSteps, validateText(withField(R"({"unit":"C","steps":[{"duration":60,"setpoint":1001}]})")));
    TEST_ASSERT_EQUAL(Error::None, validateText(withField(R"({"unit":"F","steps":[{"duration":60,"setpoint":1001}]})"))); // 538 C
    // keys this version does not know are allowed (a later "triggers" field)
    TEST_ASSERT_EQUAL(Error::None, validateText(withField(R"({"triggers":[{"at":"FC_START","fan":80}]})")));
}

void test_library_ids_and_busy() {
    TEST_ASSERT_TRUE(proflib::validId("city-roast_2"));
    TEST_ASSERT_FALSE(proflib::validId(""));
    TEST_ASSERT_FALSE(proflib::validId("City"));
    TEST_ASSERT_FALSE(proflib::validId("../x"));
    TEST_ASSERT_FALSE(proflib::validId("a.json"));
    TEST_ASSERT_TRUE(proflib::validId(std::string(32, 'a').c_str()));
    TEST_ASSERT_FALSE(proflib::validId(std::string(33, 'a').c_str()));
    TEST_ASSERT_EQUAL_STRING("city-roast-2", proflib::slug("  City Roast #2!").c_str());
    TEST_ASSERT_EQUAL_STRING("profile", proflib::slug("\xC3\xA9\xC3\xA9").c_str());
    TEST_ASSERT_EQUAL(24, proflib::slug("a very long profile name that keeps going").size());

    Snapshot s;
    TEST_ASSERT_NULL(proflib::busyReason(s));
    s.mode = Mode::Pid; // PID holding with the heater at 0 is idle enough
    TEST_ASSERT_NULL(proflib::busyReason(s));
    s.heaterPct = 0.5f;
    TEST_ASSERT_EQUAL_STRING("heater on", proflib::busyReason(s));
    s.heaterPct = 0;
    s.mode = Mode::Profile;
    TEST_ASSERT_EQUAL_STRING("profile running", proflib::busyReason(s));
    s.mode = Mode::Autotune;
    TEST_ASSERT_EQUAL_STRING("autotune running", proflib::busyReason(s));
}

namespace {
proflib::Error saveText(proflib::Library& lib, const char* id, bool overwrite, const std::string& text,
                        std::string* idOut = nullptr) {
    JsonDocument d;
    deserializeJson(d, text);
    std::string out;
    bool created = false;
    const proflib::Error e = lib.save(id, overwrite, d.as<JsonObjectConst>(), out, created);
    if (idOut)
        *idOut = out;
    return e;
}
} // namespace

void test_library_save_get_delete() {
    using proflib::Error;
    FakeStorage fs;
    proflib::Library lib(fs);
    std::string id;
    TEST_ASSERT_EQUAL(Error::Unavailable, saveText(lib, nullptr, false, kGoodProfile)); // before begin()
    TEST_ASSERT_TRUE(lib.begin());
    TEST_ASSERT_EQUAL(Error::None, saveText(lib, nullptr, false, kGoodProfile, &id));
    TEST_ASSERT_EQUAL_STRING("city-roast", id.c_str());
    TEST_ASSERT_TRUE(fs.files.count("city-roast.json") == 1 && fs.files.count("city-roast.tmp") == 0);
    // same name again: a new id, not an overwrite
    TEST_ASSERT_EQUAL(Error::None, saveText(lib, nullptr, false, kGoodProfile, &id));
    TEST_ASSERT_EQUAL_STRING("city-roast-2", id.c_str());
    // an existing id needs overwrite
    TEST_ASSERT_EQUAL(Error::Exists, saveText(lib, "city-roast", false, withField(R"({"name":"Changed"})")));
    TEST_ASSERT_EQUAL(Error::None, saveText(lib, "city-roast", true, withField(R"({"name":"Changed"})")));
    TEST_ASSERT_EQUAL(Error::BadId, saveText(lib, "../etc", true, kGoodProfile));
    TEST_ASSERT_EQUAL(Error::NewerVersion, saveText(lib, "new", false, withField(R"({"version":2})")));

    // list: metadata only, sorted by name
    TEST_ASSERT_EQUAL(2, lib.list().size());
    TEST_ASSERT_EQUAL_STRING("Changed", lib.list()[0].name.c_str());
    TEST_ASSERT_EQUAL_STRING("City roast", lib.list()[1].name.c_str());
    TEST_ASSERT_EQUAL(2, lib.list()[1].steps);
    TEST_ASSERT_EQUAL_FLOAT(180, lib.list()[1].durationS);
    TEST_ASSERT_EQUAL('F', lib.list()[1].unit);

    // get returns the stored text, unknown keys kept
    TEST_ASSERT_EQUAL(Error::None, saveText(lib, "t", false, withField(R"({"triggers":[{"at":"DRY_END"}]})")));
    std::string text;
    TEST_ASSERT_EQUAL(Error::None, lib.get("t", text));
    TEST_ASSERT_NOT_NULL(std::strstr(text.c_str(), "\"triggers\":[{\"at\":\"DRY_END\"}]"));
    TEST_ASSERT_EQUAL(Error::NotFound, lib.get("missing", text));

    TEST_ASSERT_EQUAL(Error::None, lib.remove("t"));
    TEST_ASSERT_EQUAL(Error::NotFound, lib.remove("t"));
    TEST_ASSERT_EQUAL(0, fs.files.count("t.json"));
}

void test_library_limits() {
    using proflib::Error;
    FakeStorage fs;
    proflib::Library lib(fs);
    lib.begin();
    // over 8 KB serialized, even with valid fields
    const std::string big = withField(("{\"extra\":\"" + std::string(proflib::kMaxFileBytes, 'x') + "\"}").c_str());
    TEST_ASSERT_EQUAL(Error::TooLarge, saveText(lib, "big", false, big));
    TEST_ASSERT_EQUAL(0, fs.writes);
    for (size_t i = 0; i < proflib::kMaxProfiles; i++)
        TEST_ASSERT_EQUAL(Error::None, saveText(lib, ("p" + std::to_string(i)).c_str(), false, kGoodProfile));
    TEST_ASSERT_EQUAL(Error::Full, saveText(lib, "one-more", false, kGoodProfile));
    TEST_ASSERT_EQUAL(Error::None, saveText(lib, "p7", true, kGoodProfile)); // overwriting is not a new profile
    TEST_ASSERT_EQUAL(proflib::kMaxProfiles, lib.list().size());
}

void test_library_failed_write_keeps_old_file() {
    using proflib::Error;
    FakeStorage fs;
    proflib::Library lib(fs);
    lib.begin();
    TEST_ASSERT_EQUAL(Error::None, saveText(lib, "a", false, kGoodProfile));
    const std::string before = fs.files["a.json"];
    fs.failWrites = true;
    TEST_ASSERT_EQUAL(Error::Storage, saveText(lib, "a", true, withField(R"({"name":"Other"})")));
    TEST_ASSERT_EQUAL_STRING(before.c_str(), fs.files["a.json"].c_str());
    TEST_ASSERT_EQUAL(0, fs.files.count("a.tmp"));
    TEST_ASSERT_EQUAL_STRING("City roast", lib.list()[0].name.c_str());
}

void test_library_begin_recovers_and_indexes() {
    FakeStorage fs;
    fs.files["a.json"] = kGoodProfile;
    fs.files["a.tmp"] = "{\"version\":1,\"na"; // power cut during a save
    fs.files["broken.json"] = "{not json";
    fs.files["Upper.json"] = kGoodProfile; // not a valid id: ignored
    fs.files["notes.txt"] = "x";
    proflib::Library lib(fs);
    TEST_ASSERT_TRUE(lib.begin());
    TEST_ASSERT_EQUAL(0, fs.files.count("a.tmp"));
    TEST_ASSERT_EQUAL(2, lib.list().size());
    const proflib::Meta& broken = lib.list()[0].id == "broken" ? lib.list()[0] : lib.list()[1];
    TEST_ASSERT_FALSE(broken.valid); // listed so it can be deleted
    RoastProfile p;
    TEST_ASSERT_EQUAL(proflib::Error::Storage, lib.load("broken", p));
    TEST_ASSERT_EQUAL(proflib::Error::None, lib.remove("broken"));
}

void test_library_load_converts_unit() {
    FakeStorage fs;
    fs.files["f.json"] = kGoodProfile; // 300 F, then 400 F
    proflib::Library lib(fs);
    lib.begin();
    RoastProfile p;
    TEST_ASSERT_EQUAL(proflib::Error::None, lib.load("f", p));
    TEST_ASSERT_EQUAL(2, p.size());
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 148.8f, p.at(0).setpointC); // 300 F = 148.9 C, floored to 0.1
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 204.4f, p.at(180).setpointC);
    TEST_ASSERT_EQUAL_FLOAT(60, p.at(100).fanPct);
    TEST_ASSERT_EQUAL(proflib::Error::NotFound, lib.load("nope", p));
    TEST_ASSERT_EQUAL(2, p.size()); // a failed load keeps the previous profile
}

// ---- recipe (core/recipe.h) ----
namespace {
const char* parseRecipe(const char* text, recipe::Recipe& r, char unit = 'C') {
    JsonDocument d;
    deserializeJson(d, text);
    return recipe::parse(d.as<JsonObjectConst>(), unit, r);
}
recipe::Inputs at(uint32_t ms, float bt = NAN, float et = NAN, float ror = NAN) {
    recipe::Inputs in;
    in.nowMs = ms;
    in.btC = bt;
    in.etC = et;
    in.rorC = ror;
    in.setpointC = 150;
    return in;
}
bool has(const recipe::Output& o, Command::Kind k, float a) {
    for (size_t i = 0; i < o.count; i++)
        if (o.commands[i].kind == k && std::fabs(o.commands[i].a - a) < 0.01f)
            return true;
    return false;
}
} // namespace

void test_recipe_parse_limits() {
    recipe::Recipe r;
    TEST_ASSERT_EQUAL_STRING("", parseRecipe(R"({"steps":[{"trigger":{"type":"bt","value":389},"actions":[{"type":"fan","value":50}]}]})", r, 'F'));
    TEST_ASSERT_EQUAL(1, r.count);
    TEST_ASSERT_FALSE(r.guided);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 198.33f, r.steps[0].trigger.value); // 389 F
    TEST_ASSERT_TRUE(r.steps[0].trigger.op == recipe::Op::Gte);         // default
    TEST_ASSERT_EQUAL_STRING("", parseRecipe(R"({"mode":"guided","steps":[{"trigger":{"type":"ror","op":"near","value":18,"tol":9},"actions":[{"type":"note","text":"hi"}]}]})", r, 'F'));
    TEST_ASSERT_TRUE(r.guided);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 10, r.steps[0].trigger.value); // F/min to C/min
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 5, r.steps[0].trigger.tol);
    TEST_ASSERT_EQUAL_STRING("", parseRecipe(R"({"steps":[{"trigger":{"type":"event","value":"FC_START"},"actions":[{"type":"setpoint","value":220,"ramp":90},{"type":"hold"},{"type":"cool"},{"type":"end"}]}]})", r));
    TEST_ASSERT_EQUAL(4, r.steps[0].actionCount);
    // refused
    const char* bad[] = {
        R"({"steps":[]})",
        R"({"mode":"fast","steps":[{"trigger":{"type":"time","value":1},"actions":[{"type":"end"}]}]})",
        R"({"steps":[{"trigger":{"type":"time","value":3601},"actions":[{"type":"end"}]}]})",
        R"({"steps":[{"trigger":{"type":"time","value":-1},"actions":[{"type":"end"}]}]})",
        R"({"steps":[{"trigger":{"type":"bt","value":351},"actions":[{"type":"end"}]}]})",
        R"({"steps":[{"trigger":{"type":"ror","value":101},"actions":[{"type":"end"}]}]})",
        R"({"steps":[{"trigger":{"type":"bt","op":"eq","value":200},"actions":[{"type":"end"}]}]})",
        R"({"steps":[{"trigger":{"type":"bt","op":"near","value":200,"tol":21},"actions":[{"type":"end"}]}]})",
        R"({"steps":[{"trigger":{"type":"bt","value":"hot"},"actions":[{"type":"end"}]}]})",
        R"({"steps":[{"trigger":{"type":"event","value":"CRACK"},"actions":[{"type":"end"}]}]})",
        R"({"steps":[{"trigger":{"type":"drum","value":1},"actions":[{"type":"end"}]}]})",
        R"({"steps":[{"trigger":{"type":"time","value":1},"actions":[]}]})",
        R"({"steps":[{"trigger":{"type":"time","value":1},"actions":[{"type":"end"},{"type":"end"},{"type":"end"},{"type":"end"},{"type":"end"}]}]})",
        R"({"steps":[{"trigger":{"type":"time","value":1},"actions":[{"type":"power","value":101}]}]})",
        R"({"steps":[{"trigger":{"type":"time","value":1},"actions":[{"type":"fan","value":-1}]}]})",
        R"({"steps":[{"trigger":{"type":"time","value":1},"actions":[{"type":"setpoint","value":400}]}]})",
        R"({"steps":[{"trigger":{"type":"time","value":1},"actions":[{"type":"setpoint","value":200,"ramp":3601}]}]})",
        R"({"steps":[{"trigger":{"type":"time","value":1},"actions":[{"type":"drum","value":50}]}]})",
        R"({"steps":[{"trigger":{"type":"time","value":1},"anyOrder":1,"actions":[{"type":"end"}]}]})",
        R"({"steps":[{"trigger":{"type":"time","value":1},"actions":[{"type":"note"}]}]})",
    };
    for (const char* b : bad)
        TEST_ASSERT_NOT_EQUAL_MESSAGE(0, std::strlen(parseRecipe(b, r)), b);
    std::string many = R"({"steps":[)";
    for (int i = 0; i < 33; i++)
        many += std::string(i ? "," : "") + R"({"trigger":{"type":"time","value":1},"actions":[{"type":"end"}]})";
    TEST_ASSERT_NOT_EQUAL(0, std::strlen(parseRecipe((many + "]}").c_str(), r)));
}

void test_recipe_comparators() {
    recipe::Recipe r;
    parseRecipe(R"({"steps":[
        {"trigger":{"type":"bt","op":"gte","value":200},"anyOrder":true,"actions":[{"type":"fan","value":10}]},
        {"trigger":{"type":"et","op":"lte","value":100},"anyOrder":true,"actions":[{"type":"fan","value":20}]},
        {"trigger":{"type":"ror","op":"near","value":10,"tol":1},"anyOrder":true,"actions":[{"type":"fan","value":30}]}]})", r);
    recipe::Runner run;
    recipe::RoastClock clock;
    TEST_ASSERT_TRUE(run.load(r));
    TEST_ASSERT_TRUE(run.start());
    recipe::Output o;
    run.tick(at(0, 190, 150, 20), clock, o); // all false: armed
    TEST_ASSERT_EQUAL(0, o.count);
    o = recipe::Output();
    run.tick(at(1000, 199.9f, 100.1f, 11.1f), clock, o);
    TEST_ASSERT_EQUAL(0, o.count);
    o = recipe::Output();
    run.tick(at(2000, 200, 150, 11), clock, o); // gte at the edge, near at the tolerance
    TEST_ASSERT_TRUE(has(o, Command::Kind::SetFan, 10));
    TEST_ASSERT_TRUE(has(o, Command::Kind::SetFan, 30));
    TEST_ASSERT_FALSE(has(o, Command::Kind::SetFan, 20));
    o = recipe::Output();
    run.tick(at(3000, 250, 100, 10), clock, o); // lte at the edge; the others fired once only
    TEST_ASSERT_EQUAL(1, o.count);
    TEST_ASSERT_TRUE(has(o, Command::Kind::SetFan, 20));
    TEST_ASSERT_TRUE(run.state() == recipe::State::Done);
    TEST_ASSERT_EQUAL(3, run.firedCount());
}

void test_recipe_edge_and_order() {
    recipe::Recipe r;
    parseRecipe(R"({"steps":[
        {"trigger":{"type":"bt","value":150},"actions":[{"type":"power","value":80}]},
        {"trigger":{"type":"bt","value":160},"actions":[{"type":"fan","value":50}]},
        {"trigger":{"type":"bt","value":300},"actions":[{"type":"end"}]}]})", r);
    recipe::Runner run;
    recipe::RoastClock clock;
    run.load(r);
    run.start();
    recipe::Output o;
    run.tick(at(0, 170), clock, o); // already true at start: steps 0 and 1 are not armed yet
    TEST_ASSERT_EQUAL(0, o.count);
    run.tick(at(500, NAN), clock, o); // no reading does not count as false
    TEST_ASSERT_EQUAL(0, o.count);
    run.tick(at(1000, 155), clock, o); // step 0 true, step 1 seen false
    TEST_ASSERT_EQUAL(0, o.count);
    run.tick(at(2000, 140), clock, o); // both false now
    TEST_ASSERT_EQUAL(0, run.firedCount());
    TEST_ASSERT_EQUAL(0, run.next());
    run.tick(at(3000, 165), clock, o); // step 0 fires, then step 1 in the same tick, in order
    TEST_ASSERT_EQUAL(2, run.firedCount());
    TEST_ASSERT_EQUAL(0, run.fired(0).step);
    TEST_ASSERT_EQUAL(1, run.fired(1).step);
    TEST_ASSERT_TRUE(o.commands[0].kind == Command::Kind::SetMode && o.commands[0].a == static_cast<float>(Mode::Manual));
    TEST_ASSERT_TRUE(has(o, Command::Kind::SetHeater, 80));
    TEST_ASSERT_TRUE(o.commands[2].kind == Command::Kind::SetFan);
    TEST_ASSERT_TRUE(std::isnan(run.fired(0).atS)); // before CHARGE
    TEST_ASSERT_EQUAL(2, run.next());

    // a later step does not fire before the earlier one
    parseRecipe(R"({"steps":[
        {"trigger":{"type":"event","value":"FC_START"},"actions":[{"type":"fan","value":70}]},
        {"trigger":{"type":"bt","value":100},"actions":[{"type":"fan","value":40}]}]})", r);
    recipe::Runner seq;
    seq.load(r);
    seq.start();
    o = recipe::Output();
    seq.tick(at(0, 90), clock, o);
    seq.tick(at(1000, 120), clock, o);
    TEST_ASSERT_EQUAL(0, seq.firedCount());
    TEST_ASSERT_TRUE(seq.skipNext(clock, 1000)); // the user skips the event step
    TEST_ASSERT_TRUE(seq.fired(0).outcome == recipe::Outcome::Skipped);
    seq.tick(at(2000, 120), clock, o); // step 1 was seen false at 90 C, so it fires now
    TEST_ASSERT_TRUE(has(o, Command::Kind::SetFan, 40));
    TEST_ASSERT_FALSE(has(o, Command::Kind::SetFan, 70));
}

void test_recipe_time_and_events() {
    recipe::Recipe r;
    parseRecipe(R"({"steps":[
        {"trigger":{"type":"event","value":"DRY_END"},"actions":[{"type":"power","value":70}]},
        {"trigger":{"type":"time","value":60},"actions":[{"type":"fan","value":45}]}]})", r);
    recipe::Runner run;
    recipe::RoastClock clock;
    run.load(r);
    run.start();
    recipe::Output o;
    run.tick(at(1000), clock, o); // event not seen, time false before CHARGE
    clock.mark(recipe::Event::Charge, 5000);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 2, clock.sinceCharge(7000));
    run.tick(at(6000), clock, o);
    clock.mark(recipe::Event::DryEnd, 40000);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 35, clock.at(recipe::Event::DryEnd));
    run.tick(at(41000), clock, o);
    TEST_ASSERT_EQUAL(1, run.firedCount());
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 36, run.fired(0).atS);
    run.tick(at(64000), clock, o); // 59 s
    TEST_ASSERT_EQUAL(1, run.firedCount());
    run.tick(at(65000), clock, o); // 60 s
    TEST_ASSERT_EQUAL(2, run.firedCount());
    TEST_ASSERT_TRUE(has(o, Command::Kind::SetFan, 45));
    TEST_ASSERT_TRUE(run.state() == recipe::State::Done);
    // CHARGE starts a new roast: earlier events are cleared; undo CHARGE stops the clock
    clock.mark(recipe::Event::Charge, 70000);
    TEST_ASSERT_FALSE(clock.seen(recipe::Event::DryEnd));
    clock.undo(recipe::Event::Charge);
    TEST_ASSERT_FALSE(clock.charged());
    TEST_ASSERT_TRUE(std::isnan(clock.sinceCharge(80000)));
}

void test_recipe_setpoint_ramp_and_mixing() {
    recipe::Recipe r;
    parseRecipe(R"({"steps":[
        {"trigger":{"type":"bt","value":100},"actions":[{"type":"setpoint","value":250,"ramp":10}]},
        {"trigger":{"type":"bt","value":200},"actions":[{"type":"power","value":40}]}]})", r);
    recipe::Runner run;
    recipe::RoastClock clock;
    run.load(r);
    run.start();
    recipe::Output o;
    run.tick(at(0, 90), clock, o);
    run.tick(at(1000, 110), clock, o); // ramp from BT 110 to 250 over 10 s
    TEST_ASSERT_TRUE(o.commands[0].kind == Command::Kind::SetMode && o.commands[0].a == static_cast<float>(Mode::Pid));
    TEST_ASSERT_TRUE(has(o, Command::Kind::SetSetpoint, 110));
    TEST_ASSERT_TRUE(run.followingSetpoint());
    TEST_ASSERT_TRUE(run.ramping());
    o = recipe::Output();
    run.tick(at(1200, 120), clock, o); // under 500 ms: nothing sent
    TEST_ASSERT_EQUAL(0, o.count);
    run.tick(at(6000, 150), clock, o); // half way
    TEST_ASSERT_TRUE(has(o, Command::Kind::SetSetpoint, 180));
    o = recipe::Output();
    run.tick(at(20000, 190), clock, o); // ramp ends at the target
    TEST_ASSERT_TRUE(has(o, Command::Kind::SetSetpoint, 250));
    TEST_ASSERT_FALSE(run.ramping());
    o = recipe::Output();
    run.tick(at(21000, 205), clock, o); // a power step switches back to Manual
    TEST_ASSERT_TRUE(o.commands[0].kind == Command::Kind::SetMode && o.commands[0].a == static_cast<float>(Mode::Manual));
    TEST_ASSERT_TRUE(has(o, Command::Kind::SetHeater, 40));
    TEST_ASSERT_FALSE(run.followingSetpoint());
}

// getRecipe status strings must be owned by the document. ArduinoJson 7 stores
// a const char[N] array by pointer, so a local unit buffer went stale before
// serialization and sent stray bytes (the "unit":"t??????" bench fault).
void test_recipe_status_strings_owned() {
    // The old pattern: the document aliases the caller's array.
    char buf[2] = {'F', '\0'};
    JsonDocument alias;
    alias["unit"] = static_cast<const char(&)[2]>(buf);
    TEST_ASSERT_TRUE(alias["unit"].as<JsonString>().isStatic());
    buf[0] = 't';
    TEST_ASSERT_EQUAL_STRING("t", alias["unit"].as<const char*>());

    TEST_ASSERT_EQUAL_STRING("C", recipe::unitName('C'));
    TEST_ASSERT_EQUAL_STRING("F", recipe::unitName('F'));
    TEST_ASSERT_EQUAL_STRING("K", recipe::unitName('K'));
    TEST_ASSERT_NULL(recipe::unitName('t'));
    TEST_ASSERT_NULL(recipe::unitName('f'));
    TEST_ASSERT_NULL(recipe::unitName('\0'));

    recipe::Recipe r;
    const char* guided = R"({"mode":"guided","steps":[
        {"trigger":{"type":"bt","value":300},"actions":[{"type":"fan","value":50}]}]})";
    TEST_ASSERT_NOT_EQUAL(0, std::strlen(parseRecipe(guided, r, 't'))); // unknown unit refused
    TEST_ASSERT_EQUAL_STRING("", parseRecipe(guided, r, 'F'));
    recipe::Runner run;
    recipe::RoastClock clock;
    TEST_ASSERT_TRUE(run.load(r));

    JsonDocument doc;
    {
        char name[16] = "Test Recipe 2";
        char unit = 'F';
        recipe::writeStatus(doc.to<JsonObject>(), run, clock, 0, name, unit);
        std::memset(name, '?', sizeof(name) - 1); // caller storage reused before the reply is sent
        unit = 't';
    }
    std::string out;
    serializeJson(doc, out);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, out.find(R"("name":"Test Recipe 2")"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, out.find(R"("unit":"F")"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, out.find(R"("mode":"guided")"));
    TEST_ASSERT_EQUAL(std::string::npos, out.find('?'));

    // A loaded recipe does not command anything until started.
    TEST_ASSERT_TRUE(run.state() == recipe::State::Ready);
    recipe::Output o;
    run.tick(at(1000, 200), clock, o);
    TEST_ASSERT_EQUAL(0, o.count);

    // Owned name copy: terminated, cut at a UTF-8 boundary.
    char small[4];
    recipe::copyText(small, sizeof(small), "ab\xC3\xA9"); // "abé" needs 5 bytes
    TEST_ASSERT_EQUAL_STRING("ab", small);
    recipe::copyText(small, sizeof(small), nullptr);
    TEST_ASSERT_EQUAL_STRING("", small);
    recipe::copyText(small, sizeof(small), "abc");
    TEST_ASSERT_EQUAL_STRING("abc", small);
}

void test_recipe_guided_and_aborts() {
    recipe::Recipe r;
    parseRecipe(R"({"mode":"guided","steps":[
        {"trigger":{"type":"bt","value":100},"actions":[{"type":"fan","value":50}]},
        {"trigger":{"type":"bt","value":120},"actions":[{"type":"cool"}]}]})", r);
    recipe::Runner run;
    recipe::RoastClock clock;
    run.load(r);
    run.start();
    TEST_ASSERT_FALSE(run.load(r)); // not while running
    recipe::Output o;
    run.tick(at(0, 90), clock, o);
    run.tick(at(1000, 105), clock, o); // fires as a prompt: nothing applied
    TEST_ASSERT_EQUAL(0, o.count);
    TEST_ASSERT_EQUAL(0, run.prompt());
    TEST_ASSERT_FALSE(run.confirm(1, at(1000), o));
    TEST_ASSERT_TRUE(run.confirm(0, at(1500), o));
    TEST_ASSERT_TRUE(has(o, Command::Kind::SetFan, 50));
    TEST_ASSERT_TRUE(run.fired(0).outcome == recipe::Outcome::Confirmed);
    o = recipe::Output();
    run.tick(at(2000, 125), clock, o); // cool prompt
    TEST_ASSERT_TRUE(run.dismiss(1));
    TEST_ASSERT_TRUE(run.fired(1).outcome == recipe::Outcome::Skipped);
    TEST_ASSERT_FALSE(o.stop);
    run.tick(at(3000, 125), clock, o);
    TEST_ASSERT_TRUE(run.state() == recipe::State::Done);

    // auto cool runs the stop path and ends the recipe
    parseRecipe(R"({"steps":[{"trigger":{"type":"bt","value":100},"actions":[{"type":"cool"}]},
        {"trigger":{"type":"bt","value":110},"actions":[{"type":"fan","value":1}]}]})", r);
    run.load(r);
    run.start();
    o = recipe::Output();
    run.tick(at(0, 90), clock, o);
    run.tick(at(1000, 120), clock, o);
    TEST_ASSERT_TRUE(o.stop);
    TEST_ASSERT_FALSE(has(o, Command::Kind::SetFan, 1));
    TEST_ASSERT_TRUE(run.state() == recipe::State::Done);
    TEST_ASSERT_EQUAL_STRING("cool", run.reason());

    // a safety trip aborts; nothing fires after
    run.start();
    o = recipe::Output();
    run.tick(at(0, 90), clock, o);
    recipe::Inputs trip = at(1000, 120);
    trip.trip = SafetyTrip::CommsLost;
    run.tick(trip, clock, o);
    TEST_ASSERT_TRUE(run.state() == recipe::State::Aborted);
    TEST_ASSERT_EQUAL_STRING("comms", run.reason());
    TEST_ASSERT_EQUAL(0, o.count);
    run.tick(at(2000, 120), clock, o);
    TEST_ASSERT_EQUAL(0, o.count);
    run.start();
    run.abort("stop");
    TEST_ASSERT_EQUAL_STRING("stop", run.reason());
}

void test_library_recipe_profiles() {
    using proflib::Error;
    const char* rec = R"({"version":1,"name":"Recipe only","unit":"F","recipe":{"steps":[)"
                      R"({"trigger":{"type":"bt","value":389},"actions":[{"type":"fan","value":50},{"type":"power","value":60}]}]}})";
    TEST_ASSERT_EQUAL(Error::None, validateText(rec));
    TEST_ASSERT_EQUAL(Error::BadSteps, validateText(R"({"version":1,"name":"x","unit":"C"})"));
    const char* why = "";
    JsonDocument d;
    deserializeJson(d, R"({"version":1,"name":"x","unit":"C","recipe":{"steps":[{"trigger":{"type":"bt","value":999},"actions":[{"type":"end"}]}]}})");
    TEST_ASSERT_EQUAL(Error::BadRecipe, proflib::validate(d.as<JsonObjectConst>(), &why));
    TEST_ASSERT_EQUAL_STRING("temperature trigger must be 0..350 C", why);
    TEST_ASSERT_EQUAL(Error::None, validateText(withField(R"({"recipe":{"mode":"guided","steps":[{"trigger":{"type":"time","value":30},"actions":[{"type":"note","text":"x"}]}]}})")));

    FakeStorage fs;
    fs.files["r.json"] = rec;
    fs.files["both.json"] = withField(R"({"recipe":{"steps":[{"trigger":{"type":"time","value":30},"actions":[{"type":"fan","value":40}]}]}})");
    proflib::Library lib(fs);
    lib.begin();
    TEST_ASSERT_EQUAL(1, lib.list()[1].recipeSteps); // sorted: "City roast", "Recipe only"
    TEST_ASSERT_EQUAL(0, lib.list()[1].steps);
    RoastProfile sched;
    std::unique_ptr<recipe::Recipe> r(new recipe::Recipe());
    proflib::Loaded what;
    std::string text;
    TEST_ASSERT_EQUAL(Error::None, lib.load("r", sched, *r, what, &text));
    TEST_ASSERT_TRUE(what.recipe);
    TEST_ASSERT_FALSE(what.schedule);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 198.33f, r->steps[0].trigger.value);
    TEST_ASSERT_EQUAL('{', text[0]);
    TEST_ASSERT_EQUAL(Error::BadSteps, lib.load("r", sched)); // nothing to schedule
    TEST_ASSERT_EQUAL(Error::None, lib.load("both", sched, *r, what, &text));
    TEST_ASSERT_TRUE(what.recipe && what.schedule);
    TEST_ASSERT_EQUAL(2, sched.size());
}

void runAutotuneTests();
void runFanTests();
void runWebSocketTests();
void runDummyTests();
void runRampShapeTests();
void runRecordTests();
void runAuxFanTests();

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_moving_average_seeds_with_first_sample);
    RUN_TEST(test_max31855_decode);
    RUN_TEST(test_max31855_fault_bits);
    RUN_TEST(test_board_temperature_skips_missing_chip);
    RUN_TEST(test_probe_channels_independent_faults);
    RUN_TEST(test_max6675_decode);
    RUN_TEST(test_time_proportional_window);
    RUN_TEST(test_pid_integrator_does_not_wind_up);
    RUN_TEST(test_pid_bumpless_reset);
    RUN_TEST(test_leaving_autotune_starts_from_zero);
    RUN_TEST(test_relay_autotune_identifies_and_controls);
    RUN_TEST(test_legacy_autotune_math_is_plant_independent);
    RUN_TEST(test_profile_matches_readme_example);
    RUN_TEST(test_profile_in_fahrenheit);
    RUN_TEST(test_protocol_values);
    RUN_TEST(test_protocol_watchdog);
    RUN_TEST(test_protocol_cooldown_fan);
    RUN_TEST(test_protocol_cooldown_end);
    RUN_TEST(test_cooldown_ends_below_et);
    RUN_TEST(test_protocol_only_key);
    RUN_TEST(test_protocol_profile);
    RUN_TEST(test_protocol_profile_from_page);
    RUN_TEST(test_protocol_profile_rejects_malformed);
    RUN_TEST(test_profile_failed_load_keeps_previous);
    RUN_TEST(test_protocol_basic_and_data);
    RUN_TEST(test_protocol_yaeger_compat);
    RUN_TEST(test_autotune_status_text);
    RUN_TEST(test_unit_conversion);
    RUN_TEST(test_correction_shifts_pid_input);
    RUN_TEST(test_correction_in_fahrenheit);
    RUN_TEST(test_correction_keeps_nan);
    RUN_TEST(test_overtemp_uses_raw_and_corrected);
    RUN_TEST(test_snapshot_stays_uncorrected);
    RUN_TEST(test_pv_step_stops_autotune_and_resets_pid);
    RUN_TEST(test_sensor_fault_rules);
    RUN_TEST(test_zero_cross_age);
    RUN_TEST(test_output_command_stops_autotune);
    RUN_TEST(test_autotune_required_gate);
    RUN_TEST(test_mains_dropout_under_grace_does_not_trip);
    RUN_TEST(test_mains_absent_past_grace_trips);
    RUN_TEST(test_mains_heater_off_resets_grace);
    RUN_TEST(test_temp_unit_boot_uses_saved_or_default);
    RUN_TEST(test_temp_unit_saves_only_changes);
    RUN_TEST(test_library_validate_format);
    RUN_TEST(test_library_ids_and_busy);
    RUN_TEST(test_library_save_get_delete);
    RUN_TEST(test_library_limits);
    RUN_TEST(test_library_failed_write_keeps_old_file);
    RUN_TEST(test_library_begin_recovers_and_indexes);
    RUN_TEST(test_library_load_converts_unit);
    RUN_TEST(test_recipe_parse_limits);
    RUN_TEST(test_recipe_comparators);
    RUN_TEST(test_recipe_edge_and_order);
    RUN_TEST(test_recipe_time_and_events);
    RUN_TEST(test_recipe_setpoint_ramp_and_mixing);
    RUN_TEST(test_recipe_guided_and_aborts);
    RUN_TEST(test_recipe_status_strings_owned);
    RUN_TEST(test_library_recipe_profiles);
    runAutotuneTests();  // test_autotune.cpp
    runFanTests();       // test_autotune.cpp
    runWebSocketTests(); // test_websocket.cpp
    runDummyTests();     // test_dummy.cpp
    runRampShapeTests(); // test_ramp_shape.cpp
    runRecordTests();    // test_records.cpp
    runAuxFanTests();    // test_aux_fans.cpp
    return UNITY_END();
}
