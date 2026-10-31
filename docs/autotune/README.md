# Autotune

Croaster PID's autotune finds PID gains for the roaster it is installed in.
One command (`{"command":"autotune"}`, or **Start autotune** in the page's
Controls tab) runs the whole test: no setpoint is needed. The board heats to
each configured test temperature in turn, measures how the drum responds
there, and calculates gains from the measurements. Nothing changes until the
user saves the results.

Design decision: D29 in `docs/design.md`. Ticket: T57 in `docs/tickets.md`.

| Document | Contents |
|---|---|
| [process.md](process.md) | What a run does, phase by phase, with the state diagram, the safety stops and how long it takes |
| [tuning-rules.md](tuning-rules.md) | The method, the tuning rules compared on a simulated drum, and why the default is Ziegler–Nichols PI |
| [fan-test.md](fan-test.md) | The optional fan test: how airflow removes heat, what it measures, and how fan feedforward and fan assist use it |
| [implementation.md](implementation.md) | Code layout, protocol, settings, page, tests and build sizes |

## Goal

- Gains that track a roast curve on the board's own drum, heater and probe,
  measured rather than copied from other hardware (the defaults come from a
  different machine and are tuned for ET, see D18).
- Measured over the roasting range, not at one temperature: a drum roaster
  loses more heat as it gets hotter, so its response changes with
  temperature. The test covers three points by default, 250, 300 and
  350 °F (121.1, 148.9 and 176.7 °C), from the drying phase to the
  middle of the roast.
- No higher than it needs to go. The top point is 350 °F because this build
  bypasses the roaster's own controls, its thermal cutoff included, so the
  firmware's limits are the only protection while the drum runs empty and
  unattended. The gains still apply through a whole roast: the response
  changes slowly with temperature (about 10 % across the points on the
  simulated drum), and the recommended set is the most cautious of the
  points, so it keeps its margin above 350 °F
  ([tuning-rules.md](tuning-rules.md#simulation)).
- Visible: the page and the OLED show which point and which step the run is
  on, a progress bar with the time left, and a log of each measurement.
- Safe: the heater goes to 0 on any fault or stop, the board never exceeds
  its over-temperature limit, and every phase has a time limit.
- Reviewable: the results (per point and recommended) wait on the board until
  the user saves or discards them.

## Using it

1. Empty drum, drum turning, fan at the usual roast speed: set the fan
   before starting, because a fan change during the run stops it (D23).
   Start with the roaster cold or cooled: BT must be under the first test
   point plus 30 °C (about 300 °F) or the run refuses to start.
2. Controls > Autotune > **Start autotune**. The dialog says what will happen
   and about how long it takes, with the **Fan test** checkbox (on by
   default; it adds about 30 minutes, see [fan-test.md](fan-test.md)).
   Nothing starts until Start.
3. Watch the step list and the log. STOP, Heater off or Stop autotune end the
   run at any time; the heater turns off and the gains stay as they were.
4. When it finishes, a dialog shows each point's result and the current
   versus new Kp, Ki and Kd. **Save** applies the new gains and stores them in
   NVS; **Cancel** discards them. Closing the dialog with Escape leaves the
   results in the panel (Review and save…, Discard). A page reload shows the
   dialog again while the board still holds them. Results are kept in RAM
   only: a reboot drops them.

Autotune and a running recipe are mutually exclusive: starting one while the
other runs is refused with a message (`recipeRunning`, `autotuneRunning`).

## How this compares to 3D-printer autotune

Marlin (`M303`) and Klipper (`PID_CALIBRATE`) do not step through
temperatures: each run oscillates around the one target the user gives
(Marlin: `M303 S<temp> C<cycles>`; Klipper: `TARGET=`), with the heater
switched between full power and off, and computes Ziegler–Nichols gains from
that single point. Running it at a few temperatures is something users do by
hand. This autotune does the multi-point run itself because a roaster sweeps
roughly 150 °C during every roast, unlike a hotend that holds one
temperature. It keeps what those tools do well: Marlin's bias correction that
evens out the heating and cooling halves of each cycle, a minimum number of
cycles, and an overshoot limit. See [tuning-rules.md](tuning-rules.md) for
sources.
