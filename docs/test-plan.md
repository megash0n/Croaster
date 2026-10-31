# Functional test list — Croaster PID control board

A running list of behaviours to build a proper test plan around. One line per
check: what to do, then the expected outcome after the arrow. Add to it as
features and bench sessions happen.

Status tags:
- **host**: covered by a host test (`pio test -e native` in
  `boards/esp32s3-pid`); the test name follows.
- **host (fork)**: covered by a host test in the `MAX6675_Thermocouple` fork
  (`test/host`, run steps in its README); the test name follows.
- **build 2026-10-26**: checked by a clean build on that date.
- **bench 2026-10-26**: seen on the bench on that date (ET probe, OLED and SSR
  DC side connected; no BT probe, no AC, `CPID_REQUIRE_MAINS 0` except where
  noted).
- **untested**: not checked yet. "Bench todo" marks items queued for the next
  session.

## Probes and ambient
- ET probe at room temperature → ET about 74 °F in telemetry and on the OLED. **bench 2026-10-26**
- MAX31855 frame decode (temperature, cold junction, sign) → correct °C values. **host** `test_max31855_decode`
- Open, short-to-GND and short-to-VCC faults → reading rejected, cold junction still valid. **host** `test_max31855_fault_bits`
- MAX6675 frame decode and open-probe flag → correct °C or rejected. **host** `test_max6675_decode`
- A fault on one probe → the other probe keeps reporting. **host** `test_probe_channels_independent_faults`
- Moving average → seeded with the first sample, no ramp from 0. **host** `test_moving_average_seeds_with_first_sample`
- No BT chip fitted → `Amb` is the ET chip's board temperature, not 32 °F. **host** `test_board_temperature_skips_missing_chip`; bench todo
- BT chip fitted without a probe → `Amb` from the BT chip, BT absent from telemetry. **host** `test_board_temperature_skips_missing_chip`; bench untested
- Unplug the PID input probe in PID mode → `trip:"sensor"` after 2 s, heater 0. **untested**
- Manual, heater on, both probes unplugged → `trip:"sensor"` after 2 s, heater 0; heater 0 → no trip; one probe left → no trip (D23). **host** `test_sensor_fault_rules`; bench untested

## MAX6675 library fork (D21)
Stock MAX6675 boards (`esp32s3`, `esp32c3`, `esp8266`) read probes through
`SmoothThermocouple`; neither Croaster PID nor Croaster monitor does (both read through
`croaster-control` `hw/thermocouples`; the monitor through `app/MonitorProbes`, D11, D26).
- First reading seeds the average; 0 °C keeps the average. **host (fork)** `testFirstSampleSeedsAverage`, `testZeroKeepsAverage`, `testZeroSeed`
- 100, 100, NAN, 100 → 100, 100, NAN, 100 (recovers). **host (fork)** `testRecoversAfterNan`
- MAX6675 frames decode as before, with a wait before every pin change; 33 us of waits per read. **host (fork)** `testMax6675Decode`, `testMax6675ReadTime`
- MAX6675 board, logic analyser on SCK and CS → CS low for well under 1 ms per probe (was about 33 ms), SCK period 2 us or more, reads 250 ms apart. **untested**
- MAX6675 board at room temperature and in boiling water → same readings as on the old library (about 100 °C / 212 °F in water). **untested**
- MAX6675 board, unplug one probe → that probe `N/A` on the screen and absent from `getArtisanData`, the other unaffected. **untested**
- Plug it back in at a different temperature → the new temperature on the next sample, no slow ramp from the old value (before: `N/A` until reboot). **untested**
- `esp32s3-debug` (stock) links without `SmoothThermocoupleFix.cpp`. **build 2026-10-26** (upstream board files, in a copy pinned to `espressif32 @ 6.13.0`; see T13)
- Croaster PID after the library change: ET at room temperature as before; unplug ET → `N/A`, replug → reading returns. **untested**

## Croaster monitor (`boards/esp32s3-monitor`, D11, D26)
- Builds with `CMON_TC_MAX31855` and with `CMON_TC_MAX6675`. **build 2026-10-28** (MAX6675 in a temporary config copy; RAM 54 652 B, flash 1 658 691 B for both)
- MAX31855 monitor: BT and ET at room temperature on the OLED and in `getArtisanData`; boiling water ≈ 100 °C / 212 °F. **untested**
- MAX6675 monitor: same checks on MAX6675 modules. **untested**
- Unplug one probe → that probe `N/A` after the third bad read (under 1 s), the other unaffected; plug back → reading returns. **untested**
- No control code: `getControl` gets no control reply; no heater, fan or control symbols in the ELF. **build 2026-10-28** (ELF symbols); reply untested

## Browser UI (`croaster-webui`, D27)
- Roast math in the page: least-squares RoR, turning point, unit conversion, axis ticks, clock, CSV. **host** `node --test croaster-webui/test/*.test.js` (`roast.test.js`)
- Page checks: every script parses, nothing loaded from outside the board, every element id the script uses exists, every form has a command, `restartesp`/`erase`/`dummyToggle`/`wifiConnect` ask first. **host** `web_check.test.js`
- Appearance (T50): hex parsing, every preset complete and above 4.5:1 on its text pairs, neon chart series apart from green, migration of `"dark"`/`"light"`/JSON/junk, validation and clamping, import/export round trip and rejects, WCAG contrast values, translucent cards blended over the background, CSS properties. **host** `theme.test.js`; `:root` fallbacks equal the default scheme and every token is used, `theme.js` loads in `<head>`. **host** `web_check.test.js`
- Asset step: same output twice, gzip without timestamp, committed header current, under 100 KB. **host** `python3 -m unittest discover -s croaster-webui/test`
- Page against a mock board (Chrome headless, WebSocket on another port): tiles, chart, auto turning point, Monitoring and Controls render, phone width stacks the layout, no script errors. **build 2026-10-29**
- Appearance against the mock board (Chrome headless): a saved `"dark"` becomes the neon JSON scheme; presets switch colours live including the chart; a low-contrast text colour lists warnings; an invalid hex is marked and not applied; a pasted JSON with one bad value loads the rest and names the skipped value; non-JSON is refused; line width and density apply; the panel fills a 390 px phone screen without sideways scroll; no script errors. **build 2026-10-29**
- Appearance on a phone (iOS Safari and Android Chrome) against the board: colour pickers open, Download JSON saves a file, Load file reads it back, the scheme survives a reload. **untested**
- First boot with no WiFi saved → portal on port 80 works as before; the page is not served until the board joins WiFi and the portal closes. **untested**
- Open `http://<board ip>/` → BT, ET, RoR, heater, fan, SV and mode match the OLED within one second; Monitoring lists the same values as `getArtisanData`, `getControl` and `getDeviceInfo`. **untested**
- Croaster monitor → no Controls tab and no STOP button; Start and End roast in the top bar (End roast greyed out until Start), End roast asks first and sends nothing. **untested** (mock board at 320 px: **build 2026-10-29**)
- Views and Start (T52) against a mock board (Chrome headless): `/`, `#monitoring`, `#live`, `#controls` and an unknown hash each load for 3 s and send only `getControl`/`getDeviceInfo` besides `getArtisanData` (no other command), open Monitoring unless the hash names a view, and show Idle with Start enabled and STOP, every event button and Undo disabled; tab clicks send nothing; Controls steppers are enabled while idle; Start → Preheat, Start greyed out, STOP enabled, only Charge enabled, nothing sent; Charge → clock 0:00, earlier samples kept before it, Dry end/FC start/Drop enabled and FC end/SC start not; FC start → Dry end disabled, FC end and SC start enabled; Drop → all events disabled and End roast shown; STOP → Idle, Start enabled; End roast after Drop → Idle, nothing sent. **build 2026-10-29**
- Recording (T51) against a mock board (Chrome headless): connecting, switching Live/Monitoring/Controls and reloading leave the clock at Idle with no samples; STOP greyed out and a click opens nothing; Start → Preheat and samples; Charge → Roast; STOP → confirm, Cancel sends nothing, Confirm sends `{"command":"stop"}` once, clock Idle, STOP greyed out, no samples after a reload, "board reports heater 0 %, Manual, fan 65 %" logged; Heater off asks first and sends `stop` without starting a roast; Start over an old roast asks first; a saved roast with its last sample an hour old loads as Idle (ended); no script errors. **build 2026-10-29**
- Session state, stale-session check and stop confirmation (`isRecording`, `endSession`, `resumeSession`, `sessionState`, `stopConfirmed`) and event order (`eventAllowed`). **host** `roast.test.js`; STOP disabled in the markup, sent only from a confirm, samples only while recording, Heater off has a confirm, tab order and landing view, Start and event buttons send nothing, every `send()` call site listed and the worker sends only the three polls. **host** `web_check.test.js`
- Top bar (T51) against a mock board (Chrome headless) at 320, 375, 412 and 1280 px, text size 100 % and 140 %, comfortable and compact: logo, name, device line, link dot, clock label and value, Appearance, Start, STOP and End roast do not overlap or leave the screen, the bar does not scroll sideways, and the sticky tabs sit below the bar; both boards (T52). **build 2026-10-29**
- Connection (T53) against a mock board (Chrome headless, 390 px): Monitoring > Connection shows the URL, open, a round trip in ms and 1 attempt; Control state and Device fill in; a board restart (uptime back to 0) is counted and logged; 14 s of silence → dropped at 10 s and reconnected, the board holds one socket; hidden while heating or recording keeps the socket, hidden while idle closes it and showing the page reopens it; Clear roast is disabled while recording, asks first, deletes the samples and sends nothing; no script errors. **build 2026-10-29**
- URL and worker (T53): `socketUrl` (http, https → `wss://<host>/ws`, override), `mayPause`, poll and reply matching with round trip, 10 s silence drop with an immediate new socket, 8 s handshake limit and backoff, a constructor error, pause and resume. **host** `link.test.js`
- Profile load (T54): the page's exact payload (2 steps, 300 F) loads; 22 malformed variants (empty, not an array, non-object steps, missing or string duration and setpoint, 0, negative, over 3600 s, 1e308, setpoint out of range, fan out of range or a string, one bad step among good ones) and 33 steps are rejected; 32 steps load; a rejected load keeps the previous profile. **host** `pio test -e native`
- Profile load on hardware (T54): serial at boot shows `# Loop stack: 16384 B, ... free`; Controls > Load to device with 2 and with 32 steps replies `Loaded N steps.` with no reset (uptime keeps counting); `getControl` `loopStackFree` stays above about 8000 after loads, getControl and getDeviceInfo; heater stays 0 in Manual throughout. **untested**
- WiFi sleep off (T53): serial log shows `# WiFi sleep: off` at boot, no reset loop, BLE still advertises; ping from a PC on the LAN under about 20 ms on average (was 148 ms); Connection round trip under about 50 ms. **untested**
- Two browsers (laptop and phone) open for 10 min, USB unplugged or the serial program closed → dot stays green on both, Connection shows no restarts and one connection each. Lock the phone for a minute while idle → the laptop stays green; unlock → the phone reconnects. **untested**
- Same with the USB plugged into a PC and a serial monitor open, then closed without unplugging → no stalls (green, round trip under 300 ms), no restart unless the program toggles DTR/RTS (boot log `rst:0x15`). **untested**
- Page over https through a proxy that forwards `/ws` to port 81 → Connection shows `wss://<host>/ws`, open. **untested**
- Open the page on the board, do nothing for a minute → Monitoring shown, clock stays Idle, Session says not recording, the OLED shows no change in heater, fan or mode; open Live and Controls → still Idle, nothing changes. **untested**
- Start → recording, Start greyed out, STOP enabled, only Charge enabled, heater and fan unchanged; Charge → clock from 0:00; Dry end, FC start, Drop in order; STOP → Idle, Start enabled. Preheat from Controls (heater, fan, PID) works before Start. **untested**
- Each control on Croaster PID (heater/fan steppers, setpoint, mode, PID input, gains, cooldown fan, watchdog, unit, interval, corrections, autotune, profile load and start, screen buttons, `getDeviceInfo`) → the board's reply or error shows inline and the OLED or `getControl` agrees. **untested**
- STOP from every section while a roast records → confirm, then heater 0, Manual, fan at the cooldown speed (65 % default), OLED header agrees, the reply log shows the board's confirmation, clock Idle, Start enabled. Profile and PID runs end the same way. **untested**
- No roast recording → STOP greyed out; Controls > Safety > Heater off → confirm, then heater 0, Manual, cooldown fan. **untested**
- `restartesp`, `erase`, `dummyToggle`, `wifiConnect` → confirm dialog first; Cancel sends nothing. **untested**
- Charge, events, reload the page → events, clock and chart are kept; CSV has one row per second with the events. **untested**
- Heater in Manual at a low % (AC connected, nothing on the drum or an empty drum), page open, switch to another tab for 5+ minutes → no `comms` trip; back on the tab, the chart has no gap. **untested**
- Same, then close the tab (Artisan closed) → `comms` trip after the watchdog timeout. **untested**
- Artisan and the page connected together → both keep 1 s data, no reconnects. **untested**
- Release build: control loop timing unchanged with the page open (`docs/design.md` D27 "Loop timing"). **untested**

## WebSocket and ICRM app (T60)
- Connections closed before the upgrade, then the app connects → client count 1, broadcast runs (upstream count reached -13). **host** `test_ws_upstream_count_goes_negative`, `test_ws_count_after_unfinished_connections`
- `CroasterWebSocketManager` reads the count from `server.connectedClients()` and still broadcasts `getJsonData()` every `interval` s. **host** `test_ws_manager_source_counts_from_library`
- Broadcast keeps IiemB's fields and order (`id`, `name`, `data.interval`, `timer`, `BT`, `DBT`, `ET`, `DET`, `cBT`, `cET`, `temp`), BT/ET left out while NaN, control fields after `temp`. **host** `test_broadcast_upstream_fields_in_order`
- Control fields never replace the app's fields; payload ASCII, no NaN or Infinity, parses, in every mode, trip and autotune state. **host** `test_broadcast_extension_keeps_app_fields`
- Page open, hide the tab or drop WiFi a few times, then open ICRM → serial count never below 0, `# [SOCKET-JSON]` every 3 s, app stays connected over 10 min. **untested** (bench todo)
- ICRM and the page connected together, close the page → count 1, ICRM keeps receiving. **untested** (bench todo)

## Profile library and recipes (D28, T55)
- Format and limits: 50 profiles, 8 KB, name 40, notes 1 KB, ids, unknown keys kept, newer version refused, °F/°C conversion, interrupted save (a `.tmp` beside the `.json`) cleaned up at boot, busy rule. **host** `pio test -e native`
- Recipe parser and runner: limits and reasons, `gte`/`lte`/`near`, false-to-true firing, ordered and any-order arming, unknown readings, time before CHARGE, setpoint ramps, power and setpoint steps mixed (Manual then PID), guided prompts and missed prompts, trip abort, cool and end. **host** `pio test -e native` (`test_recipe_*`, `test_library_recipe_profiles`)
- Page: format checks with the board's reasons, editor round trips, behind-schedule check. **host** `library.test.js`; send call sites **host** `web_check.test.js`
- Page against a mock board: list, Save refused while heating with the reason shown, overwrite asks, Edit, Delete asks, Import keeps unknown keys, unavailable after 4 s; recipe editor default heater + fan, a blank value names the step, Load to device payload in °F, Run, "Step 1 fired at 0:42: power 60 %, fan 50 %", next step, Abort, guided Set now, Heater off aborts. **build 2026-10-29** (`recipe.js` against `mock.js`)
- First boot: serial shows `# Library: LittleFS on "spiffs" in N ms, 0 of 1408 KB used, 0 profiles` (note N: the first mount formats the partition); the next boot mounts without formatting and N is small. **untested**
- Save, load, delete from the page: save a profile with a schedule and a recipe, reboot, it is still listed; Load to board replies with both parts; Delete removes it. **untested**
- Power cut during save: save a large profile and pull USB power during the save; after boot the profile is either the old or the new version, never empty or unlisted, and no `.tmp` remains. **untested**
- Save refused while heating: heater above 0 (Manual 10 %) → Save and Delete greyed with "Not while the heater is on"; `librarySave` sent by hand replies `busy` / `heater on`. **untested**
- OTA keeps the library: save two profiles, update the firmware over WiFi, both are still listed and load. **untested**
- `getControl` `loopStackFree` stays above about 8000 after `libraryList` with 50 profiles, `libraryGet` of an 8 KB profile, `libraryLoad` and `getRecipe` with 32 steps. **untested**
- Recipe status strings: the `getRecipe` fields are copied or static (a `const char[N]` stored by pointer reproduces the stale bytes), the unit is C, F or K or the recipe is refused, the name is cut at a UTF-8 boundary, a loaded recipe applies nothing before Start. **host** `pio test -e native` (`test_recipe_status_strings_owned`)
- Load "Test Recipe 2" (guided, °F) with Load to device, then stay on the page for a minute and reload it twice: the page stays connected, `getRecipe` shows `"unit":"F"` and the name, Library lists each profile's unit as C or F, serial shows no `WebSocket Client Disconnected`. **untested**
- Recipe triggers on the roaster: steps at BT 300 °F (heater 60 %, fan 50 %) and at FC_START (setpoint 410 °F over 1:00) → each fires once at its point, the fired list shows the roast time, the mode switches Manual then PID, SV ramps. **untested** (needs AC)
- Recipe aborts: STOP, Abort recipe, a comms watchdog trip (close every page) and an OTA start each end the recipe with that reason and nothing more is applied. **untested**
- Guided mode: a fired step shows Set now and Skip; Set now applies it; an unanswered prompt becomes "missed" when the next fires. **untested**
- Behind-schedule warning: setpoint step well above what the roaster can reach, heater at 100 % for 20 s with BT more than 15 °F below SV → the warning shows; it clears when BT catches up. **untested** (needs AC)

## Config checks (pins)
- Accepted GPIOs on both boards: 1, 2, 4-18, 21, 38-42, 47 (shared `hw/esp32s3_opi_pins.h` plus board wiring, T46). **build 2026-10-28** (host check of 0..48 against the old rules; same sizes)
- A probe pin on GPIO 35 (PSRAM) or 43 (UART0) fails the pin `static_assert` on Croaster PID and Croaster monitor. **build 2026-10-28**

## OLED
- Temperatures shown in °F. **bench 2026-10-26**
- Mode changes (Manual, PID, Tuning) show in the header. **bench 2026-10-26**
- Header reads "H<n> Fan<n> <mode>", or "TRIP <cause> Fan<n>" when tripped. **untested**

## Artisan mapping
- Input 1 = BT, Input 2 = ET on the curves and LCDs. **untested** (no BT probe yet)
- Heater and Fan LCDs match the OLED's H and Fan values (fix in 067e316). **bench todo**
- SV LCD shows the firmware SV in °F (fix in 067e316). **bench todo**
- Fan, Setpoint and Heater sliders send `fan`, `setpoint` and `burner` commands. **bench 2026-10-26**
- Event buttons (heater %, PID, Manual, Autotune, Stop, Clear trip) send their commands. **bench 2026-10-26**
- yaeger's `setBurner`/`setFan` → heater/fan change; flat `{"BurnerVal":60}` → ignored (D8). **host** `test_protocol_yaeger_compat`; bench untested

## Manual
- Heater slider → BurnerVal follows; SSR duty is BurnerVal × 90 %. **bench 2026-10-26** (BurnerVal only; duty unmeasured)
- SV change in Manual → no effect on heater or fan. **bench 2026-10-26**
- Heater > 0 with fan below 30 % → fan raised to 30 %. **untested**

## PID
- SV below ET in PID → heater output decays toward 0 (integral unwinding). **bench 2026-10-26**
- Integral doesn't wind up while saturated; output leaves 100 % soon after PV passes SV. **host** `test_pid_integrator_does_not_wind_up`
- Manual → PID keeps the current heater output (bumpless). **host** `test_pid_bumpless_reset`
- PID holds SV on the real heater without sustained oscillation. **untested** (needs AC)
- Fresh NVS, no `pidInput` sent → `Target` reads BT (D18). **host** (default only, `test_protocol_basic_and_data`); bench untested
- `{"command":{"pidInput":"ET"}}` → `Target` reads ET and survives a reboot; the BT default doesn't override it. **untested**
- PID input BT with no BT probe → PID mode trips `sensor` after 2 s; autotune refused. **untested**
- Autotune on BT on the real heater → gains that hold a BT setpoint. **untested** (needs BT probe and AC)

## Autotune entry, exit and gates
- Autotune with SV below 212 °F → refused, mode stays Manual. **bench 2026-10-26**; superseded by D29: autotune needs no setpoint
- Autotune with SV at or above 212 °F → mode Tuning, BurnerVal 100. **bench 2026-10-26**; D29: any SV, mode Tuning, BurnerVal 100 while heating to the first point
- Autotune with fan below 30 % → fan set to 60 %, autotune keeps running (the start rule is not a fan change). **host** (`test_output_command_stops_autotune`: only commands count); bench untested
- Relay identifies a simulated roaster and the gains hold its setpoint. **host** `test_relay_autotune_identifies_and_controls`
- Autotune → PID or Manual → starts from 0 %, not the relay's 100 % (fix in 067e316). **host** `test_leaving_autotune_starts_from_zero`; bench todo
- Autotune → PID or Manual → `autotune` reads "aborted"; Stop → "aborted: stop". **untested**
- Autotune with SV below 212 °F → `autotune` reads "need setpoint >= 212 F" in every sample; OLED shows "AT: SV<212F" for 10 s, again on each retry (D17). **host** (text, `test_autotune_status_text`); bench untested
- Autotune with °C selected → "need setpoint >= 100 C". **host** `test_autotune_status_text`
- Autotune with no probe on the PID input → "need a valid probe", OLED "AT: no probe". **untested**
- Trip during autotune → `autotune` "aborted: <trip>"; OLED keeps the TRIP line. **untested**
- Autotune fails → "failed: <reason>"; OLED "AT fail: ..." within 20 characters. **host** (text); bench untested
- Autotune with no heat (no AC) → fails "no heat-up" after 180 s (D29), heater 0. **host** `test_at_abort_no_heat_up`; bench untested
- Autotune completes on the real heater → Manual, heater 0, cooldown fan, results pending; gains saved to NVS only after Save (D29). **untested** (needs AC)
- `{"command":{"pidInput":"ET"}}` during autotune on BT → `autotune` "aborted: input changed", OLED "AT abort: input", heater 0 (D23). **host** (`test_pv_step_stops_autotune_and_resets_pid`, text in `test_autotune_status_text`); bench untested
- `{"command":{"fan":80}}` during autotune at fan 60 % → `autotune` "aborted: fan changed", OLED "AT abort: fan", heater 0, fan 80 %; `{"command":{"fan":60}}` at 60 % → keeps running (D23). **host** `test_output_command_stops_autotune`, text in `test_autotune_status_text`; bench todo (no AC needed: heater 100 on the relay is enough to see it drop to 0)
- `{"command":{"burner":50}}` or an Artisan heater button during autotune → "aborted: heater changed", OLED "AT abort: heater", heater stays 0, Manual (D23). **host** `test_output_command_stops_autotune`; bench todo
- `pidInput` change in PID → heater output continues without a jump; the same input again → nothing happens (D23). **host** `test_pv_step_stops_autotune_and_resets_pid`; bench untested
- Autotune on the real heater → note the swing around SV and the time taken; compare with the README (simulated ±10 to ±45 °C, 4 to 14 min). **untested** (needs AC); superseded by "Multi-point autotune" below

## Multi-point autotune (D29, T57)
Process and limits: `docs/autotune/process.md`.

- Default points 250, 300 and 350 °F (121.1, 148.9, 176.7 °C); top point plus the overshoot stop under 207 °C (405 °F). **host** `test_at_default_points`
- Three-point run on a simulated nonlinear drum → all points `ok`, peak BT at least 4 °C under the overshoot stop, recommendation holds a step, a 6 °C/min ramp and a hold (overshoot < 4 °C, ramp error < 5 °C, hold < 1 °C). **host** `test_at_multi_point_run`
- Relay reads Ku within 30 % (and below) and Tu within 15 % of a FOPDT plant's analytic values. **host** `test_at_fopdt_ultimate_point`
- Sensor fault, over temperature, overshoot, no heat-up, no oscillation, time limit, abort command → heater 0 from that step. **host** (`test_at_abort_*`)
- Start above the first point + 30 °C → `failed: too hot to start`; empty plan → `failed: no test points`. **host**
- `recipeStart` during autotune → `autotuneRunning`; `autotune` during a running recipe → `recipeRunning`; the page shows the message under Recipe and under Autotune. **untested**
- Page: Start autotune → confirm with the points and minutes, nothing sent on Cancel; Stop autotune asks first and sends `stop`. **host** (`web_check.test.js`); bench untested
- Page: step list, progress bar and log advance with the board; OLED shows `AT2/3 cyc3/6 350F` style lines. **untested**
- Page: results dialog shows per-point results and current vs new gains; Save → `getControl` gains change and survive a reboot, `tuned` true; Cancel → gains unchanged, `autotune` "discarded"; Escape → results stay, Review and save… reopens; reload with results pending → dialog again. **untested**
- Reboot with results pending → results gone, gains unchanged. **untested**

Bench run on the Behmor 2000AB (needs AC; about 45 to 65 min):

1. Empty drum, chaff tray in, drum turning. Roaster at room temperature.
2. Board on its supply, BT probe reading, `getControl` `pidInput` BT. Fan
   to the usual roast speed (a fan change during the run aborts it).
3. Page > Controls > Autotune > Start autotune > OK. Start a recording in
   Live to chart BT against the dashed test-point line.
4. Check: heater 100 % until about 15 °C under 250 °F; then the heater
   switches between two levels and BT cycles a few degrees around 250 °F
   (expect a first peak up to about 295 °F, under the 304 °F overshoot stop);
   the log adds one line per cycle (swing, period). Note each point's swing,
   period and time.
5. Check the stops once on a separate run: STOP during heating → heater 0
   within 1 s, Manual, cooldown fan, `aborted: stop`.
6. At the end: Save. Then PID mode at 350 °F and a short ramp profile; note
   overshoot and lag. Compare with `docs/autotune/tuning-rules.md`.
7. Record: total time, per-point Ku/Tu/bias, the recommended gains, and the
   highest BT at each point (the run stops above 304, 354 and 404 °F). The
   Behmor's own controls are bypassed, so stay with the roaster for the run.

## Autotune required (D24)
- `CPID_REQUIRE_AUTOTUNE 1`, erased flash (`pio run -t erase`, then upload) → `getControl` `tuned` false; `{"command":{"mode":"pid"}}` → mode stays Manual, `autotune` "autotune required", OLED "Autotune first" for 10 s. **host** (gate and text, `test_autotune_required_gate`); bench todo
- Same board, profile loaded, `startProfile` → refused the same way; profile still loaded. **host** (gate); bench todo
- Same board → Manual heater and `autotune` still work. **host** (gate); bench todo
- `{"command":{"pid":{"kp":2,"ki":0.02,"kd":5}}}` → `tuned` true, PID allowed, and still after a reboot (NVS `tuned`). **host** (parse); bench todo
- Autotune completes → `tuned` true, PID allowed, survives reboot. **untested** (needs AC)
- Autotune fails or is aborted → `tuned` stays false. **untested**
- Board with gains saved before this change (no `tuned` key) → `tuned` true. **untested**
- `CPID_REQUIRE_AUTOTUNE 0` → PID on default gains at once. **host** (gate); bench todo

## Watchdog and trips
- Artisan Off, then On → `trip:"comms"`, heater 0, fan at the trip speed; stays tripped after reconnect. **bench 2026-10-26** (when it tripped is unconfirmed; it should only trip while heating)
- Artisan Off while heating → trip after the 10 s timeout, not before. **untested**
- Clear trip, or any heater, mode, autotune or profile command → trip clears. **untested**
- Fan or setpoint command while tripped → applied, trip stays. **untested**
- Watchdog timeout and enabled flag parse from JSON. **host** `test_protocol_watchdog`
- `{"command":{"watchdog":{"enabled":false}}}` → no comms trip, survives reboot. **untested**
- Probe above `CPID_OVER_TEMP_C` (set low to test) → `trip:"overtemp"`, heater 0, fan 100 %. **untested**
- OTA starts → `trip:"ota"`, heater 0. **untested**

## Stop and cooldown
- Stop → heater 0, fan 100 % at once, SV kept, mode Manual. **bench 2026-10-26** (before D19; the stop fan is now 65 %)
- Stop with defaults → fan 65 % (D19). **untested**
- Any trip (comms, sensor, nomains) → fan to 100 % once when it starts; a fan command afterwards still applies (D19). **untested**
- `cooldownFan` as a number or `{"stop","trip"}` object, range 0..100, both in `getControl`. **host** `test_protocol_cooldown_fan`
- `{"command":{"cooldownFan":{"trip":80}}}` then a comms trip → fan 80 %, survives reboot. **untested**
- `{"command":{"cooldownFan":65}}` then Stop → fan 65 %, survives reboot. **untested**

## Mains detection
- `CPID_REQUIRE_MAINS 1`, no AC → heat refused, `trip:"nomains"`. **bench 2026-10-26** (before the grace period; bench todo: trip now comes about 2 s after heat is set)
- Grace period (`CPID_MAINS_GRACE_MS` 2000): a dropout under 2 s while heating does not trip; over 2 s trips; heater 0 resets the count. **host** `test_mains_dropout_under_grace_does_not_trip`, `test_mains_absent_past_grace_trips`, `test_mains_heater_off_resets_grace`; bench todo: pull the dimmer's Z-C wire for 1 s while heating (no trip), then for 3 s (trip)
- AC connected → `getControl` `mainsOk` true, heat allowed. **untested**
- `CPID_REQUIRE_MAINS 1`, AC connected, heater on for an hour → no `nomains` trip; a crossing stored after the control task read the timer counts as recent (D23). **host** `test_zero_cross_age`; bench untested

## °F conversion
- °F/°C/K conversions both ways. **host** `test_unit_conversion`
- Nested setpoints in °F convert to °C; SV written back in °F. **host** `test_protocol_values`, `test_protocol_yaeger_compat`, `test_protocol_basic_and_data`
- Profile steps in °F convert to °C. **host** `test_profile_in_fahrenheit`
- Artisan setpoint 383 °F → SV 383 in telemetry and Artisan's SV LCD. **host** (conversion only, `test_unit_conversion`); bench todo
- `{"command":{"tempUnit":"C"}}` → OLED and telemetry in °C, and still °C after a reboot (saved in NVS, D27). **untested**
- Nothing saved yet (fresh or erased NVS) → boots in `CMON_TEMP_UNIT`. A saved unit wins over `CMON_TEMP_UNIT`; an invalid saved value falls back to it. **host** `test_temp_unit_boot_uses_saved_or_default`
- `tempUnit` saves only a unit the board applied and only when it changed (`"X"` or the same unit again → no NVS write). **host** (rule) `test_temp_unit_saves_only_changes`; on the board **untested**

## Setpoint caps
- Setpoint above 482 °F (250 °C) → SV capped at 482 °F. **untested** (cap is in the controller, not host-tested)
- Negative setpoint → SV 0 °C (32 °F). **untested**

## Profiles
- README example profile → setpoints follow the ramps. **host** `test_profile_matches_readme_example`
- Profile load and `startProfile` over WebSocket → mode Profile, SV follows, watchdog exempt. **untested**

## Correction offsets (D22)
- Correction offset → PID input shifts by it; ET, BT and MAX inputs; `Amb` unchanged. **host** `test_correction_shifts_pid_input`
- `correctionBt` +18 in °F → PID input +10 °C and matches the displayed BT. **host** `test_correction_in_fahrenheit` (the core getter's ÷ 1.8 is checked on the bench only)
- Faulted probe plus an offset → still NaN; no over-temperature trip from it. **host** `test_correction_keeps_nan`
- Negative offset hiding a hot probe → over-temperature trips on the raw reading. **host** `test_overtemp_uses_raw_and_corrected`
- Snapshot BT/ET → uncorrected, so the offset is added once. **host** `test_snapshot_stays_uncorrected`
- °F mode, `{"command":{"correctionBt":10}}`, PID on BT, SV 300 °F → displayed BT settles at 300 °F, not 310 °F. **untested** (needs AC)
- `{"command":{"correctionBt":-50}}` with `CPID_OVER_TEMP_C` set low → `trip:"overtemp"` when the raw BT passes the limit, while the display still reads 50 °F lower. **untested**
- `{"command":{"correctionBt":9}}` in °F, then `{"command":{"tempUnit":"C"}}` → `cBT` 5 in the data, the displayed BT moves by the unit change only (no extra 4 °C jump), PID holds the displayed SV. Back to °F → `cBT` 9. `tempUnit` "K" → `cBT` stays 5. **bench todo** (no AC needed: watch `cBT` and BT in `getArtisanData`)
- Same `tempUnit` sent again → `cBT`/`cET` unchanged. **bench todo**
- Correction change during autotune → `autotune` "aborted: correction changed", OLED "AT abort: offset", heater 0. **host** (text, `test_autotune_status_text`); bench untested
- Correction change in PID → heater output continues without a jump. **untested**
- Stock boards (`esp32c3`, `esp8266`) → same RAM and flash use and the same warnings with the getters added. **build 2026-10-26**
