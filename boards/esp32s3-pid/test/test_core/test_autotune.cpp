// Multi-point autotune (core/autotune_sequence.h, autotune_protocol.h) against
// simulated drums. Called from test_main.cpp (runAutotuneTests).
#include <unity.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>
#include <string>

#include <ArduinoJson.h>

#include "core/autotune_protocol.h"
#include "core/autotune_sequence.h"
#include "core/autotune_status.h"
#include "core/control_protocol.h"
#include "core/fan_assist.h"
#include "app/config_store.h"
#include "core/roaster_model.h"
#include "core/pid.h"
#include "core/recipe.h"

using namespace autotune;

namespace {

// First order plus dead time with probe noise and MAX31855 quantisation.
struct FopdtDrum {
    double fan = NAN; // no airflow term
    double T = 25, amb = 25, K = 3.0 /*C per %*/, tau = 240, L = 30, dt = 0.25, noise = 0.2;
    double hist[2048] = {};
    int head = 0;
    std::mt19937 rng{7};
    std::normal_distribution<double> nrm{0, 1};
    FopdtDrum() {
        for (double& h : hist)
            h = amb;
    }
    float step(float u) {
        u = std::min(std::max(u, 0.0f), 100.0f);
        hist[head] = u;
        head = (head + 1) % 2048;
        const int nd = (int)std::lround(L / dt);
        const double ud = hist[(head + 2048 - 1 - nd) % 2048];
        T += (amb + K * ud - T) * dt / tau;
        return (float)(std::round((T + noise * nrm(rng)) * 4) / 4);
    }
    // Ultimate point of K e^{-Ls}/(tau s + 1): phase -pi at wu, Ku = 1/|G(j wu)|.
    void ultimate(double& ku, double& tu) const {
        double lo = 1e-4, hi = 3.14159 / L;
        for (int i = 0; i < 100; i++) {
            const double w = 0.5 * (lo + hi);
            (std::atan(w * tau) + w * L < 3.14159265) ? lo = w : hi = w;
        }
        const double w = 0.5 * (lo + hi);
        ku = std::sqrt(1 + w * tau * w * tau) / K;
        tu = 2 * 3.14159265 / w;
    }
};

// Behmor-like drum: element lag, heat loss rising with temperature (so gain and
// time constant fall as it gets hotter), transport delay and a probe lag.
// Airflow: forced convection adds a loss conductance h_f (f/100)^0.8 (turbulent
// flow, Nu ~ Re^0.8 with the air speed proportional to the fan). With `fan`
// unset (NaN) the drum has no fan term, as before the fan test.
struct Drum {
    double x1 = 0, T = 25, Tp = 25;
    double tau1 = 20, tauP = 8, dead = 10, dt = 0.25, kloss = 0.004, tau150 = 250, noise = 0.25;
    double fan = NAN, hf = 0.0016, fanExp = 0.8;
    double h0, C;
    double hist[800];
    int head = 0, nd;
    std::mt19937 rng{1};
    std::normal_distribution<double> nrm{0, 1};
    Drum() {
        init();
    }
    void init() {
        h0 = 1.0 / (275 * (1 + kloss * 275)); // 100 % holds 300 C
        C = tau150 * h0 * (1 + kloss * 125);
        nd = (int)(dead / dt);
        for (double& h : hist)
            h = 25;
    }
    float step(float u) {
        u = std::min(std::max(u, 0.0f), 100.0f);
        x1 += (u / 100 - x1) * dt / tau1;
        const double hA = h0 * (1 + kloss * (T - 25)) + fanLoss();
        T += (x1 - hA * (T - 25)) / C * dt;
        hist[head] = T;
        head = (head + 1) % 800;
        const double Td = hist[(head + 800 - nd) % 800];
        Tp += (Td - Tp) * dt / tauP;
        return (float)(std::round((Tp + noise * nrm(rng)) * 4) / 4);
    }
    double fanLoss() const {
        return std::isfinite(fan) ? hf * std::pow(std::max(0.0, fan) / 100, fanExp) : 0;
    }
    // Steady heater % that holds `t` at fan `f`.
    double hold(double t, double f) const {
        const double hA = h0 * (1 + kloss * (t - 25)) + (std::isfinite(f) ? hf * std::pow(f / 100, fanExp) : 0);
        return 100 * hA * (t - 25);
    }
};

template <typename Plant>
float runToEnd(Sequence& s, Plant& p, float pv, float maxS = 20000, float* peakOut = nullptr) {
    float t = 0, peak = 0;
    while (s.running() && t < maxS) {
        const float u = s.update(pv);
        TEST_ASSERT_TRUE(u >= 0 && u <= 100);
        if (std::isfinite(s.fanRequest()))
            p.fan = s.fanRequest();
        pv = p.step(u);
        peak = std::max(peak, pv);
        t += 0.25f;
    }
    if (peakOut)
        *peakOut = peak;
    return t;
}

Config onePoint(float c) {
    Config cfg;
    cfg.pointsC[0] = c;
    cfg.pointCount = 1;
    cfg.fanTest = false;
    return cfg;
}

// The PID points only (the fan test has its own tests).
Config pointsOnly() {
    Config c;
    c.fanTest = false;
    return c;
}

void assertOff(Sequence& s, float pv) {
    TEST_ASSERT_FALSE(s.running());
    TEST_ASSERT_EQUAL_FLOAT(0, s.progress().output);
    for (int i = 0; i < 8; i++)
        TEST_ASSERT_EQUAL_FLOAT(0, s.update(pv)); // stays off
}

} // namespace

// The relay finds the ultimate point of a dead-time plant within the
// describing-function error, and the cycles agree.
void test_at_fopdt_ultimate_point() {
    FopdtDrum p;
    Sequence s;
    Config c = onePoint(177);
    s.start(c, 25);
    runToEnd(s, p, 25);
    TEST_ASSERT_EQUAL(Phase::Done, s.phase());
    const PointResult& r = s.point(0);
    TEST_ASSERT_EQUAL(PointStatus::Ok, r.status);
    TEST_ASSERT_TRUE(r.amplitudeCv < c.maxCv && r.periodCv < c.maxCv);
    double ku, tu;
    p.ultimate(ku, tu);
    // The describing function reads Ku low on lag-dominant plants (about 20 %
    // here), which errs on the cautious side; Tu is close.
    TEST_ASSERT_FLOAT_WITHIN(0.3 * ku, ku, r.ku);
    TEST_ASSERT_TRUE(r.ku < ku);
    TEST_ASSERT_FLOAT_WITHIN(0.15 * tu, tu, r.tuS);
    // Static gain from the bias that holds the target: exact for a linear plant
    // up to the asymmetry the bias leaves.
    TEST_ASSERT_FLOAT_WITHIN(0.15 * p.K, p.K, r.model.gainCPerPct);
    TEST_ASSERT_FLOAT_WITHIN(0.5 * p.L, p.L, r.model.deadS);
    TEST_ASSERT_FLOAT_WITHIN(0.5 * p.tau, p.tau, r.model.tauS);
    TEST_ASSERT_EQUAL_FLOAT(0, s.progress().output);
}

// Three points on a nonlinear drum: each converges, the recommendation is no
// more aggressive than any point, and it controls the drum without overshoot.
void test_at_multi_point_run() {
    Drum p;
    Sequence s;
    Config c = pointsOnly();
    s.start(c, 25);
    TEST_ASSERT_EQUAL(3, s.progress().pointCount);
    int lastPoint = 0;
    float pv = 25, t = 0;
    bool sawPhase[8] = {};
    float peakAbove = 0; // highest BT above the current test point
    while (s.running() && t < 20000) {
        const float u = s.update(pv);
        pv = p.step(u);
        t += 0.25f;
        const Progress& g = s.progress();
        sawPhase[(int)g.phase] = true;
        if (s.running())
            peakAbove = std::max(peakAbove, pv - g.targetC);
        TEST_ASSERT_TRUE(g.point >= lastPoint); // points in order
        lastPoint = g.point;
        TEST_ASSERT_TRUE(g.remainingS >= 0);
    }
    TEST_ASSERT_EQUAL(Phase::Done, s.phase());
    TEST_ASSERT_TRUE(sawPhase[(int)Phase::Heating] && sawPhase[(int)Phase::Settling] &&
                     sawPhase[(int)Phase::Oscillating]);
    TEST_ASSERT_TRUE(t < c.totalTimeoutS);
    TEST_ASSERT_TRUE(peakAbove < c.maxOvershootC - 4); // margin to the overshoot stop
    TEST_ASSERT_EQUAL(3, s.pointsUsed());
    float tuMin = 1e9f;
    for (int i = 0; i < 3; i++) {
        const PointResult& r = s.point(i);
        TEST_ASSERT_EQUAL(PointStatus::Ok, r.status);
        TEST_ASSERT_EQUAL_FLOAT(c.pointsC[i], r.targetC);
        TEST_ASSERT_TRUE(r.gains.kp >= s.recommended().kp);
        TEST_ASSERT_TRUE(r.gains.kp / r.gains.ki <= s.recommended().kp / s.recommended().ki + 1e-3f);
        TEST_ASSERT_TRUE(r.bias > 10 && r.bias < 90);
        tuMin = std::min(tuMin, r.tuS);
    }
    // Hotter points need more heat to hold: the bias rises with the point.
    TEST_ASSERT_TRUE(s.point(0).bias < s.point(1).bias && s.point(1).bias < s.point(2).bias);
    TEST_ASSERT_TRUE(tuMin > 60);
    TEST_ASSERT_EQUAL(0, s.recommended().kd); // default rule is PI

    // Closed loop on a fresh drum: step to 177 C, ramp 6 C/min to 210 C, hold.
    Drum q;
    Pid::Config pc;
    Pid pid(pc);
    pid.setGains(s.recommended());
    pv = 25;
    float overshoot = 0, rampErr = 0, holdErr = 0;
    for (float tt = 0; tt < 2400; tt += 0.25f) {
        const float sp = tt < 900 ? 177 : (tt < 1230 ? 177 + (tt - 900) * 0.1f : 210);
        pv = q.step(pid.update(sp, pv));
        if (tt > 100 && tt < 900)
            overshoot = std::max(overshoot, pv - 177);
        if (tt > 900 && tt < 1230)
            rampErr += std::fabs(pv - sp) * 0.25f / 330;
        if (tt > 1500)
            holdErr += std::fabs(pv - 210) * 0.25f / 900;
    }
    TEST_ASSERT_TRUE(overshoot < 4);
    TEST_ASSERT_TRUE(rampErr < 5);
    TEST_ASSERT_TRUE(holdErr < 1);
}

void test_at_abort_sensor_fault() {
    Drum p;
    Sequence s;
    s.start(Config(), 25);
    float pv = 25;
    for (int i = 0; i < 400; i++)
        pv = p.step(s.update(pv));
    TEST_ASSERT_TRUE(s.progress().output > 0);
    s.update(NAN);
    TEST_ASSERT_EQUAL(Phase::Failed, s.phase());
    TEST_ASSERT_EQUAL_STRING("sensor fault", s.failure());
    assertOff(s, pv);
}

void test_at_abort_over_temperature() {
    Sequence s;
    Config c = onePoint(177);
    c.abortAboveC = 190;
    s.start(c, 25);
    s.update(150);
    TEST_ASSERT_TRUE(s.running());
    s.update(190.5f);
    TEST_ASSERT_EQUAL_STRING("over temperature", s.failure());
    assertOff(s, 150);
}

void test_at_abort_overshoot() {
    Sequence s;
    Config c = onePoint(177);
    s.start(c, 25);
    s.update(170); // into the approach band: relay
    TEST_ASSERT_EQUAL(Phase::Settling, s.phase());
    s.update(177 + c.maxOvershootC + 0.5f);
    TEST_ASSERT_EQUAL(Phase::Failed, s.phase());
    TEST_ASSERT_EQUAL_STRING("overshoot", s.failure());
    assertOff(s, 177);
}

void test_at_abort_no_heat_up() {
    Sequence s;
    Config c = onePoint(177);
    s.start(c, 25);
    float t = 0;
    while (s.running() && t < 1000) {
        const float u = s.update(25 + t * 0.01f); // 0.6 C/min: stalled
        t += 0.25f;
        if (s.running())
            TEST_ASSERT_EQUAL_FLOAT(c.outputMax, u);
    }
    TEST_ASSERT_EQUAL_STRING("no heat-up", s.failure());
    TEST_ASSERT_FLOAT_WITHIN(1, c.stallSeconds, t);
    assertOff(s, 30);
}

void test_at_abort_no_oscillation() {
    Sequence s;
    Config c = onePoint(177);
    s.start(c, 25);
    float t = 0;
    while (s.running() && t < 3000) {
        s.update(172); // never crosses the band: the heater cannot reach the point
        t += 0.25f;
    }
    TEST_ASSERT_EQUAL_STRING("no oscillation", s.failure());
    TEST_ASSERT_FLOAT_WITHIN(1, c.cycleTimeoutS, t);
    assertOff(s, 172);
}

void test_at_abort_time_limit() {
    Drum p;
    Sequence s;
    Config c;
    c.totalTimeoutS = 600;
    s.start(c, 25);
    const float t = runToEnd(s, p, 25);
    TEST_ASSERT_EQUAL_STRING("time limit", s.failure());
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 600, t);
    assertOff(s, 150);
}

void test_at_abort_command() {
    Drum p;
    Sequence s;
    s.start(Config(), 25);
    float pv = 25;
    for (int i = 0; i < 2000; i++)
        pv = p.step(s.update(pv));
    s.abort("stop");
    TEST_ASSERT_EQUAL(Phase::Aborted, s.phase());
    TEST_ASSERT_EQUAL_STRING("stop", s.failure());
    assertOff(s, pv);
    s.abort("again"); // no effect once ended
    TEST_ASSERT_EQUAL_STRING("stop", s.failure());
}

void test_at_refuses_hot_start_and_empty_plan() {
    Sequence s;
    Config c = onePoint(150);
    s.start(c, 150 + c.maxOvershootC + 1);
    TEST_ASSERT_EQUAL(Phase::Failed, s.phase());
    TEST_ASSERT_EQUAL_STRING("too hot to start", s.failure());
    TEST_ASSERT_EQUAL_FLOAT(0, s.update(150));
    Config none;
    none.pointCount = 0;
    s.start(none, 25);
    TEST_ASSERT_EQUAL_STRING("no test points", s.failure());
    TEST_ASSERT_EQUAL_FLOAT(0, s.update(25));
}

// Cycles that never agree: every point is unsettled and the run fails without
// a recommendation.
void test_at_inconsistent_cycles_fail() {
    Drum p;
    Sequence s;
    Config c;
    c.maxCv = 0.0001f;
    s.start(c, 25);
    runToEnd(s, p, 25);
    TEST_ASSERT_EQUAL(Phase::Failed, s.phase());
    TEST_ASSERT_EQUAL_STRING("no consistent oscillation", s.failure());
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL(PointStatus::Unsettled, s.point(i).status);
        TEST_ASSERT_EQUAL(c.maxCycles, s.point(i).cycles);
    }
}

void test_at_make_plan() {
    Config c;
    c.abortAboveC = 270;
    c.maxOvershootC = 30;
    const float req[] = {204, 0, 149, 300, 151};
    TEST_ASSERT_EQUAL(3, makePlan(req, 5, 100, 250, c));
    TEST_ASSERT_EQUAL_FLOAT(149, c.pointsC[0]); // sorted; 151 dropped (within 5 C)
    TEST_ASSERT_EQUAL_FLOAT(204, c.pointsC[1]);
    TEST_ASSERT_EQUAL_FLOAT(240, c.pointsC[2]); // 300 clamped under 270 - 30
    TEST_ASSERT_EQUAL_FLOAT(0, c.pointsC[3]);
    const float low[] = {50};
    TEST_ASSERT_EQUAL(1, makePlan(low, 1, 100, 250, c));
    TEST_ASSERT_EQUAL_FLOAT(100, c.pointsC[0]);
    const float none[] = {0, NAN};
    TEST_ASSERT_EQUAL(0, makePlan(none, 2, 100, 250, c));
}

void test_at_rules() {
    const Model none{NAN, NAN, NAN};
    PidGains g = Sequence::gainsFor(Rule::ZieglerNicholsPi, 4, 120, none);
    TEST_ASSERT_FLOAT_WITHIN(1e-4, 1.8f, g.kp);
    TEST_ASSERT_FLOAT_WITHIN(1e-5, 1.8f / 100, g.ki);
    TEST_ASSERT_EQUAL_FLOAT(0, g.kd);
    g = Sequence::gainsFor(Rule::TyreusLuybenPid, 4.4f, 120, none);
    TEST_ASSERT_FLOAT_WITHIN(1e-4, 2.0f, g.kp);
    TEST_ASSERT_FLOAT_WITHIN(1e-5, 2.0f / 264, g.ki);
    TEST_ASSERT_FLOAT_WITHIN(1e-3, 2.0f * 120 / 6.3f, g.kd);
    g = Sequence::gainsFor(Rule::TyreusLuybenPi, 3.22f, 100, none);
    TEST_ASSERT_FLOAT_WITHIN(1e-4, 1.0f, g.kp);
    const Model m{3, 300, 30};
    g = Sequence::gainsFor(Rule::SimcPi, 4, 120, m);
    TEST_ASSERT_FLOAT_WITHIN(1e-4, 300.0f / (2 * 3 * 30), g.kp); // tau / (2 K L)
    TEST_ASSERT_FLOAT_WITHIN(1e-5, g.kp / 240, g.ki);           // min(tau, 8 L)
    // A model the ultimate point cannot support is reported as unknown.
    const Model bad = Sequence::fitModel(0.1f, 100, 177, 25, 50);
    TEST_ASSERT_TRUE(std::isnan(bad.tauS));
}

void test_at_estimate() {
    Config c = pointsOnly();
    const float cold = Sequence::estimateSeconds(c, 25);
    const float warm = Sequence::estimateSeconds(c, 140);
    TEST_ASSERT_TRUE(cold > warm && warm > 0);
    TEST_ASSERT_TRUE(cold <= c.totalTimeoutS);
    // Simulated drum: the estimate from the measured rate and period ends near
    // the real finish once the first point is measured.
    Drum p;
    Sequence s;
    s.start(c, 25);
    float pv = 25, t = 0, predicted = -1;
    while (s.running()) {
        pv = p.step(s.update(pv));
        t += 0.25f;
        if (predicted < 0 && s.progress().point == 1)
            predicted = t + s.progress().remainingS;
    }
    TEST_ASSERT_FLOAT_WITHIN(0.3f * t, t, predicted);
}

void test_at_status_text() {
    Snapshot s;
    s.mode = Mode::Autotune;
    s.autotune = AutotuneStatus::Running;
    s.autotuneProgress.phase = Phase::Oscillating;
    s.autotuneProgress.point = 1;
    s.autotuneProgress.pointCount = 3;
    s.autotuneProgress.cycle = 3;
    s.autotuneProgress.cycleOf = 6;
    s.autotuneProgress.targetC = 176.67f;
    char b[48];
    autotunetext::full(s, 'F', b, sizeof b);
    TEST_ASSERT_EQUAL_STRING("running: cycle 3/6 at 350 F (2/3)", b);
    autotunetext::brief(s, 'F', b, sizeof b);
    TEST_ASSERT_EQUAL_STRING("AT2/3 cyc3/6 350F", b);
    s.autotuneProgress.phase = Phase::Heating;
    autotunetext::full(s, 'F', b, sizeof b);
    TEST_ASSERT_EQUAL_STRING("running: heating to 350 F (2/3)", b);
    autotunetext::brief(s, 'C', b, sizeof b);
    TEST_ASSERT_EQUAL_STRING("AT2/3 heat 177C", b);
    s.autotuneProgress.phase = Phase::Settling;
    s.autotuneProgress.cycle = 1;
    s.autotuneProgress.cycleOf = 2;
    autotunetext::full(s, 'F', b, sizeof b);
    TEST_ASSERT_EQUAL_STRING("running: settling 1/2 at 350 F (2/3)", b);
    s.autotune = AutotuneStatus::Pending;
    autotunetext::full(s, 'F', b, sizeof b);
    TEST_ASSERT_EQUAL_STRING("results ready", b);
    autotunetext::brief(s, 'F', b, sizeof b);
    TEST_ASSERT_EQUAL_STRING("AT results ready", b);
    s.autotune = AutotuneStatus::Discarded;
    autotunetext::full(s, 'F', b, sizeof b);
    TEST_ASSERT_EQUAL_STRING("discarded", b);
    s.autotune = AutotuneStatus::Failed;
    s.autotuneReason = "no oscillation";
    autotunetext::brief(s, 'F', b, sizeof b);
    TEST_ASSERT_EQUAL_STRING("AT fail: no cycle", b);
    // Worst-case lengths fit the header.
    s.autotune = AutotuneStatus::Running;
    s.autotuneProgress.phase = Phase::Oscillating;
    s.autotuneProgress.cycle = 12;
    s.autotuneProgress.cycleOf = 12;
    s.autotuneProgress.targetC = 240;
    autotunetext::brief(s, 'F', b, sizeof b);
    TEST_ASSERT_TRUE(std::strlen(b) <= 20);
    TEST_ASSERT_EQUAL_STRING("AT2/3 cyc12/12 464F", b);
}

// Default points: 250, 300 and 350 F; the run's ceiling (top point plus the
// overshoot stop) stays near 400 F.
void test_at_default_points() {
    const Config c;
    TEST_ASSERT_EQUAL(3, c.pointCount);
    const int f[] = {250, 300, 350};
    for (int i = 0; i < 3; i++)
        TEST_ASSERT_EQUAL(f[i], (int)std::lround(c.pointsC[i] * 1.8f + 32));
    TEST_ASSERT_TRUE(c.pointsC[2] + c.maxOvershootC < 207);
    Config plan;
    TEST_ASSERT_EQUAL(3, makePlan(c.pointsC, 3, 100, 250, plan));
    TEST_ASSERT_EQUAL_FLOAT(c.pointsC[2], plan.pointsC[2]);
}

void test_at_protocol() {
    Command c;
    JsonDocument in;
    in["autotune"] = "apply";
    TEST_ASSERT_TRUE(autotuneproto::parseAction(in["autotune"], c));
    TEST_ASSERT_EQUAL(Command::Kind::ApplyAutotune, c.kind);
    in["autotune"] = "discard";
    TEST_ASSERT_TRUE(autotuneproto::parseAction(in["autotune"], c));
    TEST_ASSERT_EQUAL(Command::Kind::DiscardAutotune, c.kind);
    in["autotune"] = "start";
    TEST_ASSERT_FALSE(autotuneproto::parseAction(in["autotune"], c));
    in["autotune"] = 1;
    TEST_ASSERT_FALSE(autotuneproto::parseAction(in["autotune"], c));

    Drum p;
    Sequence seq;
    Config cfg;
    seq.start(cfg, 25);
    runToEnd(seq, p, 25);
    TEST_ASSERT_EQUAL(Phase::Done, seq.phase());
    Report r;
    seq.fillReport(r);
    Snapshot s;
    s.autotune = AutotuneStatus::Pending;
    s.autotunePending = true;
    s.gains = {1, 0.1f, 0.01f};
    JsonDocument doc;
    autotuneproto::writeReport(s, r, cfg, 1800, 'F', doc.to<JsonObject>());
    TEST_ASSERT_EQUAL_STRING("results ready", doc["status"]);
    TEST_ASSERT_TRUE(doc["pending"].as<bool>());
    TEST_ASSERT_FALSE(doc["running"].as<bool>());
    TEST_ASSERT_EQUAL_STRING("done", doc["phase"]);
    TEST_ASSERT_EQUAL(250, doc["plan"]["points"][0].as<int>());
    TEST_ASSERT_EQUAL(350, doc["plan"]["points"][2].as<int>());
    TEST_ASSERT_EQUAL(1800, doc["plan"]["estimate"].as<int>());
    TEST_ASSERT_EQUAL_STRING("ziegler-nichols-pi", doc["plan"]["rule"]);
    TEST_ASSERT_TRUE(doc["progress"].isNull());
    TEST_ASSERT_EQUAL(3, doc["results"].size());
    TEST_ASSERT_EQUAL(250, doc["results"][0]["target"].as<int>());
    TEST_ASSERT_EQUAL_STRING("ok", doc["results"][0]["status"]);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, r.points[0].amplitudeC * 1.8f, doc["results"][0]["amplitude"].as<float>());
    TEST_ASSERT_FLOAT_WITHIN(1e-3f * r.recommended.kp, r.recommended.kp, doc["recommended"]["kp"].as<float>());
    TEST_ASSERT_EQUAL(3, doc["recommended"]["used"].as<int>());
    TEST_ASSERT_EQUAL_FLOAT(1, doc["current"]["kp"].as<float>());
    const JsonArrayConst log = doc["log"].as<JsonArrayConst>();
    TEST_ASSERT_TRUE(log.size() > 6);
    TEST_ASSERT_TRUE(log.size() <= (size_t)kLogSize);
    const std::string last = log[log.size() - 1]["text"].as<const char*>();
    TEST_ASSERT_EQUAL_STRING("done: Kp", last.substr(0, 8).c_str());
    // While running: progress in the display unit.
    Sequence run;
    run.start(cfg, 25);
    Drum q;
    float pv = 25;
    for (int i = 0; i < 200; i++)
        pv = q.step(run.update(pv));
    run.fillReport(r);
    s.mode = Mode::Autotune;
    s.autotune = AutotuneStatus::Running;
    s.autotunePending = false;
    s.autotuneProgress = run.progress();
    doc.clear();
    autotuneproto::writeReport(s, r, cfg, 1800, 'F', doc.to<JsonObject>());
    TEST_ASSERT_TRUE(doc["running"].as<bool>());
    TEST_ASSERT_EQUAL_STRING("heating", doc["phase"]);
    TEST_ASSERT_EQUAL(250, doc["progress"]["target"].as<int>());
    TEST_ASSERT_EQUAL(0, doc["results"].size());
    TEST_ASSERT_TRUE(doc["recommended"].isNull());
    TEST_ASSERT_EQUAL_STRING("point 1: heating to 250 F", doc["log"][0]["text"]);
}

void runAutotuneTests() {
    RUN_TEST(test_at_fopdt_ultimate_point);
    RUN_TEST(test_at_multi_point_run);
    RUN_TEST(test_at_abort_sensor_fault);
    RUN_TEST(test_at_abort_over_temperature);
    RUN_TEST(test_at_abort_overshoot);
    RUN_TEST(test_at_abort_no_heat_up);
    RUN_TEST(test_at_abort_no_oscillation);
    RUN_TEST(test_at_abort_time_limit);
    RUN_TEST(test_at_abort_command);
    RUN_TEST(test_at_refuses_hot_start_and_empty_plan);
    RUN_TEST(test_at_inconsistent_cycles_fail);
    RUN_TEST(test_at_make_plan);
    RUN_TEST(test_at_default_points);
    RUN_TEST(test_at_rules);
    RUN_TEST(test_at_estimate);
    RUN_TEST(test_at_status_text);
    RUN_TEST(test_at_protocol);
}

// ---------- fan test, feedforward and fan assist ----------

namespace {
// One full run with the fan test on the airflow drum; the user's fan is 60 %.
struct FanRun {
    Drum p;
    Sequence s;
    Config c;
    float t = 0, minPv = 1e9f, pointsEndT = -1;
    void run() {
        p.fan = 60;
        s.start(c, 25, 60);
        float pv = 25;
        while (s.running() && t < 20000) {
            const float u = s.update(pv);
            TEST_ASSERT_TRUE(u >= 0 && u <= 100);
            const float f = s.fanRequest();
            if (std::isfinite(f))
                p.fan = f;
            if (pointsEndT < 0 && (s.phase() == Phase::FanHold))
                pointsEndT = t;
            if (s.phase() == Phase::FanShed)
                minPv = std::min(minPv, pv);
            pv = p.step(u);
            t += 0.25f;
        }
    }
};

const Report& fanReport() {
    static FanRun r;
    static Report rep;
    static bool done = false;
    if (!done) {
        r.run();
        r.s.fillReport(rep);
        done = true;
    }
    return rep;
}

// PI at 300 F on the airflow drum, fan stepped 30 -> 90 % at 900 s.
// Returns the largest BT drop under the setpoint after the step.
float fanStepDip(const roastermodel::RoasterModel* m, const PidGains& g) {
    Drum q;
    q.fan = 30;
    Pid::Config pc;
    Pid pid(pc);
    pid.setGains(g);
    const float sp = 148.9f;
    float pv = 25, dip = 0;
    for (float tt = 0; tt < 2400; tt += 0.25f) {
        if (tt >= 900)
            q.fan = 90;
        const float ff = m ? roastermodel::fanFeedforward(*m, (float)q.fan, sp) : 0;
        pv = q.step(pid.update(sp, pv, ff));
        if (tt > 900)
            dip = std::max(dip, sp - pv);
    }
    return dip;
}
} // namespace

// The fan test measures the drum's airflow loss: holding heater per level close
// to the drum's own, BT dips and recovers on each step up, the heater-off fall
// grows with airflow, and the drum never falls past the shed bound.
void test_at_fan_test_on_drum() {
    const Report& r = fanReport();
    TEST_ASSERT_EQUAL(Phase::Done, r.phase);
    const FanTestResult& f = r.fan;
    {
        char m2[300];
        for (int i = 0; i < r.logCount; i++) {
            autotuneproto::logText(r.log[i], r, 'C', m2, sizeof m2);
            char m3[340];
            snprintf(m3, sizeof m3, "%.0f %s", r.log[i].tS, m2);
            TEST_MESSAGE(m3);
        }
    }
    TEST_ASSERT_EQUAL(FanTestStatus::Ok, f.status);
    TEST_ASSERT_EQUAL(3, f.levelCount);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 148.9f, f.testC);
    Drum ref;
    char msg[200];
    for (int i = 0; i < 3; i++) {
        const FanLevelResult& L = f.levels[i];
        const double hold = ref.hold(f.testC, L.fanPct);
        const double k = (ref.h0 * (1 + ref.kloss * (f.testC - 25)) + ref.hf * std::pow(L.fanPct / 100, ref.fanExp)) / ref.C;
        snprintf(msg, sizeof msg, "fan %.0f: hold %.1f (drum %.1f) dip %.2f rec %.0f s gain %.3f tau %.0f (drum %.0f) shed %.2f C/min (drum %.2f)",
                 L.fanPct, L.holdPct, hold, L.dipC, L.recoveryS, L.gainCPerPct, L.tauS, 1 / k, L.shedCpm,
                 k * (f.testC - 25) * 60);
        TEST_MESSAGE(msg);
        TEST_ASSERT_FLOAT_WITHIN(2.0f, (float)hold, L.holdPct);
        // The window starts 40 s after the heater goes off, while the element
        // still gives off some heat: the fall reads 65..100 % of the drum's own
        // heater-off rate at the test temperature, and the time constant long.
        const double rate = k * (f.testC - 25) * 60;
        TEST_ASSERT_TRUE(L.shedCpm > 0.65 * rate && L.shedCpm <= 1.0 * rate);
        TEST_ASSERT_TRUE(L.tauS >= 1 / k && L.tauS < 1.6 / k);
        TEST_ASSERT_TRUE(L.gainCPerPct < 0);
        if (i > 0) {
            TEST_ASSERT_TRUE(L.holdPct > f.levels[i - 1].holdPct);
            TEST_ASSERT_TRUE(L.shedCpm > f.levels[i - 1].shedCpm);
            TEST_ASSERT_TRUE(L.dipC > 0.5f && L.recoveryS > 0);
        } else {
            TEST_ASSERT_TRUE(std::isnan(L.dipC));
        }
    }
    snprintf(msg, sizeof msg, "fan test %.0f s (%.1f min)", f.durationS, f.durationS / 60);
    TEST_MESSAGE(msg);
    TEST_ASSERT_TRUE(f.durationS < Config().fanTimeoutS);
    TEST_ASSERT_TRUE(f.durationS > 600);
    // Last log entry is still the recommendation.
    TEST_ASSERT_EQUAL(Phase::Done, r.log[r.logCount - 1].phase);
}

// The bounded heater-off window, the fan returning to the user's after the
// run, and the estimate including the fan test.
void test_at_fan_test_bounds() {
    FanRun r;
    r.run();
    TEST_ASSERT_EQUAL(Phase::Done, r.s.phase());
    const Config& c = r.c;
    TEST_ASSERT_TRUE(r.minPv > r.s.fanResult().testC - c.fanShedMaxDropC - 2);
    TEST_ASSERT_TRUE(std::isnan(r.s.fanRequest()));
    TEST_ASSERT_EQUAL_FLOAT(60, r.s.fanResult().startFanPct);
    const float withFan = Sequence::estimateSeconds(c, 25);
    Config off = c;
    off.fanTest = false;
    const float without = Sequence::estimateSeconds(off, 25);
    char msg[160];
    snprintf(msg, sizeof msg, "run %.0f s, points end %.0f s, fan test adds %.0f s; estimate adds %.0f s",
             r.t, r.pointsEndT, r.t - r.pointsEndT, withFan - without);
    TEST_MESSAGE(msg);
    TEST_ASSERT_TRUE(withFan > without);
    TEST_ASSERT_FLOAT_WITHIN(0.35f * (r.t - r.pointsEndT), r.t - r.pointsEndT, withFan - without);
    // Off: the points alone, no fan requests.
    Sequence s;
    Drum p;
    p.fan = 60;
    s.start(off, 25, 60);
    float pv = 25;
    while (s.running()) {
        pv = p.step(s.update(pv));
        TEST_ASSERT_TRUE(std::isnan(s.fanRequest()));
    }
    TEST_ASSERT_EQUAL(Phase::Done, s.phase());
    TEST_ASSERT_EQUAL(FanTestStatus::Skipped, s.fanResult().status);
}

// A fan test that cannot hold the first level keeps the PID results.
void test_at_fan_test_failure_keeps_gains() {
    Drum p;
    p.fan = 60;
    Sequence s;
    Config c;
    c.fanApproachMaxS = 30; // too short to fall from 350 F to 300 F
    s.start(c, 25, 60);
    float pv = 25;
    while (s.running()) {
        const float f = s.fanRequest();
        const float u = s.update(pv);
        if (std::isfinite(f))
            p.fan = f;
        pv = p.step(u);
    }
    TEST_ASSERT_EQUAL(Phase::Done, s.phase());
    TEST_ASSERT_TRUE(s.recommended().kp > 0);
    TEST_ASSERT_EQUAL(FanTestStatus::Failed, s.fanResult().status);
    TEST_ASSERT_EQUAL_STRING("no steady hold", s.fanResult().failure);
    const roastermodel::RoasterModel m = roastermodel::fromReport([&] {
        static Report rep;
        s.fillReport(rep);
        return rep;
    }(), 60);
    TEST_ASSERT_TRUE(m.valid);
    TEST_ASSERT_FALSE(m.fan[0].valid);
    TEST_ASSERT_EQUAL_FLOAT(0, roastermodel::fanFeedforward(m, 90, 150));
}

// Fan feedforward from the measured holds cuts the BT dip when the fan steps up.
void test_fan_feedforward_dip() {
    const Report& r = fanReport();
    const roastermodel::RoasterModel m = roastermodel::fromReport(r, 60);
    TEST_ASSERT_TRUE(m.valid && m.fan[0].valid);
    const float without = fanStepDip(nullptr, r.recommended);
    const float with = fanStepDip(&m, r.recommended);
    char msg[120];
    snprintf(msg, sizeof msg, "fan 30->90 %% at 300 F: BT dip %.2f C without feedforward, %.2f C with", without, with);
    TEST_MESSAGE(msg);
    TEST_ASSERT_TRUE(without > 2);
    TEST_ASSERT_TRUE(with < 0.5f * without);
    // The feedforward is the hold difference, scaled with BT over ambient.
    const float ff150 = roastermodel::fanFeedforward(m, 90, 148.9f);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, r.fan.levels[2].holdPct - r.fan.levels[0].holdPct, ff150);
    TEST_ASSERT_TRUE(roastermodel::fanFeedforward(m, 90, 200) > ff150);
    TEST_ASSERT_EQUAL_FLOAT(0, roastermodel::fanFeedforward(m, 30, 150));
    // Without a fan test: no feedforward.
    roastermodel::RoasterModel none;
    TEST_ASSERT_EQUAL_FLOAT(0, roastermodel::fanFeedforward(none, 90, 150));
}

// Ramp feedforward (tau / K * slope) and the achievable rate of rise.
void test_ramp_feedforward_and_max_ror() {
    const Report& r = fanReport();
    const roastermodel::RoasterModel m = roastermodel::fromReport(r, 60);
    // 8 C/min ramp from 150 C to 190 C on the drum at fan 60 %.
    float err[2] = {0, 0};
    for (int k = 0; k < 2; k++) {
        Drum q;
        q.fan = 60;
        Pid::Config pc;
        Pid pid(pc);
        pid.setGains(r.recommended);
        float pv = 25;
        for (float tt = 0; tt < 1500; tt += 0.25f) {
            const float slope = tt >= 900 && tt < 1200 ? 8.0f / 60 : 0;
            const float sp = tt < 900 ? 150 : std::min(190.0f, 150 + (tt - 900) * 8.0f / 60);
            const float ff = (k ? roastermodel::rampFeedforward(m, sp, 60, slope) : 0) +
                             roastermodel::fanFeedforward(m, 60, sp);
            pv = q.step(pid.update(sp, pv, ff));
            if (tt > 960 && tt < 1200)
                err[k] += (sp - pv) * 0.25f / 240;
        }
    }
    char msg[160];
    snprintf(msg, sizeof msg, "8 C/min ramp: mean lag %.2f C without ramp feedforward, %.2f C with", err[0], err[1]);
    TEST_MESSAGE(msg);
    TEST_ASSERT_TRUE(std::fabs(err[1]) < 0.5f * std::fabs(err[0]));
    // Achievable rate of rise at 150 C, fan 60 %, heater 100 %: compare with the drum.
    Drum q;
    q.fan = 60;
    q.noise = 0;
    Pid::Config pc;
    Pid pid(pc);
    pid.setGains(r.recommended);
    float pv = 25;
    for (float tt = 0; tt < 1200; tt += 0.25f)
        pv = q.step(pid.update(150, pv, roastermodel::fanFeedforward(m, 60, 150)));
    const float t0 = pv;
    float t1 = pv;
    for (float tt = 0; tt < 120; tt += 0.25f)
        t1 = q.step(100);
    const float measured = (t1 - t0) / 2; // C/min over the first 2 min, lags included
    const float est = roastermodel::maxRorCpm(m, 150, 60, 100);
    snprintf(msg, sizeof msg, "max RoR at 150 C, fan 60 %%: estimate %.1f C/min, drum %.1f C/min over 2 min", est, measured);
    TEST_MESSAGE(msg);
    TEST_ASSERT_TRUE(est > 0);
    TEST_ASSERT_FLOAT_WITHIN(0.5f * est, est, measured);
    TEST_ASSERT_TRUE(roastermodel::maxRorCpm(m, 200, 60, 100) < est); // less headroom hotter
    TEST_ASSERT_TRUE(roastermodel::maxRorCpm(m, 150, 90, 100) < est); // more airflow, less headroom
    TEST_ASSERT_TRUE(std::isnan(roastermodel::maxRorCpm(roastermodel::RoasterModel{}, 150, 60, 100)));
}

// Shaped setpoint ramps on the drum, driven by the recipe runner (setpoint
// every 0.5 s) with fan and ramp feedforward as the controller adds them: BT
// follows the shape of the rate of climb, not only the end points.
void test_shaped_ramps_on_drum() {
    const Report& r = fanReport();
    const roastermodel::RoasterModel m = roastermodel::fromReport(r, 60);
    const char* shapes[] = {"linear", "normal", "front", "back"};
    char msg[200];
    for (const char* shape : shapes) {
        char text[300];
        snprintf(text, sizeof text,
                 R"({"steps":[{"trigger":{"type":"event","value":"CHARGE"},"actions":[{"type":"setpoint","value":200,"ramp":300,"shape":"%s"}]}]})",
                 shape);
        JsonDocument d;
        deserializeJson(d, text);
        static recipe::Recipe rec;
        TEST_ASSERT_EQUAL_STRING("", recipe::parse(d.as<JsonObjectConst>(), 'C', rec));
        recipe::Runner run;
        recipe::RoastClock clock;
        run.load(rec);
        Drum q;
        q.fan = 60;
        Pid::Config pc;
        Pid pid(pc);
        pid.setGains(r.recommended);
        roastermodel::SetpointSlope slope;
        float pv = 25, sp = 150, lagMax = 0, lagSum = 0, over = 0, atHalf = NAN, at0 = NAN, atEnd = NAN;
        int n = 0;
        for (float tt = 0; tt < 1500; tt += 0.25f) {
            const uint32_t ms = static_cast<uint32_t>(tt * 1000);
            if (tt == 899.5f) // settled at 150 C: start the recipe, then CHARGE
                run.start();
            if (tt == 900)
                clock.mark(recipe::Event::Charge, ms);
            if (tt >= 899.5f) {
                recipe::Inputs in;
                in.nowMs = ms;
                in.btC = pv;
                in.setpointC = sp;
                in.heaterPct = 50;
                recipe::Output o;
                run.tick(in, clock, o);
                for (size_t i = 0; i < o.count; i++)
                    if (o.commands[i].kind == Command::Kind::SetSetpoint)
                        sp = o.commands[i].a;
            }
            const float ff = roastermodel::rampFeedforward(m, sp, 60, slope.update(sp, 0.25f)) +
                             roastermodel::fanFeedforward(m, 60, sp);
            pv = q.step(pid.update(sp, pv, ff));
            if (tt >= 900 && tt < 1200) {
                lagMax = std::max(lagMax, std::fabs(sp - pv));
                lagSum += (sp - pv) * (sp - pv);
                n++;
            }
            if (tt == 900)
                at0 = pv;
            if (tt == 1050)
                atHalf = pv;
            if (tt == 1200)
                atEnd = pv;
            if (tt > 1200)
                over = std::max(over, pv - 200);
        }
        const float firstHalf = (atHalf - at0) / 2.5f, secondHalf = (atEnd - atHalf) / 2.5f; // C/min
        float uPeak = 0;
        const uint8_t* rates = recipe::shapeRates(rec.steps[0].actions[0].shape);
        const float need = 50.0f / 5 * recipe::peakFactor(rates, &uPeak);
        const float peakC = 150 + 50 * recipe::rampFraction(rates, uPeak);
        snprintf(msg, sizeof msg,
                 "%-6s 150->200 C in 5 min: BT RoR %.1f then %.1f C/min (path %.1f / %.1f), lag rms %.2f max %.2f C, "
                 "overshoot %.2f C; peak need %.0f C/min at %.0f C, model max %.0f",
                 shape, firstHalf, secondHalf, 50 * recipe::rampFraction(rates, 0.5f) / 2.5f,
                 50 * (1 - recipe::rampFraction(rates, 0.5f)) / 2.5f, std::sqrt(lagSum / n), lagMax, over, need, peakC,
                 roastermodel::maxRorCpm(m, peakC, 60, 90));
        TEST_MESSAGE(msg);
        TEST_ASSERT_TRUE(run.state() == recipe::State::Done);
        TEST_ASSERT_FLOAT_WITHIN(2.0f, 200, pv);
        TEST_ASSERT_TRUE(lagMax < 10); // dead time and probe lag: the steep start of "front" lags most
        // "back" ends at its fastest rate: BT carries that momentum past the target.
        TEST_ASSERT_TRUE(over < (std::strcmp(shape, "back") ? 2.5f : 6.0f));
        if (!std::strcmp(shape, "front"))
            TEST_ASSERT_TRUE(firstHalf > 2 * secondHalf);
        if (!std::strcmp(shape, "back"))
            TEST_ASSERT_TRUE(secondHalf > 2 * firstHalf);
        if (!std::strcmp(shape, "linear"))
            TEST_ASSERT_FLOAT_WITHIN(0.35f * firstHalf, firstHalf, secondHalf);
    }
}

// A front ramp to 200 C started from Manual 60 % when BT passes 150 C, with
// the controller setpoint stale at 0 (power steps before it). The ramp starts
// at BT and the PID restarts from 60 %, so BT keeps rising. The
// old start (from the stale setpoint) would have held the heater near 0 until
// the setpoint caught up with BT.
void test_ramp_from_manual_on_drum() {
    const Report& r = fanReport();
    const roastermodel::RoasterModel m = roastermodel::fromReport(r, 60);
    JsonDocument d;
    deserializeJson(d, R"({"steps":[{"trigger":{"type":"bt","value":150},"actions":[{"type":"setpoint","value":200,"ramp":300,"shape":"front"}]}]})");
    static recipe::Recipe rec;
    TEST_ASSERT_EQUAL_STRING("", recipe::parse(d.as<JsonObjectConst>(), 'C', rec));
    float rorBefore[2] = {}, rorMin[2] = {}, over[2] = {}, startAt[2] = {};
    for (int newStart = 0; newStart < 2; newStart++) {
        recipe::Runner run;
        recipe::RoastClock clock;
        run.load(rec);
        run.start();
        Drum q;
        q.fan = 60;
        Pid::Config pc;
        Pid pid(pc);
        pid.setGains(r.recommended);
        roastermodel::SetpointSlope slope;
        float pv = 25, sp = 0, heater = 60, t0 = NAN;
        bool pidOn = false;
        float hist[2400];
        int n = 0;
        rorMin[newStart] = 1e9f;
        for (float tt = 0; tt < 1200 && n < 2400; tt += 0.5f) {
            const uint32_t ms = static_cast<uint32_t>(tt * 1000);
            recipe::Inputs in;
            in.nowMs = ms;
            in.btC = pv;
            in.setpointC = sp;
            in.heaterPct = heater;
            recipe::Output o;
            run.tick(in, clock, o);
            for (size_t i = 0; i < o.count; i++) {
                const Command& c = o.commands[i];
                if (c.kind != Command::Kind::SetSetpoint)
                    continue;
                if (!pidOn) { // the ramp start: what the controller does
                    pidOn = true;
                    t0 = tt;
                    startAt[newStart] = c.a;
                    sp = newStart ? c.a : 0; // old: ramp from the stale setpoint 0
                    const float ff = roastermodel::rampFeedforward(m, sp, 60, slope.value()) +
                                     roastermodel::fanFeedforward(m, 60, sp);
                    pid.reset(sp, pv, newStart ? heater : 0, ff); // old: PID from 0 %
                } else if (newStart) {
                    sp = c.a;
                } else { // old path: the same shape over 0 -> 200
                    const float u = fminf(1, (tt - t0) / 300);
                    sp = 200 * recipe::rampFraction(recipe::shapeRates(recipe::Shape::Front), u);
                }
            }
            for (int k = 0; k < 2; k++) { // control at 0.25 s, recipe at 0.5 s
                if (pidOn)
                    heater = pid.update(sp, pv, roastermodel::rampFeedforward(m, sp, 60, slope.update(sp, 0.25f)) +
                                                    roastermodel::fanFeedforward(m, 60, sp));
                pv = q.step(heater);
            }
            hist[n++] = pv;
            const int w = 60; // 30 s window
            if (n > w && std::isfinite(t0)) {
                const float ror = (hist[n - 1] - hist[n - 1 - w]) * 2; // C/min
                if (tt < t0 && tt > t0 - 1)
                    rorBefore[newStart] = ror;
                if (tt >= t0 + 30 && tt <= t0 + 120)
                    rorMin[newStart] = fminf(rorMin[newStart], ror);
                if (tt > t0 + 300)
                    over[newStart] = fmaxf(over[newStart], pv - 200);
            }
            if (!std::isfinite(t0) && n > 60)
                rorBefore[newStart] = (hist[n - 1] - hist[n - 61]) * 2;
        }
        TEST_ASSERT_TRUE(pidOn);
    }
    char msg[220];
    snprintf(msg, sizeof msg,
             "front 150->200 C from Manual 60 %%: RoR %.1f C/min before; lowest 30 s RoR in the first 2 min "
             "%.1f C/min starting at BT (%.1f C), %.1f C/min from the stale setpoint; overshoot %.2f C",
             rorBefore[1], rorMin[1], startAt[1], rorMin[0], over[1]);
    TEST_MESSAGE(msg);
    TEST_ASSERT_TRUE(startAt[1] >= 150 && startAt[1] < 152);
    TEST_ASSERT_TRUE(rorMin[1] > 0.5f * rorBefore[1]); // momentum kept
    TEST_ASSERT_TRUE(rorMin[0] < rorMin[1]);
    TEST_ASSERT_TRUE(over[1] < 2.5f);
}

// Fan assist: bounded, rate-limited, never below the base fan, never in
// Manual (stop, cooldown) or during a trip, and off unless enabled.
void test_fan_assist_bounds() {
    fanassist::Settings st;
    st.enabled = true;
    fanassist::Assist a;
    fanassist::Inputs in;
    in.mode = Mode::Pid;
    in.btC = 200, in.setpointC = 190, in.heaterPct = 0, in.baseFanPct = 50;
    float prev = 0;
    for (int i = 0; i < 400; i++) {
        const float e = a.update(st, in, 0.25f);
        TEST_ASSERT_TRUE(e >= 0 && e <= st.maxExtraPct);
        TEST_ASSERT_TRUE(e - prev <= st.riseRatePctPerS * 0.25f + 1e-4f);
        prev = e;
    }
    TEST_ASSERT_EQUAL_FLOAT(st.maxExtraPct, a.extra());
    // Near the top of the fan range: never past 100 %.
    in.baseFanPct = 95;
    a.update(st, in, 0.25f);
    TEST_ASSERT_TRUE(in.baseFanPct + a.extra() <= 100);
    in.baseFanPct = 50;
    // Heater back on: returns to the base at the fall rate.
    for (int i = 0; i < 400; i++)
        a.update(st, in, 0.25f);
    in.heaterPct = 30;
    const float before = a.extra();
    a.update(st, in, 0.25f);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, before - st.fallRatePctPerS * 0.25f, a.extra());
    for (int i = 0; i < 400; i++)
        a.update(st, in, 0.25f);
    TEST_ASSERT_EQUAL_FLOAT(0, a.extra());
    // Safety paths: cleared at once.
    in.heaterPct = 0;
    for (int i = 0; i < 400; i++)
        a.update(st, in, 0.25f);
    TEST_ASSERT_TRUE(a.extra() > 0);
    fanassist::Inputs trip = in;
    trip.trip = SafetyTrip::OverTemp;
    TEST_ASSERT_EQUAL_FLOAT(0, a.update(st, trip, 0.25f));
    for (int i = 0; i < 400; i++)
        a.update(st, in, 0.25f);
    fanassist::Inputs manual = in;
    manual.mode = Mode::Manual; // stop and cooldown run in Manual
    TEST_ASSERT_EQUAL_FLOAT(0, a.update(st, manual, 0.25f));
    fanassist::Inputs profile = in; // auto modes: PID and Profile
    profile.mode = Mode::Profile;
    TEST_ASSERT_TRUE(a.update(st, profile, 0.25f) > 0);
    // On by default; the controller enables it only with a saved fan test.
    TEST_ASSERT_TRUE(ControlSettings{}.fanAssist);
    fanassist::Inputs tuning = in;
    tuning.mode = Mode::Autotune;
    TEST_ASSERT_EQUAL_FLOAT(0, a.update(st, tuning, 0.25f));
    fanassist::Settings off = st;
    off.enabled = false;
    TEST_ASSERT_EQUAL_FLOAT(0, a.update(off, in, 0.25f));
    // BT on or under the path: nothing.
    fanassist::Inputs under = in;
    under.btC = 189;
    fanassist::Assist b;
    for (int i = 0; i < 400; i++)
        TEST_ASSERT_EQUAL_FLOAT(0, b.update(st, under, 0.25f));

    // On the drum: BT 15 C above a lowered setpoint with the heater at 0;
    // assist brings BT to the path sooner.
    float reach[2];
    for (int k = 0; k < 2; k++) {
        Drum q;
        q.fan = 40;
        q.noise = 0;
        fanassist::Assist as;
        fanassist::Settings s2 = st;
        s2.enabled = k == 1;
        float pv = 25;
        Pid::Config pc;
        Pid pid(pc);
        pid.setGains({2, 0.01f, 0});
        reach[k] = -1;
        for (float tt = 0; tt < 2400; tt += 0.25f) {
            const float sp = tt < 1200 ? 205 : 170;
            const float u = pid.update(sp, pv);
            fanassist::Inputs i2;
            i2.mode = Mode::Pid, i2.btC = pv, i2.setpointC = sp, i2.heaterPct = u, i2.baseFanPct = 40;
            q.fan = 40 + as.update(s2, i2, 0.25f);
            TEST_ASSERT_TRUE(q.fan >= 40 && q.fan <= 40 + st.maxExtraPct);
            pv = q.step(u);
            if (tt > 1200 && reach[k] < 0 && pv < 172)
                reach[k] = tt - 1200;
        }
    }
    char msg[120];
    snprintf(msg, sizeof msg, "BT 205 -> 170 C with heater off: %.0f s without assist, %.0f s with", reach[0], reach[1]);
    TEST_MESSAGE(msg);
    TEST_ASSERT_TRUE(reach[1] > 0 && reach[1] < reach[0]);
}

// Feedforward through the PID: zero ff is the old controller; reset with ff is bumpless.
void test_pid_feedforward() {
    Pid::Config pc;
    Pid a(pc), b(pc);
    a.setGains({2, 0.02f, 0});
    b.setGains({2, 0.02f, 0});
    for (int i = 0; i < 100; i++) {
        const float ua = a.update(150, 140 + i * 0.1f), ub = b.update(150, 140 + i * 0.1f, 0);
        TEST_ASSERT_EQUAL_FLOAT(ua, ub);
    }
    Pid c(pc);
    c.setGains({2, 0.02f, 0});
    c.reset(150, 149, 40, 15);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 40, c.update(150, 149, 15));
    // A large ff with the error at zero: output is ff plus the integrator, clamped.
    Pid d(pc);
    d.setGains({2, 0.02f, 0});
    d.reset(150, 150, 10, 30);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 10, d.update(150, 150, 30)); // integrator went negative
    TEST_ASSERT_EQUAL_FLOAT(100, d.update(150, 100, 200));
}

// Setpoint slope for ramp feedforward: follows a recipe ramp sent every 0.5 s,
// ignores a step to a new target.
void test_setpoint_slope() {
    roastermodel::SetpointSlope sl;
    float sp = 150;
    for (int i = 0; i < 400; i++) { // 8 C/min, a new setpoint every 2nd step
        if (i % 2 == 0)
            sp += 8.0f / 60 * 0.5f;
        sl.update(sp, 0.25f);
    }
    TEST_ASSERT_FLOAT_WITHIN(0.02f, 8.0f / 60, sl.value());
    sl.update(sp + 20, 0.25f); // step: a new target
    TEST_ASSERT_EQUAL_FLOAT(0, sl.value());
    sl.update(NAN, 0.25f);
    TEST_ASSERT_EQUAL_FLOAT(0, sl.value());
}

// Fan levels from config: zeros dropped, sorted, close duplicates dropped.
void test_make_fan_levels() {
    Config c;
    c.fanLevels[0] = 90, c.fanLevels[1] = 0, c.fanLevels[2] = 30, c.fanLevels[3] = 32;
    c.fanLevelCount = 4;
    makeFanLevels(c);
    TEST_ASSERT_EQUAL(2, c.fanLevelCount);
    TEST_ASSERT_EQUAL_FLOAT(30, c.fanLevels[0]);
    TEST_ASSERT_EQUAL_FLOAT(90, c.fanLevels[1]);
    TEST_ASSERT_EQUAL_FLOAT(0, c.fanLevels[2]);
}

// Saving a run without a fan test keeps the previous fan test.
void test_model_merge() {
    const Report& r = fanReport();
    const roastermodel::RoasterModel withFan = roastermodel::fromReport(r, 60);
    roastermodel::RoasterModel fresh = withFan;
    fresh.fan[0] = roastermodel::FanModel{};
    const roastermodel::RoasterModel m = roastermodel::merge(withFan, fresh);
    TEST_ASSERT_TRUE(m.fan[0].valid);
    TEST_ASSERT_EQUAL_FLOAT(withFan.fan[0].levels[1].holdPct, m.fan[0].levels[1].holdPct);
    TEST_ASSERT_TRUE(roastermodel::usable(m));
    roastermodel::RoasterModel old = withFan;
    old.version = 0; // another layout: nothing kept
    TEST_ASSERT_FALSE(roastermodel::merge(old, fresh).fan[0].valid);
    TEST_ASSERT_FALSE(roastermodel::usable(roastermodel::RoasterModel{}));
}

// Stopping or a probe fault during the fan test keeps the gains measured before it.
void test_fan_test_stop_keeps_gains() {
    for (int mode = 0; mode < 2; mode++) {
        Drum p;
        Sequence s;
        Config c;
        p.fan = 60;
        s.start(c, 25, 60);
        float pv = 25, t = 0, holdT = -1;
        while (s.running() && t < 20000) {
            const float u = s.update(pv);
            const float f = s.fanRequest();
            if (std::isfinite(f))
                p.fan = f;
            if (holdT < 0 && s.phase() == Phase::FanHold)
                holdT = t;
            if (holdT >= 0 && t > holdT + 900) // into the second level
                break;
            pv = p.step(u);
            t += 0.25f;
        }
        TEST_ASSERT_TRUE(s.running());
        if (mode == 0)
            s.abort("stopped");
        else
            s.update(NAN); // the probe stops reading
        TEST_ASSERT_FALSE(s.running());
        TEST_ASSERT_TRUE(s.phase() == Phase::Done);
        static Report r;
        s.fillReport(r);
        TEST_ASSERT_TRUE(r.recommended.kp > 0);
        TEST_ASSERT_TRUE(r.fan.status == FanTestStatus::Partial || r.fan.status == FanTestStatus::Failed);
        TEST_ASSERT_TRUE(std::isfinite(r.fan.levels[0].holdPct));
        TEST_ASSERT_TRUE(std::isnan(s.fanRequest()));
    }
}

// Wire format: fan test flag on the autotune start, the fanAssist command and data field.
void test_fan_protocol() {
    JsonDocument d;
    deserializeJson(d, R"({"command":"autotune","fanTest":false})");
    Command c;
    TEST_ASSERT_TRUE(controlproto::basicCommand("autotune", c, d.as<JsonObjectConst>()));
    TEST_ASSERT_TRUE(c.kind == Command::Kind::StartAutotune);
    TEST_ASSERT_EQUAL_FLOAT(0, c.a);
    deserializeJson(d, R"({"command":"autotune"})");
    controlproto::basicCommand("autotune", c, d.as<JsonObjectConst>());
    TEST_ASSERT_TRUE(std::isnan(c.a));
    deserializeJson(d, R"({"fanAssist":true})");
    const controlproto::Parsed p = controlproto::parseNested(d.as<JsonObjectConst>(), 'F', "fanAssist");
    TEST_ASSERT_TRUE(p.hasFanAssist && p.fanAssist);
    TEST_ASSERT_EQUAL(1, (int)p.count);
    TEST_ASSERT_TRUE(p.commands[0].kind == Command::Kind::SetFanAssist);
    deserializeJson(d, R"({"fanAssist":1})"); // not a bool: ignored
    TEST_ASSERT_FALSE(controlproto::parseNested(d.as<JsonObjectConst>(), 'F', "fanAssist").hasFanAssist);
    Snapshot s;
    s.fanPct = 72, s.fanAssistPct = 12;
    JsonDocument out;
    controlproto::writeDataFields(s, 'F', out.to<JsonObject>());
    TEST_ASSERT_EQUAL_FLOAT(12, out["FanAssist"].as<float>());
    JsonDocument info;
    s.fanAssistOn = true, s.fanTested = false;
    controlproto::writeControlInfo(s, CooldownFan{}, WatchdogSettings{}, 'F', info.to<JsonObject>());
    TEST_ASSERT_TRUE(info["fanAssist"]["enabled"].as<bool>());
    TEST_ASSERT_FALSE(info["fanAssist"]["available"].as<bool>());
}

// getAutotune: the fan test object, the plan's fan fields and the saved model.
void test_fan_report_json() {
    const Report& r = fanReport();
    const roastermodel::RoasterModel m = roastermodel::fromReport(r, 60);
    Snapshot s;
    Config plan;
    JsonDocument d;
    autotuneproto::writeReport(s, r, plan, 3000, 'F', d.to<JsonObject>(), &m, 90, 1500);
    TEST_ASSERT_TRUE(d["plan"]["fanTest"].as<bool>());
    TEST_ASSERT_EQUAL_FLOAT(1500, d["plan"]["fanEstimate"].as<float>());
    TEST_ASSERT_EQUAL_STRING("ok", d["fanTest"]["status"]);
    TEST_ASSERT_EQUAL(3, d["fanTest"]["levels"].size());
    TEST_ASSERT_EQUAL_FLOAT(90, d["saved"]["heaterMax"].as<float>());
    TEST_ASSERT_EQUAL(3, d["saved"]["fan"]["levels"].size());
    JsonDocument e;
    autotuneproto::writeReport(s, r, plan, 3000, 'F', e.to<JsonObject>());
    TEST_ASSERT_TRUE(e["saved"].isNull());
}

void runFanTests() {
    RUN_TEST(test_setpoint_slope);
    RUN_TEST(test_make_fan_levels);
    RUN_TEST(test_model_merge);
    RUN_TEST(test_fan_protocol);
    RUN_TEST(test_fan_test_stop_keeps_gains);
    RUN_TEST(test_fan_report_json);
    RUN_TEST(test_at_fan_test_on_drum);
    RUN_TEST(test_at_fan_test_bounds);
    RUN_TEST(test_at_fan_test_failure_keeps_gains);
    RUN_TEST(test_fan_feedforward_dip);
    RUN_TEST(test_ramp_feedforward_and_max_ror);
    RUN_TEST(test_shaped_ramps_on_drum);
    RUN_TEST(test_ramp_from_manual_on_drum);
    RUN_TEST(test_fan_assist_bounds);
    RUN_TEST(test_pid_feedforward);
}
