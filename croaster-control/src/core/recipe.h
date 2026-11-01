#pragma once
// Recipe: steps that fire on a condition during a roast and change the outputs.
// Runs on the board (ControlPlugin's loop task, app/RecipePlugin.h), so a step
// fires even with no browser watching. Pure logic, host-tested.
//
// JSON (the "recipe" member of a library profile, core/profile_library.h):
//   {"mode":"auto"|"guided",
//    "steps":[{"trigger":{"type":"bt","op":"gte","value":389},
//              "label":"Push to first crack", "anyOrder":false,
//              "actions":[{"type":"fan","value":50},{"type":"power","value":60}]}]}
// Triggers (temperatures in the profile's "unit"):
//   time   s since CHARGE (false before CHARGE)       value 0..3600
//   bt/et  corrected probe temperature                value 0..350 C
//   ror    BT rate of rise, unit per minute           value -50..100 C/min
//   event  a roast event reported with roastEvent     value "CHARGE", "TP", "DRY_END",
//          "FC_START", "FC_END", "SC_START", "SC_END", "DROP"
//   op: "gte" (default) | "lte" | "near" (|x - value| <= "tol"; default 5 s, 2 C, 1 C/min)
// Actions (1..4 per step):
//   power v     Manual mode, heater v %             fan v        fan v %
//   setpoint v  PID mode, target v
//   hold        stop a ramp and keep the outputs as they are
//   power and setpoint take "ramp": s (0..3600) to move there gradually
//   instead of at once. A power ramp starts from the current heater %. A
//   setpoint ramp starts from the PID input (BT by default) when the step
//   applies, whatever mode or setpoint came before (rampStartC below).
//   "shape" sets how the rate of climb is spread over the ramp: "linear"
//   (default, constant), "normal" (slow, fast, slow),
//   "front" (fast first, easing into the target), "back" (slow first) or
//   "custom" with "curve": 21 rates 0..100 at 0, 5, ... 100 % of the ramp time.
//   The value at time t is start + (target - start) * F(t / ramp), F the
//   normalised running area under the rate curve.
//   cool        the stop command: heater off, Manual, cooldown fan; ends the recipe
//   end         ends the recipe, outputs unchanged
//   note        "text" (up to 80 characters) shown on the page; changes nothing
// "chargeTemp" (optional, 0..350 C): when the recipe starts before CHARGE, PID
// heats to this setpoint so the drum is ready for the beans.
//
// Firing: a step fires once, on a false-to-true change of its condition seen
// while the recipe runs (a condition already true at start must go false
// first). Steps fire in order: only the first step not yet fired is armed,
// plus steps marked "anyOrder", which are armed from the start. A step that
// can never fire can be skipped (skipNext). In guided mode a fired step
// prompts instead; confirm() applies its actions.
// Safety: actions become ordinary controller commands, so every interlock
// (trips, setpoint ceiling, heater ceiling, minimum fan) still applies. A
// safety trip aborts the recipe (the plugin also aborts it on stop and OTA).
#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>

#include "control_types.h"

namespace recipe {

constexpr size_t kMaxSteps = 32;
constexpr size_t kMaxActions = 4;
constexpr size_t kMaxNoteChars = 80;
constexpr size_t kMaxLabelChars = 40;
constexpr float kMaxTimeS = 3600;
constexpr float kMinTempC = 0, kMaxTempC = 350;
constexpr float kMinRorC = -50, kMaxRorC = 100;
constexpr float kMaxRampS = 3600;
constexpr size_t kCurvePoints = 21; // rate at 0, 5, ... 100 % of the ramp time
constexpr size_t kMaxCurves = 8;    // custom curves per recipe

enum class Event : uint8_t { Charge, Tp, DryEnd, FcStart, FcEnd, ScStart, ScEnd, Drop, Count };
// "CHARGE" ... "DROP"; false for anything else.
bool parseEvent(const char* s, Event& out);
const char* eventName(Event e);

enum class TriggerType : uint8_t { Time, Bt, Et, Ror, Event };
enum class Op : uint8_t { Gte, Lte, Near };
enum class ActionType : uint8_t { Power, Fan, Setpoint, Hold, Cool, End, Note };
enum class Shape : uint8_t { Linear, Normal, Front, Back, Custom };

// "linear" ... "custom"; false for anything else.
bool parseShape(const char* s, Shape& out);
const char* shapeName(Shape s);
// The rate curve of a built-in shape (kCurvePoints values, 0..100); nullptr for Custom.
const uint8_t* shapeRates(Shape s);
// Fraction of the change done at `u` (0..1) of the ramp time: the area under
// the piecewise-linear rate curve up to `u` over the whole area. Clamped to
// 0..1; a curve with no area gives u (linear).
float rampFraction(const uint8_t* rates, float u);
// Highest rate over the mean rate: how much steeper than a linear ramp the
// steepest part is. `atU` gets where it is (0..1). 1 for a flat curve.
float peakFactor(const uint8_t* rates, float* atU = nullptr);

struct Trigger {
    TriggerType type = TriggerType::Time;
    Op op = Op::Gte;
    float value = 0; // s, C or C/min
    float tol = 0;
    Event event = Event::Charge;
};
struct Action {
    ActionType type = ActionType::Note;
    float value = 0; // %, or C for setpoint
    float rampS = 0;
    Shape shape = Shape::Linear;
    uint8_t curve = 0; // index into Recipe::curves when shape is Custom
};
struct Step {
    Trigger trigger;
    Action actions[kMaxActions];
    uint8_t actionCount = 0;
    bool anyOrder = false;
};
struct Recipe {
    bool guided = false;
    float chargeC = NAN; // preheat setpoint before CHARGE; NaN = none
    Step steps[kMaxSteps];
    size_t count = 0;
    uint8_t curves[kMaxCurves][kCurvePoints] = {};
    uint8_t curveCount = 0;
    // The rate curve an action ramps along.
    const uint8_t* rates(const Action& a) const;
};

// Parses and checks a recipe object; temperatures are in `unit` ('C', 'F' or
// 'K'; anything else is refused). Returns "" and fills `out` (converted to C)
// on success, else a short reason. `out` may be partly written on failure.
const char* parse(JsonObjectConst json, char unit, Recipe& out);

// "C", "F" or "K" as a string literal; nullptr for any other unit. Literals are
// safe to store in a JsonDocument by pointer. ArduinoJson 7 also stores a
// `const char[N]` array by pointer, so a local array must never be assigned.
const char* unitName(char unit);

// Copies `src` into `dst` (capacity `cap` including the terminator), cutting
// at a UTF-8 character boundary. `dst` is always terminated.
void copyText(char* dst, size_t cap, const char* src);

class RoastClock;
class Runner;

// Fields of the getRecipe reply (all but "ok" and the recipe text) into `r`.
// Every string is copied or static, so `r` stays valid after the caller's
// buffers change. `name` and `unit` are only written while a recipe is loaded.
void writeStatus(JsonObject r, const Runner& run, const RoastClock& clock, uint32_t nowMs, const char* name, char unit);

// Roast events reported by the page and the clock since CHARGE. CHARGE starts
// a new roast: it clears the other events.
class RoastClock {
public:
    void mark(Event e, uint32_t nowMs);
    void undo(Event e);
    bool seen(Event e) const {
        return (mask_ >> static_cast<int>(e)) & 1u;
    }
    bool charged() const {
        return seen(Event::Charge);
    }
    // Seconds since CHARGE; NaN before CHARGE.
    float sinceCharge(uint32_t nowMs) const;
    // Seconds since CHARGE when `e` was marked; NaN if not marked.
    float at(Event e) const;

private:
    uint16_t mask_ = 0;
    uint32_t chargeMs_ = 0;
    float atS_[static_cast<int>(Event::Count)] = {};
};

// One tick's readings, on the scale the user sees (corrected), in C.
struct Inputs {
    uint32_t nowMs = 0;
    float btC = NAN, etC = NAN, rorC = NAN; // NaN = no reading
    SafetyTrip trip = SafetyTrip::None;
    float setpointC = 0; // current controller setpoint (fallback setpoint ramp start)
    float heaterPct = 0; // current heater output (power ramp start)
    PidInput pidInput = PidInput::BT; // the probe the PID controls (setpoint ramp start)
};

// The PID input in `in` (BT, ET or the higher of the two); NaN when unreadable.
float pidInputC(const Inputs& in);

// Where a setpoint ramp to `targetC` starts. Normally the PID input, so the
// shape spreads the whole climb from where the beans are to the target. NaN
// when there is nothing to ramp: the input is already at or above the target
// (the step holds the target instead; ramps never go down), or the input is
// unreadable and the current setpoint is no usable start either (0, not
// finite, or at or above the target). With an unreadable input the current
// setpoint is used when it lies below the target; the controller's sensor
// trip ends PID within 2 s in that case anyway.
float rampStartC(const Inputs& in, float targetC);

// SetSetpoint's `b` for a plain setpoint step or the charge preheat: 1 (restart
// the PID bumpless from the present heater output) when the PID input is below
// the target, else 0 (BT at or above the target, or unreadable: the PID's
// normal response, which cuts the heater).
float bumplessFlag(const Inputs& in, float targetC);

enum class State : uint8_t { Empty, Ready, Running, Done, Aborted };
const char* stateName(State s);

enum class Outcome : uint8_t { Applied, Prompted, Confirmed, Skipped, Missed };
const char* outcomeName(Outcome o);

struct Fired {
    uint8_t step = 0;
    float atS = NAN; // since CHARGE; NaN before CHARGE
    Outcome outcome = Outcome::Applied;
};

// Commands produced by a tick; the caller submits them in order.
struct Output {
    static constexpr size_t kMax = 2 * kMaxActions * 2 + 2;
    Command commands[kMax];
    size_t count = 0;
    bool stop = false; // run the stop path (cool action)
    void add(Command::Kind k, float a = 0, float b = 0) {
        if (count < kMax) {
            Command c{k};
            c.a = a;
            c.b = b;
            commands[count++] = c;
        }
    }
};

class Runner {
public:
    static constexpr uint32_t kRampEveryMs = 500;

    // Replaces the loaded recipe (state Ready). Not while running.
    bool load(const Recipe& r);
    void clear();
    bool start();
    void abort(const char* reason);
    // Evaluates the triggers; fills `out` with the commands of fired steps.
    void tick(const Inputs& in, const RoastClock& clock, Output& out);
    // Guided mode: apply (confirm) or dismiss (skip) the pending prompt.
    bool confirm(uint8_t step, const Inputs& in, Output& out);
    bool dismiss(uint8_t step);
    // Marks the next armed step as fired without running it.
    bool skipNext(const RoastClock& clock, uint32_t nowMs);

    State state() const {
        return state_;
    }
    const char* reason() const {
        return reason_;
    }
    const Recipe& recipe() const {
        return recipe_;
    }
    // Index of the next step in sequence, -1 when none is left.
    int next() const;
    int prompt() const {
        return prompt_;
    }
    size_t firedCount() const {
        return firedCount_;
    }
    const Fired& fired(size_t i) const {
        return log_[i];
    }
    uint32_t seq() const {
        return seq_;
    }
    bool ramping() const {
        return rampActive_;
    }
    // The ramp in progress (valid while ramping()): a heater ramp in %, or a
    // setpoint ramp in C, `elapsedS` of `rampS` done along `rates`.
    struct Ramp {
        bool power = false;
        float from = 0, to = 0, rampS = 0, elapsedS = 0;
        const uint8_t* rates = nullptr;
    };
    Ramp ramp(uint32_t nowMs) const;
    // True after a setpoint step (PID follows the target), false after a
    // power step or before either: the page's behind-schedule warning.
    bool followingSetpoint() const {
        return followSetpoint_;
    }

private:
    enum class Cond : uint8_t { False, True, Unknown };
    Cond evaluate(const Trigger& t, const Inputs& in, const RoastClock& clock) const;
    void fire(uint8_t i, const Inputs& in, const RoastClock& clock, Output& out);
    void apply(const Step& s, const Inputs& in, Output& out);
    void record(uint8_t i, float atS, Outcome o);
    void finish(State s, const char* reason);

    Recipe recipe_;
    State state_ = State::Empty;
    const char* reason_ = "";
    bool done_[kMaxSteps] = {};
    bool seenFalse_[kMaxSteps] = {};
    Fired log_[kMaxSteps];
    size_t firedCount_ = 0;
    uint32_t seq_ = 0; // increments on every fired step and state change
    int prompt_ = -1;
    void startRamp(const Action& a, float from, uint32_t nowMs, Output& out, float restart = 0);
    bool rampActive_ = false;
    bool rampPower_ = false;
    bool pendingCharge_ = false;
    const uint8_t* rampRates_ = nullptr;
    float rampFromC_ = 0, rampToC_ = 0, rampS_ = 0; // % for a power ramp
    uint32_t rampStartMs_ = 0, rampSentMs_ = 0;
    bool pendingEnd_ = false;
    bool followSetpoint_ = false;
};

} // namespace recipe
