#include "autotune_sequence.h"

#include <algorithm>
#include <cmath>

namespace autotune {

namespace {
constexpr float kPi = 3.14159265f;
constexpr float kRateWindowS = 10;   // heat-up rate sample window
constexpr float kMinSwingRatio = 2;  // amplitude must exceed this many hysteresis bands
constexpr int kExtraSettle = 2;      // settling may run this many cycles past settleCycles
constexpr float kRecoverC = 0.5f;    // fan step: BT back within this of the held temperature
constexpr float kFallCps = 0.3f;     // fan test estimate: heater-off fall toward the held temperature, C/s
constexpr float kArriveC = 3;        // fan hold: BT this close counts as arrived
constexpr float kReheatS = 90;       // fan test estimate: back up to the held temperature after a window
constexpr float kReheatBandC = 2.5f; // reheat: BT this close to the held temperature

bool fanPhase(Phase p) {
    return p == Phase::FanHold || p == Phase::FanShed;
}

float meanOf(const float* v, int n) {
    float s = 0;
    for (int i = 0; i < n; i++)
        s += v[i];
    return s / n;
}
float cvOf(const float* v, int n) {
    const float m = meanOf(v, n);
    if (!(m > 0))
        return INFINITY;
    float s = 0;
    for (int i = 0; i < n; i++)
        s += (v[i] - m) * (v[i] - m);
    return std::sqrt(s / n) / m;
}
void sortUp(float* v, int n) { // n <= kMaxPoints
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && v[j - 1] > v[j]; j--)
            std::swap(v[j - 1], v[j]);
}
float medianOf(float* v, int n) {
    sortUp(v, n);
    return n % 2 ? v[n / 2] : 0.5f * (v[n / 2 - 1] + v[n / 2]);
}
} // namespace

const char* phaseName(Phase p) {
    switch (p) {
    case Phase::Heating: return "heating";
    case Phase::Settling: return "settling";
    case Phase::Oscillating: return "oscillating";
    case Phase::Computing: return "computing";
    case Phase::Done: return "done";
    case Phase::Failed: return "failed";
    case Phase::Aborted: return "aborted";
    case Phase::FanHold: return "fan-hold";
    case Phase::FanShed: return "fan-shed";
    case Phase::Idle:
    default: return "idle";
    }
}

const char* ruleName(Rule r) {
    switch (r) {
    case Rule::TyreusLuybenPid: return "tyreus-luyben-pid";
    case Rule::SimcPi: return "simc-pi";
    case Rule::ZieglerNichols: return "ziegler-nichols";
    case Rule::TyreusLuybenPi: return "tyreus-luyben-pi";
    case Rule::ZieglerNicholsPi:
    default: return "ziegler-nichols-pi";
    }
}

const char* pointStatusName(PointStatus s) {
    switch (s) {
    case PointStatus::Ok: return "ok";
    case PointStatus::Unsettled: return "unsettled";
    case PointStatus::SmallSwing: return "small swing";
    case PointStatus::Pending:
    default: return "pending";
    }
}

int makePlan(const float* requestedC, int n, float minC, float maxSetpointC, Config& cfg) {
    const float hi = std::min(maxSetpointC, cfg.abortAboveC - cfg.maxOvershootC);
    float v[kMaxPoints];
    int k = 0;
    for (int i = 0; i < n && k < kMaxPoints; i++) {
        const float p = requestedC[i];
        if (!std::isfinite(p) || p <= 0)
            continue;
        v[k++] = std::min(std::max(p, minC), hi);
    }
    sortUp(v, k);
    int out = 0;
    for (int i = 0; i < k; i++)
        if (hi >= minC && (out == 0 || v[i] - cfg.pointsC[out - 1] >= 5))
            cfg.pointsC[out++] = v[i];
    for (int i = out; i < kMaxPoints; i++)
        cfg.pointsC[i] = 0;
    cfg.pointCount = out;
    return out;
}

PidGains Sequence::gainsFor(Rule r, float ku, float tu, const Model& m) {
    float kp, ti, td = 0;
    switch (r) {
    case Rule::ZieglerNichols:
        kp = 0.6f * ku, ti = tu / 2, td = tu / 8;
        break;
    case Rule::TyreusLuybenPid:
        kp = ku / 2.2f, ti = 2.2f * tu, td = tu / 6.3f;
        break;
    case Rule::SimcPi:
        // SIMC with tau_c = L: Kc = tau / (2 K L), tau_I = min(tau, 8 L).
        if (std::isfinite(m.gainCPerPct) && std::isfinite(m.tauS) && m.deadS > 0) {
            kp = m.tauS / (2 * m.gainCPerPct * m.deadS);
            ti = std::min(m.tauS, 8 * m.deadS);
            break;
        }
        // no model: fall back to Tyreus-Luyben PI
        kp = ku / 3.22f, ti = 2.2f * tu;
        break;
    case Rule::TyreusLuybenPi:
        kp = ku / 3.22f, ti = 2.2f * tu;
        break;
    case Rule::ZieglerNicholsPi:
    default:
        kp = 0.45f * ku, ti = tu / 1.2f;
        break;
    }
    PidGains g;
    g.kp = kp;
    g.ki = ti > 0 ? kp / ti : 0;
    g.kd = kp * td;
    return g;
}

Model Sequence::fitModel(float ku, float tu, float targetC, float ambientC, float bias) {
    Model m{NAN, NAN, NAN};
    if (!(bias > 0) || !(tu > 0) || !(targetC > ambientC))
        return m;
    m.gainCPerPct = (targetC - ambientC) / bias;
    const float kku = m.gainCPerPct * ku;
    if (!(kku > 1.0f))
        return m; // a gain this low cannot reach -180 degrees with a single lag
    const float w = 2 * kPi / tu;
    m.tauS = std::sqrt(kku * kku - 1) / w;
    m.deadS = (kPi - std::atan(w * m.tauS)) / w;
    return m;
}

const char* fanTestStatusName(FanTestStatus s) {
    switch (s) {
    case FanTestStatus::Running: return "running";
    case FanTestStatus::Ok: return "ok";
    case FanTestStatus::Partial: return "partial";
    case FanTestStatus::Failed: return "failed";
    case FanTestStatus::Skipped:
    default: return "skipped";
    }
}

float Sequence::estimateSeconds(const Config& c, float pv) {
    Sequence s;
    s.cfg_ = c;
    s.p_.pointCount = (uint8_t)c.pointCount;
    s.p_.phase = Phase::Heating;
    return s.remaining(std::isfinite(pv) ? pv : c.ambientC);
}

float Sequence::remaining(float pv) const {
    const float rate = rate_ > 0.02f ? rate_ : cfg_.expectedRateCps;
    const float per = lastPeriod_ > 0 ? lastPeriod_ : cfg_.expectedPeriodS;
    const float perPoint = (cfg_.settleCycles + 1 + cfg_.minCycles) * per;
    float s = 0, from = pv;
    for (int i = p_.point; i < cfg_.pointCount; i++) {
        const float to = cfg_.pointsC[i];
        const bool active = i == p_.point;
        if (!active || p_.phase == Phase::Heating) {
            s += std::max(0.0f, to - cfg_.approachC - from) / rate;
            s += perPoint;
        } else if (p_.phase == Phase::Settling) {
            s += (cfg_.settleCycles - settled_ + cfg_.minCycles + 0.5f) * per;
        } else if (p_.phase == Phase::Oscillating) {
            s += std::max(cfg_.minCycles - measured_, 1) * per;
        }
        from = to;
    }
    s = std::min(s, std::max(0.0f, cfg_.totalTimeoutS - t_));
    return s + fanRemaining();
}

// Fan test time still to come: the heater-off fall from the top point to the
// held temperature, each level's minimum hold plus the blocks a PI loop
// typically needs to settle after a fan step, and the shed windows with their reheats.
float Sequence::fanRemaining() const {
    if (!cfg_.fanTest || cfg_.fanLevelCount <= 0 || cfg_.pointCount <= 0)
        return 0;
    const int n = cfg_.fanLevelCount;
    const float window = cfg_.fanShedSkipS + cfg_.fanShedWindowS;
    const float shed = n * window + (n - 1) * (kReheatS + cfg_.fanReheatHoldS);
    const float hold = cfg_.fanSettleMinS + 4 * cfg_.fanBlockS;
    float s = 0;
    if (p_.phase == Phase::FanShed) {
        s = std::max(0.0f, shed - (t_ - fanShedStartT_));
    } else if (p_.phase == Phase::FanHold) {
        const float into = arriveT_ >= 0 ? t_ - arriveT_ : 0;
        s = std::max(0.0f, hold - into) + (n - 1 - p_.fanLevel) * hold + shed;
        if (arriveT_ < 0)
            s += std::max(0.0f, holdPv_ - fan_.testC) / kFallCps;
    } else {
        const float top = cfg_.pointsC[cfg_.pointCount - 1];
        s = std::max(0.0f, top - fanTestTemp()) / kFallCps + n * hold + shed;
    }
    const float left = fanPhase(p_.phase) ? cfg_.fanTimeoutS - (t_ - fanStartT_) : cfg_.fanTimeoutS;
    return std::min(s, std::max(0.0f, left));
}

float Sequence::holdEstimate() const {
    const float tf = fan_.testC, amb = cfg_.ambientC;
    int best = -1;
    for (int i = 0; i < cfg_.pointCount; i++)
        if (pts_[i].bias > 0 && (best < 0 || std::fabs(pts_[i].targetC - tf) < std::fabs(pts_[best].targetC - tf)))
            best = i;
    if (best < 0 || !(pts_[best].targetC - amb > 1))
        return 0;
    return std::min(cfg_.outputMax, pts_[best].bias * (tf - amb) / (pts_[best].targetC - amb));
}

float Sequence::fanTestTemp() const {
    if (cfg_.fanTestC > 0)
        return cfg_.fanTestC;
    return cfg_.pointCount > 0 ? cfg_.pointsC[(cfg_.pointCount - 1) / 2] : 0;
}

void Sequence::start(const Config& c, float pv) {
    start(c, pv, NAN);
}

void makeFanLevels(Config& cfg) {
    const int n = std::max(0, std::min(cfg.fanLevelCount, kMaxFanLevels));
    float v[kMaxFanLevels];
    int k = 0;
    for (int i = 0; i < n; i++)
        if (std::isfinite(cfg.fanLevels[i]) && cfg.fanLevels[i] > 0)
            v[k++] = std::min(100.0f, cfg.fanLevels[i]);
    std::sort(v, v + k);
    int m = 0;
    for (int i = 0; i < k; i++)
        if (m == 0 || v[i] - v[m - 1] >= 5)
            v[m++] = v[i];
    for (int i = 0; i < kMaxFanLevels; i++)
        cfg.fanLevels[i] = i < m ? v[i] : 0;
    cfg.fanLevelCount = m;
}

void Sequence::start(const Config& c, float pv, float fanPct) {
    const uint32_t changes = changes_;
    *this = Sequence();
    changes_ = changes + 1; // a new run always reads as a change
    cfg_ = c;
    makeFanLevels(cfg_);
    fan_.startFanPct = fanPct;
    fan_.ambientC = cfg_.ambientC;
    cfg_.pointCount = std::max(0, std::min(cfg_.pointCount, kMaxPoints));
    cfg_.minCycles = std::max(2, std::min(cfg_.minCycles, kMaxCycles));
    cfg_.maxCycles = std::max(cfg_.minCycles, std::min(cfg_.maxCycles, kMaxCycles));
    p_.pointCount = (uint8_t)cfg_.pointCount;
    p_.fanLevelCount = (uint8_t)(cfg_.fanTest ? cfg_.fanLevelCount : 0); // known from the start, for the step list
    for (int i = 0; i < cfg_.pointCount; i++)
        pts_[i].targetC = cfg_.pointsC[i];
    bias_ = cfg_.initialBias;
    if (cfg_.pointCount == 0) {
        p_.phase = Phase::Heating; // so fail() records it
        fail("no test points");
        return;
    }
    p_.targetC = cfg_.pointsC[0];
    enter(Phase::Heating);
    if (std::isfinite(pv) && pv > p_.targetC + cfg_.maxOvershootC)
        fail("too hot to start");
    p_.remainingS = remaining(std::isfinite(pv) ? pv : cfg_.ambientC);
}

void Sequence::enter(Phase ph) {
    p_.phase = ph;
    phaseT_ = t_;
    p_.cycle = 0;
    p_.cycleOf = ph == Phase::Settling ? (uint8_t)cfg_.settleCycles
                 : ph == Phase::Oscillating ? (uint8_t)cfg_.maxCycles
                                            : 0;
    if (ph == Phase::Heating) {
        stallT_ = rateT_ = t_;
        stallPv_ = NAN; // taken from the next sample
        p_.amplitudeC = p_.periodS = 0;
    }
    if (ph == Phase::Settling || ph == Phase::Oscillating) {
        lastSwitchT_ = t_;
        if (ph == Phase::Settling) {
            lastLowT_ = lastHighT_ = -1;
            settled_ = 0;
        }
        measured_ = 0;
    }
    addLog(p_.targetC, 0);
}

void Sequence::addLog(float a, float b) {
    LogEntry& e = log_[logHead_];
    e = LogEntry{t_, p_.phase, p_.point, p_.cycle, a, b};
    logHead_ = (logHead_ + 1) % kLogSize;
    logCount_ = std::min(logCount_ + 1, kLogSize);
    changes_++;
}

void Sequence::fillReport(Report& r) const {
    r.seq = changes_;
    r.phase = p_.phase;
    r.failure = failure_;
    r.rule = cfg_.rule;
    r.pointCount = cfg_.pointCount;
    for (int i = 0; i < kMaxPoints; i++)
        r.points[i] = pts_[i];
    r.recommended = rec_;
    r.used = used_;
    r.logCount = logCount_;
    for (int i = 0; i < logCount_; i++)
        r.log[i] = logAt(i);
    r.fan = fan_;
}

// The gains are computed before the fan test starts, so a stop or a fault
// during the fan test ends only the fan test: the run finishes with the gains
// and whatever fan levels were measured.
bool Sequence::endFanTestEarly(const char* why) {
    if (p_.phase != Phase::FanHold && p_.phase != Phase::FanShed)
        return false;
    finishFanTest(FanTestStatus::Partial, why);
    return true;
}

void Sequence::fail(const char* why) {
    if (!running() || endFanTestEarly(why))
        return;
    failure_ = why;
    p_.output = 0;
    p_.remainingS = 0;
    p_.phase = Phase::Failed;
    addLog(0, 0);
}

void Sequence::abort(const char* reason) {
    if (!running() || endFanTestEarly(reason))
        return;
    failure_ = reason;
    p_.output = 0;
    p_.remainingS = 0;
    p_.phase = Phase::Aborted;
    addLog(0, 0);
}

void Sequence::setStep() {
    bias_ = std::min(std::max(bias_, cfg_.minBias), cfg_.outputMax - cfg_.minBias);
    d_ = std::min(cfg_.relayStep, std::min(bias_, cfg_.outputMax - bias_));
}

float Sequence::update(float pv) {
    if (!running())
        return 0;
    t_ += cfg_.dtSeconds;
    p_.elapsedS = t_;
    if (!std::isfinite(pv)) {
        fail("sensor fault");
        return 0;
    }
    if (pv > cfg_.abortAboveC) {
        fail("over temperature");
        return 0;
    }
    // The fan test starts from the top point, above its own held temperature.
    const bool fanRun = fanPhase(p_.phase);
    const float ref = fanRun ? std::max(p_.targetC, cfg_.pointsC[cfg_.pointCount - 1]) : p_.targetC;
    if (pv > ref + cfg_.maxOvershootC) {
        fail("overshoot");
        return 0;
    }
    if (!fanRun && t_ > cfg_.totalTimeoutS) {
        fail("time limit");
        return 0;
    }
    if (fanRun && t_ - fanStartT_ > cfg_.fanTimeoutS) {
        finishFanTest(FanTestStatus::Partial, "time limit");
        return 0;
    }

    switch (p_.phase) {
    case Phase::Heating:
        if (std::isnan(stallPv_))
            stallPv_ = ratePv_ = pv;
        if (t_ - rateT_ >= kRateWindowS) {
            const float r = (pv - ratePv_) / (t_ - rateT_);
            rate_ = rate_ > 0 ? 0.7f * rate_ + 0.3f * r : r;
            rateT_ = t_, ratePv_ = pv;
        }
        if (pv >= p_.targetC - cfg_.approachC) {
            if (p_.point > 0 && pts_[p_.point - 1].bias > 0) {
                // Heat loss grows about linearly with the rise over ambient.
                const PointResult& prev = pts_[p_.point - 1];
                bias_ = prev.bias * (p_.targetC - cfg_.ambientC) / (prev.targetC - cfg_.ambientC);
            }
            setStep();
            high_ = pv < p_.targetC;
            enter(Phase::Settling);
            break;
        }
        if (t_ - stallT_ >= cfg_.stallSeconds) {
            if (pv - stallPv_ < cfg_.minRiseC) {
                fail("no heat-up");
                return 0;
            }
            stallT_ = t_, stallPv_ = pv;
        }
        p_.output = cfg_.outputMax;
        break;
    case Phase::Settling:
    case Phase::Oscillating:
        relayStep(pv);
        if (!running())
            return 0;
        break;
    case Phase::Computing:
        finishPoint();
        break;
    case Phase::FanHold:
        fanHoldStep(pv);
        break;
    case Phase::FanShed:
        fanShedStep(pv);
        break;
    default:
        break;
    }
    if (running())
        p_.remainingS = remaining(pv);
    return running() ? p_.output : 0;
}

void Sequence::relayStep(float pv) {
    cycMax_ = std::max(cycMax_, pv);
    cycMin_ = std::min(cycMin_, pv);
    if (t_ - lastSwitchT_ > cfg_.cycleTimeoutS) {
        fail("no oscillation");
        return;
    }
    const float h = cfg_.hysteresisC;
    if (!high_ && pv < p_.targetC - h) {
        high_ = true;
        lastSwitchT_ = t_;
        lastHighT_ = t_;
    } else if (high_ && pv > p_.targetC + h) {
        high_ = false;
        lastSwitchT_ = t_;
        // A cycle is switch-low to switch-low; its swing spans both peaks.
        if (lastLowT_ >= 0 && lastHighT_ > lastLowT_) {
            const float period = t_ - lastLowT_;
            const float tLow = lastHighT_ - lastLowT_, tHigh = t_ - lastHighT_;
            const float amp = (cycMax_ - cycMin_) / 2;
            p_.amplitudeC = amp, p_.periodS = period;
            lastPeriod_ = period;
            if (p_.phase == Phase::Settling) {
                // Centre the duty: more time high than low means the bias is short.
                bias_ += d_ * (tHigh - tLow) / (tHigh + tLow);
                setStep();
                settled_++;
                p_.cycle = (uint8_t)std::min(settled_, 255);
                const float asym = std::fabs(tHigh - tLow) / (tHigh + tLow);
                addLog(amp, period);
                if (settled_ >= cfg_.settleCycles && (asym < 0.2f || settled_ >= cfg_.settleCycles + kExtraSettle))
                    enter(Phase::Oscillating);
            } else {
                amps_[measured_] = amp, periods_[measured_] = period;
                measured_++;
                p_.cycle = (uint8_t)measured_;
                addLog(amp, period);
                bool converged = false;
                if (measured_ >= cfg_.minCycles) {
                    const int n = cfg_.minCycles, from = measured_ - n;
                    converged = cvOf(amps_ + from, n) < cfg_.maxCv && cvOf(periods_ + from, n) < cfg_.maxCv;
                }
                if (converged || measured_ >= cfg_.maxCycles) {
                    pts_[p_.point].status = converged ? PointStatus::Ok : PointStatus::Unsettled;
                    p_.phase = Phase::Computing;
                    p_.output = 0;
                    phaseT_ = t_;
                    addLog(0, 0);
                    cycMax_ = -1e9f, cycMin_ = 1e9f;
                    lastLowT_ = t_;
                    return;
                }
            }
        }
        lastLowT_ = t_;
        cycMax_ = -1e9f, cycMin_ = 1e9f;
    }
    p_.output = relayOut();
}

void Sequence::finishPoint() {
    PointResult& r = pts_[p_.point];
    const int n = std::min(cfg_.minCycles, measured_), from = measured_ - n;
    r.cycles = (uint8_t)measured_;
    r.amplitudeC = meanOf(amps_ + from, n);
    r.periodS = meanOf(periods_ + from, n);
    r.amplitudeCv = cvOf(amps_ + from, n);
    r.periodCv = cvOf(periods_ + from, n);
    r.bias = bias_, r.step = d_;
    const float h = cfg_.hysteresisC, a = r.amplitudeC;
    if (a < kMinSwingRatio * h)
        r.status = PointStatus::SmallSwing;
    // Describing function of a relay with hysteresis: N(a) = 4d / (pi sqrt(a^2 - h^2)).
    r.ku = a > h ? 4 * d_ / (kPi * std::sqrt(a * a - h * h)) : 0;
    r.tuS = r.periodS;
    r.model = fitModel(r.ku, r.tuS, r.targetC, cfg_.ambientC, r.bias);
    r.gains = gainsFor(cfg_.rule, r.ku, r.tuS, r.model);
    addLog(r.ku, r.tuS);

    if (p_.point + 1 < cfg_.pointCount) {
        p_.point++;
        p_.targetC = cfg_.pointsC[p_.point];
        enter(Phase::Heating);
        p_.output = cfg_.outputMax;
        return;
    }
    finishRun();
}

void Sequence::finishRun() {
    // Conservative envelope over the consistent points: the lowest Kp, the
    // slowest integral (largest Ti) and the median Td. Each point's own rule
    // set is at least this cautious in P and I.
    float kps[kMaxPoints], tis[kMaxPoints], tds[kMaxPoints];
    int n = 0;
    for (int i = 0; i < cfg_.pointCount; i++) {
        const PointResult& r = pts_[i];
        if (r.status != PointStatus::Ok || !(r.gains.kp > 0) || !(r.gains.ki > 0))
            continue;
        kps[n] = r.gains.kp;
        tis[n] = r.gains.kp / r.gains.ki;
        tds[n] = r.gains.kd / r.gains.kp;
        n++;
    }
    used_ = n;
    if (n == 0) {
        p_.output = 0;
        p_.phase = Phase::Computing; // still running for fail()
        fail("no consistent oscillation");
        return;
    }
    const float kp = *std::min_element(kps, kps + n);
    const float ti = *std::max_element(tis, tis + n);
    const float td = medianOf(tds, n);
    rec_.kp = kp;
    rec_.ki = kp / ti;
    rec_.kd = kp * td;
    p_.output = 0;
    if (cfg_.fanTest && cfg_.fanLevelCount > 0) {
        startFanTest();
        return;
    }
    p_.remainingS = 0;
    p_.phase = Phase::Done;
    addLog(rec_.kp, rec_.ki);
}

// ---------- fan test (docs/autotune/fan-test.md) ----------
// Hold the test temperature with PI from the new gains at each fan level in
// turn (ascending, the way a roaster raises airflow through a roast), then
// turn the heater off and step back down through the levels, timing the fall.

void Sequence::startFanTest() {
    fan_.status = FanTestStatus::Running;
    fan_.channel = cfg_.fanChannel;
    fan_.testC = fanTestTemp();
    fan_.ambientC = cfg_.ambientC;
    fan_.levelCount = cfg_.fanLevelCount;
    for (int i = 0; i < fan_.levelCount; i++)
        fan_.levels[i] = FanLevelResult{}, fan_.levels[i].fanPct = cfg_.fanLevels[i];
    fanStartT_ = t_;
    holdPv_ = p_.targetC;
    Pid::Config pc;
    pc.dtSeconds = cfg_.dtSeconds;
    pc.outMin = 0;
    pc.outMax = cfg_.outputMax;
    hold_ = Pid(pc);
    hold_.setGains(rec_);
    p_.targetC = fan_.testC;
    p_.fanLevelCount = (uint8_t)fan_.levelCount;
    enterFanLevel(0);
}

void Sequence::enterFanLevel(int i) {
    p_.phase = Phase::FanHold;
    phaseT_ = levelT_ = t_;
    p_.fanLevel = (uint8_t)i;
    p_.fan = cfg_.fanLevels[i];
    p_.cycle = 0, p_.cycleOf = 0;
    blockT_ = t_, blockPv_ = blockU_ = 0, blockN_ = 0, prevBlockU_ = NAN;
    satT_ = -1;
    if (i == 0) {
        arriveT_ = -1;
        approach_ = 0;
        stepT_ = -1;
    } else {
        arriveT_ = t_; // already at the held temperature
        stepT_ = t_;
        dipMin_ = 1e9f;
        recovered_ = false;
    }
    addLog(p_.fan, 0);
}

void Sequence::pushTrend(float pv) {
    trend_[trendI_] = pv;
    trendI_ = (trendI_ + 1) % kTrendN;
    if (trendN_ < kTrendN)
        trendN_++;
}

// BT slope, C/s, over the last ~10 s (0 until enough samples).
float Sequence::trendCps() const {
    const int span = std::min(kTrendN - 1, std::max(1, (int)(10 / cfg_.dtSeconds)));
    if (trendN_ <= span)
        return 0;
    const float now = trend_[(trendI_ + kTrendN - 1) % kTrendN];
    const float then = trend_[(trendI_ + kTrendN - 1 - span) % kTrendN];
    return (now - then) / (span * cfg_.dtSeconds);
}

// How far ahead to act on the trend: the heat path's delay, taken as twice the
// dead time the points fitted (element plus probe lag), 15..60 s.
float Sequence::leadS() const {
    float dead = 0;
    int n = 0;
    for (int i = 0; i < cfg_.pointCount; i++)
        if (std::isfinite(pts_[i].model.deadS) && pts_[i].model.deadS > 0)
            dead += pts_[i].model.deadS, n++;
    const float lead = n > 0 ? 2 * dead / n : 30;
    return std::min(60.0f, std::max(15.0f, lead));
}

// Brings BT to tf without a large overshoot: coastOut until the trend projects
// BT reaching tf within the lead time, then the expected hold until BT stops
// moving toward tf or crosses it, then PI with the integrator at the hold.
// Returns true once PI is running.
bool Sequence::approach(float pv, float tf, float hold, float coastOut) {
    if (approach_ >= 2) {
        p_.output = hold_.update(tf, pv);
        return true;
    }
    const float rate = trendCps();
    const float ahead = pv + rate * leadS();
    const bool above = pv > tf;
    if (approach_ == 0) {
        const bool near = std::fabs(pv - tf) <= kArriveC;
        if (near || (above ? ahead <= tf : ahead >= tf))
            approach_ = 1;
    }
    if (approach_ == 1) {
        const bool settled = above ? (rate >= -0.02f || pv <= tf) : (rate <= 0.02f || pv >= tf);
        if (settled || std::fabs(pv - tf) <= 0.5f) {
            approach_ = 2;
            hold_.reset(tf, pv, hold + rec_.kp * (tf - pv));
            p_.output = hold_.update(tf, pv);
            return true;
        }
        p_.output = hold;
        return false;
    }
    p_.output = coastOut;
    return false;
}

void Sequence::fanHoldStep(float pv) {
    FanLevelResult& L = fan_.levels[p_.fanLevel];
    const float tf = fan_.testC;
    pushTrend(pv);
    if (arriveT_ >= 0)
        p_.output = hold_.update(tf, pv);
    // Dip and recovery after the step into this level.
    if (stepT_ >= 0 && !recovered_) {
        dipMin_ = std::min(dipMin_, pv);
        if (pv >= tf - kRecoverC && (tf - dipMin_ > kRecoverC || t_ - stepT_ > cfg_.fanBlockS)) {
            recovered_ = true;
            L.dipC = std::max(0.0f, tf - dipMin_);
            L.recoveryS = tf - dipMin_ > kRecoverC ? t_ - stepT_ : 0;
        }
    }
    if (arriveT_ < 0) {
        // Falling from the top point: coast with the heater off, then give the
        // hold the points measured (scaled to this temperature) before BT gets
        // there, so the drum's momentum does not carry it far below.
        if (approach(pv, tf, holdEstimate(), 0)) {
            arriveT_ = t_;
            blockT_ = t_, blockPv_ = blockU_ = 0, blockN_ = 0;
        } else if (t_ - levelT_ > cfg_.fanApproachMaxS) {
            finishFanTest(FanTestStatus::Failed, "no steady hold");
            return;
        }
        return;
    }
    // Heater pinned at its maximum: this airflow takes more heat than the
    // element gives at this temperature, so higher levels cannot be held either.
    if (p_.output >= cfg_.outputMax - 0.01f) {
        if (satT_ < 0)
            satT_ = t_;
        if (t_ - satT_ >= cfg_.fanSaturateS) {
            enterShed();
            return;
        }
    } else {
        satT_ = -1;
    }
    blockPv_ += pv, blockU_ += p_.output, blockN_++;
    if (t_ - blockT_ >= cfg_.fanBlockS && blockN_ > 0) {
        const float pvMean = blockPv_ / blockN_, uMean = blockU_ / blockN_;
        const bool steady = t_ - arriveT_ >= cfg_.fanSettleMinS && std::fabs(pvMean - tf) < cfg_.fanHoldBandC &&
                            std::isfinite(prevBlockU_) && std::fabs(uMean - prevBlockU_) < cfg_.fanHoldDriftPct;
        prevBlockU_ = uMean;
        blockT_ = t_, blockPv_ = blockU_ = 0, blockN_ = 0;
        p_.cycle++;
        if (steady) {
            // Heat loss scales with BT above ambient: quote the hold at tf.
            const float rise = pvMean - fan_.ambientC;
            L.holdPct = rise > 1 ? uMean * (tf - fan_.ambientC) / rise : uMean;
            if (stepT_ >= 0 && !recovered_) // never fell more than the band
                L.dipC = std::max(0.0f, tf - dipMin_), L.recoveryS = 0;
            addLog(L.fanPct, L.holdPct);
            if (p_.fanLevel + 1 < fan_.levelCount)
                enterFanLevel(p_.fanLevel + 1);
            else
                enterShed();
            return;
        }
    }
    if (t_ - arriveT_ > cfg_.fanSettleMaxS) {
        if (p_.fanLevel == 0) {
            finishFanTest(FanTestStatus::Failed, "no steady hold");
            return;
        }
        enterShed(); // keep the levels already measured
    }
}

void Sequence::enterShed() {
    p_.phase = Phase::FanShed;
    phaseT_ = fanShedStartT_ = t_;
    // From the highest level that held (BT is steady there now) down to the first.
    int top = 0;
    for (int i = 0; i < fan_.levelCount; i++)
        if (std::isfinite(fan_.levels[i].holdPct))
            top = i;
    enterShedLevel(top);
    shedOff_ = true; // already steady at this level: straight to the heater-off window
    shedT_ = t_;
}

// Next level: hold the test temperature again at this fan before the window.
void Sequence::enterShedLevel(int i) {
    shedLevel_ = i;
    p_.fanLevel = (uint8_t)i;
    p_.fan = cfg_.fanLevels[i];
    shedOff_ = false;
    shedT_ = t_;
    bandT_ = -1;
    approach_ = 0;
    sn_ = st_ = sp_ = stt_ = stp_ = 0;
    addLog(p_.fan, 0);
}

void Sequence::fanShedStep(float pv) {
    const float tf = fan_.testC;
    pushTrend(pv);
    if (!shedOff_) {
        // Full heat back up, then this level's measured hold, then PI.
        if (!approach(pv, tf, fan_.levels[shedLevel_].holdPct, cfg_.outputMax)) {
            if (t_ - shedT_ > cfg_.fanSettleMaxS)
                finishFanTest(FanTestStatus::Partial, "no steady hold");
            return;
        }
        if (std::fabs(pv - tf) <= kReheatBandC) {
            if (bandT_ < 0)
                bandT_ = t_;
        } else {
            bandT_ = -1;
        }
        if (bandT_ >= 0 && t_ - bandT_ >= cfg_.fanReheatHoldS) {
            shedOff_ = true;
            shedT_ = t_;
        } else if (t_ - shedT_ > cfg_.fanSettleMaxS) {
            finishFanTest(FanTestStatus::Partial, "no steady hold");
        }
        return;
    }
    p_.output = 0;
    const float into = t_ - shedT_;
    if (into > cfg_.fanShedSkipS) {
        const double x = into - cfg_.fanShedSkipS;
        sn_ += 1, st_ += x, sp_ += pv, stt_ += x * x, stp_ += x * pv;
    }
    // Bounded fall: the drum stays in the roasting range.
    const bool bound = pv < tf - cfg_.fanShedMaxDropC;
    if (into < cfg_.fanShedSkipS + cfg_.fanShedWindowS && !bound)
        return;
    shedMeasure();
    if (shedLevel_ == 0) {
        finishFanTest(FanTestStatus::Ok, "");
        return;
    }
    enterShedLevel(shedLevel_ - 1);
}

// Least-squares slope of BT over the window: the heater-off fall rate. With
// Newton cooling dT/dt = -k (T - ambient), k from the slope at the window's
// mean BT; the rate is then quoted at the test temperature.
void Sequence::shedMeasure() {
    FanLevelResult& L = fan_.levels[shedLevel_];
    const double den = sn_ * stt_ - st_ * st_;
    if (sn_ >= 20 && den > 0) {
        const double slope = (sn_ * stp_ - st_ * sp_) / den; // C/s, negative
        const float rise = (float)(sp_ / sn_) - fan_.ambientC;
        if (slope < 0 && rise > 1) {
            const float k = (float)(-slope) / rise;
            L.tauS = 1 / k;
            L.shedCpm = k * (fan_.testC - fan_.ambientC) * 60;
        }
    }
    addLog(L.fanPct, std::isfinite(L.shedCpm) ? L.shedCpm : 0);
}

void Sequence::finishFanTest(FanTestStatus st, const char* why) {
    const float rise = fan_.testC - fan_.ambientC;
    int held = 0, shed = 0;
    for (int i = 0; i < fan_.levelCount; i++) {
        held += std::isfinite(fan_.levels[i].holdPct);
        shed += std::isfinite(fan_.levels[i].shedCpm);
    }
    // Open-loop fan gain at a fixed heater: steady BT - ambient = heat in / loss
    // conductance, and the hold heater is proportional to that conductance, so
    // dBT/dfan = -(BT - ambient) * (dH/dfan) / H.
    for (int i = 0; i < fan_.levelCount; i++) {
        FanLevelResult& L = fan_.levels[i];
        if (!std::isfinite(L.holdPct) || !(L.holdPct > 0))
            continue;
        int a = i > 0 && std::isfinite(fan_.levels[i - 1].holdPct) ? i - 1 : i;
        int b = i + 1 < fan_.levelCount && std::isfinite(fan_.levels[i + 1].holdPct) ? i + 1 : i;
        if (a == b)
            continue;
        const float dh = fan_.levels[b].holdPct - fan_.levels[a].holdPct;
        const float df = fan_.levels[b].fanPct - fan_.levels[a].fanPct;
        if (df > 0)
            L.gainCPerPct = -rise * (dh / df) / L.holdPct;
    }
    if (st == FanTestStatus::Ok && (held < fan_.levelCount || shed < fan_.levelCount))
        st = held >= 2 ? FanTestStatus::Partial : FanTestStatus::Failed;
    if (st == FanTestStatus::Partial && held < 2)
        st = FanTestStatus::Failed;
    if (st == FanTestStatus::Failed && !*why)
        why = "too few levels";
    fan_.status = st;
    fan_.failure = why;
    fan_.durationS = t_ - fanStartT_;
    p_.fan = NAN;
    p_.output = 0;
    p_.remainingS = 0;
    p_.phase = Phase::Done;
    addLog(rec_.kp, rec_.ki);
}

} // namespace autotune
