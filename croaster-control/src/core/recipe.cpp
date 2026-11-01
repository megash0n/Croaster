#include "recipe.h"

#include <cmath>
#include <cstring>

namespace recipe {

namespace {
const char* const kShapeNames[] = {"linear", "normal", "front", "back", "custom"};
// Rate of climb at 0, 5, ... 100 % of the ramp time (0..100, steps of 5).
const uint8_t kLinear[kCurvePoints] = {50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50,
                                       50, 50, 50, 50, 50, 50, 50, 50, 50, 50};
// exp(-((u - 0.5) / 0.2)^2 / 2), rounded to steps of 5 (low ends kept above 0).
const uint8_t kNormal[kCurvePoints] = {5, 10, 15, 20, 30, 45, 60, 75, 90, 95, 100, 95, 90, 75, 60, 45, 30, 20, 15, 10, 5};
const uint8_t kFront[kCurvePoints] = {100, 95, 90, 85, 80, 75, 70, 65, 60, 55, 50, 45, 40, 35, 30, 25, 20, 15, 10, 5, 0};
const uint8_t kBack[kCurvePoints] = {0, 5, 10, 15, 20, 25, 30, 35, 40, 45, 50, 55, 60, 65, 70, 75, 80, 85, 90, 95, 100};

const char* const kEventNames[] = {"CHARGE", "TP", "DRY_END", "FC_START", "FC_END", "SC_START", "SC_END", "DROP"};
static_assert(sizeof(kEventNames) / sizeof(kEventNames[0]) == static_cast<size_t>(Event::Count), "event names");

bool eq(const char* a, const char* b) {
    return a && b && std::strcmp(a, b) == 0;
}
size_t utf8Length(const char* s) {
    size_t n = 0;
    for (; *s; s++)
        if ((static_cast<unsigned char>(*s) & 0xC0) != 0x80)
            n++;
    return n;
}
// A temperature difference in `unit` to C.
float deltaToC(float d, char unit) {
    return unit == 'F' ? d * 5.0f / 9.0f : d;
}
bool number(JsonVariantConst v, float& out) {
    if (!v.is<float>())
        return false;
    out = v.as<float>();
    return std::isfinite(out);
}
bool percent(JsonVariantConst v, float& out) {
    return number(v, out) && out >= 0 && out <= 100;
}

const char* parseTrigger(JsonVariantConst j, char unit, Trigger& t) {
    if (!j.is<JsonObjectConst>())
        return "trigger missing";
    const char* type = j["type"];
    const char* op = j["op"] | "gte";
    if (eq(op, "gte"))
        t.op = Op::Gte;
    else if (eq(op, "lte"))
        t.op = Op::Lte;
    else if (eq(op, "near"))
        t.op = Op::Near;
    else
        return "trigger op must be gte, lte or near";

    if (eq(type, "event")) {
        t.type = TriggerType::Event;
        if (!parseEvent(j["value"].as<const char*>(), t.event))
            return "unknown event";
        return "";
    }
    float v = 0;
    if (!number(j["value"], v))
        return "trigger value missing";
    float defTol = 0, maxTol = 0;
    if (eq(type, "time")) {
        t.type = TriggerType::Time;
        if (!(v >= 0 && v <= kMaxTimeS))
            return "time trigger must be 0..3600 s";
        t.value = v;
        defTol = 5;
        maxTol = 60;
        if (!j["tol"].isNull()) {
            if (!number(j["tol"], t.tol) || !(t.tol > 0 && t.tol <= maxTol))
                return "tolerance out of range";
        } else {
            t.tol = defTol;
        }
        return "";
    }
    if (eq(type, "bt") || eq(type, "et")) {
        t.type = eq(type, "bt") ? TriggerType::Bt : TriggerType::Et;
        t.value = toCelsius(v, unit);
        if (!(t.value >= kMinTempC && t.value <= kMaxTempC))
            return "temperature trigger must be 0..350 C";
        defTol = 2;
        maxTol = 20;
    } else if (eq(type, "ror")) {
        t.type = TriggerType::Ror;
        t.value = deltaToC(v, unit);
        if (!(t.value >= kMinRorC && t.value <= kMaxRorC))
            return "RoR trigger must be -50..100 C/min";
        defTol = 1;
        maxTol = 20;
    } else {
        return "trigger type must be time, bt, et, ror or event";
    }
    if (!j["tol"].isNull()) {
        float tol = 0;
        if (!number(j["tol"], tol))
            return "tolerance out of range";
        t.tol = deltaToC(tol, unit);
        if (!(t.tol > 0 && t.tol <= maxTol))
            return "tolerance out of range";
    } else {
        t.tol = defTol;
    }
    return "";
}

// "ramp", "shape" and "curve" of a power or setpoint action.
const char* parseRamp(JsonVariantConst j, Action& a, Recipe& r) {
    if (!j["ramp"].isNull() && (!number(j["ramp"], a.rampS) || !(a.rampS >= 0 && a.rampS <= kMaxRampS)))
        return "ramp must be 0..3600 s";
    JsonVariantConst shape = j["shape"];
    if (!shape.isNull() && !parseShape(shape.as<const char*>(), a.shape))
        return "shape must be linear, normal, front, back or custom";
    if (a.shape != Shape::Custom)
        return j["curve"].isNull() ? "" : "curve needs shape custom";
    JsonArrayConst c = j["curve"].as<JsonArrayConst>();
    if (c.isNull() || c.size() != kCurvePoints)
        return "curve must have 21 rates";
    if (r.curveCount >= kMaxCurves)
        return "a recipe has up to 8 custom curves";
    uint8_t* out = r.curves[r.curveCount];
    int sum = 0;
    size_t i = 0;
    for (JsonVariantConst v : c) {
        float x = 0;
        if (!percent(v, x))
            return "curve rates must be 0..100";
        out[i] = static_cast<uint8_t>(std::lround(x));
        sum += out[i++];
    }
    if (sum == 0)
        return "curve needs a rate above 0";
    a.curve = r.curveCount++;
    return "";
}

const char* parseAction(JsonVariantConst j, char unit, Action& a, Recipe& r) {
    if (!j.is<JsonObjectConst>())
        return "action must be an object";
    const char* type = j["type"];
    if (eq(type, "power") || eq(type, "fan")) {
        a.type = eq(type, "power") ? ActionType::Power : ActionType::Fan;
        if (!percent(j["value"], a.value))
            return "power and fan must be 0..100 %";
        if (a.type == ActionType::Fan)
            return j["ramp"].isNull() && j["shape"].isNull() ? "" : "fan does not ramp";
        return parseRamp(j, a, r);
    }
    if (eq(type, "setpoint")) {
        a.type = ActionType::Setpoint;
        float v = 0;
        if (!number(j["value"], v))
            return "setpoint value missing";
        a.value = toCelsius(v, unit);
        if (!(a.value >= kMinTempC && a.value <= kMaxTempC))
            return "setpoint must be 0..350 C";
        return parseRamp(j, a, r);
    }
    if (eq(type, "hold")) {
        a.type = ActionType::Hold;
        return "";
    }
    if (eq(type, "cool")) {
        a.type = ActionType::Cool;
        return "";
    }
    if (eq(type, "end")) {
        a.type = ActionType::End;
        return "";
    }
    if (eq(type, "note")) {
        a.type = ActionType::Note;
        if (!j["text"].is<const char*>() || utf8Length(j["text"].as<const char*>()) > kMaxNoteChars)
            return "note text must be up to 80 characters";
        return "";
    }
    return "action type must be power, fan, setpoint, hold, cool, end or note";
}
} // namespace

bool parseShape(const char* s, Shape& out) {
    for (size_t i = 0; i < sizeof(kShapeNames) / sizeof(kShapeNames[0]); i++)
        if (eq(s, kShapeNames[i])) {
            out = static_cast<Shape>(i);
            return true;
        }
    return false;
}

const char* shapeName(Shape s) {
    const size_t i = static_cast<size_t>(s);
    return i < sizeof(kShapeNames) / sizeof(kShapeNames[0]) ? kShapeNames[i] : "linear";
}

const uint8_t* shapeRates(Shape s) {
    switch (s) {
    case Shape::Normal:
        return kNormal;
    case Shape::Front:
        return kFront;
    case Shape::Back:
        return kBack;
    case Shape::Custom:
        return nullptr;
    default:
        return kLinear;
    }
}

float rampFraction(const uint8_t* rates, float u) {
    if (!(u > 0))
        return 0;
    if (u >= 1 || !rates)
        return u >= 1 ? 1 : u;
    constexpr size_t n = kCurvePoints - 1;
    float total = 0, done = 0;
    const float pos = u * n;
    for (size_t i = 0; i < n; i++) {
        const float a = rates[i], b = rates[i + 1];
        total += (a + b) / 2;
        if (pos >= i + 1)
            done += (a + b) / 2;
        else if (pos > i) {
            const float x = pos - i; // trapezoid up to x of this segment
            done += a * x + (b - a) * x * x / 2;
        }
    }
    return total > 0 ? fminf(1, fmaxf(0, done / total)) : u;
}

float peakFactor(const uint8_t* rates, float* atU) {
    if (atU)
        *atU = 0;
    if (!rates)
        return 1;
    constexpr size_t n = kCurvePoints - 1;
    float total = 0;
    size_t peak = 0;
    for (size_t i = 0; i < n; i++)
        total += (rates[i] + rates[i + 1]) / 2.0f;
    for (size_t i = 1; i < kCurvePoints; i++)
        if (rates[i] > rates[peak])
            peak = i;
    if (atU)
        *atU = static_cast<float>(peak) / n;
    return total > 0 ? rates[peak] / (total / n) : 1;
}

const uint8_t* Recipe::rates(const Action& a) const {
    if (a.shape != Shape::Custom)
        return shapeRates(a.shape);
    return a.curve < curveCount ? curves[a.curve] : kLinear;
}

bool parseEvent(const char* s, Event& out) {
    for (size_t i = 0; i < static_cast<size_t>(Event::Count); i++)
        if (eq(s, kEventNames[i])) {
            out = static_cast<Event>(i);
            return true;
        }
    return false;
}

const char* eventName(Event e) {
    return static_cast<size_t>(e) < static_cast<size_t>(Event::Count) ? kEventNames[static_cast<size_t>(e)] : "";
}

const char* unitName(char unit) {
    switch (unit) {
    case 'C':
        return "C";
    case 'F':
        return "F";
    case 'K':
        return "K";
    default:
        return nullptr;
    }
}

void copyText(char* dst, size_t cap, const char* src) {
    if (cap == 0)
        return;
    size_t n = src ? std::strlen(src) : 0;
    if (n >= cap) {
        n = cap - 1;
        while (n > 0 && (static_cast<unsigned char>(src[n]) & 0xC0) == 0x80)
            n--; // do not split a multi-byte character
    }
    if (n)
        std::memcpy(dst, src, n);
    dst[n] = '\0';
}

namespace {
void putSeconds(JsonObject o, const char* key, float s) {
    if (std::isfinite(s))
        o[key] = std::round(s * 10) / 10;
    else
        o[key] = nullptr;
}
} // namespace

void writeStatus(JsonObject r, const Runner& run, const RoastClock& clock, uint32_t nowMs, const char* name, char unit) {
    r["state"] = stateName(run.state());
    r["reason"] = run.reason();
    r["seq"] = run.seq();
    if (run.state() != State::Empty) {
        r["name"] = static_cast<const char*>(name ? name : ""); // const char*: copied
        r["unit"] = unitName(unit);                             // literal or null
        r["mode"] = run.recipe().guided ? "guided" : "auto";
        r["next"] = run.next();
        r["prompt"] = run.prompt();
        r["follow"] = run.state() == State::Running && run.followingSetpoint();
        if (std::isfinite(run.recipe().chargeC))
            r["chargeTemp"] = std::round(fromCelsius(run.recipe().chargeC, unit) * 10) / 10;
    }
    if (run.state() == State::Running && run.ramping()) {
        const Runner::Ramp rp = run.ramp(nowMs);
        JsonObject o = r["ramp"].to<JsonObject>();
        o["kind"] = rp.power ? "power" : "setpoint";
        o["from"] = std::round(rp.from * 10) / 10; // %, or C for a setpoint ramp
        o["to"] = std::round(rp.to * 10) / 10;
        o["duration"] = rp.rampS;
        putSeconds(o, "elapsed", rp.elapsedS);
        JsonArray c = o["rates"].to<JsonArray>();
        for (size_t i = 0; rp.rates && i < kCurvePoints; i++)
            c.add(rp.rates[i]);
    }
    putSeconds(r, "sinceCharge", clock.sinceCharge(nowMs));
    JsonObject ev = r["events"].to<JsonObject>();
    for (int i = 0; i < static_cast<int>(Event::Count); i++) {
        const Event e = static_cast<Event>(i);
        if (clock.seen(e))
            putSeconds(ev, eventName(e), clock.at(e));
    }
    JsonArray fired = r["fired"].to<JsonArray>();
    for (size_t i = 0; i < run.firedCount(); i++) {
        const Fired& f = run.fired(i);
        JsonObject o = fired.add<JsonObject>();
        o["step"] = f.step;
        putSeconds(o, "at", f.atS);
        o["how"] = outcomeName(f.outcome);
    }
}

const char* parse(JsonObjectConst j, char unit, Recipe& out) {
    if (!unitName(unit))
        return "unit must be C, F or K";
    if (j.isNull())
        return "recipe must be an object";
    const char* mode = j["mode"] | "auto";
    if (!eq(mode, "auto") && !eq(mode, "guided"))
        return "mode must be auto or guided";
    out.guided = eq(mode, "guided");
    out.chargeC = NAN;
    out.curveCount = 0;
    if (!j["chargeTemp"].isNull()) {
        float v = 0;
        if (!number(j["chargeTemp"], v))
            return "chargeTemp must be a number";
        out.chargeC = toCelsius(v, unit);
        if (!(out.chargeC >= kMinTempC && out.chargeC <= kMaxTempC))
            return "chargeTemp must be 0..350 C";
    }
    JsonArrayConst steps = j["steps"].as<JsonArrayConst>();
    if (steps.isNull() || steps.size() == 0 || steps.size() > kMaxSteps)
        return "a recipe has 1 to 32 steps";
    out.count = 0;
    for (JsonVariantConst s : steps) {
        if (!s.is<JsonObjectConst>())
            return "step must be an object";
        Step& st = out.steps[out.count];
        st = Step();
        const char* e = parseTrigger(s["trigger"], unit, st.trigger);
        if (*e)
            return e;
        JsonVariantConst any = s["anyOrder"];
        if (!any.isNull() && !any.is<bool>())
            return "anyOrder must be true or false";
        st.anyOrder = any | false;
        JsonVariantConst label = s["label"];
        if (!label.isNull() && (!label.is<const char*>() || utf8Length(label.as<const char*>()) > kMaxLabelChars))
            return "label must be up to 40 characters";
        JsonArrayConst actions = s["actions"].as<JsonArrayConst>();
        if (actions.isNull() || actions.size() == 0 || actions.size() > kMaxActions)
            return "a step has 1 to 4 actions";
        for (JsonVariantConst a : actions) {
            e = parseAction(a, unit, st.actions[st.actionCount], out);
            if (*e)
                return e;
            st.actionCount++;
        }
        out.count++;
    }
    return "";
}

void RoastClock::mark(Event e, uint32_t nowMs) {
    if (e == Event::Charge) {
        mask_ = 0;
        chargeMs_ = nowMs;
    }
    mask_ |= 1u << static_cast<int>(e);
    atS_[static_cast<int>(e)] = sinceCharge(nowMs);
}

void RoastClock::undo(Event e) {
    if (e == Event::Charge)
        mask_ = 0; // without CHARGE there is no roast clock
    else
        mask_ &= ~(1u << static_cast<int>(e));
}

float RoastClock::sinceCharge(uint32_t nowMs) const {
    return charged() ? (nowMs - chargeMs_) / 1000.0f : NAN;
}

float RoastClock::at(Event e) const {
    return seen(e) ? atS_[static_cast<int>(e)] : NAN;
}

const char* stateName(State s) {
    switch (s) {
    case State::Ready:
        return "ready";
    case State::Running:
        return "running";
    case State::Done:
        return "done";
    case State::Aborted:
        return "aborted";
    default:
        return "empty";
    }
}

const char* outcomeName(Outcome o) {
    switch (o) {
    case Outcome::Prompted:
        return "prompted";
    case Outcome::Confirmed:
        return "confirmed";
    case Outcome::Skipped:
        return "skipped";
    case Outcome::Missed:
        return "missed";
    default:
        return "applied";
    }
}

bool Runner::load(const Recipe& r) {
    if (state_ == State::Running || r.count == 0 || r.count > kMaxSteps)
        return false;
    recipe_ = r;
    state_ = State::Ready;
    reason_ = "";
    firedCount_ = 0;
    prompt_ = -1;
    rampActive_ = false;
    seq_++;
    return true;
}

void Runner::clear() {
    if (state_ == State::Running)
        finish(State::Aborted, "cleared");
    recipe_.count = 0;
    state_ = State::Empty;
    seq_++;
}

bool Runner::start() {
    if (state_ == State::Empty || state_ == State::Running)
        return false;
    for (size_t i = 0; i < kMaxSteps; i++)
        done_[i] = seenFalse_[i] = false;
    firedCount_ = 0;
    prompt_ = -1;
    rampActive_ = false;
    pendingEnd_ = false;
    pendingCharge_ = std::isfinite(recipe_.chargeC);
    followSetpoint_ = false;
    reason_ = "";
    state_ = State::Running;
    seq_++;
    return true;
}

void Runner::finish(State s, const char* reason) {
    state_ = s;
    reason_ = reason;
    rampActive_ = false;
    if (prompt_ >= 0) { // a prompt left open is not applied
        for (size_t i = firedCount_; i-- > 0;)
            if (log_[i].step == prompt_ && log_[i].outcome == Outcome::Prompted) {
                log_[i].outcome = Outcome::Missed;
                break;
            }
        prompt_ = -1;
    }
    seq_++;
}

void Runner::abort(const char* reason) {
    if (state_ == State::Running)
        finish(State::Aborted, reason);
}

int Runner::next() const {
    for (size_t i = 0; i < recipe_.count; i++)
        if (!done_[i] && !recipe_.steps[i].anyOrder)
            return static_cast<int>(i);
    return -1;
}

Runner::Cond Runner::evaluate(const Trigger& t, const Inputs& in, const RoastClock& clock) const {
    if (t.type == TriggerType::Event)
        return clock.seen(t.event) ? Cond::True : Cond::False;
    float x = NAN;
    switch (t.type) {
    case TriggerType::Time:
        if (!clock.charged())
            return Cond::False; // the roast clock has not started
        x = clock.sinceCharge(in.nowMs);
        break;
    case TriggerType::Bt:
        x = in.btC;
        break;
    case TriggerType::Et:
        x = in.etC;
        break;
    case TriggerType::Ror:
    default:
        x = in.rorC;
        break;
    }
    if (!std::isfinite(x))
        return Cond::Unknown; // no reading: neither arms nor fires
    bool c = false;
    switch (t.op) {
    case Op::Gte:
        c = x >= t.value;
        break;
    case Op::Lte:
        c = x <= t.value;
        break;
    case Op::Near:
    default:
        c = std::fabs(x - t.value) <= t.tol;
        break;
    }
    return c ? Cond::True : Cond::False;
}

void Runner::record(uint8_t i, float atS, Outcome o) {
    if (firedCount_ < kMaxSteps)
        log_[firedCount_++] = Fired{i, atS, o};
    seq_++;
}

float pidInputC(const Inputs& in) {
    switch (in.pidInput) {
    case PidInput::ET:
        return in.etC;
    case PidInput::MAX:
        return std::isfinite(in.btC) && std::isfinite(in.etC) ? fmaxf(in.btC, in.etC) : NAN;
    case PidInput::BT:
    default:
        return in.btC;
    }
}

float rampStartC(const Inputs& in, float targetC) {
    const float pv = pidInputC(in);
    if (std::isfinite(pv))
        return pv < targetC ? pv : NAN;
    const float sp = in.setpointC;
    return std::isfinite(sp) && sp > 0 && sp < targetC ? sp : NAN;
}

float bumplessFlag(const Inputs& in, float targetC) {
    const float pv = pidInputC(in);
    return std::isfinite(pv) && pv < targetC ? 1.0f : 0.0f;
}

void Runner::startRamp(const Action& a, float from, uint32_t nowMs, Output& out, float restart) {
    rampActive_ = true;
    rampPower_ = a.type == ActionType::Power;
    rampRates_ = recipe_.rates(a);
    rampFromC_ = from;
    rampToC_ = a.value;
    rampS_ = a.rampS;
    rampStartMs_ = rampSentMs_ = nowMs;
    out.add(rampPower_ ? Command::Kind::SetHeater : Command::Kind::SetSetpoint, from, restart);
}

Runner::Ramp Runner::ramp(uint32_t nowMs) const {
    Ramp r;
    if (!rampActive_)
        return r;
    r.power = rampPower_;
    r.from = rampFromC_;
    r.to = rampToC_;
    r.rampS = rampS_;
    r.elapsedS = fminf(rampS_, (nowMs - rampStartMs_) / 1000.0f);
    r.rates = rampRates_;
    return r;
}

void Runner::apply(const Step& s, const Inputs& in, Output& out) {
    for (uint8_t k = 0; k < s.actionCount; k++) {
        const Action& a = s.actions[k];
        switch (a.type) {
        case ActionType::Power:
            rampActive_ = false;
            followSetpoint_ = false;
            out.add(Command::Kind::SetMode, static_cast<float>(Mode::Manual));
            if (a.rampS > 0 && std::isfinite(in.heaterPct))
                startRamp(a, fmaxf(0, fminf(100, in.heaterPct)), in.nowMs, out);
            else
                out.add(Command::Kind::SetHeater, fmaxf(0, fminf(100, a.value)));
            break;
        case ActionType::Fan:
            out.add(Command::Kind::SetFan, fmaxf(0, fminf(100, a.value)));
            break;
        case ActionType::Setpoint:
            out.add(Command::Kind::SetMode, static_cast<float>(Mode::Pid));
            followSetpoint_ = true;
            rampActive_ = false;
            if (a.rampS > 0 && std::isfinite(rampStartC(in, a.value)))
                // b = 1: the controller restarts the PID bumpless at the new
                // setpoint, so the heater carries on from its present output.
                startRamp(a, rampStartC(in, a.value), in.nowMs, out, 1);
            else
                out.add(Command::Kind::SetSetpoint, a.value, bumplessFlag(in, a.value));
            break;
        case ActionType::Hold:
            rampActive_ = false;
            break;
        case ActionType::Cool:
            out.stop = true;
            pendingEnd_ = true;
            break;
        case ActionType::End:
            pendingEnd_ = true;
            break;
        case ActionType::Note:
        default:
            break;
        }
    }
}

void Runner::fire(uint8_t i, const Inputs& in, const RoastClock& clock, Output& out) {
    done_[i] = true;
    const float at = clock.sinceCharge(in.nowMs);
    if (recipe_.guided) {
        if (prompt_ >= 0) // an unanswered prompt is replaced by the newer one
            for (size_t k = firedCount_; k-- > 0;)
                if (log_[k].step == prompt_ && log_[k].outcome == Outcome::Prompted) {
                    log_[k].outcome = Outcome::Missed;
                    break;
                }
        prompt_ = i;
        record(i, at, Outcome::Prompted);
        return;
    }
    record(i, at, Outcome::Applied);
    apply(recipe_.steps[i], in, out);
}

void Runner::tick(const Inputs& in, const RoastClock& clock, Output& out) {
    if (state_ != State::Running)
        return;
    if (in.trip != SafetyTrip::None) {
        finish(State::Aborted, tripName(in.trip));
        return;
    }
    if (pendingCharge_) { // preheat to the charge temperature, once, before CHARGE
        pendingCharge_ = false;
        if (!clock.charged()) {
            out.add(Command::Kind::SetMode, static_cast<float>(Mode::Pid));
            out.add(Command::Kind::SetSetpoint, recipe_.chargeC, bumplessFlag(in, recipe_.chargeC));
        }
    }
    Cond cond[kMaxSteps];
    for (size_t i = 0; i < recipe_.count; i++) {
        cond[i] = done_[i] ? Cond::Unknown : evaluate(recipe_.steps[i].trigger, in, clock);
        if (cond[i] == Cond::False)
            seenFalse_[i] = true;
    }
    // In order: the next step, and again after it fires (the one after may already be true).
    for (int n = next(); n >= 0 && !pendingEnd_; n = next()) {
        if (!(seenFalse_[n] && cond[n] == Cond::True))
            break;
        fire(static_cast<uint8_t>(n), in, clock, out);
    }
    for (size_t i = 0; i < recipe_.count && !pendingEnd_; i++)
        if (recipe_.steps[i].anyOrder && !done_[i] && seenFalse_[i] && cond[i] == Cond::True)
            fire(static_cast<uint8_t>(i), in, clock, out);

    if (rampActive_ && in.nowMs - rampSentMs_ >= kRampEveryMs) {
        const float p = fminf(1, (in.nowMs - rampStartMs_) / 1000.0f / rampS_);
        const float f = p >= 1 ? 1 : rampFraction(rampRates_, p);
        out.add(rampPower_ ? Command::Kind::SetHeater : Command::Kind::SetSetpoint,
                rampFromC_ + (rampToC_ - rampFromC_) * f);
        rampSentMs_ = in.nowMs;
        if (p >= 1)
            rampActive_ = false;
    }
    if (pendingEnd_) {
        finish(State::Done, out.stop ? "cool" : "end");
        return;
    }
    bool all = true;
    for (size_t i = 0; i < recipe_.count; i++)
        all = all && done_[i];
    if (all && !rampActive_ && prompt_ < 0)
        finish(State::Done, "complete");
}

bool Runner::confirm(uint8_t step, const Inputs& in, Output& out) {
    if (state_ != State::Running || prompt_ != step)
        return false;
    prompt_ = -1;
    for (size_t k = firedCount_; k-- > 0;)
        if (log_[k].step == step && log_[k].outcome == Outcome::Prompted) {
            log_[k].outcome = Outcome::Confirmed;
            break;
        }
    seq_++;
    apply(recipe_.steps[step], in, out);
    if (pendingEnd_)
        finish(State::Done, out.stop ? "cool" : "end");
    return true;
}

bool Runner::dismiss(uint8_t step) {
    if (state_ != State::Running || prompt_ != step)
        return false;
    prompt_ = -1;
    for (size_t k = firedCount_; k-- > 0;)
        if (log_[k].step == step && log_[k].outcome == Outcome::Prompted) {
            log_[k].outcome = Outcome::Skipped;
            break;
        }
    seq_++;
    return true;
}

bool Runner::skipNext(const RoastClock& clock, uint32_t nowMs) {
    const int n = next();
    if (state_ != State::Running || n < 0)
        return false;
    done_[n] = true;
    record(static_cast<uint8_t>(n), clock.sinceCharge(nowMs), Outcome::Skipped);
    return true;
}

} // namespace recipe
