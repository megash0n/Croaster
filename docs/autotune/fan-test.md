# Autotune fan test

An optional last stage of autotune, on by default
(`CPID_AUTOTUNE_FAN_TEST`, the **Fan test** checkbox in the start dialog). It
measures how airflow removes heat from the beans, so the controller can
allow for fan changes and use the fan to shed heat. Design decision: D30 in
`docs/design.md`. Ticket: T62.

## Why airflow matters to BT

The drum gets its heat from the element and loses it to the air drawn
through it and to the drum surfaces. More airflow carries more heat out by
convection, so at a fixed heater the beans settle at a lower temperature,
and when the fan steps up during a roast BT dips until the heater catches
up. The PID loop sees a fan change only after the dip has started, and the
integral term takes minutes to find the new heater level. The fan test
measures that loss directly at a few fan speeds, so the heater can move
together with the fan (feedforward) and the fan can do what the heater cannot
do once it is already at 0 %: take heat away.

## What a run does

After the last PID test point the gains are already calculated. The fan test
then runs at the middle test point (300 °F by default, or
`CPID_AUTOTUNE_FAN_TEST_C`), using the new gains in a PI loop:

1. **Approach.** From the top point BT coasts with the heater off until its
   10-second trend projects it onto the test temperature, then holds with the
   expected heater, then hands over to PI once BT stops moving toward the
   target or comes within 0.5 °C. The projection lead is twice the dead time
   the PID points measured (15 to 60 s).
2. **Hold at each fan level**, lowest first (default 30, 60, 90 %,
   `CPID_AUTOTUNE_FAN_LEVEL1..3`; 0 skips a level). PI holds the
   temperature in 60-second blocks until at least 90 s have passed, the block
   mean is within 1 °C of the target and the heater mean moves less than
   0.5 percentage points between blocks. The level's **hold heater** is the last block's mean,
   corrected to the target temperature. After each step up in fan the run
   records the **BT dip** below the target and the **recovery time** back to
   within 0.5 °C.
3. **Heater-off check**, highest level first. The heater goes off; after 40 s
   (the element and probe lag) a 20-second window fits the fall rate. The
   window ends early if BT falls 30 °C (`CPID_AUTOTUNE_FAN_SHED_DROP_C`),
   so the check never cools the drum far. Between levels the drum reheats
   (full heat, then the level's hold heater, then PI) and settles for 45 s
   within 2.5 °C of the target.
4. **Done.** The gains and the fan results wait in RAM for Save or Cancel;
   the board is in Manual with the heater off and the stop cooldown fan, as
   after a run without the fan test.

From the holds the run derives the **fan gain**, °C of steady BT per % fan at
a fixed heater: steady BT − ambient = heat in ÷ loss, and the hold heater is
proportional to the loss, so dBT/dfan = −(BT − ambient)·(dH/dfan)/H. From the
heater-off falls it derives the loss rate k = −slope / (BT − ambient), the
time constant 1/k and the **heater-off fall**, k·(target − ambient) in
°C/min, the rate BT would drop in the first minute with the heater off at
that fan.

The heater-off fall reads 65 to 100 % of the true rate, and the time
constant reads long: at 40 to 60 s after switching off, the element still
radiates some stored heat and the probe still lags. Longer waits would read
closer but cool the drum further. The value is used as a relative measure
between fan levels and for the assist band, not as an absolute cooling rate.

The fan test time limit is 40 minutes (`CPID_AUTOTUNE_FAN_TIME_LIMIT_MIN`).
Stopping or a fault during the fan test ends only the fan test: the gains
and the levels measured so far wait for Save as usual. A level gets 15
minutes to hold steady or to reheat; one that does not ends the test as
partial ("no steady hold"). Fewer than two held levels is a failure, and Save
then keeps the gains and the previous fan test.

## Results and saving

The results dialog and the Autotune panel show, per level: hold heater, BT
dip and recovery after the step, fan gain and heater-off fall. **Save**
stores the gains and a roaster model (per-point gain, time constant and hold
heater, plus the fan levels) in NVS (`model` blob, versioned). A later run
without a fan test keeps the saved fan results; airflow loss does not change
when gains are re-tuned. The model has room for a second fan channel
(`kMaxFanChannels`); only the exhaust fan is measured today.

## How the controller uses it

- **Fan feedforward** (`CPID_FAN_FEEDFORWARD`). In PID and Profile the heater
  gets the hold-heater difference between the current fan and the lowest
  tested level at the current setpoint, interpolated between levels and
  scaled by BT − ambient. The integrator carries the rest. Simulated 30 → 90 %
  fan step at 300 °F: BT dip 6.4 °C without, 2.4 °C with.
- **Fan assist** (`CPID_FAN_ASSIST`, default on; **Fan assist** in Controls,
  saved as `fanAs`). Late in drying and approaching first crack the beans
  can carry more heat than the curve wants: BT runs above the setpoint with
  the heater already at or near 0 %, and the heater has nothing left to
  remove. Fan assist then raises the fan above the user's or recipe's value,
  5 % per °C beyond a 2 °C band, up to `CPID_FAN_ASSIST_MAX` (default 20 %),
  rising at most 1 %/s and falling at 2 %/s, and returns it once BT is back on
  the path. It never lowers the fan below that value, never undercuts the
  heating fan floor, acts only in PID and Profile, never during cooldown, a
  trip or autotune, and does nothing until a fan test is saved. The extra is
  reported as `FanAssist` in data and shown under the fan tile on Live. On
  the simulated drum the effect is small (BT 205 → 170 °C with the heater off:
  78 s without, 76 s with) because the drum walls dominate its loss; on a
  roaster where airflow is a larger share of the loss it does more. The bench
  check measures it on the real drum.
- **Ramp feedforward** (`CPID_RAMP_FEEDFORWARD`). While the setpoint moves
  (a recipe ramp or a profile) the heater gets τ·slope/K from the test points
  at the current fan, limited to ±40 %. Simulated 8 °C/min ramp: mean lag
  3.5 °C without, 1.3 °C with.
- **Rate-of-rise check.** `getAutotune` returns the saved model (`saved`) so
  the recipe editor can warn when a ramp asks for more than the heater can
  deliver.

## Protocol

| Message | Effect |
|---|---|
| `{"command":"autotune","fanTest":false}` | Start without the fan test; omitted: `CPID_AUTOTUNE_FAN_TEST` |
| `{"command":{"fanAssist":true}}` | Fan assist on or off, saved |
| `getAutotune` `plan.fanTest`, `fanLevels`, `fanTarget`, `fanTimeLimit`, `fanEstimate` | The fan test plan and the time it adds |
| `getAutotune` `progress.fanLevel`, `fanLevels`, `fan` | Fan test progress; `fanLevels` is 0 when the run has no fan test |
| `getAutotune` `fanTest` | `status` (running, ok, partial, failed), `failure`, `channel`, `target`, `duration`, `levels[]` with `fan`, `hold`, `dip`, `recovery`, `gain`, `tau`, `shed` |
| `getAutotune` `saved` | The saved model: points, `fan.levels`, `heaterMax` |
| data `FanAssist` | Extra fan % from fan assist (part of `FanVal`) |
| `getControl` `fanAssist` | `enabled`, `available` (fan test saved), `extra`, `max` |

During the fan test the board drives the fan itself; a fan command from
outside stops autotune (D23), as during the PID points. The fan test's own
fan changes do not.

## Simulation (airflow drum, host test `test_at_fan_test_on_drum`)

| Fan | Hold heater (drum) | Dip / recovery | Fan gain | Heater-off fall (drum) |
|---|---|---|---|---|
| 30 % | 39.3 % (39.7) | — | −0.66 °C/% | 28.0 °C/min (36.6) |
| 60 % | 45.5 % (45.3) | 0.65 °C / 4 s | −0.51 °C/% | 31.3 °C/min (41.8) |
| 90 % | 50.5 % (50.3) | 0.65 °C / 19 s | −0.41 °C/% | 34.5 °C/min (46.5) |

Fan test 29 minutes (the PID points take 37); the start estimate is within
15 % of that.
