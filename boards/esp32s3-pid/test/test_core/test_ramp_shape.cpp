// Recipe ramp shapes, heater ramps and the charge temperature (core/recipe.h).
#include <ArduinoJson.h>
#include <unity.h>

#include <cmath>
#include <cstdio>

#include "core/pid.h"
#include "core/recipe.h"

namespace {
const char* parse(const char* text, recipe::Recipe& r, char unit = 'C') {
    JsonDocument d;
    deserializeJson(d, text);
    return recipe::parse(d.as<JsonObjectConst>(), unit, r);
}
recipe::Inputs at(uint32_t ms, float bt, float heater = 20) {
    recipe::Inputs in;
    in.nowMs = ms;
    in.btC = bt;
    in.setpointC = 150;
    in.heaterPct = heater;
    return in;
}
// Value of the last command of kind `k`; NaN when none.
float last(const recipe::Output& o, Command::Kind k) {
    float v = NAN;
    for (size_t i = 0; i < o.count; i++)
        if (o.commands[i].kind == k)
            v = o.commands[i].a;
    return v;
}
// The last command of kind `k`; kind Stop with a = NaN when none.
Command lastCommand(const recipe::Output& o, Command::Kind k) {
    Command c{Command::Kind::Stop};
    c.a = NAN;
    for (size_t i = 0; i < o.count; i++)
        if (o.commands[i].kind == k)
            c = o.commands[i];
    return c;
}
recipe::Runner& started(recipe::Runner& run, const char* text) {
    static recipe::Recipe r;
    TEST_ASSERT_EQUAL_STRING("", parse(text, r));
    run.load(r);
    run.start();
    return run;
}
} // namespace

void test_shape_fraction() {
    using recipe::Shape;
    const uint8_t* lin = recipe::shapeRates(Shape::Linear);
    const uint8_t* front = recipe::shapeRates(Shape::Front);
    const uint8_t* back = recipe::shapeRates(Shape::Back);
    const uint8_t* normal = recipe::shapeRates(Shape::Normal);
    TEST_ASSERT_NULL(recipe::shapeRates(Shape::Custom));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.5f, recipe::rampFraction(lin, 0.5f));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.75f, recipe::rampFraction(front, 0.5f)); // 2u - u^2
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.1f * 2 - 0.01f, recipe::rampFraction(front, 0.1f));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.25f, recipe::rampFraction(back, 0.5f)); // u^2
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.5f, recipe::rampFraction(normal, 0.5f));
    TEST_ASSERT_TRUE(recipe::rampFraction(normal, 0.2f) < 0.2f); // slow start
    TEST_ASSERT_EQUAL_FLOAT(0, recipe::rampFraction(front, -1));
    TEST_ASSERT_EQUAL_FLOAT(1, recipe::rampFraction(front, 2));
    float prev = 0;
    for (int i = 1; i <= 100; i++) { // never goes back
        const float f = recipe::rampFraction(normal, i / 100.0f);
        TEST_ASSERT_TRUE(f >= prev);
        prev = f;
    }
    float u = -1;
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1, recipe::peakFactor(lin, &u));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 2, recipe::peakFactor(front, &u));
    TEST_ASSERT_EQUAL_FLOAT(0, u);
    recipe::peakFactor(back, &u);
    TEST_ASSERT_EQUAL_FLOAT(1, u);
    const float pn = recipe::peakFactor(normal, &u);
    TEST_ASSERT_EQUAL_FLOAT(0.5f, u);
    TEST_ASSERT_TRUE(pn > 1.9f && pn < 2.1f);
}

void test_shape_parse() {
    recipe::Recipe r;
    const char* curve21 = "[0,5,10,15,20,25,30,35,40,45,50,55,60,65,70,75,80,85,90,95,100]";
    char text[512];
    TEST_ASSERT_EQUAL_STRING("shape must be linear, normal, front, back or custom",
                             parse(R"({"steps":[{"trigger":{"type":"bt","value":100},"actions":[{"type":"setpoint","value":200,"ramp":60,"shape":"s"}]}]})", r));
    TEST_ASSERT_EQUAL_STRING("curve must have 21 rates",
                             parse(R"({"steps":[{"trigger":{"type":"bt","value":100},"actions":[{"type":"setpoint","value":200,"ramp":60,"shape":"custom"}]}]})", r));
    TEST_ASSERT_EQUAL_STRING("curve rates must be 0..100",
                             parse(R"({"steps":[{"trigger":{"type":"bt","value":100},"actions":[{"type":"power","value":60,"ramp":60,"shape":"custom","curve":[0,5,10,15,20,25,30,35,40,45,50,55,60,65,70,75,80,85,90,95,101]}]}]})", r));
    TEST_ASSERT_EQUAL_STRING("curve needs a rate above 0",
                             parse(R"({"steps":[{"trigger":{"type":"bt","value":100},"actions":[{"type":"power","value":60,"ramp":60,"shape":"custom","curve":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]}]}]})", r));
    snprintf(text, sizeof text, R"({"steps":[{"trigger":{"type":"bt","value":100},"actions":[{"type":"setpoint","value":200,"curve":%s}]}]})", curve21);
    TEST_ASSERT_EQUAL_STRING("curve needs shape custom", parse(text, r));
    TEST_ASSERT_EQUAL_STRING("fan does not ramp",
                             parse(R"({"steps":[{"trigger":{"type":"bt","value":100},"actions":[{"type":"fan","value":60,"ramp":30}]}]})", r));
    TEST_ASSERT_EQUAL_STRING("chargeTemp must be 0..350 C",
                             parse(R"({"chargeTemp":700,"steps":[{"trigger":{"type":"bt","value":100},"actions":[{"type":"fan","value":60}]}]})", r, 'F'));
    TEST_ASSERT_EQUAL_STRING("", parse(R"({"chargeTemp":392,"steps":[{"trigger":{"type":"bt","value":300},"actions":[{"type":"fan","value":60}]}]})", r, 'F'));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 200, r.chargeC);
    TEST_ASSERT_EQUAL_STRING("", parse(R"({"steps":[{"trigger":{"type":"bt","value":100},"actions":[{"type":"fan","value":60}]}]})", r));
    TEST_ASSERT_TRUE(std::isnan(r.chargeC));

    // Up to 8 custom curves; a ninth is refused.
    JsonDocument d;
    JsonArray steps = d["steps"].to<JsonArray>();
    for (int i = 0; i < 9; i++) {
        JsonObject s = steps.add<JsonObject>();
        s["trigger"]["type"] = "bt";
        s["trigger"]["value"] = 100 + i;
        JsonObject a = s["actions"].add<JsonObject>();
        a["type"] = "setpoint";
        a["value"] = 200;
        a["ramp"] = 60;
        a["shape"] = "custom";
        JsonArray c = a["curve"].to<JsonArray>();
        for (int k = 0; k < 21; k++)
            c.add(k == i ? 100 : 10);
    }
    TEST_ASSERT_EQUAL_STRING("a recipe has up to 8 custom curves", recipe::parse(d.as<JsonObjectConst>(), 'C', r));
    steps.remove(8);
    TEST_ASSERT_EQUAL_STRING("", recipe::parse(d.as<JsonObjectConst>(), 'C', r));
    TEST_ASSERT_EQUAL(8, r.curveCount);
    TEST_ASSERT_EQUAL(100, r.rates(r.steps[3].actions[0])[3]); // each action keeps its own curve
    TEST_ASSERT_EQUAL(10, r.rates(r.steps[3].actions[0])[4]);
}

void test_shaped_setpoint_ramp() {
    recipe::Recipe r;
    TEST_ASSERT_EQUAL_STRING("", parse(R"({"steps":[
        {"trigger":{"type":"bt","value":100},"actions":[{"type":"setpoint","value":250,"ramp":10,"shape":"front"}]}]})", r));
    recipe::Runner run;
    recipe::RoastClock clock;
    run.load(r);
    run.start();
    recipe::Output o;
    run.tick(at(0, 90), clock, o);
    run.tick(at(1000, 110), clock, o); // from BT 110, not the old setpoint 150
    TEST_ASSERT_EQUAL_FLOAT(110, last(o, Command::Kind::SetSetpoint));
    o = recipe::Output();
    // Half the time, three quarters of the 110 -> 250 climb: the shape spans
    // the whole BT-to-target change.
    run.tick(at(6000, 150), clock, o);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 110 + 140 * 0.75f, last(o, Command::Kind::SetSetpoint));

    JsonDocument d;
    recipe::writeStatus(d.to<JsonObject>(), run, clock, 6000, "x", 'C');
    TEST_ASSERT_EQUAL_STRING("setpoint", d["ramp"]["kind"]);
    TEST_ASSERT_EQUAL_FLOAT(110, d["ramp"]["from"].as<float>());
    TEST_ASSERT_EQUAL_FLOAT(250, d["ramp"]["to"].as<float>());
    TEST_ASSERT_EQUAL_FLOAT(10, d["ramp"]["duration"].as<float>());
    TEST_ASSERT_EQUAL_FLOAT(5, d["ramp"]["elapsed"].as<float>());
    TEST_ASSERT_EQUAL(21, d["ramp"]["rates"].size());
    TEST_ASSERT_EQUAL(100, d["ramp"]["rates"][0].as<int>());

    o = recipe::Output();
    run.tick(at(11000, 240), clock, o);
    TEST_ASSERT_EQUAL_FLOAT(250, last(o, Command::Kind::SetSetpoint));
    TEST_ASSERT_FALSE(run.ramping());
    d.clear();
    recipe::writeStatus(d.to<JsonObject>(), run, clock, 11000, "x", 'C');
    TEST_ASSERT_TRUE(d["ramp"].isNull());
}

void test_power_ramp_and_hold() {
    recipe::Recipe r;
    TEST_ASSERT_EQUAL_STRING("", parse(R"({"steps":[
        {"trigger":{"type":"bt","value":100},"actions":[{"type":"power","value":80,"ramp":10,"shape":"back"}]},
        {"trigger":{"type":"bt","value":200},"actions":[{"type":"hold"}]}]})", r));
    recipe::Runner run;
    recipe::RoastClock clock;
    run.load(r);
    run.start();
    recipe::Output o;
    run.tick(at(0, 90), clock, o);
    run.tick(at(1000, 110, 20), clock, o); // from the heater's current 20 %
    TEST_ASSERT_EQUAL_FLOAT(static_cast<float>(Mode::Manual), last(o, Command::Kind::SetMode));
    TEST_ASSERT_EQUAL_FLOAT(20, last(o, Command::Kind::SetHeater));
    TEST_ASSERT_FALSE(run.followingSetpoint());
    o = recipe::Output();
    run.tick(at(6000, 150), clock, o); // half the time, a quarter of the change
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 35, last(o, Command::Kind::SetHeater));
    TEST_ASSERT_TRUE(run.ramping());
    JsonDocument d;
    recipe::writeStatus(d.to<JsonObject>(), run, clock, 6000, "x", 'C');
    TEST_ASSERT_EQUAL_STRING("power", d["ramp"]["kind"]);
    o = recipe::Output();
    run.tick(at(7000, 205), clock, o); // hold: the heater stays where the ramp left it
    TEST_ASSERT_FALSE(run.ramping());
    TEST_ASSERT_TRUE(std::isnan(last(o, Command::Kind::SetHeater)));
    TEST_ASSERT_TRUE(run.state() == recipe::State::Done);
}

void test_charge_temp_preheat() {
    recipe::Recipe r;
    TEST_ASSERT_EQUAL_STRING("", parse(R"({"chargeTemp":200,"steps":[
        {"trigger":{"type":"event","value":"CHARGE"},"actions":[{"type":"power","value":60}]}]})", r));
    recipe::Runner run;
    recipe::RoastClock clock;
    run.load(r);
    run.start();
    recipe::Output o;
    run.tick(at(0, 30), clock, o);
    TEST_ASSERT_EQUAL_FLOAT(static_cast<float>(Mode::Pid), last(o, Command::Kind::SetMode));
    TEST_ASSERT_EQUAL_FLOAT(200, last(o, Command::Kind::SetSetpoint));
    TEST_ASSERT_FALSE(run.followingSetpoint()); // preheat is not a schedule to fall behind
    JsonDocument d;
    recipe::writeStatus(d.to<JsonObject>(), run, clock, 0, "x", 'F');
    TEST_ASSERT_EQUAL_FLOAT(392, d["chargeTemp"].as<float>());
    o = recipe::Output();
    run.tick(at(500, 40), clock, o); // once
    TEST_ASSERT_EQUAL(0, o.count);
    clock.mark(recipe::Event::Charge, 1000);
    run.tick(at(1000, 190), clock, o);
    TEST_ASSERT_EQUAL_FLOAT(60, last(o, Command::Kind::SetHeater));

    // Started after CHARGE: no preheat.
    run.load(r);
    run.start();
    o = recipe::Output();
    run.tick(at(2000, 190), clock, o);
    TEST_ASSERT_TRUE(std::isnan(last(o, Command::Kind::SetSetpoint)));
}

// After Manual power steps the controller setpoint is stale (0 here). The
// ramp starts at BT and asks for a bumpless PID restart (b = 1) after the
// switch to PID.
void test_setpoint_ramp_from_manual() {
    recipe::Runner run;
    recipe::RoastClock clock;
    started(run, R"({"steps":[
        {"trigger":{"type":"bt","value":60},"actions":[{"type":"power","value":100},{"type":"fan","value":30}]},
        {"trigger":{"type":"bt","value":150},"actions":[{"type":"setpoint","value":200,"ramp":360,"shape":"front"}]}]})");
    recipe::Output o;
    recipe::Inputs in = at(0, 50);
    in.setpointC = 0;
    run.tick(in, clock, o);
    in = at(1000, 70, 100);
    in.setpointC = 0;
    run.tick(in, clock, o);
    TEST_ASSERT_EQUAL_FLOAT(100, last(o, Command::Kind::SetHeater));
    o = recipe::Output();
    in = at(2000, 151, 100);
    in.setpointC = 0;
    run.tick(in, clock, o);
    TEST_ASSERT_TRUE(o.commands[0].kind == Command::Kind::SetMode &&
                     o.commands[0].a == static_cast<float>(Mode::Pid));
    const Command c = lastCommand(o, Command::Kind::SetSetpoint);
    TEST_ASSERT_EQUAL_FLOAT(151, c.a);
    TEST_ASSERT_EQUAL_FLOAT(1, c.b);
    TEST_ASSERT_TRUE(run.ramping());
    o = recipe::Output();
    in = at(2000 + 180000, 180, 100); // half the time: three quarters of 151 -> 200
    run.tick(in, clock, o);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 151 + 49 * 0.75f, last(o, Command::Kind::SetSetpoint));
    TEST_ASSERT_EQUAL_FLOAT(0, lastCommand(o, Command::Kind::SetSetpoint).b); // only the start restarts
}

// BT already at or above the target: no ramp (ramps never go down), the step
// holds the target as a plain setpoint step would.
void test_setpoint_ramp_bt_above_target() {
    recipe::Runner run;
    recipe::RoastClock clock;
    started(run, R"({"steps":[
        {"trigger":{"type":"bt","value":150},"actions":[{"type":"setpoint","value":200,"ramp":60,"shape":"front"}]}]})");
    recipe::Output o;
    run.tick(at(0, 140), clock, o);
    run.tick(at(1000, 205), clock, o);
    const Command c = lastCommand(o, Command::Kind::SetSetpoint);
    TEST_ASSERT_EQUAL_FLOAT(200, c.a);
    TEST_ASSERT_EQUAL_FLOAT(0, c.b);
    TEST_ASSERT_FALSE(run.ramping());
    TEST_ASSERT_TRUE(run.followingSetpoint());
    TEST_ASSERT_TRUE(std::isnan(recipe::rampStartC(at(0, 200), 200))); // equal: nothing to climb
}

// No PID input reading: start at the current setpoint when it is below the
// target, else go straight to the target. ET or MAX as the PID input pick
// that probe.
void test_setpoint_ramp_start_fallbacks() {
    recipe::Inputs in = at(0, NAN);
    in.setpointC = 150;
    TEST_ASSERT_EQUAL_FLOAT(150, recipe::rampStartC(in, 200));
    in.setpointC = 0;
    TEST_ASSERT_TRUE(std::isnan(recipe::rampStartC(in, 200)));
    in.setpointC = 210;
    TEST_ASSERT_TRUE(std::isnan(recipe::rampStartC(in, 200)));
    in.setpointC = NAN;
    TEST_ASSERT_TRUE(std::isnan(recipe::rampStartC(in, 200)));
    in = at(0, 120);
    in.etC = 180;
    in.pidInput = PidInput::ET;
    TEST_ASSERT_EQUAL_FLOAT(180, recipe::rampStartC(in, 200));
    in.pidInput = PidInput::MAX;
    TEST_ASSERT_EQUAL_FLOAT(180, recipe::rampStartC(in, 200));
    in.etC = NAN; // MAX needs both probes
    in.setpointC = 160;
    TEST_ASSERT_EQUAL_FLOAT(160, recipe::rampStartC(in, 200));

    // In a running recipe: BT unreadable, setpoint 0, time trigger. The step
    // sends the target at once and does not ramp.
    recipe::Runner run;
    recipe::RoastClock clock;
    started(run, R"({"steps":[
        {"trigger":{"type":"time","value":10},"actions":[{"type":"setpoint","value":200,"ramp":60}]}]})");
    clock.mark(recipe::Event::Charge, 0);
    recipe::Output o;
    in = at(5000, NAN);
    in.setpointC = 0;
    run.tick(in, clock, o);
    in.nowMs = 11000;
    run.tick(in, clock, o);
    TEST_ASSERT_EQUAL_FLOAT(200, last(o, Command::Kind::SetSetpoint));
    TEST_ASSERT_FALSE(run.ramping());
}

// What the controller does with b = 1: Pid::reset at setpoint = input keeps
// the present output, so the switch from Manual 100 % does not drop the heater.
void test_pid_restart_keeps_output() {
    Pid::Config pc;
    Pid pid(pc);
    pid.setGains(PidGains{4, 0.05f, 20});
    pid.reset(151, 151, 100, 12);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 100, pid.update(151, 151, 12));
    pid.reset(151, 151, 70, 12);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 70, pid.update(151.2f, 151, 12)); // only Kp * 0.2 C added
}

void runRampShapeTests() {
    RUN_TEST(test_shape_fraction);
    RUN_TEST(test_shape_parse);
    RUN_TEST(test_shaped_setpoint_ramp);
    RUN_TEST(test_power_ramp_and_hold);
    RUN_TEST(test_charge_temp_preheat);
    RUN_TEST(test_setpoint_ramp_from_manual);
    RUN_TEST(test_setpoint_ramp_bt_above_target);
    RUN_TEST(test_setpoint_ramp_start_fallbacks);
    RUN_TEST(test_pid_restart_keeps_output);
}
