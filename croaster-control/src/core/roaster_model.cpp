#include "roaster_model.h"

#include <algorithm>

namespace roastermodel {

namespace {
bool num(float v) {
    return std::isfinite(v);
}

// Points with a usable model, sorted by temperature.
int usablePoints(const RoasterModel& m, const PointModel** out) {
    int n = 0;
    for (int i = 0; i < m.pointCount && i < kMaxPoints; i++) {
        const PointModel& p = m.points[i];
        if (num(p.gainCPerPct) && p.gainCPerPct > 0 && num(p.tauS) && p.tauS > 0 && num(p.holdPct) && p.holdPct > 0 &&
            p.targetC - m.ambientC > 1)
            out[n++] = &p;
    }
    std::sort(out, out + n, [](const PointModel* a, const PointModel* b) { return a->targetC < b->targetC; });
    return n;
}

// Linear in temperature between points, held at the ends. `f` picks the field.
template <class F>
float alongPoints(const RoasterModel& m, float tempC, F f) {
    const PointModel* p[kMaxPoints];
    const int n = usablePoints(m, p);
    if (n == 0)
        return NAN;
    if (tempC <= p[0]->targetC)
        return f(*p[0]);
    for (int i = 1; i < n; i++) {
        if (tempC <= p[i]->targetC) {
            const float x = (tempC - p[i - 1]->targetC) / (p[i]->targetC - p[i - 1]->targetC);
            return f(*p[i - 1]) + x * (f(*p[i]) - f(*p[i - 1]));
        }
    }
    return f(*p[n - 1]);
}

// Holding heater per C above ambient at `fanPct`, from the fan test levels.
float fanConductance(const FanModel& fm, float fanPct, float ambientC) {
    const float rise = fm.testC - ambientC;
    if (!fm.valid || !(rise > 1))
        return NAN;
    const FanLevel* lv[kMaxFanLevels];
    int n = 0;
    for (int i = 0; i < fm.levelCount && i < kMaxFanLevels; i++)
        if (num(fm.levels[i].holdPct))
            lv[n++] = &fm.levels[i];
    if (n == 0)
        return NAN;
    float h = lv[n - 1]->holdPct;
    if (fanPct <= lv[0]->fanPct) {
        h = lv[0]->holdPct;
    } else {
        for (int i = 1; i < n; i++) {
            if (fanPct <= lv[i]->fanPct) {
                const float x = (fanPct - lv[i - 1]->fanPct) / (lv[i]->fanPct - lv[i - 1]->fanPct);
                h = lv[i - 1]->holdPct + x * (lv[i]->holdPct - lv[i - 1]->holdPct);
                break;
            }
        }
    }
    return h / rise;
}

const FanModel* fanOf(const RoasterModel& m, int channel) {
    if (channel < 0 || channel >= kMaxFanChannels || !m.fan[channel].valid)
        return nullptr;
    return &m.fan[channel];
}
} // namespace

RoasterModel fromReport(const autotune::Report& r, float runFanPct) {
    RoasterModel m;
    m.ambientC = r.fan.ambientC > 0 ? r.fan.ambientC : 25;
    m.runFanPct = runFanPct;
    m.pointCount = std::min(r.pointCount, kMaxPoints);
    for (int i = 0; i < m.pointCount; i++) {
        const autotune::PointResult& p = r.points[i];
        PointModel& o = m.points[i];
        o.targetC = p.targetC;
        if (p.status == autotune::PointStatus::Ok) {
            o.gainCPerPct = p.model.gainCPerPct;
            o.tauS = p.model.tauS;
            o.holdPct = p.bias;
        }
    }
    const PointModel* tmp[kMaxPoints];
    m.valid = usablePoints(m, tmp) > 0;
    const autotune::FanTestResult& f = r.fan;
    if ((f.status == autotune::FanTestStatus::Ok || f.status == autotune::FanTestStatus::Partial) &&
        f.channel < kMaxFanChannels) {
        FanModel& fm = m.fan[f.channel];
        fm.testC = f.testC;
        fm.levelCount = std::min(f.levelCount, kMaxFanLevels);
        int held = 0;
        for (int i = 0; i < fm.levelCount; i++) {
            const autotune::FanLevelResult& a = f.levels[i];
            fm.levels[i] = FanLevel{a.fanPct, a.holdPct, a.dipC, a.recoveryS, a.gainCPerPct, a.tauS, a.shedCpm};
            held += num(a.holdPct);
        }
        fm.valid = held >= 2;
    }
    return m;
}

float holdPct(const RoasterModel& m, float tempC, float fanPct, int channel) {
    if (!num(tempC))
        return NAN;
    const float rise = std::max(0.0f, tempC - m.ambientC);
    const FanModel* fm = fanOf(m, channel);
    if (fm) {
        const float g = fanConductance(*fm, fanPct, m.ambientC);
        if (num(g)) {
            // The fan test ran at one temperature; the points give how the loss
            // per degree changes with temperature (radiation, drum wall).
            const float at = alongPoints(m, tempC, [&](const PointModel& p) {
                return p.holdPct / (p.targetC - m.ambientC);
            });
            const float ref = alongPoints(m, fm->testC, [&](const PointModel& p) {
                return p.holdPct / (p.targetC - m.ambientC);
            });
            const float shape = num(at) && num(ref) && ref > 0 ? at / ref : 1;
            return g * shape * rise;
        }
    }
    return alongPoints(m, tempC, [&](const PointModel& p) { return p.holdPct / (p.targetC - m.ambientC); }) * rise;
}

float fanFeedforward(const RoasterModel& m, float fanPct, float tempC, int channel) {
    const FanModel* fm = fanOf(m, channel);
    if (!fm || !num(fanPct) || !num(tempC))
        return 0;
    float lowest = NAN;
    for (int i = 0; i < fm->levelCount; i++)
        if (num(fm->levels[i].holdPct)) {
            lowest = fm->levels[i].fanPct;
            break;
        }
    const float a = holdPct(m, tempC, fanPct, channel), b = holdPct(m, tempC, lowest, channel);
    return num(a) && num(b) ? a - b : 0;
}

float rampFeedforward(const RoasterModel& m, float tempC, float fanPct, float slopeCps) {
    if (!m.valid || !num(slopeCps) || !num(tempC))
        return 0;
    const float tau = alongPoints(m, tempC, [](const PointModel& p) { return p.tauS; });
    const float h = holdPct(m, tempC, fanPct);
    const float rise = tempC - m.ambientC;
    if (!num(tau) || !num(h) || !(h > 0) || !(rise > 1))
        return 0;
    const float k = rise / h; // C per heater % at this fan
    return std::min(kMaxRampFfPct, std::max(-kMaxRampFfPct, tau * slopeCps / k));
}

float maxRorCpm(const RoasterModel& m, float tempC, float fanPct, float heaterMaxPct) {
    if (!m.valid || !num(tempC))
        return NAN;
    const float tau = alongPoints(m, tempC, [](const PointModel& p) { return p.tauS; });
    const float h = holdPct(m, tempC, fanPct);
    const float rise = tempC - m.ambientC;
    if (!num(tau) || !num(h) || !(h > 0) || !(rise > 1))
        return NAN;
    const float k = rise / h;
    return std::max(0.0f, k * (heaterMaxPct - h) / tau * 60);
}

RoasterModel merge(const RoasterModel& previous, const RoasterModel& fresh) {
    RoasterModel m = fresh;
    if (previous.version != kVersion)
        return m;
    for (int c = 0; c < kMaxFanChannels; c++)
        if (!m.fan[c].valid && previous.fan[c].valid)
            m.fan[c] = previous.fan[c];
    return m;
}

float SetpointSlope::update(float sp, float dtS) {
    if (!num(sp) || !(dtS > 0)) {
        reset();
        return 0;
    }
    if (!primed_) {
        primed_ = true;
        last_ = sp;
        slope_ = 0;
        return 0;
    }
    const float d = sp - last_;
    last_ = sp;
    if (std::fabs(d) > kStepC) {
        slope_ = 0;
        return 0;
    }
    const float raw = std::min(kMaxCps, std::max(-kMaxCps, d / dtS));
    slope_ += (raw - slope_) * std::min(1.0f, dtS / kFilterS);
    return slope_;
}

} // namespace roastermodel
