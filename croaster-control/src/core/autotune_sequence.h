#pragma once
// Multi-point relay autotune (docs/autotune/process.md). One start() and one
// update() per control sample; the caller applies the returned heater request.
// For each test point, lowest first: heat at full request to just below the
// point, run a biased relay with hysteresis for a few settling cycles while the
// bias centres the duty, then measure cycles until the last few agree
// (amplitude and period coefficient of variation under maxCv). Ku and Tu come
// from the describing function, a first-order-plus-dead-time model from Ku, Tu
// and the relay bias, and gains from the configured rule. The recommended set
// is the conservative envelope of the per-point sets (tuning-rules.md).
// Optional fan test after the points (docs/autotune/fan-test.md): at one held
// temperature the fan steps through a few levels; at each the sequence records
// the heater needed to hold BT, the BT dip and recovery after the step, and,
// with the heater off, how fast airflow sheds heat from the drum and beans.
// The caller applies fanRequest() while it is finite.
// Pure (no Arduino), so it is host-tested.
#include <cmath>
#include <cstdint>

#include "pid.h"

namespace autotune {

constexpr int kMaxPoints = 4;
constexpr int kMaxCycles = 12; // measured cycles kept per point
constexpr int kLogSize = 24;   // phase-change log entries kept (oldest dropped)
constexpr int kMaxFanLevels = 4;

// FanHold and FanShed run after the last point's Computing, only with Config::fanTest.
enum class Phase : uint8_t { Idle, Heating, Settling, Oscillating, Computing, Done, Failed, Aborted, FanHold, FanShed };
// docs/autotune/tuning-rules.md compares them on a simulated drum.
enum class Rule : uint8_t { ZieglerNicholsPi, TyreusLuybenPi, TyreusLuybenPid, SimcPi, ZieglerNichols };
enum class PointStatus : uint8_t { Pending, Ok, Unsettled, SmallSwing };
// Skipped: not run (option off, or the points failed). Partial: some levels measured.
enum class FanTestStatus : uint8_t { Skipped, Running, Ok, Partial, Failed };

const char* phaseName(Phase p);
const char* ruleName(Rule r);
const char* pointStatusName(PointStatus s);
const char* fanTestStatusName(FanTestStatus s);

struct Config {
    float pointsC[kMaxPoints] = {121.1f, 148.9f, 176.7f, 0}; // 250, 300, 350 F; ascending, see makePlan()
    int pointCount = 3;
    float dtSeconds = 0.25f;
    float outputMax = 100;   // heater request while heating to a point
    float relayStep = 40;    // relay amplitude d (%), limited to bias and outputMax - bias
    float initialBias = 30;  // relay centre (%) at the first point; adapts from there
    float minBias = 10;      // bias limits while it adapts
    float hysteresisC = 1.0f; // relay band; wider than twice the PV noise
    float approachC = 15;    // full heat until PV >= point - approachC
    int settleCycles = 2;    // unmeasured cycles while the bias adapts
    int minCycles = 3;       // measured cycles in the consistency window
    int maxCycles = 6;       // give up on consistency after this many
    float maxCv = 0.10f;     // amplitude and period CV limit over the window
    float maxOvershootC = 30; // PV above point + this: fail
    float abortAboveC = 260; // PV above this: fail (set under the over-temperature trip)
    float stallSeconds = 180; // heating: PV must rise minRiseC within this window
    float minRiseC = 3;
    float cycleTimeoutS = 900;  // relay phases: no switch for this long -> fail
    float totalTimeoutS = 5400; // whole run
    float ambientC = 25;        // for the model's static gain
    float expectedRateCps = 0.25f; // heat-up rate assumed before one is measured (time estimate)
    float expectedPeriodS = 180;   // cycle period assumed before one is measured
    Rule rule = Rule::ZieglerNicholsPi;

    // Fan test (docs/autotune/fan-test.md). Levels ascending; 0 = unused.
    bool fanTest = true;
    float fanTestC = 0;       // held BT; 0 = the middle test point
    float fanLevels[kMaxFanLevels] = {30, 60, 90, 0};
    int fanLevelCount = 3;
    uint8_t fanChannel = 0;   // 0 = exhaust fan; a second fan output would be 1
    float fanBlockS = 60;     // steady check: consecutive block means
    float fanSettleMinS = 90; // per level, after BT first reaches the band
    float fanSettleMaxS = 900;
    float fanApproachMaxS = 1200; // first level: time to reach the held temperature
    float fanHoldBandC = 1.0f;    // block mean BT within this of the held temperature
    float fanHoldDriftPct = 0.5f; // block mean heater within this of the previous block
    float fanSaturateS = 120;     // heater at outputMax this long: the level cannot be held
    float fanShedSkipS = 40;      // heater off: wait out the element's stored heat and the probe lag
    float fanShedWindowS = 20;    // regression window per level
    float fanShedMaxDropC = 30;   // end a heater-off window this far under the held temperature
    float fanReheatHoldS = 45;    // back at the held temperature this long before the next window
    float fanTimeoutS = 2400;     // whole fan test
};

// Builds the run's test points from the configured ones: drops non-positive and
// non-finite entries, clamps each into [minC, maxSetpointC] and under
// abortAboveC - maxOvershootC, sorts ascending and drops points closer than 5 C.
// Returns the number of points kept (0: nothing to test).
int makePlan(const float* requestedC, int n, float minC, float maxSetpointC, Config& cfg);

// Fan test levels: drops non-positive and non-finite entries, caps at 100 %,
// sorts ascending and drops levels closer than 5 %. Updates fanLevelCount.
void makeFanLevels(Config& cfg);

struct Model { // first-order plus dead time, K e^(-Ls) / (tau s + 1); NaN when not identifiable
    float gainCPerPct, tauS, deadS;
};

struct PointResult {
    float targetC = 0;
    PointStatus status = PointStatus::Pending;
    uint8_t cycles = 0;               // measured cycles
    float amplitudeC = 0, periodS = 0; // means over the window
    float amplitudeCv = 0, periodCv = 0;
    float bias = 0, step = 0;         // relay centre and amplitude, %
    float ku = 0, tuS = 0;
    Model model{};
    PidGains gains;                   // cfg.rule applied to this point
};

// One fan level. Temperatures C, heater and fan %, times s.
struct FanLevelResult {
    float fanPct = 0;
    float holdPct = NAN;   // steady heater that holds BT at the test temperature
    float dipC = NAN;      // BT drop after stepping to this level (NaN: first level, from the user's fan)
    float recoveryS = NAN; // step to BT back within 0.5 C of the test temperature
    float gainCPerPct = NAN; // open-loop BT change per % fan at a fixed heater (negative)
    float tauS = NAN;        // drum time constant at this airflow: (BT - ambient) / shed rate
    float shedCpm = NAN;     // heater off: BT fall rate at the test temperature, C/min
};

struct FanTestResult {
    FanTestStatus status = FanTestStatus::Skipped;
    const char* failure = ""; // string literal
    uint8_t channel = 0;
    float testC = 0, ambientC = 25;
    float startFanPct = 0;    // fan before the test (the user's)
    float durationS = 0;
    int levelCount = 0;
    FanLevelResult levels[kMaxFanLevels];
};

struct Progress {
    Phase phase = Phase::Idle;
    uint8_t point = 0, pointCount = 0; // 0-based index of the active point
    uint8_t cycle = 0;                 // settling: cycles done; oscillating: measured cycles
    uint8_t cycleOf = 0;               // settleCycles or maxCycles
    float targetC = 0;
    float output = 0;                  // heater request this sample
    float fan = NAN;                   // fan request this sample; NaN = leave the fan alone
    uint8_t fanLevel = 0, fanLevelCount = 0; // fan test: active level (0-based) and count
    float amplitudeC = 0, periodS = 0; // last full cycle at this point (0 before one)
    float elapsedS = 0, remainingS = 0;
};

struct LogEntry {
    float tS;
    Phase phase;
    uint8_t point, cycle;
    float a, b; // Heating: PV, target; Oscillating: amplitude, period; Computing: Ku, Tu;
                // FanHold: fan, hold heater (0 at entry); FanShed: fan, shed C/min (0 at entry)
};

// Results and log for the network side (getAutotune); progress travels in the
// Snapshot instead, since it changes every sample.
struct Report {
    uint32_t seq = 0; // Sequence::changes() when filled
    Phase phase = Phase::Idle;
    const char* failure = "";
    Rule rule = Rule::ZieglerNicholsPi;
    int pointCount = 0;
    PointResult points[kMaxPoints];
    PidGains recommended;
    int used = 0; // points behind `recommended`
    int logCount = 0;
    LogEntry log[kLogSize] = {}; // oldest first
    FanTestResult fan;
};

class Sequence {
public:
    void start(const Config& c, float pv);
    // `fanPct` is the fan before the run (the fan test returns to it). Feed one
    // sample every dtSeconds; update() returns the heater request (0 unless running).
    void start(const Config& c, float pv, float fanPct);
    float update(float pv);
    // Fan request for this sample while the fan test runs; NaN otherwise.
    float fanRequest() const {
        return running() ? p_.fan : NAN;
    }
    const FanTestResult& fanResult() const {
        return fan_;
    }
    // Ends a running test with the heater at 0. `reason` must be a string literal.
    void abort(const char* reason);

    bool running() const {
        return p_.phase != Phase::Idle && p_.phase != Phase::Done && p_.phase != Phase::Failed &&
               p_.phase != Phase::Aborted;
    }
    Phase phase() const {
        return p_.phase;
    }
    const Progress& progress() const {
        return p_;
    }
    const char* failure() const {
        return failure_;
    }
    int pointCount() const {
        return cfg_.pointCount;
    }
    const PointResult& point(int i) const {
        return pts_[i];
    }
    const Config& config() const {
        return cfg_;
    }
    // Valid when phase() == Done.
    const PidGains& recommended() const {
        return rec_;
    }
    int pointsUsed() const {
        return used_;
    }
    int logCount() const {
        return logCount_;
    }
    const LogEntry& logAt(int i) const { // 0 = oldest kept
        return log_[(logHead_ + kLogSize - logCount_ + i) % kLogSize];
    }

    // Increments on every log entry (phase change, cycle, result).
    uint32_t changes() const {
        return changes_;
    }
    void fillReport(Report& r) const;

    static PidGains gainsFor(Rule r, float ku, float tuS, const Model& m);
    // FOPDT from the relay point: static gain from the bias that holds the
    // target, tau and L from the ultimate point (|G(jwu)| = 1/Ku, phase -pi).
    static Model fitModel(float ku, float tuS, float targetC, float ambientC, float bias);
    // Expected run time from `pv` with no measurements (the confirm text).
    static float estimateSeconds(const Config& c, float pv);

private:
    void enter(Phase ph);
    void addLog(float a, float b);
    void fail(const char* why);
    void relayStep(float pv);
    void finishPoint();
    void finishRun();
    void startFanTest();
    void fanHoldStep(float pv);
    void fanShedStep(float pv);
    bool endFanTestEarly(const char* why);
    void finishFanTest(FanTestStatus st, const char* why);
    void enterFanLevel(int i);
    void enterShed();
    void enterShedLevel(int i);
    void shedMeasure();
    void pushTrend(float pv);
    float trendCps() const;
    float leadS() const;
    bool approach(float pv, float tf, float hold, float coastOut);
    float remaining(float pv) const;
    float fanRemaining() const;
    float fanTestTemp() const;
    float holdEstimate() const;
    float relayOut() const {
        return high_ ? bias_ + d_ : bias_ - d_;
    }
    void setStep();

    Config cfg_;
    Progress p_;
    PointResult pts_[kMaxPoints];
    PidGains rec_;
    int used_ = 0;
    const char* failure_ = "";
    float t_ = 0, phaseT_ = 0;
    // heating
    float stallT_ = 0, stallPv_ = 0, rateT_ = 0, ratePv_ = 0, rate_ = 0;
    // relay
    bool high_ = true;
    float bias_ = 50, d_ = 40;
    float lastSwitchT_ = 0, lastLowT_ = -1, lastHighT_ = -1;
    float cycMax_ = -1e9f, cycMin_ = 1e9f;
    float amps_[kMaxCycles] = {}, periods_[kMaxCycles] = {};
    int measured_ = 0, settled_ = 0;
    float lastPeriod_ = 0;
    LogEntry log_[kLogSize] = {};
    int logHead_ = 0, logCount_ = 0;
    uint32_t changes_ = 0;
    // fan test
    FanTestResult fan_;
    Pid hold_{Pid::Config{}};
    float fanStartT_ = 0, fanShedStartT_ = 0, holdPv_ = 0, levelT_ = 0, arriveT_ = -1, blockT_ = 0, satT_ = -1;
    float blockPv_ = 0, blockU_ = 0, prevBlockU_ = NAN;
    int blockN_ = 0;
    float stepT_ = -1, dipMin_ = 0;
    bool recovered_ = false;
    int shedLevel_ = -1;
    // BT trend over the last ~10 s and the approach stage: 0 coasting (heater
    // off from above, full from below), 1 heater at the expected hold, 2 PI.
    static constexpr int kTrendN = 64;
    float trend_[kTrendN];
    int trendN_ = 0, trendI_ = 0;
    uint8_t approach_ = 2;
    bool shedOff_ = false;
    float shedT_ = 0, bandT_ = -1;
    double sn_ = 0, st_ = 0, sp_ = 0, stt_ = 0, stp_ = 0;
};

} // namespace autotune
