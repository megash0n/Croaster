# Croaster PID user guide

This guide is for roasting on a Behmor 2000AB fitted with the Croaster PID
board (ESP32-S3 build, `boards/esp32s3-pid`). It explains what each feature
does during a roast and why it helps. For how the firmware works, see
[how.md](how.md); for the reasoning behind each decision, see
[design.md](design.md); for autotune in depth, see
[autotune/README.md](autotune/README.md).

Temperatures are given in °F with °C in parentheses. The page shows the unit
set on the board (see "Units").

## Before you start

- Pin-by-pin wiring for the board and the optional fans: [wiring.md](wiring.md).
- The board drives the Behmor's heater and fan directly. **The Behmor's own
  controls and thermal cutoff are bypassed**, so the board's limits are the
  only protection. Stay with the roaster whenever the heater is on.
- Heater % is a request. The board scales it to a hard ceiling of 90 % duty,
  so "Heater 100 %" on the page means 90 % element on-time.
- Whenever the heater is on, the board keeps the fan at 30 % or more, in
  every mode, so the element always has airflow.
- A page or app must stay connected while the board heats, except in Profile
  mode. If the board hears nothing for 10 seconds it turns the heater off
  (the comms watchdog, see "Safety trips").

## Roasting terms used here

- **BT** (bean temperature) is the probe in the bean mass. **ET** is the
  air or environment probe.
- **RoR** (rate of rise) is how fast BT is climbing, in °F per minute. A
  roast is steered by its RoR more than by any single temperature.
- **Charge** is when the beans go in. Cold beans pull BT down; the lowest
  point after charge is the **turning point** (TP).
- **Drying** runs from TP to roughly 300 °F (149 °C), where the beans turn
  from green to yellow. **Maillard** browning follows.
- **First crack** (FC) is the audible crack as water vapour and CO2 split
  the beans, typically around 385 to 400 °F (196 to 204 °C) on a BT probe,
  depending on where the probe sits. **Development** is the time from first
  crack to drop.
- **Momentum** is the heat the bean mass carries. Beans keep climbing for a
  while after the heater is cut, so a roast arriving at first crack with a
  high RoR tends to rush ("flick"), and one arriving with almost no RoR can
  stall ("crash"). A steadily falling RoR into first crack is the usual aim.
- **Airflow** removes heat by convection and carries away chaff and smoke.
  More fan lowers BT at a given heater and slows the climb.

## Modes

The Live tab shows the current mode in the Mode tile. Controls > Outputs and
setpoint has Manual and PID buttons.

### The principle

Manual does exactly what you set, or exactly what a recipe step says. The
helpers that act on their own (fan feedforward, ramp feedforward and fan
assist) only run when the board is controlling BT to a setpoint, in PID or
Profile mode. Two rules apply in every mode, including Manual: the 90 %
heater ceiling and the 30 % fan floor while heating.

### Manual

Heater and fan stay at the values you set on Controls or that a recipe power
step sets. Nothing adjusts them for you. Use Manual when you want to steer
by hand, as you would with the Behmor's P1 to P5 buttons, but in 1 % steps.

### PID

The board holds BT at the setpoint by moving the heater. With a saved
autotune it also uses the roaster model:

- **Fan feedforward**: when the fan changes, the heater moves at the same
  moment by the amount the fan test measured, instead of waiting for BT to
  dip. This keeps the curve smooth when you add airflow late in the roast.
- **Ramp feedforward**: while the setpoint climbs (a recipe ramp or a
  profile), the heater adds the extra power that climb needs, so BT lags the
  plan less.
- **Fan assist** (if enabled): see "Fan assist" below.

PID and Profile are refused until an autotune has been saved ("autotune
required"); this also applies to recipe setpoint steps and the charge
temperature. The PID input is BT by default; Controls > PID input can switch
it to ET or MAX (the higher of the two). The setpoint is capped at 482 °F (250 °C).

### Profile

A timed schedule of setpoints, set in Controls > Profile: each step has a
duration, a setpoint, how the setpoint moves to it, and an optional fan %.
Load to device, then Start profile. The board runs the schedule in PID with
the same helpers. Profile mode keeps running without a connected page (the
comms watchdog does not apply).

### Recipes

A recipe is not a separate mode. It is a list of steps the board applies
during the roast, and each step switches the mode it needs: a power step
switches to Manual with exactly the step's heater % and fan; a setpoint step
switches to PID. See "Recipes".

### Autotune

The board runs the heater itself to measure the roaster. You cannot change
the heater or fan during a run; doing so stops it. See "Autotune".

## Web page walkthrough

Open the board's address in a browser. The page has three tabs in the side
bar, plus the Events list, Session buttons and a Replies log.

### Monitoring

- **Live readings**: every value the board sends.
- **Control state**: mode, setpoint, gains, trips and settings.
- **Device**: firmware, board and network details.
- **Connection**: the page's link to the board: address, state, last
  message, round trip time, connections opened, last close reason, board
  WiFi signal, uptime and restarts seen. If the page keeps reconnecting,
  this card shows why.

### Live

Tiles for BT, ET, RoR, heater, fan, setpoint and mode, a chart of BT, ET and
RoR over time with heater and fan strips beneath, and the event buttons.
"Smoothed RoR (30 s)" replaces the board's RoR with a 30-second slope, which
is steadier for judging the trend. A recipe panel appears when a recipe is
loaded (see "Recipes").

### Controls

Heater and fan steppers, setpoint, mode, PID input and gains, fan assist,
safety settings, autotune, profile and recipe editors, the device library,
device settings (unit, corrections) and maintenance.

### Roast flow

1. **Preheat (optional).** While no roast is recording you can heat from
   Controls: set a heater % in Manual, or a setpoint in PID. A recipe with a
   charge temperature does this for you (see "Charge temperature").
2. **Start.** Press Start in the top bar. This only begins a recording in
   the page; nothing is sent to the board. The clock reads "Preheat" and
   counts from Start, so the preheat is on the chart. If an earlier roast is
   still kept, the page asks before clearing it.
3. **Charge.** Press Charge on Live when the beans go in. The roast clock
   starts at 0:00 here, and the board starts its own roast clock for recipe
   time triggers.
4. **Events.** Mark Dry end, FC start, FC end, SC start, SC end and Drop as
   they happen. The turning point is found automatically ("Find TP
   automatically") once BT has risen a couple of degrees past its lowest
   point; untick it to mark TP by hand. Undo last event removes the most
   recent mark. Events are sent to the board, so recipe steps can wait for
   them.
5. **Stop.** STOP in the top bar asks for confirmation, then turns the
   heater off, switches the board to Manual (ending PID, a profile, a recipe
   or autotune), sets the setpoint off, sets the cooldown fan and ends the
   recording. End roast asks the same way and does the same.
   Drop is the end of heating: it marks Drop, then asks whether to end the
   roast now (Cancel keeps heating; Drop stays marked). The save dialog
   opens once per roast: at Drop or End roast, else after STOP or when the
   cooldown ends.
6. **Keep or clear.** Download CSV saves the samples and events. Clear roast
   deletes the last roast from this browser after asking; the board is not
   changed.

Controls > Safety > Heater off works at any time, recording or not.

## Autotune

Autotune measures how your roaster responds and calculates PID gains for
it. Run it once before using PID, recipes with setpoint steps or profiles,
and again if you change the probe, the drum or the mains supply. Method and
results: [autotune/process.md](autotune/process.md) and
[autotune/tuning-rules.md](autotune/tuning-rules.md).

### What it does

The board heats to three test points, 250, 300 and 350 °F (121, 149 and
177 °C), lowest first. At each point it switches the heater between two
levels so BT cycles a few degrees around the point, and measures the size
and period of the swing until three cycles agree. From that it calculates
gains for each point and recommends the most cautious set. The top point is
kept at 350 °F because the Behmor's own cutoff is bypassed.

### Before you start

- Empty drum, chaff tray in, drum turning, door closed.
- BT under 304 °F (151 °C). A hotter start is refused with "too hot to
  start"; let it cool.
- Fan at your usual roast speed. A fan below 30 % is raised to 60 % at the
  start. Changing the fan or heater during the run stops it.
- Plan on 45 to 65 minutes on a Behmor, plus about 29 minutes for the
  optional fan test. The run gives up after 90 minutes.
- **Stay with the roaster for the whole run.**

### Running it

Controls > Autotune > Start autotune opens a dialog with the estimated time
and the Fan test checkbox. Start begins the run. The panel shows progress,
the steps, time left and a log line per cycle. While it runs, the Live
chart plots BT, ET, RoR, heater and fan against the dashed test-point line,
labelled with the current point and cycle. That plot is not recorded or
saved and clears when the run ends. A roast recording, when one is running,
takes the chart instead.

Note the highest BT at each point. The run stops itself if BT passes a point
by 54 °F (30 °C): above 304, 354 or 404 °F (151, 179 or 207 °C).

Stop autotune turns the heater off and keeps the current gains.

### Results

When the run ends, the board turns the heater off and the results window
opens. It shows each test point's result and gains, the current and new Kp,
Ki and Kd side by side, and the fan test results if it ran.

- **Save** applies the new gains and stores them, with the roaster model,
  on the board. They survive a reboot.
- **Cancel** keeps the current gains and discards the results.
- Closing the window without choosing keeps the results waiting; "Review
  and save…" reopens it. A reboot before choosing discards them.

## Fan test and fan assist

### What the fan test measures

The fan test is an optional last stage of autotune, ticked by default. At
the middle point (300 °F, 149 °C) the board holds BT steady at fan 30, 60
and 90 % and records, at each level:

- the heater needed to hold the temperature (more airflow needs more heat);
- the BT dip and recovery time after the fan steps up;
- the fan gain: how much steady BT falls per % of fan at a fixed heater;
- how fast BT falls with the heater off, which is how much heat the airflow
  can take away. Each heater-off check ends after at most 54 °F (30 °C) of
  fall.

Details: [autotune/fan-test.md](autotune/fan-test.md). Save stores the fan
results with the gains. A later autotune without the fan test keeps the
saved fan results.

### How the results are used

- **Fan feedforward** (PID and Profile): the heater moves with the fan by
  the measured amount, so raising the fan for development does not drop BT
  before the PID reacts.
- **Fan assist** (PID and Profile, when enabled): approaching first crack
  the beans can carry more heat than the plan wants. BT runs above the
  setpoint with the heater already near 0 %, and the heater has nothing left
  to remove. Fan assist then adds airflow to shed heat: about 3 % per °F
  (5 % per °C) once BT is more than about 4 °F (2 °C) over the setpoint, up
  to 20 % above your fan, rising at most 1 % per second. It eases back once
  BT is on the path.
- **Ramp feasibility**: the recipe editor uses the model to warn about
  ramps the roaster cannot follow (see "Ramp shapes").

### Fan assist limits

- It never lowers the fan below your value or the recipe's.
- It acts only in PID and Profile: never in Manual, during autotune, after
  STOP (cooldown) or during a trip.
- It does nothing until a fan test has been saved.
- Live shows the extra as "+N assist" under the Fan tile.

### Turning fan assist on or off

Controls > Outputs and setpoint > Fan assist: tick or untick "enabled" and
press Save. It is on by default, and the setting survives a reboot.

## Recipes

A recipe is a list of steps the board applies at points in the roast. Build
it in Controls > Recipe, press Load to device, then Run recipe (on Controls
or on Live). Run recipe also starts a roast recording when none is running,
as Start does, and the saved roast keeps the recipe's name. Up to 32 steps,
each with up to 4 actions.

### Triggers

Each step has one condition:

- **Time** since Charge (m:ss). Time steps wait until Charge is marked.
- **BT** or **ET** at or above, at or below, or near a temperature.
- **RoR** at or above, at or below, or near a rate.
- **Event**: Charge, Turning point, Dry end, FC start, FC end, SC start, SC
  end or Drop, as marked on Live.

A step fires once, when its condition changes from false to true. A
condition that is already true when the step comes up must go false first.
Steps are taken in order unless "any order" is ticked for a step.

### Actions

- **Heater % and fan %** (the default for a new step): switches to Manual
  and applies exactly those values. Use it for open-loop roasting by power,
  the way most Behmor roasts are run.
- **Setpoint**: switches to PID at that temperature, optionally ramping to
  it over a time (see "Ramp shapes").
- **Fan**: sets the fan only.
- **Hold**: stops a running ramp where it is.
- **Cool**: the same as STOP: heater off, cooldown fan, recipe ends. The
  page recording continues until you end it.
- **End**: the recipe ends and the heater and fan stay as they are.
- **Note**: logged only.

### Auto and guided

In Auto mode the board applies each step as it fires. In Guided mode the
page shows the step with Set now and Skip; a prompt not answered before the
next step fires is marked missed.

Changing the heater or fan by hand while a recipe runs is allowed and does
not abort it. STOP, Abort recipe, a trip or a firmware update ends the
recipe.

### Charge temperature

"Charge at" is optional. When set, Run recipe switches the board to PID at
that temperature so the drum is preheated when you charge. Preheating is
not treated as following the curve, so the behind-schedule warning stays
off until the first setpoint step.

### Behind-schedule warning

While a setpoint step is being followed, Live warns that the roaster cannot
keep up when BT is more than the warning threshold below the setpoint
(default 15 °F, 8 °C, set in Controls > Recipe) and the heater has been at
100 % for 20 seconds. It clears when BT catches up. A roast that falls this
far behind is losing momentum, usually because of a large batch, low line
voltage or too much fan; lengthen the ramp or reduce the fan.

### Device library

Profiles and recipes can be saved on the board, up to 50, in its flash
storage. In Controls > Device library:

- **Save editors to device** stores the current profile and recipe editors
  under a name, with notes.
- **Load to board**, **Edit**, **Export** (download as a file) and
  **Delete** for each saved entry.
- **Import from PC** reads an exported file; you can save it to the board
  or open it in the editors.

Save and Delete are refused while the heater is on, a profile is running or
autotune is running, so a flash write never interrupts a roast.

## Ramp shapes

A setpoint or power step with a ramp time takes a shape. The shape answers
the question that matters most in a roast: how fast BT should climb at each
moment, not only where it ends. Design notes: D31 in
[design.md](design.md).

### Reading the shape

The shape's vertical axis is the rate of climb, drawn over the ramp time.
For a setpoint step that is the RoR the plan asks for; for a power step it
is how fast the heater % moves toward its target. The area under the curve
is the progress, so every shape reaches the target exactly at the end of
the ramp time; the shape only changes how the climb is spread.

- **Linear**: a constant rate.
- **Normal**: slow start, fastest in the middle, slow finish.
- **Front**: fastest at the start, easing into the target. This gives the
  declining RoR most roasters aim for between drying and first crack.
- **Back**: slow start, fastest at the end.
- **Custom**: your own curve. Edit opens a touch editor with 21 points, one
  every 5 % of the ramp time, each 0 to 100 in steps of 5. Drag the points,
  start from a preset, and watch the path preview below. The text gives
  where the climb is steepest (every place, if it ties) and how many times
  steeper than linear it is. Only the shape counts, so on release the points
  scale until the tallest is at 100. Use curve saves it.

The spark next to the Shape menu previews the path.

### Where a ramp starts

A setpoint ramp starts at BT when its step fires (at the PID input, if you
changed that to ET or MAX), whatever mode or setpoint came before. The
shape is spread over the whole climb from there to the target, so a front
ramp still climbs hardest first. The PID takes over from the heater % it
finds: after full power in Manual it carries on near full power instead of
cutting the heater, so BT keeps its momentum.

- If BT is already at or above the target, the step does not ramp. It sets
  the target at once and the PID holds it. Ramps never go down.
- If the PID input cannot be read at that moment, the ramp starts at the
  current setpoint when that is below the target, or the step sets the
  target at once. A sensor trip follows within 2 seconds in either case.
- Each ramp starts at BT again, so if BT lagged a ramp, the next one starts
  from where the beans are.

A power ramp starts from the current heater %.

The feasibility check below does not know BT ahead of time. It assumes a
ramp starts at its step's BT trigger value, or else at the target of the
setpoint step before it. Give a ramp step a BT trigger when you want the
check to be exact.

### Feasibility warning

With a saved autotune, the editor checks each rising setpoint ramp. It
takes the steepest rate the shape asks for, finds the temperature where
that happens, and compares it with the most the model says your roaster can
climb there at that fan with the heater at its ceiling. If the ramp asks
for more, a warning appears under the step, for example "Needs 30 °F/min
near 380 °F; the saved autotune model gives about 22 °F/min at fan 40 %."
Lengthen the ramp, lower the fan or pick a flatter shape.

### On Live

While a setpoint ramp runs, Live draws the rest of the plan as a dashed
setpoint line and shows a line such as "Setpoint ramp 300 °F → 390 °F,
front, 0:40 of 4:00." The behind-schedule warning follows the shaped plan.

### What the shapes do to BT

On the simulated drum (fan 60 %, 302 to 392 °F or 150 to 200 °C, over 5
minutes, with feedforward), BT followed each shape:

- **Front**: about 29 °F/min (16 °C/min) in the first half and 6 °F/min
  (3 °C/min) in the second, close to the plan's 27 and 9. Lag is largest at
  the steep start (up to 16 °F, 9 °C), from the time heat takes to reach
  the beans. Overshoot under 1 °F.
- **Normal**: about 16 °F/min in each half, overshoot under 1 °F.
- **Linear**: about 19 then 15 °F/min, overshoot about 3 °F (2 °C).
- **Back**: about 8 then 25 °F/min, and BT overshot the target by about
  9 °F (5 °C). It ends at its fastest rate, so the beans carry that
  momentum past the target. It also asked for more than the model allowed
  near the top, and the editor flags it.

In practice:

- Use **front** or **normal** into first crack. Both arrive with a falling
  or low RoR, which avoids a flick.
- Front and normal ease to almost no climb at the end of the ramp. Set the
  ramp target a little above where first crack happens on your probe and
  start the next step on FC start, so the roast reaches first crack while
  BT is still climbing and does not stall.
- Avoid **back** before first crack; reserve it for short moves where
  overshoot does not matter.
- A steep front ramp starts with a large lag; start it after drying, when
  the beans take heat readily, and check the feasibility warning.

### Worked example

A Behmor roast, about half a pound, with a first crack near 390 °F
(199 °C) on your probe. Adjust the temperatures to your probe and beans.

- Charge at: blank (start cold, as a Behmor usually does).
- Step 1. When: event Charge. Then: heater 100 %, fan 30 %. Full power
  through the turning point and drying, in Manual.
- Step 2. When: BT at or above 300 °F (149 °C). Then: setpoint 400 °F
  (204 °C) over 6:00, shape Front. The ramp starts at BT, about 300 °F, and
  the PID carries on from 100 %. The plan climbs at about 33 °F/min at
  first and eases toward first crack, giving a declining RoR. If the plan
  starts faster than the beans can follow, the heater simply stays at full
  until BT meets it.
- Step 3. When: event FC start. Then: heater 60 %, fan 50 % (for example).
  Fixed power and more airflow through development, in Manual: no helpers
  change these values.
- Step 4. When: event Drop. Then: Cool.

Before the roast, check the step 2 row for a feasibility warning. During
the roast, watch the dashed plan on Live: if BT runs well below it with the
heater at 100 %, the behind-schedule warning appears; for the next roast
lengthen step 2's ramp or lower its fan.

## Safety trips and cooldown

### Trips

A trip turns the heater off, switches the board to Manual, sets the trip
cooldown fan (100 % by default) and shows a banner across the top of the page. The
Mode tile shows "TRIP" and the reason. Fix the cause, then press Controls >
Safety > Clear trip. Clearing does not restart heating; set the heater or
mode again. If the cause is still there, the trip comes back at once.

- **overtemp**: a probe read above 527 °F (275 °C). The fan always goes to
  100 % for this trip.
- **sensor**: the PID input probe stopped reading for 2 seconds in PID,
  Profile or autotune, or both probes stopped reading while Manual heats.
  Check the thermocouple plugs and wires.
- **comms**: no message from any page or app for the watchdog timeout
  (10 seconds by default) while heating, outside Profile mode. Usually WiFi
  dropped or the page was closed. Controls > Safety > Comms watchdog sets
  the timeout (2 to 600 s) or turns it off.
- **ota**: a firmware update is being written. The heater stays off until
  it finishes.
- **nomains**: the fan dimmer has seen no mains zero-cross for 2 seconds
  while heating. Controls > Safety > Mains trip turns it on or off (on by
  default on AC dimmer builds; saved on the board). Turning it off asks
  first: the heater then keeps running when mains to the fan is lost. It is
  meant for bench tests without mains.

### Cooldown

After STOP, Heater off, or a recipe Cool step, the heater goes off, the
board switches to Manual and the fan runs at the cooldown speed: 65 % by
default. After a trip it runs at the trip speed: 100 % by default. Both are
set in Controls > Safety > Cooldown fan and survive a reboot.

The cooldown fan stays at that speed until you change it. The board does not
end cooldown on its own; for example, it does not stop the fan when ET falls
below a temperature. Watch the beans and turn the fan down when they are
cool.

## Units

Controls > Device settings > Unit sets °F, °C or K for the page, the
display and connected apps. The board saves the unit and uses it after a
reboot; until one is saved the build default is °F.

Saved profiles and recipes keep their own unit and are converted when
loaded or shown. The controller and its safety limits work in °C
internally, whatever unit is shown.

Maintenance > Fake readings (dummy mode) follows the unit: about 86 to
104 °F, or 30 to 40 °C. Fake readings feed the page and connected apps only;
the PID, autotune and safety checks always read the real probes. The
Fake readings button is green while fake readings are on, so a page left
in dummy mode is easy to spot. It reads the board's state from the data it
sends every second: it is correct after a page reload and turns green or
back within about a second of a switch or a reconnect. While the page is
not connected the button shows off.

## See also

- [how.md](how.md): how the board, page and recipes work inside.
- [autotune/](autotune/README.md): autotune goal, process, tuning rules and
  the fan test.
- [design.md](design.md): decisions D28 (library and recipes), D29
  (autotune), D30 (fan test and feedforward) and D31 (ramp shapes).
- [test-plan.md](test-plan.md): bench checks for each feature.
- [Board README](../boards/esp32s3-pid/README.md): wiring, settings and
  commands.
