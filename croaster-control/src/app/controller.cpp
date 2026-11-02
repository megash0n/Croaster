#include "controller.h"

#include <Arduino.h>
#include <Update.h>

#include <cmath>

void Controller::begin(const PidGains& gains, bool gainsTuned, PidInput input, float tripFanPct, float cooldownEndC,
                       int core, UBaseType_t prio) {
    tripFan_ = tripFanPct;
    cooldownEndC_ = cooldownEndC;
    fanBase_ = fan_.get();
    pid_.setGains(gains);
    st_.gains = gains;
    st_.gainsTuned = gainsTuned;
    st_.pidInput = input;
    queue_ = xQueueCreate(16, sizeof(Command));
    xTaskCreatePinnedToCore(taskEntry, "control", 6144, this, prio, nullptr, core);
}

bool Controller::submit(const Command& c) {
    return queue_ && xQueueSend(queue_, &c, 0) == pdTRUE;
}

Snapshot Controller::snapshot() const {
    portENTER_CRITICAL(&mux_);
    Snapshot s = published_;
    portEXIT_CRITICAL(&mux_);
    return s;
}

bool Controller::loadProfile(const RoastProfile& p) {
    // Called from the network task; the control task takes it on StartProfile.
    // A later load before StartProfile replaces the earlier one.
    if (p.size() == 0)
        return false;
    portENTER_CRITICAL(&mux_);
    pendingProfile_ = p;
    profilePending_ = true;
    portEXIT_CRITICAL(&mux_);
    return true;
}

void Controller::configureAutotune(const autotune::Config& c) {
    atCfg_ = c;
    atCfg_.dtSeconds = pidCfg().dtSeconds;
    // Fail inside the sequence (a clear reason) before the over-temperature trip.
    atCfg_.abortAboveC = fminf(c.abortAboveC, lim_.overTempC - 5);
    autotune::makePlan(c.pointsC, c.pointCount, kAutotuneMinSetpointC, lim_.maxSetpointC, atCfg_);
    autotune::makeFanLevels(atCfg_);
}

void Controller::configureModelUse(const ModelUse& u) {
    use_ = u;
}

void Controller::setRoasterModel(const roastermodel::RoasterModel& m) {
    model_ = roastermodel::usable(m) ? m : roastermodel::RoasterModel{};
    modelPublished_ = model_;
}

void Controller::roasterModel(roastermodel::RoasterModel& out) const {
    portENTER_CRITICAL(&mux_);
    out = modelPublished_;
    portEXIT_CRITICAL(&mux_);
}

float Controller::feedforward(float sp) const {
    float ff = 0;
    if (use_.fanFeedforward)
        ff += roastermodel::fanFeedforward(model_, fanBase_, sp);
    if (use_.rampFeedforward)
        ff += roastermodel::rampFeedforward(model_, sp, fanBase_, spSlope_.value());
    return ff;
}

void Controller::setBaseFan(float pct) {
    fanBase_ = fmaxf(0, fminf(pct, 100));
    fan_.set(fanBase_ + assist_.extra());
}

void Controller::autotuneReport(autotune::Report& out) const {
    portENTER_CRITICAL(&mux_);
    out = atReport_;
    portEXIT_CRITICAL(&mux_);
}

void Controller::taskEntry(void* self) {
    static_cast<Controller*>(self)->run();
}

void Controller::run() {
    TickType_t wake = xTaskGetTickCount();
    uint32_t n = 0;
    for (;;) {
        Command c;
        while (xQueueReceive(queue_, &c, 0) == pdTRUE)
            apply(c);
        const uint32_t now = millis();
        if (n++ % kControlEvery == 0)
            controlStep(now);
        heater_.tick(now);
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(kTickMs));
    }
}

float Controller::pidInput(const ThermoSample& s) const {
    return selectPidInput(st_.pidInput, corrected(s, correction_));
}

void Controller::handlePvStep(float newPv, const char* abortReason) {
    switch (onPvStep(st_.mode)) {
    case PvStepAction::AbortAutotune:
        setMode(Mode::Manual, newPv, abortReason);
        break;
    case PvStepAction::ResetPid:
        if (std::isfinite(newPv))
            pid_.reset(st_.setpoint, newPv, heater_.requested(), feedforward(st_.setpoint));
        break;
    default:
        break;
    }
}

void Controller::setMode(Mode m, float pv, const char* abortReason) {
    const bool leavingAutotune = st_.mode == Mode::Autotune && m != Mode::Autotune;
    const float carry = startOutputOnModeChange(st_.mode, m, heater_.requested());
    if (m == Mode::Pid || m == Mode::Profile) {
        if (st_.mode == Mode::Manual || st_.mode == Mode::Autotune)
            pid_.reset(st_.setpoint, pv, carry, feedforward(st_.setpoint));
    }
    if (m == Mode::Manual)
        manualHeater_ = carry;
    if (leavingAutotune && autotune_.running()) {
        autotune_.abort(*abortReason ? abortReason : "stopped");
        if (autotune_.phase() == autotune::Phase::Done) { // stopped in the fan test: the gains wait for a choice
            st_.autotunePending = true;
            setAutotuneStatus(AutotuneStatus::Pending);
        } else {
            setAutotuneStatus(AutotuneStatus::Aborted, abortReason);
        }
    }
    st_.mode = m;
}

void Controller::setAutotuneStatus(AutotuneStatus status, const char* reason) {
    st_.autotune = status;
    st_.autotuneReason = reason;
    st_.autotuneStatusSeq++;
}

void Controller::apply(const Command& c) {
    const float pv = pidInput(last_);
    // A fresh operator action re-arms after a trip; controlStep() re-trips at once if
    // the cause (over-temp, dead probe, no mains) is still present.
    if (c.kind == Command::Kind::SetHeater || c.kind == Command::Kind::SetMode ||
        c.kind == Command::Kind::StartProfile || c.kind == Command::Kind::StartAutotune)
        st_.trip = SafetyTrip::None;
    // The operator took over: Manual does what is set, so the cooldown no longer ends on its own.
    if ((c.kind == Command::Kind::SetHeater && c.a > 0) || c.kind == Command::Kind::SetFan ||
        c.kind == Command::Kind::SetMode || c.kind == Command::Kind::StartProfile ||
        c.kind == Command::Kind::StartAutotune)
        cooldown_.cancel();
    if (const char* why = outputCommandStopsAutotune(st_.mode, c.kind, c.a, fanBase_)) {
        setMode(Mode::Manual, pv, why); // heater 0
        if (c.kind == Command::Kind::SetHeater)
            return; // stays off: the command only stops autotune
    }
    switch (c.kind) {
    case Command::Kind::SetHeater:
        manualHeater_ = c.a;
        break; // used in Manual mode
    case Command::Kind::SetFan:
        if (st_.mode == Mode::Autotune)
            fanBase_ = fmaxf(0, fminf(c.a, 100)); // same value (else stopped above); the fan test owns the output
        else
            setBaseFan(c.a);
        break;
    case Command::Kind::SetSetpoint: {
        const float prev = st_.setpoint;
        st_.setpoint = fmaxf(0, fminf(c.a, lim_.maxSetpointC));
        // Autotune keeps the setpoint it started with, so a new one would leave SV
        // showing a target the relay ignores. Stop tuning (heater off) instead.
        if (st_.mode == Mode::Autotune && st_.setpoint != prev) {
            setMode(Mode::Manual, pv, "setpoint changed");
        }
        // legacy Control::setSetpoint: going from 0 to a real setpoint clears the
        // PID history so it starts from zero output, not from stale state.
        // Recipe setpoint steps, ramps and the charge preheat send b = 1 when
        // the PID input is below the target: restart from the present heater
        // output so the move to PID does not drop the heater to 0. The
        // integrator clamp still lets a large step raise the output at once.
        if (c.b == 1 && st_.mode == Mode::Pid)
            pid_.reset(st_.setpoint, pv, heater_.requested(), feedforward(st_.setpoint));
        else if (prev <= 0 && st_.setpoint > 0 && st_.mode == Mode::Pid)
            pid_.reset(st_.setpoint, pv, 0, feedforward(st_.setpoint));
        break;
    }
    case Command::Kind::SetMode: {
        const Mode m = static_cast<Mode>(static_cast<int>(c.a));
        if (modeNeedsTuning(m, lim_.requireTunedGains, st_.gainsTuned))
            setAutotuneStatus(AutotuneStatus::Required);
        else if (m == Mode::Manual || m == Mode::Pid)
            setMode(m, pv);
        break;
    }
    case Command::Kind::SetPidInput: {
        const auto in = static_cast<PidInput>(static_cast<int>(c.a));
        if (in == st_.pidInput)
            break;
        st_.pidInput = in;
        handlePvStep(pidInput(last_), "input changed");
        break;
    }
    case Command::Kind::SetGains:
        st_.gains = {c.a, c.b, c.c};
        pid_.setGains(st_.gains);
        st_.gainsTuned = true; // the pid command counts as tuned
        break;
    case Command::Kind::StartAutotune: {
        // Test points come from configureAutotune(); the setpoint plays no part.
        if (st_.mode == Mode::Autotune)
            break; // already running
        if (!std::isfinite(pv)) {
            setAutotuneStatus(AutotuneStatus::NeedProbe);
            break;
        }
        if (fanBase_ < 30)
            setBaseFan(60); // legacy CommandLoop behaviour
        st_.autotunePending = false; // a new run replaces unapplied results
        autotune::Config run = atCfg_;
        if (std::isfinite(c.a))
            run.fanTest = c.a != 0;
        autotune_.start(run, pv, fanBase_);
        if (!autotune_.running()) {
            setAutotuneStatus(AutotuneStatus::Failed, autotune_.failure());
            break;
        }
        if (st_.mode != Mode::Manual)
            setMode(Mode::Manual, pv);
        setAutotuneStatus(AutotuneStatus::Running);
        st_.mode = Mode::Autotune;
        break;
    }
    case Command::Kind::ApplyAutotune:
        if (!st_.autotunePending)
            break;
        st_.gains = autotune_.recommended();
        pid_.setGains(st_.gains);
        st_.gainsTuned = true;
        autotune_.fillReport(scratchReport_);
        model_ = roastermodel::merge(model_, roastermodel::fromReport(scratchReport_, autotune_.fanResult().startFanPct));
        portENTER_CRITICAL(&mux_);
        modelPublished_ = model_;
        portEXIT_CRITICAL(&mux_);
        st_.autotuneSeq++; // ControlPlugin saves the gains to NVS
        st_.autotunePending = false;
        setAutotuneStatus(AutotuneStatus::Done);
        break;
    case Command::Kind::DiscardAutotune:
        if (!st_.autotunePending)
            break;
        st_.autotunePending = false;
        setAutotuneStatus(AutotuneStatus::Discarded);
        break;
    case Command::Kind::StartProfile:
        if (modeNeedsTuning(Mode::Profile, lim_.requireTunedGains, st_.gainsTuned)) {
            setAutotuneStatus(AutotuneStatus::Required); // a loaded profile stays pending
            break;
        }
        portENTER_CRITICAL(&mux_);
        if (profilePending_) {
            profile_ = pendingProfile_;
            profilePending_ = false;
        }
        portEXIT_CRITICAL(&mux_);
        if (profile_.size()) {
            profileStartMs_ = millis();
            setMode(Mode::Profile, pv);
        }
        break;
    case Command::Kind::Stop:
        manualHeater_ = 0;
        setMode(Mode::Manual, pv, "stop");
        manualHeater_ = 0;
        if (c.b == 1) {
            assist_.reset();
            setBaseFan(c.a);
            cooldown_.start();
        }
        break;
    case Command::Kind::EmergencyOff:
        manualHeater_ = 0;
        setMode(Mode::Manual, pv);
        manualHeater_ = 0;
        assist_.reset();
        setBaseFan(100);
        cooldown_.start();
        break;
    case Command::Kind::SetTripFan:
        tripFan_ = c.a;
        break;
    case Command::Kind::ClearTrip:
        st_.trip = SafetyTrip::None;
        break;
    case Command::Kind::SetCorrection: {
        correction_ = {c.a, c.b};
        handlePvStep(pidInput(last_), "correction changed");
        break;
    }
    case Command::Kind::SetFanAssist:
        use_.fanAssist = c.a != 0;
        break;
    case Command::Kind::SetCooldownEnd:
        cooldownEndC_ = c.a;
        break;
    case Command::Kind::SetMainsTrip:
        lim_.requireMains = c.a != 0;
        break;
    case Command::Kind::Trip:
        st_.trip = static_cast<SafetyTrip>(static_cast<int>(c.a));
        manualHeater_ = 0;
        setMode(Mode::Manual, pv, tripName(st_.trip));
        break;
    }
}

void Controller::controlStep(uint32_t now) {
    last_ = tc_.read();
    const float pv = pidInput(last_);
    float out = 0;

    switch (st_.mode) {
    case Mode::Manual:
        out = manualHeater_;
        break;
    case Mode::Pid:
        spSlope_.update(st_.setpoint, pidCfg().dtSeconds); // a recipe ramp moves the setpoint
        out = pid_.update(st_.setpoint, pv, feedforward(st_.setpoint));
        if (!(st_.setpoint > 0))
            out = 0; // setpoint 0 = heater off (an idle switch to PID does not heat)
        break;
    case Mode::Profile: {
        st_.profileElapsedS = (now - profileStartMs_) / 1000.0f;
        const ProfileTarget t = profile_.at(st_.profileElapsedS);
        // NaN would pass fminf as maxSetpointC; treat it as 0 (heater off) instead.
        st_.setpoint = std::isfinite(t.setpointC) ? fmaxf(0, fminf(t.setpointC, lim_.maxSetpointC)) : 0;
        if (std::isfinite(t.fanPct))
            fanBase_ = fmaxf(0, fminf(t.fanPct, 100));
        spSlope_.update(st_.setpoint, pidCfg().dtSeconds);
        out = pid_.update(st_.setpoint, pv, feedforward(st_.setpoint));
        break;
    }
    case Mode::Autotune:
        out = autotune_.update(pv);
        if (!autotune_.running()) {
            // Heater off either way; ControlPlugin sets the stop cooldown fan.
            // Results wait for ApplyAutotune or DiscardAutotune.
            if (autotune_.phase() == autotune::Phase::Done) {
                st_.autotunePending = true;
                setAutotuneStatus(AutotuneStatus::Pending);
            } else {
                setAutotuneStatus(AutotuneStatus::Failed, autotune_.failure());
            }
            manualHeater_ = 0;
            st_.mode = Mode::Manual;
            out = 0;
        }
        break;
    }

    if (st_.mode != Mode::Pid && st_.mode != Mode::Profile)
        spSlope_.reset();

    // ---- fan: the fan test's level, or the base fan plus fan assist ----
    const float testFan = st_.mode == Mode::Autotune ? autotune_.fanRequest() : NAN;
    if (std::isfinite(testFan)) {
        assist_.reset();
        fan_.set(testFan);
        testFanOut_ = true;
    } else {
        if (testFanOut_)
            fan_.set(fanBase_); // fan test over or stopped: back to the operator's fan
        testFanOut_ = false;
        fanassist::Settings as = use_.assist;
        as.enabled = use_.fanAssist && model_.fan[0].valid;
        fanassist::Inputs in;
        in.mode = st_.mode;
        in.trip = st_.trip;
        in.btC = pv;
        in.setpointC = st_.setpoint;
        in.heaterPct = out;
        in.baseFanPct = fanBase_;
        const float before = assist_.extra();
        const float extra = assist_.update(as, in, pidCfg().dtSeconds);
        // Only while assist is (or was) active, so the base fan the operator set
        // is not rewritten every step (a trip's fan stays until a command).
        if (extra > 0 || before > 0 || st_.mode == Mode::Autotune || st_.mode == Mode::Profile)
            fan_.set(fanBase_ + extra);
    }

    // ---- safety interlocks (order matters: last one wins) ----
    const bool overTemp = overTemperature(last_, correction_, lim_.overTempC);
    if (overTemp) {
        st_.trip = SafetyTrip::OverTemp;
        assist_.reset();
        fanBase_ = 100;
        fan_.set(100);
    }

    if (sensorFault(st_.mode, pv, last_, out)) {
        if (!sensorBadSinceMs_)
            sensorBadSinceMs_ = now;
        if (now - sensorBadSinceMs_ > 2000)
            st_.trip = SafetyTrip::SensorFault;
    } else {
        sensorBadSinceMs_ = 0;
    }

    // Flash writes stall this task for tens of ms at a time: never heat during OTA.
    if (Update.isRunning())
        st_.trip = SafetyTrip::Firmware;

    st_.mainsOk = fan_.mainsDetected();
    if (mainsWatch_.update(out > 0 && lim_.requireMains, st_.mainsOk, now, lim_.mainsGraceMs))
        st_.trip = SafetyTrip::NoMainsSync;

    if (st_.trip == SafetyTrip::None) {
        tripFanApplied_ = false;
    } else {
        // Once per trip, so a fan command while tripped still takes effect.
        if (!tripFanApplied_) {
            assist_.reset();
            fanBase_ = st_.trip == SafetyTrip::OverTemp ? 100 : tripFan_;
            fan_.set(fanBase_);
            cooldown_.start();
        }
        tripFanApplied_ = true;
        out = 0;
        if (st_.mode != Mode::Manual)
            setMode(Mode::Manual, pv, tripName(st_.trip));
        manualHeater_ = 0;
    }
    // Fan floor only while the element actually switches on: a PID output below
    // one SSR pulse (idle PID near or above the setpoint) leaves the fan alone.
    if (heater_.heats(out) && fan_.get() < lim_.minFanWithHeat) {
        fan_.set(lim_.minFanWithHeat);
        if (!std::isfinite(testFan))
            fanBase_ = fmaxf(fanBase_, lim_.minFanWithHeat);
    }
    // ---- cooldown end: ET below the end temperature -> fan off, idle ----
    if (st_.mode != Mode::Manual)
        cooldown_.cancel();
    if (cooldown_.update(corrected(last_, correction_).et, cooldownEndC_, overTemp)) {
        assist_.reset();
        setBaseFan(0);
        out = 0;
    }
    heater_.set(out);

    // ---- publish ----
    publishProbes(last_, st_); // uncorrected: CroasterCore adds the correction itself
    st_.heaterPct = heater_.requested();
    st_.fanPct = fan_.get();
    st_.fanBasePct = fanBase_;
    st_.fanAssistPct = assist_.extra();
    st_.fanAssistOn = use_.fanAssist;
    st_.fanTested = model_.fan[0].valid;
    st_.cooling = cooldown_.active();
    st_.uptimeMs = now;
    st_.autotuneProgress = autotune_.progress();
    // The report (about 1 KB) is copied only when the sequence logged something.
    const bool reportChanged = autotune_.changes() != atReportSeq_;
    if (reportChanged)
        autotune_.fillReport(scratchReport_);
    portENTER_CRITICAL(&mux_);
    published_ = st_;
    if (reportChanged)
        atReport_ = scratchReport_;
    portEXIT_CRITICAL(&mux_);
    atReportSeq_ = autotune_.changes();
}
