#include "autotune_protocol.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "autotune_status.h"

namespace autotuneproto {

namespace {
using autotune::Phase;

template <typename T>
void putNum(T&& v, float x, float scale = 10.0f) { // JSON has no NaN
    if (std::isfinite(x))
        v = std::round(x * scale) / scale;
    else
        v = nullptr;
}
template <typename T>
void putSig(T&& v, float x) { // gains: four significant digits
    if (!std::isfinite(x)) {
        v = nullptr;
        return;
    }
    if (x == 0) {
        v = 0;
        return;
    }
    const float m = std::pow(10.0f, 3 - std::floor(std::log10(std::fabs(x))));
    v = std::round(x * m) / m;
}
float delta(float c, char unit) { // a temperature difference
    return unit == 'F' ? c * 9.0f / 5.0f : c;
}
} // namespace

bool parseAction(JsonVariantConst v, Command& out) {
    const char* a = v.as<const char*>();
    if (!a)
        return false;
    if (std::strcmp(a, "apply") == 0)
        out = Command{Command::Kind::ApplyAutotune};
    else if (std::strcmp(a, "discard") == 0)
        out = Command{Command::Kind::DiscardAutotune};
    else
        return false;
    return true;
}

void logText(const autotune::LogEntry& e, const autotune::Report& r, char unit, char* out, size_t n) {
    const int pt = e.point + 1;
    switch (e.phase) {
    case Phase::Heating:
        snprintf(out, n, "point %d: heating to %.0f %c", pt, fromCelsius(e.a, unit), unit);
        break;
    case Phase::Settling:
        if (e.cycle == 0)
            snprintf(out, n, "point %d: settling at %.0f %c", pt, fromCelsius(e.a, unit), unit);
        else
            snprintf(out, n, "point %d: settling cycle %d, swing %.1f %c, period %.0f s", pt, e.cycle,
                     delta(e.a, unit), unit, e.b);
        break;
    case Phase::Oscillating:
        if (e.cycle == 0)
            snprintf(out, n, "point %d: measuring at %.0f %c", pt, fromCelsius(e.a, unit), unit);
        else
            snprintf(out, n, "point %d: cycle %d, swing %.1f %c, period %.0f s", pt, e.cycle, delta(e.a, unit),
                     unit, e.b);
        break;
    case Phase::Computing:
        if (e.a == 0 && e.b == 0)
            snprintf(out, n, "point %d: computing", pt);
        else
            snprintf(out, n, "point %d: Ku %.3g, Tu %.0f s (%s)", pt, e.a, e.b,
                     autotune::pointStatusName(r.points[e.point].status));
        break;
    case Phase::FanHold:
        if (e.b == 0)
            snprintf(out, n, "fan test: hold at fan %.0f %%", e.a);
        else
            snprintf(out, n, "fan test: fan %.0f %% holds with heater %.1f %%", e.a, e.b);
        break;
    case Phase::FanShed:
        if (e.b == 0)
            snprintf(out, n, "fan test: heat-shed check at fan %.0f %%", e.a);
        else
            snprintf(out, n, "fan test: fan %.0f %% sheds %.1f %c/min", e.a, delta(e.b, unit), unit);
        break;
    case Phase::Done:
        snprintf(out, n, "done: Kp %.3g, Ki %.3g", e.a, e.b);
        break;
    case Phase::Failed:
        snprintf(out, n, "failed: %s", r.failure);
        break;
    case Phase::Aborted:
        snprintf(out, n, "aborted: %s", r.failure);
        break;
    default:
        snprintf(out, n, "idle");
        break;
    }
}

void writeReport(const Snapshot& s, const autotune::Report& r, const autotune::Config& plan, float estimateS,
                 char unit, JsonObject o, const roastermodel::RoasterModel* saved, float heaterMaxPct,
                 float fanEstimateS) {
    char text[64];
    autotunetext::full(s, unit, text, sizeof text);
    o["status"] = text;
    char u[2] = {unit, 0};
    o["unit"] = u; // copied
    const bool running = s.mode == Mode::Autotune;
    o["running"] = running;
    o["pending"] = s.autotunePending;
    o["phase"] = running ? autotune::phaseName(s.autotuneProgress.phase) : autotune::phaseName(r.phase);
    o["failure"] = r.failure;

    JsonObject pl = o["plan"].to<JsonObject>();
    JsonArray pts = pl["points"].to<JsonArray>();
    for (int i = 0; i < plan.pointCount; i++)
        pts.add(std::round(fromCelsius(plan.pointsC[i], unit)));
    pl["settleCycles"] = plan.settleCycles;
    pl["minCycles"] = plan.minCycles;
    pl["maxCycles"] = plan.maxCycles;
    putNum(pl["maxCv"], plan.maxCv, 100.0f);
    pl["rule"] = autotune::ruleName(plan.rule);
    putNum(pl["estimate"], estimateS, 1.0f);
    putNum(pl["timeLimit"], plan.totalTimeoutS, 1.0f);
    pl["fanTest"] = plan.fanTest && plan.fanLevelCount > 0;
    JsonArray fl = pl["fanLevels"].to<JsonArray>();
    for (int i = 0; i < plan.fanLevelCount && i < autotune::kMaxFanLevels; i++)
        fl.add(plan.fanLevels[i]);
    {
        const float tf = plan.fanTestC > 0 ? plan.fanTestC
                         : plan.pointCount > 0 ? plan.pointsC[(plan.pointCount - 1) / 2]
                                               : NAN;
        putNum(pl["fanTarget"], fromCelsius(tf, unit), 1.0f);
    }
    putNum(pl["fanTimeLimit"], plan.fanTimeoutS, 1.0f);
    putNum(pl["fanEstimate"], fanEstimateS, 1.0f);

    if (running) {
        const autotune::Progress& p = s.autotuneProgress;
        JsonObject g = o["progress"].to<JsonObject>();
        g["point"] = p.point;
        g["points"] = p.pointCount;
        g["cycle"] = p.cycle;
        g["cycleOf"] = p.cycleOf;
        g["fanLevel"] = p.fanLevel;
        g["fanLevels"] = p.fanLevelCount;
        putNum(g["fan"], p.fan, 1.0f);
        putNum(g["target"], fromCelsius(p.targetC, unit));
        putNum(g["output"], p.output);
        putNum(g["amplitude"], delta(p.amplitudeC, unit));
        putNum(g["period"], p.periodS, 1.0f);
        putNum(g["elapsed"], p.elapsedS, 1.0f);
        putNum(g["remaining"], p.remainingS, 1.0f);
    }

    JsonObject cur = o["current"].to<JsonObject>();
    putSig(cur["kp"], s.gains.kp);
    putSig(cur["ki"], s.gains.ki);
    putSig(cur["kd"], s.gains.kd);

    JsonArray res = o["results"].to<JsonArray>();
    for (int i = 0; i < r.pointCount; i++) {
        const autotune::PointResult& p = r.points[i];
        if (p.status == autotune::PointStatus::Pending)
            continue;
        JsonObject e = res.add<JsonObject>();
        putNum(e["target"], fromCelsius(p.targetC, unit), 1.0f);
        e["status"] = autotune::pointStatusName(p.status);
        e["cycles"] = p.cycles;
        putNum(e["amplitude"], delta(p.amplitudeC, unit), 100.0f);
        putNum(e["period"], p.periodS);
        putNum(e["amplitudeCv"], p.amplitudeCv, 1000.0f);
        putNum(e["periodCv"], p.periodCv, 1000.0f);
        putNum(e["bias"], p.bias);
        putNum(e["step"], p.step);
        putSig(e["ku"], p.ku);
        putNum(e["tu"], p.tuS);
        JsonObject m = e["model"].to<JsonObject>();
        putSig(m["gain"], p.model.gainCPerPct);
        putNum(m["tau"], p.model.tauS, 1.0f);
        putNum(m["dead"], p.model.deadS);
        putSig(e["kp"], p.gains.kp);
        putSig(e["ki"], p.gains.ki);
        putSig(e["kd"], p.gains.kd);
    }
    if (r.phase == Phase::Done) {
        JsonObject rec = o["recommended"].to<JsonObject>();
        putSig(rec["kp"], r.recommended.kp);
        putSig(rec["ki"], r.recommended.ki);
        putSig(rec["kd"], r.recommended.kd);
        rec["used"] = r.used;
        rec["rule"] = autotune::ruleName(r.rule);
    }

    const autotune::FanTestResult& f = r.fan;
    if (f.status != autotune::FanTestStatus::Skipped) {
        JsonObject ft = o["fanTest"].to<JsonObject>();
        ft["status"] = autotune::fanTestStatusName(f.status);
        ft["failure"] = f.failure;
        ft["channel"] = f.channel == 0 ? "exhaust" : "second";
        putNum(ft["target"], fromCelsius(f.testC, unit), 1.0f);
        putNum(ft["duration"], f.durationS, 1.0f);
        JsonArray lv = ft["levels"].to<JsonArray>();
        for (int i = 0; i < f.levelCount; i++) {
            const autotune::FanLevelResult& L = f.levels[i];
            JsonObject e = lv.add<JsonObject>();
            putNum(e["fan"], L.fanPct, 1.0f);
            putNum(e["hold"], L.holdPct);
            putNum(e["dip"], delta(L.dipC, unit));
            putNum(e["recovery"], L.recoveryS, 1.0f);
            putSig(e["gain"], delta(L.gainCPerPct, unit));
            putNum(e["tau"], L.tauS, 1.0f);
            putNum(e["shed"], delta(L.shedCpm, unit));
        }
    }
    if (saved)
        writeSaved(*saved, heaterMaxPct, unit, o["saved"].to<JsonObject>());
    else
        o["saved"] = nullptr;

    JsonArray log = o["log"].to<JsonArray>();
    for (int i = 0; i < r.logCount; i++) {
        JsonObject e = log.add<JsonObject>();
        putNum(e["t"], r.log[i].tS, 1.0f);
        logText(r.log[i], r, unit, text, sizeof text);
        e["text"] = text;
    }
}

void writeSaved(const roastermodel::RoasterModel& m, float heaterMaxPct, char unit, JsonObject o) {
    putNum(o["ambient"], fromCelsius(m.ambientC, unit));
    putNum(o["runFan"], m.runFanPct, 1.0f);
    putNum(o["heaterMax"], heaterMaxPct, 1.0f);
    JsonArray pts = o["points"].to<JsonArray>();
    for (int i = 0; i < m.pointCount && i < roastermodel::kMaxPoints; i++) {
        const roastermodel::PointModel& p = m.points[i];
        if (!std::isfinite(p.gainCPerPct))
            continue;
        JsonObject e = pts.add<JsonObject>();
        putNum(e["target"], fromCelsius(p.targetC, unit));
        putSig(e["gain"], delta(p.gainCPerPct, unit));
        putNum(e["tau"], p.tauS, 1.0f);
        putNum(e["hold"], p.holdPct);
    }
    const roastermodel::FanModel& f = m.fan[0];
    if (!f.valid) {
        o["fan"] = nullptr;
        return;
    }
    JsonObject fo = o["fan"].to<JsonObject>();
    putNum(fo["target"], fromCelsius(f.testC, unit));
    JsonArray lv = fo["levels"].to<JsonArray>();
    for (int i = 0; i < f.levelCount; i++) {
        JsonObject e = lv.add<JsonObject>();
        putNum(e["fan"], f.levels[i].fanPct, 1.0f);
        putNum(e["hold"], f.levels[i].holdPct);
        putNum(e["shed"], delta(f.levels[i].shedCpm, unit));
    }
}

} // namespace autotuneproto
