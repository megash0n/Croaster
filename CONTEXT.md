# CONTEXT.md — Croaster firmware (current state)

## What this repo is
The **Croaster** coffee-roaster firmware, structured as a reusable
display/pin-agnostic library (`croaster/`) plus standalone per-board PlatformIO
projects (`boards/<board>/`). It is a git submodule of the ICRM
Flutter app (remote `git@github.com:IiemB/Croaster.git`) and is consumed via
`lib_deps = https://github.com/IiemB/Croaster.git`.

## Repo layout
| Path | Purpose |
|:---|:---|
| `croaster/src/` | Library core: `CroasterCore`, `CroasterDisplay` (interface), `CroasterApp` (begin()/loop()), `CroasterCommandHandler`, `CroasterBleManager`, `CroasterWebSocketManager`, `CroasterOtaHandler`, `CroasterWiFiManager`, `CroasterDeviceIdentity`, `CroasterPinConfig`, `CroasterConstants` |
| `boards/common/` | Shared SSD1306 display + animation (small lib, added via `../common`) |
| `boards/esp32c3/` | ESP32-C3 Super Mini — SSD1306 OLED; custom OTA partition (`custom32c3sm.csv` lives in this folder) |
| `boards/esp8266/` | NodeMCU / ESP-12E — SSD1306 OLED |
| `boards/esp32s3/` | ES3C28P 2.8" ILI9341 LCD — LVGL 9 UI + FT6336G touch (`src/ui/`) |
| `boards/esp32s3-pid/` | Croaster PID **control** board (Lonely Binary S3 N16R8): SSR heater, AC-dimmer/PWM fan (always both), 2× MAX31855, OLED; master `src/config.h` + `ConfigCheck.h`; Artisan `.aset` in `artisan/` |
| `boards/esp32s3-monitor/` | Croaster monitor (same S3 N16R8 hardware, D26): 2× MAX31855 or MAX6675 + OLED, no heater, fan or control task; `src/config.h` (`CMON_` only) + `ConfigCheck.h` |
| `croaster-control/` | Control add-on lib `CroasterControl` (D1). `src/core/`: pure logic (PID, multi-point autotune `autotune_sequence` + `autotune_protocol` and the older single-point relay, profiles, profile library format, recipe runner, MAX31855/MAX6675 decode, command protocol, safety rules), host tests in `boards/esp32s3-pid/test` (`pio test -e native`). `src/hw/`: ESP32 drivers (thermocouple SPI, SSR, dimmer/PWM fan) and `esp32s3_opi_pins.h` (ESP32-S3 octal-PSRAM pin rules both boards' `ConfigCheck.h` use). `src/app/`: control task, NVS settings, `ControlPlugin` (hooks into `CroasterApp`), `LibraryPlugin` + `library_storage` (profile library on LittleFS), `RecipePlugin` (recipe commands and tick), `AutotunePlugin` (`getAutotune`, apply/discard), `RoasterProbe` (Croaster PID) and `MonitorProbes` (Croaster monitor). `build_filter.py` limits native builds to `src/core` |
| `croaster-webui/` | Browser UI lib `CroasterWebUi` (D27): `web/` page sources (`theme.js` colour schemes, T50), `embed_assets.py` (library extra script, gzips `web/` into the committed `src/webui_assets.h`), `src/WebUiServer` (ESP-IDF `esp_http_server` on `CMON_WEBUI_PORT`), host tests in `test/` (node + python) |
| `docs/` | `user-guide.md` (operator guide for Croaster PID on the Behmor: modes, page, autotune, recipes, ramp shapes, trips), `design.md` (architecture + decisions D1–D31), `autotune/` (multi-point autotune: goal, process, implementation, tuning rules, fan test), `research/autotune.md` (single-point relay sources, superseded), `tickets.md` (remaining work), `test-plan.md` (functional tests: host, bench or untested), `analysis.md` (yaeger + Croaster review), `how.md` (walkthrough), `architecture.md` (shape + open decisions), `control-parity.md` (yaeger vs ours), `artisan-settings.md` (every `.aset` setting with source) |
| `tools/` | Build helpers: `force_include_deps.py` (pre: extra script; force-included `config.h` as a rebuild dependency, T47) |
| `builds/` | gitignored firmware output of `./build_all.sh` |

## Boards
| Board | MCU | Display | Env | Flash (build_all) |
|:---|:---|:---|:---|:---|
| `esp32c3` | ESP32-C3 | 128×64 SSD1306 | `esp32c3` | ~85.7% |
| `esp8266` | ESP8266 | 128×64 SSD1306 | `esp8266` | ~39.8% |
| `esp32s3` (ES3C28P) | ESP32-S3 | 320×240 ILI9341 + LVGL 9 | `esp32s3` | ~29.0% |
| `esp32s3-pid` | ESP32-S3 N16R8 | 0.96" 128×64 SSD1306 I2C (`boards/common` display; confirmed by Shawn) | `esp32s3-pid` | 30.3% (1 988 047 B), RAM 63 168 B (2026-10-31, T60) |
| `esp32s3-monitor` | ESP32-S3 N16R8 | same as `esp32s3-pid` | `esp32s3-monitor` | 26.7% (1 751 995 B), RAM 54 712 B (2026-10-31, T60) |

## How a board is wired
- `main.cpp` creates `CroasterCore core(dummyMode, pins)`, a display subclass
  (e.g. `CroasterDisplaySSD1306` / `CroasterDisplayS3`), and
  `CroasterApp app(core, &display, ledPin, ledOnLevel)`, then calls
  `app.begin()` / `app.loop()`.
- Board id is hardcoded per board via
  `CroasterDeviceIdentity::setBoardName("esp32s3")` in its `main.cpp`, read
  by `CroasterDeviceIdentity::boardName()` (no compile-time board guards;
  defaults to `unknown` until set).
- Builds: `build_all.sh` builds only the **release** env of each board
  (`build_type = release`; `pio run -e <board>`); `./upload_monitor.sh <board>`
  builds + uploads the **debug** variant (`<board>-debug`, `extends` the
  release env) to the connected board, then opens the serial monitor.
- Croaster PID board: control task (core 1, prio max-3) owns the SPI bus and
  publishes a snapshot; `RoasterProbe` feeds it to `CroasterCore` via the
  sensor-injection constructor; `ControlPlugin` registers control commands
  (`onJsonCommand`/`onCommand`), adds data fields via `CroasterCore::onJsonData`,
  runs the comms watchdog. Details: `docs/design.md`.
- Heater/fan control is 100 % firmware-side (PID on the ESP32). Clients send
  setpoints/modes/percentages only. Two wire formats reach the same parser
  (`controlproto`): nested `{"command":{...}}` and yaeger's named commands
  `setBurner`/`setFan`/`setPreferences`/`getPreferences` (`parseLegacy`, via
  `onCommand`). yaeger's flat keys (`{"BurnerVal":60}`) are not accepted (D8).
  Data adds `BurnerVal`, `FanVal`, `FanAssist`, `SV`, `Mode`, `trip`, `autotune`, `Amb`,
  `Target`, `pidKp/Ki/Kd`, `wifiStrength`.
- Autotune status (D17): the controller records an `AutotuneStatus` and reason
  in the snapshot; `core/autotune_status.h` turns it into text in the
  display unit. Data and `getControl` carry `autotune` ("running: cycle 3/6
  at 350 F (2/3)", "results ready", "failed: no heat-up", "aborted: comms");
  it holds until the next change. The OLED header shows a 20-character form
  ("AT2/3 cyc3/6 350F") while running and for 10 s after each change; a trip
  line wins.
- Autotune (D29, `docs/autotune/`): no setpoint; runs the
  `CPID_AUTOTUNE_POINT*_C` test points (250/300/350 °F; top point kept at
  350 °F because the roaster's own cutoff is bypassed), relay with bias
  correction, cycles until they agree, ZN PI gains (most cautious over the
  points). Results wait until `{"command":{"autotune":"apply"}}` (NVS) or
  `"discard"`; `getAutotune` returns plan, progress, results and log; data
  adds `atTarget`. Refused while a recipe runs (`recipeRunning`), and
  `recipeStart` is refused during autotune (`autotuneRunning`).
- Autotune fan test (D30, `docs/autotune/fan-test.md`): optional, on by
  default, after the points; holds the middle point at fan 30/60/90 % and
  times a bounded heater-off fall per level. Saved with the gains as a
  versioned roaster model (NVS `model`). The controller uses it for fan and
  ramp feedforward and for fan assist (extra fan when BT is above the
  setpoint with the heater at 0 %, never below the user's fan; NVS `fanAs`;
  data `FanAssist`). The controller keeps the user's or recipe's fan as the
  base fan (`fanBase_`); only outside fan changes abort autotune (D23).
- Recipe ramps (D31): `setpoint` and `power` ramps take a `shape`; the
  21-point curve is the rate of climb and its normalised area F(u) the
  progress (`recipe::rampFraction`), shared with the page in `web/ramp.js`.
  One ramp runs at a time (`Runner::Ramp`); `fan` does not ramp.
  `chargeTemp` sets a PID preheat on Start that is not a followed step. The
  page checks ramps against the saved model's `maxRorCpm` (port in
  `ramp.js`); keep the two in step.
- PID input defaults to BT (D18, `CPID_PID_INPUT`); `pidInput` command and
  NVS key `in` still switch it, and a saved value overrides the default.
  Without a BT probe, PID mode trips `sensor`: send `pidInput` ET. Default
  gains are yaeger's (tuned for ET); autotune on BT.
- Comms watchdog: no message through Croaster's command handler
  (`lastCommandMillis()`, WebSocket or BLE) for the timeout while heating
  (not in Profile) → heater off, `trip:"comms"`.
  Cooldown fan (D19): 65 % after `stop`, 100 % when any trip starts
  (`CPID_COOLDOWN_FAN_STOP`/`_TRIP`, `{"command":{"cooldownFan":{...}}}`,
  NVS keys `cool`/`coolTrip`, reported by `getControl`). The controller sets
  the trip fan once per trip; over-temperature always uses 100 %. Defaults
  `CPID_WATCHDOG_ENABLED`/`CPID_WATCHDOG_TIMEOUT_S`; runtime command
  `{"command":{"watchdog":...}}` saves to NVS.
- Artisan: `.aset` verified against Artisan source (`docs/artisan-settings.md`).
  Input 1 = BT, Input 2 = ET, one `getArtisanData` per second, extra devices
  112/113 for Heater/Fan/SV, Artisan's own PID buttons off. Extra curve 1 is
  the odd input (3, 5), curve 2 the even one (4, 6).
- Leaving Autotune by any command starts the next mode with the heater at 0;
  a setpoint change during Autotune aborts it (the test points do not use the
  setpoint). `Amb` is the cold-junction
  temperature of whichever MAX31855 answers (BT first).
- Browser UI (D27, 2026-10-29): `WebUiServer` (both boards,
  `CMON_ENABLE_WEBUI`) serves `http://<ip>/` once STA is connected and the
  WiFiManager portal is closed (AP mode = portal open; both use port 80). The
  page is a WebSocket client on :81 like Artisan (1 s `getArtisanData`, 5 s
  `getControl` on Croaster PID, 30 s `getDeviceInfo`), from a Web Worker so
  hidden tabs keep polling. `/info.json` gates Controls. Roast clock and
  events live in the browser. Tabs: Monitoring (the landing view), Live,
  Controls; a `#live` or `#controls` hash opens that view. Loading the page or
  switching views sends only the polls. A roast records only after Start (top
  bar, next to STOP; session field `active`), which sends nothing to the
  board; connecting, reconnecting, reloading or switching views never starts
  one, and a saved roast with no sample for 10 min is ended on load. Event
  buttons (`Roast.eventAllowed`): all disabled when idle, Charge only after
  Start, then the later events in order until Drop; the roast clock counts
  from Charge, earlier samples have negative times. Controls stay usable
  when idle. STOP (top bar, Croaster PID) is greyed out unless a
  roast is recording, asks first, sends `{"command":"stop"}` (heater 0,
  Manual, fan to `cooldownFan.stop`), ends the recording and logs whether the
  next readings show Manual with heater 0. Controls > Safety keeps a separate
  Heater off button (same command, with a confirm) for when no roast is
  recording. End roast (top bar on the monitor, after Drop on both boards)
  ends the recording without a command. Over https the page connects to
  `wss://<host>/ws`, which the proxy must forward to port 81. The socket is
  dropped after 10 s without a message, and closed while the page is hidden
  and nothing needs the polls. Monitoring > Connection shows the link and
  the board's uptime, restarts and WiFi signal. WiFi modem sleep is off
  (`CMON_WIFI_SLEEP 0`). Clear roast deletes an ended roast from
  the browser. Roast sessions are per browser. No password. `tempUnit` is saved to NVS
  (`app/unit_store`, namespace `croaster`, key `unit`) and applied at boot;
  `CMON_TEMP_UNIT` is the default when nothing is saved.
  Appearance (T50): `web/theme.js` holds the colour tokens, presets (default
  `neon`), validation, migration and contrast; it loads in `<head>` and
  applies the saved scheme before first paint. `croaster.theme` in
  `localStorage` is JSON (`{format, v, preset, colors, style}`); the old
  `"dark"` maps to `neon`, `"light"` to `light`. The chart's colours and line
  width are CSS custom properties, so a change restyles it without a redraw.
  `app.css` `:root` repeats the neon values as fallbacks; `web_check.test.js`
  fails if they drift from `theme.js`.
- Library features added in 0.52: roast timer (`CroasterCore::roastTimerStart/
  Pause/Reset`), `getDeviceInfo` now reports `board`/`darkMode`/`brightness`
  (display state via `CroasterDisplay::isDarkMode()/getBrightness()`), and a
  BLE-guarded `CroasterApp::ble()` accessor.

## Git state (2026-10-26)
- Upstream: remote `upstream` = `IiemB/Croaster`; fork base is `master`
  `5445a4d`. Shawn's pre-project test commits `da20956`, `77ad627`, `0845ba8`
  sit between it and this work. Stock board files were restored to `5445a4d`
  (design.md "Stock boards and upstream"); diff against `5445a4d`, not the
  fork's `master`, to see what upstream has.
- Shawn's fork: branch `rework/roaster` off `master` (`0845ba8`), pushed to
  `origin/rework/roaster` (shawns1234/Croaster). Adds the roaster board + libs + docs and these core changes:
  sensor-injection ctor, `onJsonData()`,
  `#ifndef CROASTER_HAS_BLE`,
  `CroasterCommandHandler::lastCommandMillis()`,
  `CroasterCore::correctionBtCelsius()`/`correctionEtCelsius()` (D22),
  correction offsets rescaled on a `tempUnit` change (D22), two
  warning fixes (`CroasterBleManager` initializer order,
  `CroasterDeviceIdentity` `PRIX32` format), the WebSocket client count read
  from `server.connectedClients()` (T60, could be offered upstream), dummy
  readings converted to the display unit in `readSensors()` (T61, could be
  offered upstream), and `MAX6675_Thermocouple` from
  Shawn's fork at `v2.0.3` with `SmoothThermocoupleFix.cpp` removed (D21).
- `MAX6675_Thermocouple` fork: <https://github.com/shawns1234/MAX6675_Thermocouple>,
  default branch `master`, no review branch (Shawn's call); release tags
  `vX.Y.Z`, pinned in `croaster/library.json`. A fix there needs a new tag
  and a new pin. Host tests: see its README.
- Board renamed `esp32s3-roaster` → `esp32s3-flex` (2026-10-26, D5 resolved
  by Shawn): folder, envs, board id, `.aset`.
  2026-10-28: renamed to Croaster PID (`esp32s3-pid`, `CPID_`). (design D25)
- Config prefixes (D26, Shawn 2026-10-28): `CMON_` = settings every Croaster
  board has (probes, screen, identity, BLE); `CPID_` = control add-on. Only
  names we created; the upstream author's (`CROASTER_HAS_BLE`, `croaster/`,
  stock boards) are never renamed.
- Croaster monitor (D26, 2026-10-28): `boards/esp32s3-monitor` replaces the
  old `CPID_HEATER_NONE` build; Croaster PID always has heater + fan. The
  monitor links only `hw/thermocouples` + `app/MonitorProbes` from
  `croaster-control` (no control symbols in its ELF). Host tests stay in
  `boards/esp32s3-pid/test`.
- D10 sdkconfig part resolved 2026-10-27: Croaster PID has no sdkconfig file of its
  own; `board_build.sdkconfig_path = ../esp32s3/esp32s3-sdkconfig.defaults`.
  The ini-sharing question stays open.
- Pin rules (T46, 2026-10-28): chip and octal-PSRAM module rules live once in
  `croaster-control/src/hw/esp32s3_opi_pins.h`; each board's `ConfigCheck.h`
  adds its dev-board wiring (UART0 43/44, RGB LED 48). Accepted pins unchanged.
- `nomains` trip: only after `CPID_MAINS_GRACE_MS` (2000) without a
  zero-cross while heating (`core/mains_watch.h`, Shawn 2026-10-27).
- D24 (2026-10-27): `CPID_REQUIRE_AUTOTUNE 1` refuses PID and Profile until
  NVS `tuned` is set (completed autotune or the `pid` command);
  `modeNeedsTuning()` in core.
- D11 resolved 2026-10-27: the only MAX31855 decoder is
  `croaster-control/src/core/max31855.h`; `croaster/` has no MAX31855 code
  and builds its thermocouples exactly as upstream.
- D1 resolved 2026-10-27: `control-core/` + `control-esp32/` became one
  library, `croaster-control/src/{core,hw,app}` (history kept via `git mv`).
- Decisions pending (Shawn), all in design.md: D3 (force-included config), D6 (watchdog via core `lastCommandMillis()`),
  D9 (traffic watchdog,
  configurable, vs instant cut on disconnect), D10 (Croaster PID ini vs Croaster's
  `boards/esp32s3` ini), D12 (Croaster display
  plus header hook), D13 (Croaster OTA only, no mDNS), D14 (config wizard,
  design only), D15 (MAX6675 library stays a core dependency). design.md
  lists which of these changed on 2026-10-26.
- D23 (autotune and control review, 2026-10-26): 12 proposals for Shawn,
  top ones: profile end holds SV unattended, 0/100 % relay swing, no
  convergence check. Item 4 done 2026-10-27: fan and heater commands abort
  autotune (`outputCommandStopsAutotune`).

## Git state (2026-08-30, upstream)
- Branch `dev` (HEAD `3f52109`, tracks `origin/dev`); the `boards/` + `croaster/`
  restructure, per-board `setBoardName()`, and the 0.62 bump are committed
  (merged from `decoupling` via PR #31). Working tree **clean** after this batch.
- The ESP32-S3 LVGL implementation was ported from the old single-project
  `ES3C28P` branch into `boards/esp32s3/`
  (`git show ES3C28P:<path>` is the source of the UI).
- `build_all.sh` emits `builds/Croaster_<board>_<version>.bin` (version read
  from `croaster/src/CroasterConstants.h`).
- This batch adds `format.sh` + `.clang-format` (all sources reformatted) and
  the esp32s3 `darkMode` / `brightness` custom commands.
- ⚠️ **Downstream:** the ICRM app consumes this repo via
  `lib_deps = https://github.com/IiemB/Croaster.git`, which needs `library.json`
  at the repo root — update it to the `croaster/` subfolder (or add a root
  `library.json`).

## Next steps
- Croaster PID: hardware bring-up and the rest of `docs/tickets.md` (T2–T14).
- Update the ICRM app's `lib_deps` for the `croaster/` subfolder.
- Flash + verify the esp32s3 build on real hardware (touch, chart swipe,
  dark mode, OTA over BLE/WebSocket).
- Confirm `boardName()` strings against the ICRM app's `CroasterBoardTypes`.

## Gotchas
- Dummy mode (`CMON_DUMMY_DATA`) only fakes Croaster's `tempBt`/`tempEt`
  (display unit, T61). On Croaster PID the control task still reads the real
  probes, so PID, autotune and safety see NaN without probes; only the
  recipe runner (`RecipePlugin::inputs()`) reads the core values.
- WebSockets 2.7.3 raises `DISCONNECTED` for every closed slot, including
  connections that never upgraded; count clients with `connectedClients()`,
  not ++/-- per event (T60). The ICRM app needs the periodic broadcast
  (`[SOCKET-JSON]` in the serial log) and reconnects after ~10 s without it;
  Artisan and the page poll and do not notice.
- `MAX6675_Thermocouple` comes only from the fork pin in
  `croaster/library.json`. Adding `yuriisalimov/MAX6675_Thermocouple` to a
  board's `lib_deps` installs a second copy and the link fails with multiple
  definitions.
- `onJsonCommand(key, fn)`: every registered callback gets the **whole**
  `command` object when its key is present; parse only your own key.
- Croaster's handler ignores messages without a `"command"` member, so
  yaeger's flat keys never reach the plugin (D8).
- Artisan WebSocket: replies must echo the request `id`. Command strings in the
  `.aset` go through Python `str.format`, so literal braces are doubled.
  Artisan's "Control" button is its own PC-side PID; keep it off.
- Units: control math and limits are °C. Croaster's `tempUnit` (boots in the
  saved unit, else `CMON_TEMP_UNIT`, default "F") sets the screen, telemetry and command
  temperatures; convert at the edges (`toCelsius`/`fromCelsius`). Artisan's
  mode and `channel_modes` must match the firmware unit.
- Corrections (D22): `correctionBt`/`correctionEt` live in core, in the display
  unit. `ControlPlugin::loop()` passes them to the controller as °C
  (`SetCorrection`); PID/autotune use corrected values, over-temperature uses
  max(raw, corrected). A `tempUnit` change rescales the offsets (C→F ×9/5).
  Keep the snapshot uncorrected: core adds the offset when
  it reads `RoasterProbe`, so correcting it too counts the offset twice.
- `Controller` (FreeRTOS) has no host build: put the logic a fix needs in a
  pure `croaster-control/src/core` helper (`onPvStep`, `sensorFault`,
  `crossedWithin`, `MainsWatch`, `outputCommandStopsAutotune`,
  `modeNeedsTuning`) and test that (design.md D23 item 11, T38).
- SCons does not track headers passed with `-include`. Both S3 boards load
  `tools/force_include_deps.py`, so a `config.h` edit recompiles every unit
  (~170 objects); a board without it reuses stale objects after a
  `config.h`-only change.
- `croaster-control/src/core` must not include Arduino, ESP-IDF, `hw/` or
  `app/` headers: the native env compiles only `core/` (`build_filter.py`), so
  such an include breaks `pio test -e native`.
- NVS (`Preferences`) is not usable from global constructors: load settings in
  `setup()`.
- Unpinned `platform = espressif32` resolves to whatever espressif32 is
  installed (a pioarduino install made `esp32c3` fail); pin the version.
- Upstream `boards/esp32s3` needs Arduino-ESP32 2.x (`ledcSetup` in
  `LvglUi.cpp`): it builds on `espressif32 @ 6.13.0` and fails on 8.0.0 or
  pioarduino (T13).
- `custom_sdkconfig` (pioarduino) writes `managed_components/`, `sdkconfig.*`,
  `CMakeLists.txt`, `src/idf_component.yml` into the board folder (gitignored in
  `esp32s3-pid`). `MBEDTLS_SSL_IN/OUT_CONTENT_LEN` need
  `MBEDTLS_ASYMMETRIC_CONTENT_LEN=y` to take effect.
- `.clang-format` is LLVM-based with attached braces; AGENTS.md says Allman.
- Agent rules: root `AGENTS.md` (doc updates per change, checks before a
  commit), plus `croaster-control/AGENTS.md` and `croaster-webui/AGENTS.md`.
  Sources follow `.clang-format`.
- `.gitattributes`: `* text=auto eol=lf`, LF forced for sources, scripts,
  web assets, config and docs, CRLF for `.bat`/`.cmd`/`.ps1`, images and
  `.bin`/`.gz`/`.pdf`/fonts/`.FCStd` binary. `embed_assets.py` hashes and
  embeds `web/` with CRLF folded to LF.
- PlatformIO not on PATH → `~/.platformio/penv/bin/pio` or `PIO=`.
- Libs are **live-linked** via `symlink://` lib_deps
  (`Croaster=symlink://../../croaster`, `CroasterDisplaySSD1306=symlink://../common`,
  `CroasterControl=symlink://../../croaster-control`,
  `CroasterWebUi=symlink://../../croaster-webui`)
  — editing `croaster/src/` needs no `.pio/libdeps` cleanup.
- `croaster-webui/src/webui_assets.h` is generated from `web/` by
  `embed_assets.py` on every build (only rewritten when the source hash
  changes). Commit it with the `web/` change; `python3
  croaster-webui/embed_assets.py --check` fails when it is stale. Bump
  `FORMAT` in the script when the generated layout changes.
- Web UI tests: `node --test croaster-webui/test/*.test.js` and
  `python3 -m unittest discover -s croaster-webui/test`.
- Croaster PID keeps profiles on LittleFS on the stock `spiffs` partition
  (`CPID_ENABLE_LIBRARY`, D28); OTA keeps them, a USB erase-flash does not.
  Recipe steps run on the board (`app/RecipePlugin`, `core/recipe.h`) and
  still need a connected client for the comms watchdog.
- Host tests (`pio test -e native`) call `g++`. On a machine without it, a
  `g++` wrapper script running `python3 -m ziglang c++ "$@"` (pip package
  `ziglang`) on `PATH` works.
- ESP32-C3: WiFiManager AP can fail to broadcast under the PIO core (2.0.17) —
  fixed via DIO flash mode + `WiFi.setTxPower(WIFI_POWER_18_5dBm)` in the
  configMode callback (see `boards/esp32c3/` notes).
- ESP32-S3 (LVGL): heavy UI work must run on its FreeRTOS task (Core 0), never
  the Arduino loop task; the theme switch is deferred via
  `themeRebuildPending` + `LvglUi::rebuildTheme()` on the LVGL task.
- ESP32-S3: WSS connect needs the mbedTLS PSRAM/buffer Kconfig overrides in
  `boards/esp32s3/esp32s3-sdkconfig.defaults` (otherwise
  `MBEDTLS_ERR_SSL_ALLOC_FAILED`).
