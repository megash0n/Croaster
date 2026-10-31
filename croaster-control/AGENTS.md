# croaster-control — rules for the control add-on

Heater, fan, PID, safety, autotune and recipes for Croaster PID. Layout and
data flow: `docs/how.md`; decisions: `docs/design.md`.

## Layers
- `src/core` — pure C++ without Arduino or ESP-IDF headers, host-tested.
  Control logic, parsers and protocol JSON go here.
- `src/hw` — ESP32 drivers (heater SSR, fan dimmer or PWM, thermocouples).
- `src/app` — the control task, NVS settings and the plugins that connect
  core to Croaster (`ControlPlugin`, `RecipePlugin`, `AutotunePlugin`,
  `LibraryPlugin`). Every heater, fan, mode or setpoint change goes through
  `Controller::submit`, so interlocks, caps and trips apply.

## Safety
- Trips (over temperature, sensor, comms watchdog, mains) win over every
  other source and set the trip cooldown fan. A new feature that drives the
  heater states how each trip, STOP and OTA end it.
- Values from JSON are checked and refused with a reason, not clamped
  silently; the controller clamps again on submit.
- Temperatures in core are °C; convert at the protocol edge with the unit
  the message or recipe carries.

## Tests
- `cd boards/esp32s3-pid && pio test -e native` runs every core test
  (`test/test_core/`, one file per area, each called from `test_main.cpp`).
- Control behaviour is checked on the simulated drum in `test_autotune.cpp`
  (heat capacity, dead time, fan loss): report the numbers that matter for a
  roast (lag, overshoot, rate of rise) in the decision and the test plan.
- Anything the page mirrors (recipe limits, ramp shapes, `maxRorCpm`) has a
  matching test in `croaster-webui/test/`.
