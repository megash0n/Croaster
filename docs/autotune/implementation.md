# Autotune implementation

## Code

| File | Role |
|---|---|
| `croaster-control/src/core/autotune_sequence.{h,cpp}` | `autotune::Sequence`: the multi-point run (plan, phases, relay with bias correction, convergence, Ku/Tu, model, rules, recommendation, log). Pure C++, host-tested. |
| `croaster-control/src/core/autotune_protocol.{h,cpp}` | `autotuneproto`: `getAutotune` report, `autotune` apply/discard parsing, log line text. Pure, host-tested. |
| `croaster-control/src/core/roaster_model.{h,cpp}` | `roastermodel`: the saved roaster model (per-point gain, time constant, hold heater; fan levels per channel), `merge`, hold heater, fan and ramp feedforward, highest rate of rise, `SetpointSlope`. Pure, host-tested. |
| `croaster-control/src/core/fan_assist.{h,cpp}` | `fanassist::Assist`: extra fan when BT runs above the setpoint with the heater off ([fan-test.md](fan-test.md)). Pure, host-tested. |
| `croaster-control/src/app/config_store.{h,cpp}` | NVS: the model blob (`model`) and the fan assist option (`fanAs`). |
| `croaster-control/src/core/autotune_status.h` | Telemetry `autotune` text and the 20-character OLED form, now with point, phase and cycle. |
| `croaster-control/src/app/controller.{h,cpp}` | Runs the sequence in the control task (250 ms), publishes `autotuneProgress` every step and the report when it changes, applies or discards results. |
| `croaster-control/src/app/AutotunePlugin.{h,cpp}` | Registers `getAutotune` and `{"command":{"autotune":...}}`; gives a finished run the stop path's cooldown fan; adds the saved model and the fan test estimate to the report. |
| `croaster-control/src/app/ControlPlugin.{h,cpp}`, `RecipePlugin.cpp` | Autotune and a running recipe exclude each other; `fanTest` on start, `fanAssist` command; saves the model with applied gains. |
| `croaster-webui/web/autotune.js` | Panel logic (texts, step list, result rows), shared with the Node tests. |
| `croaster-webui/web/app.js`, `index.html`, `app.css` | Controls > Autotune panel, results dialog, chart line. |
| `boards/esp32s3-pid/src/config.h`, `ConfigCheck.h`, `main.cpp` | `CPID_AUTOTUNE_*` settings, their checks, wiring. |

`core/relay_autotune.{h,cpp}` (the single-point relay) stays for its host
tests; the firmware no longer links it.

### Threading

`Sequence` runs inside the controller's FreeRTOS task. The task copies a
small `Progress` struct into the snapshot every step and the full `Report`
(about 1.2 KB: points, recommendation, 24 log entries) only when
`Sequence::changes()` moves. Both copies happen under the snapshot mutex.
`getAutotune` copies the report into the plugin's member buffer, not onto the
loop stack, and builds the JSON straight into the reply document (see T54 for
why replies avoid stack copies).

### Results

On Done the controller keeps the recommended gains as pending
(`Snapshot::autotunePending`, `AutotuneStatus::Pending`, text
`results ready`). Nothing is applied. `{"command":{"autotune":"apply"}}`
sets the gains, marks them tuned and saves them to NVS through the same path
as the `pid` command, with the roaster model (`roastermodel::merge` keeps the
previous fan test when the new run has none); `"discard"` drops them
(`discarded`). A new run, or a reboot, also drops them. The fan test runs
after the gains are calculated, so a stop or fault during it still leaves
the gains pending.

## Protocol

| Command | Reply |
|---|---|
| `{"command":"autotune"}`, `{"command":"autotune","fanTest":false}` | Starts a run (no setpoint); `fanTest` overrides `CPID_AUTOTUNE_FAN_TEST` for this run. Refused while a recipe runs: `{"command":"autotune","response":{"ok":false,"error":"recipeRunning"}}`. Ignored while a run is going. |
| `{"command":"getAutotune"}` | `{"command":"getAutotune","response":{...}}`, below. |
| `{"command":{"autotune":"apply"}}` / `"discard"` | `{"command":"autotune","response":{"ok":<results were pending>,"action":"apply"}}` |
| `{"command":"recipeStart"}` during autotune | `{"ok":false,"error":"autotuneRunning"}` |

Data samples carry `atTarget` (the current test point, display unit) while
the mode is Autotune, and `autotune` (status text) as before.

`getAutotune` response (temperatures in the display unit; swings as a
difference; gains per °C, as the controller uses them):

```json
{
  "status": "running: cycle 2/6 at 300 F (2/3)", "unit": "F",
  "running": true, "pending": false, "phase": "oscillating", "failure": "",
  "plan": { "points": [250, 300, 350], "settleCycles": 2, "minCycles": 3,
            "maxCycles": 6, "maxCv": 10, "rule": "ziegler-nichols-pi",
            "estimate": 3667, "timeLimit": 5400 },
  "progress": { "point": 1, "points": 3, "cycle": 2, "cycleOf": 6, "target": 300,
                "output": 66, "amplitude": 17.2, "period": 131,
                "elapsed": 1650, "remaining": 1000 },
  "current": { "kp": 1, "ki": 0.1, "kd": 0.01 },
  "results": [ { "target": 250, "status": "ok", "cycles": 3, "amplitude": 11.8,
                 "period": 141, "amplitudeCv": 0.009, "periodCv": 0.008,
                 "bias": 19.8, "step": 19.8, "ku": 3.88, "tu": 141,
                 "model": { "gain": 4.85, "tau": 420, "dead": 36 },
                 "kp": 1.745, "ki": 0.01489, "kd": 0 } ],
  "recommended": { "kp": 1.745, "ki": 0.01489, "kd": 0, "used": 1, "rule": "ziegler-nichols-pi" },
  "log": [ { "t": 0, "text": "point 1: heating to 250 F" } ]
}
```

`progress` is present only while running, `recommended` only after Done.
`phase` is `idle`, `heating`, `settling`, `oscillating`, `computing`, `done`,
`failed`, `aborted`, `fan-hold` or `fan-shed`; point `status` is `ok`,
`unsettled` or `small swing`. Fan test fields (`plan.fan*`, `progress.fan*`,
`fanTest`, `saved`) are listed in [fan-test.md](fan-test.md#protocol).

## Settings (`boards/esp32s3-pid/src/config.h`)

| Setting | Default | Meaning |
|---|---|---|
| `CPID_AUTOTUNE_POINT1_C` .. `POINT4_C` | 121.1, 148.9, 176.7, 0 | Test points in °C (0 = unused), 100 to under 240 with the default limits |
| `CPID_AUTOTUNE_RELAY_STEP` | 40 | Relay step either side of the bias, % |
| `CPID_AUTOTUNE_HYSTERESIS_C` | 1.0 | Relay hysteresis, °C |
| `CPID_AUTOTUNE_MIN_CYCLES` / `MAX_CYCLES` | 3 / 6 | Cycles that must agree / most cycles per point |
| `CPID_AUTOTUNE_MAX_CV_PCT` | 10 | Agreement threshold (coefficient of variation) |
| `CPID_AUTOTUNE_OVERSHOOT_C` | 30 | Fail above test point + this |
| `CPID_AUTOTUNE_TIME_LIMIT_MIN` | 90 | Whole-run limit |
| `CPID_AUTOTUNE_RULE` | `CPID_AUTOTUNE_RULE_ZN_PI` | 0 ZN PI, 1 TL PI, 2 TL PID, 3 SIMC PI |
| `CPID_AUTOTUNE_FAN_TEST` | 1 | Run the fan test after the points (the start dialog can turn it off) |
| `CPID_AUTOTUNE_FAN_LEVEL1` .. `LEVEL3` | 30, 60, 90 | Fan levels, % (0 = unused, else 10 to 100) |
| `CPID_AUTOTUNE_FAN_TEST_C` | 0 | Fan test temperature, °C (0 = the middle test point) |
| `CPID_AUTOTUNE_FAN_SHED_DROP_C` | 30 | Largest BT fall in a heater-off check (5 to 40) |
| `CPID_AUTOTUNE_FAN_TIME_LIMIT_MIN` | 40 | Fan test limit (10 to 90) |
| `CPID_FAN_FEEDFORWARD` / `CPID_RAMP_FEEDFORWARD` | 1 / 1 | Heater feedforward from the saved model |
| `CPID_FAN_ASSIST` / `CPID_FAN_ASSIST_MAX` | 1 / 20 | Fan assist default (page toggle, saved) / most extra fan, % (0 to 50) |

`CPID_REQUIRE_AUTOTUNE` (D24) is unchanged: saved autotune results count as
tuned.

## Page

Controls > Autotune: plan line (points, minutes), Start autotune (confirm,
OK/Cancel) or Stop autotune (confirm; sends `stop`), status text and refusal
message, progress bar with point and time left, step list (heat, settle,
measure per point, then calculate; done, current, failed), per-point table,
recommended gains with Review and save… and Discard, the fan test table, and
the board's log. Start opens a dialog with the plan, the time and the **Fan
test** checkbox (on by default); the time follows the checkbox.
The results dialog opens when results are waiting, including after a reload.
Live: the current test point is drawn as a dashed line while recording, and
the fan tile shows the fan assist extra. Controls > PID: Fan assist toggle.

The page asks `getAutotune` on connect, when the telemetry `autotune` text
changes, every 3 s while running and every 10 s while results wait. These
polls are read-only and kept out of the reply log.

## Tests

- `pio test -e native` (`boards/esp32s3-pid/test/test_core/test_autotune.cpp`,
  17 tests): FOPDT identification against analytic Ku/Tu, a full three-point
  run on the nonlinear drum (peak BT at least 4 °C under the overshoot stop)
  with closed-loop checks of the recommendation, the default points (250,
  300 and 350 °F, top point plus overshoot margin under 207 °C),
  every abort (sensor, over temperature, overshoot, no heat-up, no
  oscillation, time limit, command), refusal of a hot start and an empty
  plan, inconsistent cycles, plan clamping, rules, estimate, status text and
  the JSON report.
- `node --test croaster-webui/test/*.test.js` (`autotune.test.js` and
  `web_check.test.js`): confirm text, step states, progress, result and gain
  rows, refusals, polling, and that start and apply happen only from the
  confirm and the dialog.
- Fan test (`test_autotune.cpp`): full run on the airflow drum (hold heater
  within 2 % of the drum's, dips, gains, heater-off falls, duration and
  estimate), bounds, stop and probe fault during the fan test keep the
  gains, fan and ramp feedforward, highest rate of rise, fan assist bounds,
  `SetpointSlope`, `makeFanLevels`, `merge`, the `fanTest`/`fanAssist`
  protocol and the report's fan fields.
- Bench: `docs/test-plan.md` "Autotune" and "Autotune fan test".
