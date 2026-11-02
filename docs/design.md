# Design — Croaster PID control board on Croaster (ESP32-S3)

Status: skeleton, builds and host-tests pass; not yet run on hardware.
Owner decisions still open are marked **DECISION**.
Board renamed `esp32s3-roaster` → `esp32s3-flex` on 2026-10-26 (D5).
2026-10-28: renamed to Croaster PID (`esp32s3-pid`, `CPID_`). (D25)

## Goal
Shawn's roaster (SSR heater, AC-dimmer fan, 2 × MAX31855, I2C OLED, Lonely
Binary ESP32-S3 N16R8) gets yaeger's control features (PID, autotune, profiles,
Artisan control, safety) on top of Croaster's platform (WiFiManager, WebSocket,
BLE/ICRM app, OTA, RoR, display interface, per-board projects), keeping
Croaster's library/board split and extension model.

## Croaster's model (what we kept)
- `croaster/` is a display- and pin-agnostic library. Boards inject everything:
  pins (`CroasterPinConfig`), a `CroasterDisplay` subclass, LED pin, board id
  (`CroasterDeviceIdentity::setBoardName`).
- `CroasterApp` owns WiFi, WebSocket :81, BLE, OTA, and the command handler.
  Boards extend the protocol with `commands().onCommand(name, fn)` and
  `commands().onJsonCommand(key, fn)`.
- `boards/<board>/` are standalone PlatformIO projects; `boards/common/` is
  shared board-side code. `build_all.sh` builds env `<board>`;
  `upload_monitor.sh` builds env `<board>-debug`.

## Names

The whole project is a roaster, so "roaster" in a name says nothing. Rule:
board-level things say **Croaster PID** in prose and `esp32s3-pid` in code,
control features say **control**, and no class or macro starts with
`Croaster` (that prefix is the upstream author's). A bare "PID" or `pid`
always means the controller (`Pid`, `PidGains`, `Mode::Pid`, the `pid`
command), never the board (D25).

Config macro prefixes, for our settings only (D26):
- `CMON_`: settings every Croaster board has: probes, screen, identity, radio
  (`CMON_BOARD_NAME`, `CMON_TEMP_UNIT`, `CMON_THERMOCOUPLE`, `CMON_PIN_TC_*`,
  `CMON_DISPLAY`, `CMON_PIN_I2C_*`, `CMON_PIN_LED`, `CMON_ENABLE_BLE`...).
- `CPID_`: what the control add-on adds: heater, fan, mains, PID input and
  gains (`CPID_PID_*`), autotune gate, limits, cooldown, watchdog, control core.
- The upstream author's names are never renamed: `CROASTER_HAS_BLE` stays and
  is set from `CMON_ENABLE_BLE` in `config.h`.
The one exception is the control library folder, `croaster-control/` (lib
`CroasterControl`), named by Shawn on 2026-10-27 (D1). Renamed 2026-10-26 at
Shawn's request, then folded into one library on 2026-10-27:

| Before | After | What it is |
|:---|:---|:---|
| `roaster-core/` (lib `RoasterCore`), `src/core/` | `control-core/` (lib `ControlCore`), `src/control/`; since 2026-10-27 `croaster-control/src/core/` | Pure, host-tested logic: PID, autotune, profiles, decoders, protocol |
| `roaster-control/` (lib `RoasterControl`) | `control-esp32/` (lib `ControlEsp32`); since 2026-10-27 `croaster-control/src/hw/` and `src/app/` | ESP32 side: control task, SSR, fan drivers, probes, NVS, plugin |
| `roaster_protocol.*`, namespace `roasterproto` | `control_protocol.*`, `controlproto` | JSON commands and data fields |
| `roaster_types.h` | `control_types.h` | `Command`, `Snapshot`, `Mode`, `SafetyTrip` |
| `Roaster` (`app/roaster.*`) | `Controller` (`app/controller.*`) | The control task |
| `RoasterPlugin`, `RoasterPluginConfig` | `ControlPlugin`, `ControlPluginConfig` | Registers commands with `CroasterApp` |
| `RoasterSettings` | `ControlSettings` | NVS-backed settings |
| `RoasterLimits` | `SafetyLimits` | Over-temp, max setpoint, min fan, mains check |
| `RoasterOledDisplay` | removed | The Croaster PID board uses Croaster's `CroasterDisplaySSD1306` |
| `ROASTER_*` macros in `config.h` | `FLEX_*` | Board config. 2026-10-28: renamed to Croaster PID (`esp32s3-pid`, `CPID_`); the same day, settings every board has became `CMON_` (D26). |
| `{"command":"getRoaster"}`, `writeRoasterInfo` | `getControl`, `writeControlInfo` | Status query |
| FreeRTOS task `"roaster"` | `"control"` | |
| NVS namespace `"roaster"` | `"control"` | Nothing is deployed, so no settings are lost |
| `RoasterProbe` | kept | See below |

`RoasterProbe` (`croaster-control/src/app/RoasterProbe.h`) is a `Thermocouple`
adapter for the two K-type probes, ET and BT, and nothing else. It does not
touch the chips. The control task reads both MAX31855s (`hw/thermocouples`)
and publishes filtered values; each `RoasterProbe` returns one channel of that
snapshot (NaN on a fault) so `CroasterCore` computes RoR and telemetry from
the same numbers the PID uses. Shawn keeps the name for the K-type probes.

Still named "roaster": Artisan's `roasterID` field, the branch
`rework/roaster`, the `roaster-*.zip` file names, and prose about the
physical machine.

## Layers

```
boards/esp32s3-pid/          board: config.h, ConfigCheck.h, main.cpp
boards/esp32s3-monitor/      board: probes only (MonitorProbes over hw/thermocouples, no control task)
        |  constructs + injects
        v
croaster-control/src/app/  (Arduino/ESP32)  ControlPlugin  --registers-->  CroasterApp commands / onJsonData
                                    Controller (FreeRTOS task) <-- Command queue
                                    ConfigStore (NVS settings)
                                    RoasterProbe : Thermocouple  --> CroasterCore (BT/ET)
        |
        v
croaster-control/src/hw/   (ESP32 drivers)  Thermocouples (SPI), HeaterOutput (SSR),
                                    FanDimmer | FanPwm behind FanOutput
        |
        v
croaster-control/src/core/ (pure C++, host-tested)  PID, relay autotune, roast profile,
                                    MAX31855/MAX6675 decode, probe filter, safety rules,
                                    time-proportional window, command protocol
croaster/         (Croaster library) WiFi, WebSocket, BLE, OTA, RoR, display iface
```

Rules this keeps: nothing in `croaster/` knows about heaters or fans; board
config stays in the board; control logic is reusable by any future board
(e.g. a C3 control board) by adding another `boards/<board>/` folder.

### Where each feature lands

| Feature | Lands in | Why |
|:---|:---|:---|
| MAX31855 or MAX6675 on Croaster monitor (`boards/esp32s3-monitor`) | `croaster-control/src/app/MonitorProbes.h` over `hw/thermocouples` | One reader and one decoder per chip for both boards; `croaster/` keeps upstream's MAX6675 path (D11, D26). |
| MAX31855/MAX6675 decode for control | `croaster-control/src/core/max31855.h`, `max6675.h` | Pure bit decoding, host-tested; control task reads the bus itself. |
| Per-probe filter and fault count | `croaster-control/src/core/probe_channel.h` | Pure logic; both channels tested on the host. |
| PID, autotune, profiles, safety rules, command protocol | `croaster-control/src/core` | Pure logic, unit-tested on the host. |
| SSR, dimmer, PWM fan, probe reading | `croaster-control/src/hw` | ESP32 hardware, shared by any ESP32 control board. |
| Control loop + safety, NVS settings | `croaster-control/src/app/controller`, `app/config_store` | Own FreeRTOS task (see Threading). |
| Commands, data fields, watchdog | `croaster-control/src/app/ControlPlugin` | Uses only Croaster's public extension points. |
| Pins, chip/driver/display choice, limits | `boards/esp32s3-pid/src/config.h`, `boards/esp32s3-monitor/src/config.h` | Board config never in a library (AGENTS.md). |
| OLED screen | `boards/common` `CroasterDisplaySSD1306` + `setHeaderText()` | Croaster's shared screen; the board supplies the header text (heater/fan/mode or trip). |
| Browser UI (D27) | `croaster-webui/` | Served by the board; talks to Croaster's WebSocket like any client, so no core change. |
| Saved temperature unit (D16, D27) | `croaster-control/src/app/unit_store`, `core/temp_unit.h` | NVS outside core; applied through core's own `changeTemperatureUnit()`. |

### Every file we added, against Croaster (review point 9, 2026-10-26)

Relation: **new** (Croaster has nothing like it), **extends** (adds to a
Croaster part through its own extension point), **duplicates** (a second copy
of something Croaster or our code already has).

Board folder `boards/esp32s3-pid/`:

| File | Relation | Why it is here |
|:---|:---|:---|
| `src/main.cpp` | extends | Same shape as Croaster's board `main.cpp`: builds `CroasterApp`, adds the control plugin, the probe adapters and the screen header. Always builds heater and fan; probes only is `boards/esp32s3-monitor` (D26). |
| `src/config.h` | new | The one master file for pins, chips, drivers and limits (D2). Croaster boards set pins in `main.cpp`. |
| `src/ConfigCheck.h` | new | Compile-time checks on `config.h` (reserved pins, duplicates, ranges). |
| `platformio.ini` | extends | Croaster's board ini pattern (`<board>`, `<board>-debug`), plus one pioarduino env for Kconfig. |
| `artisan/croaster-pid.aset` | new | Artisan settings for heater, fan, PID and the data request. |
| `test/test_core/test_main.cpp` | new | Host tests for `croaster-control/src/core`. |
| `README.md`, `.gitignore` | new | Wiring, settings and generated-file ignores. |

Board folder `boards/esp32s3-monitor/` (2026-10-28, D26):

| File | Relation | Why it is here |
|:---|:---|:---|
| `src/main.cpp` | extends | Croaster's board `main.cpp` shape with `app/MonitorProbes` as the two sensors; no control task. |
| `src/config.h` | new | Probe chip, pins, display, unit, BLE; `CMON_` settings only. |
| `src/ConfigCheck.h` | duplicates | The checks of Croaster PID's `ConfigCheck.h` that apply to it; pin rules from `hw/esp32s3_opi_pins.h` plus the dev-board wiring (T46). |
| `platformio.ini` | extends | Croaster's board ini pattern (`<board>`, `<board>-debug`); same platform, board json, partitions, memory type and sdkconfig path as Croaster PID. |
| `README.md` | new | Wiring, settings, build. |

Library `croaster-control/` (paths under `src/`; until 2026-10-27 these were
two libraries, `control-core/` and `control-esp32/`):

| File | Relation | Why it is here |
|:---|:---|:---|
| `core/max31855.h` | new | The only MAX31855 decoder (D11). Croaster has none; one 32-bit frame gives temperature, cold junction and fault bits. |
| `core/max6675.h` | duplicates | Same decode as the `MAX6675_Thermocouple` library, which reads the bus itself and has no fault bit API. Used only if a control board picks MAX6675. |
| `core/moving_average.h`, `probe_channel.h` | new | Per-probe filter and fault count for the control loop. Croaster's `SmoothThermocouple` wraps a sensor that reads the bus, and has no fault count, so it can't sit on the control task's readings. |
| `core/pid.*`, `relay_autotune.*`, `roast_profile.*`, `time_proportional.h` | new | Control features Croaster doesn't have. |
| `core/zero_cross.h`, `mains_watch.h` | new | Mains detection for the AC dimmer: crossing age, and the grace period before a `nomains` trip. |
| `core/control_protocol.*`, `control_types.h` | extends | Parses the payloads Croaster's handler hands over (nested commands and yaeger's named commands); builds the extra data fields. |
| `app/ControlPlugin.*` | extends | Registers commands and data fields through `CroasterApp`'s public hooks; runs the comms watchdog off core's `lastCommandMillis()`. |
| `app/RoasterProbe.h` | extends | A Croaster `Thermocouple` that reads the control task's snapshot, so `CroasterCore` never touches the bus the control task owns. |
| `app/MonitorProbes.h` | extends | Croaster monitor only: two Croaster `Thermocouple`s over `hw/thermocouples`, read on the loop task (no control task). Same chip choice, decoders and fault filter as the control build (D11). |
| `app/controller.*` | new | Control task: sampling, safety, modes, outputs. |
| `app/config_store.*` | new | NVS for PID gains, PID input, cooldown fan (stop and trip) and watchdog. Croaster core stores nothing. |
| `hw/heater_output.*`, `fan_dimmer.*`, `fan_pwm.*`, `fan_output.h`, `thermocouples.*` | new | SSR, triac dimmer, PWM fan and SPI probe reading. |
| `hw/esp32s3_opi_pins.h` | new | `constexpr` GPIO rules for ESP32-S3 modules with octal PSRAM (missing pins, flash/PSRAM, USB, strapping), shared by both boards' `ConfigCheck.h` (T46). Dev-board wiring stays in each board. |
| `core/temp_unit.h`, `app/unit_store.*` | extends | Saves Croaster's `tempUnit` in NVS and applies it at boot through `changeTemperatureUnit()` (D16, D27). Croaster core stores nothing. |

Library `croaster-webui/` (2026-10-29, D27):

| File | Relation | Why it is here |
|:---|:---|:---|
| `web/*` | new | The browser page: HTML, CSS, UI script, roast math, WebSocket worker. Croaster has no web page. |
| `embed_assets.py`, `src/webui_assets.h` | new | Build step that gzips `web/` into a header (generated, committed, reproducible). |
| `src/WebUiServer.*` | new | ESP-IDF HTTP server for the page. Croaster's only HTTP server is WiFiManager's portal. |
| `test/*` | new | Node tests for the roast math and page, Python tests for the asset step. |

Clear wins done this round: the board's own SSD1306 class (now Croaster's
display plus a hook, D12), `partitions.csv` (identical to `default_16MB.csv`),
ArduinoOTA (D13), and the probe filter moved out of `hw/` into a pure,
tested `ProbeChannel`. Since then (2026-10-27): one MAX31855 decoder (D11)
and one sdkconfig file (D10).

## Data flow
1. Control task (core 1, every 10 ms tick): reads both converters on its own
   SPI bus every sample period, filters (moving average of 4, reset after 3
   faults), runs safety checks, mode logic (Manual / PID / Profile / Autotune),
   PID, and the SSR time-proportional window; writes the fan; publishes a
   `Snapshot` under a spinlock.
2. Croaster loop (Arduino loop task, core 1, lower priority): `CroasterCore`
   reads BT/ET through `RoasterProbe` (snapshot, NaN on fault), computes RoR,
   and broadcasts JSON; `onJsonData` adds `BurnerVal`, `FanVal`, `SV`, `Mode`,
   `trip`.
3. Clients (Artisan, ICRM, BLE, the browser page of D27) send JSON → `CroasterCommandHandler` →
   `ControlPlugin` callbacks → `controlproto` parse → `Command` queue → control
   task. Gains / PID input / cooldown fan are persisted to NVS by the plugin.

## Threading
- WiFi, LwIP and BLE (NimBLE) run on core 0 (IDF defaults).
- The browser UI's HTTP server (D27) is its own task at priority 3 on the
  core opposite the control task (core 0 by default). It serves constant data
  and shares nothing with the other tasks.
- Croaster's board json pins Arduino loop + events to core 1; the control task
  also runs on core 1 at `configMAX_PRIORITIES - 3`, so it preempts the loop
  (WebSocket handling, display, JSON) and never waits for it.
- Only two shared objects: a FreeRTOS queue (commands in) and a
  spinlock-protected `Snapshot` (state out). The SPI bus belongs to the control
  task alone; that is why `CroasterCore` got a constructor taking sensors.
- Dimmer: zero-cross ISR + one-shot GPTimer fires the triac gate. The
  handlers are `IRAM_ATTR`, but neither interrupt is registered as IRAM
  (`CONFIG_ARDUINO_ISR_IRAM` and `CONFIG_GPTIMER_ISR_CACHE_SAFE` are off), so
  both wait out flash writes. That is safe, because they call flash-resident
  gptimer functions. The fan may stutter for a few half-cycles during an NVS
  or OTA write.

## Changes to Croaster's core (and why)

| Change | Needed? | Reason |
|:---|:---|:---|
| `CroasterCore(bool, Thermocouple* bt, Thermocouple* et)` | needed | Two owners of one SPI bus would race; this lets the control task own it. Additive, existing constructor unchanged. |
| `CroasterCore::onJsonData(fn)` | needed | Artisan must see heater/fan/setpoint in the same `getArtisanData` payload; no existing hook. Additive. |
| `CroasterDisplaySSD1306::setHeaderText(fn)` (boards/common) | small | Lets a board show its own state in the header line instead of a second display class. Additive; default header unchanged. |
| `#ifndef CROASTER_HAS_BLE` | small | Lets a board turn BLE off (~310 KB less flash in our build). Default unchanged. |
| `CroasterCommandHandler::lastCommandMillis()` | needed for the comms watchdog | Lets an add-on see client traffic on Croaster's own WebSocket/BLE path, built-in commands included. Additive getter; `handle()` stores `millis()` when it accepts a message. See D6. |
| `CroasterBleManager` initializer order, `CroasterDeviceIdentity` `PRIX32` + `snprintf` | warning fixes | `-Wreorder` and `-Wformat` on ESP32 builds. No behaviour change; the chip id string is identical. |
| `CroasterCore::correctionBtCelsius()`/`correctionEtCelsius()` | needed for correct PID | The control task reads the probes itself, so it needs the user's correction offsets to work on the displayed scale. Two const getters; stock boards never call them. See D22. |
| `changeTemperatureUnit` rescales `correctionBt`/`correctionEt` | bug fix | The offsets are differences in the display unit; a unit change kept the number, so the applied offset grew or shrank by 1.8×. See D22. |
| ~~Removed negative-RoR clamp~~ | **reverted 2026-10-27** (Shawn) | Upstream's `deltaET > 0` / `deltaBT > 0` test is back, so a falling temperature reads RoR 0 as in upstream. Artisan computes its own RoR, no consumer needs negative values, and the display keeps upstream behavior. `updateROR` matches `5445a4d`. |

Existing boards still build with every change above (2026-10-26, temporary
copies pinned to `espressif32 @ 8.0.0`; esp8266 on its own platform). Release
sizes: `esp32c3` RAM 44 728 B, flash 1 742 297 B (91.7 %); `esp8266` RAM
36 484 B, flash 415 739 B. Rebuilt 2026-10-27 after D11 (no MAX31855 code
in `croaster/`), the RoR revert and the unit-change offset fix: `esp32c3` RAM
44 712 B, flash 1 740 813 B (91.6 %); `esp8266` RAM 36 468 B, flash
414 979 B; same warnings as upstream (`BLE2902` deprecated on esp32c3,
WiFiManager `-Wformat`). The `esp32s3` result from that round is void: it
was built from the board as edited during Shawn's testing before this project
(see "Stock boards and upstream" below).

### Stock boards and upstream

Upstream is `IiemB/Croaster` `master` at `5445a4d`, the commit this fork
branched from. Shawn's fork added three commits on top before this project
started (`da20956`, `77ad627`, `0845ba8`, author "Your Name"). They changed
`.gitignore`, the root and board `.vscode/` files, `boards/esp32s3`
(`platformio.ini` comments stripped, custom board json, `-W
no-deprecated-declarations`, `ARDUINO_USB_MODE=1`, `dummyMode = true`,
`ledPin = -48`, the backlight on the Arduino-ESP32 3.x LEDC API, `lv_conf.h`),
and added `build.log`, `croaster/src/Croaster.code-workspace`,
`To-Implement.md` and `boards/esp32-s3-devkitm1-n16r8.json`.

On 2026-10-26 the stock board files went back to upstream byte for byte. Two
exceptions: the stock inis keep D21's removal of the registry
`MAX6675_Thermocouple` (listing it next to the core's fork fails to link), and
`boards/esp32-s3-devkitm1-n16r8.json` stays because the Croaster PID board uses it.
`build.log` and `Croaster.code-workspace` are deleted. `To-Implement.md` is
Shawn's design note and stays.

Upstream's `boards/esp32s3` needs Arduino-ESP32 2.x: `LvglUi.cpp` calls
`ledcSetup`/`ledcAttachPin`, which 3.x removed. Its unpinned `platform =
espressif32` resolves to 8.0.0 (Arduino 3.3.12) today, and the build stops
there, on upstream `5445a4d` and on this branch alike. Pinned to
`espressif32 @ 6.13.0` (Arduino 2.0.17), upstream builds. T13 tracks the fix.

## Decisions where Croaster's rules and Shawn's goals rub

Each lists alternatives; the recommended one is implemented (all reversible)
unless the heading says otherwise.

Changes from Shawn's nine review points (2026-10-26):

| Decision | Status | What moved |
|:---|:---|:---|
| D1, D3 | names only | Folder and macro names (see "Names"); the choice is unchanged. |
| D6 | changed | Watchdog reads core `lastCommandMillis()`; the `getData` alias is gone. |
| D8 | changed | Rewritten: why flat keys need the hook; dropping them is now an option. |
| D9 | changed | Watchdog timeout and an off switch are settings (config.h, command, NVS). |
| D10 | facts updated | Partition table and display libs now match the shared defaults. |
| D11 | new, open | Two MAX31855 implementations. |
| D12 | new | Croaster's SSD1306 display with a header hook; U8g2 and SH1106 dropped. |
| D13 | new | Croaster's OTA only; ArduinoOTA and its mDNS name dropped. |
| D14 | new, design only | Config wizard. |
| D15 | new | `MAX6675_Thermocouple` stays a core dependency. |
| D16 | new | Temperature unit: Croaster's `tempUnit`, °F at boot on the Croaster PID board; control stays in °C. |
| D9 | note added | How a comms trip clears, and why reconnecting doesn't clear it. |
| D17 | new | Visible autotune status: `autotune` in telemetry and on the OLED. |
| D18 | new, open | PID input (ET or BT) and default gains. |
| D19 | new, open | Cooldown fan default (100 % or yaeger's 65 %). |
| D20 | new, open | Roast rules and profiles. |
| D21 | new, resolved | `MAX6675_Thermocouple` comes from Shawn's fork at `v2.0.3`. |
| D23 | new, open | Autotune and control review: three fixes made, twelve proposals. |

Resolved by Shawn on 2026-10-26:

| Decision | Choice |
|:---|:---|
| D8 | Drop flat keys and the `onOtherJson` hook (option 2). |
| D17 | Autotune status in `getArtisanData` and on the OLED header (option 3). |
| D18 | PID input defaults to BT (option 2); `pidInput` command and NVS kept; yaeger's gains kept until autotuned on BT. |
| D19 | Cooldown fan 65 % after `stop`, 100 % after any trip (option 3). |
| D21 | Fork `MAX6675_Thermocouple` and fix it there (option 3). |
| D22 | Pass Croaster's correction offsets to the controller through core getters (option 1). |

Resolved by Shawn on 2026-10-27:

| Decision | Choice |
|:---|:---|
| D1 | One library `croaster-control/` with `src/core`, `src/hw`, `src/app`; `control-core/` and `control-esp32/` are gone. |
| D10 (sdkconfig part) | "Kill ours, keep his": the Croaster PID board's `esp32s3-sdkconfig.defaults` is deleted; it sets `board_build.sdkconfig_path = ../esp32s3/esp32s3-sdkconfig.defaults`. The ini question stays open. |
| D23 (mains) | `nomains` trips after 2 s without a zero-cross while heating (`CPID_MAINS_GRACE_MS`). |
| D24 | Autotune required before PID and profiles (`CPID_REQUIRE_AUTOTUNE 1`); gains set with the `pid` command count as tuned. |
| D23 item 4 | Fan and heater commands during autotune abort it, heater off (`aborted: fan changed` / `aborted: heater changed`). |
| D11 | One MAX31855 decoder, `croaster-control/src/core/max31855.h`. `CroasterMAX31855`, `CroasterThermocoupleType` and the type argument are removed; `croaster/` builds its thermocouples exactly as upstream. The monitor build (no heater) reads the chips through `app/MonitorProbes` (since 2026-10-28 `boards/esp32s3-monitor`, D26). |

Resolved by Shawn on 2026-10-28:

| Decision | Choice |
|:---|:---|
| D25 | Board renamed `esp32s3-flex` → `esp32s3-pid` ("Croaster PID"), `FLEX_*` → `CPID_*`, `croaster-pid.aset`. |
| D26 | Croaster monitor board `esp32s3-monitor` replaces the `CPID_HEATER_NONE` build; Croaster PID always has heater + fan. Prefix split: settings every Croaster board has are `CMON_`, control settings stay `CPID_`. |

New 2026-10-31, open for Shawn:

| Decision | Summary |
|:---|:---|
| D30 | Autotune fan test, fan and ramp feedforward, fan assist. |
| D31 | Recipe ramp shapes (rate-of-climb curves), power ramps, charge temperature, ramp feasibility check. |
| D32 | Setpoint ramps start at the PID input (BT) when the step applies, with a bumpless PID restart. |
| D33 | Fake readings button is green while dummy mode is on, from a `dummy` data field. |
| D34 | Optional scroll fan, case fan (FAN1), ESP fan (FAN2) and box temperature. |

### D1. Where the control libraries live — resolved 2026-10-27: one library, `croaster-control/`
Shawn chose one top-level library next to `croaster/`, split by purpose into
`src/core` (pure, host-tested), `src/hw` (ESP32 drivers) and `src/app`
(control task, NVS settings, Croaster integration). `croaster/` stays the
monitor library; board-only files stay in `boards/esp32s3-pid/`.

Layout choice: the folders sit under `src/`, PlatformIO's default library
source dir. PlatformIO then compiles all three and puts `src/` on the include
path, so code includes `core/pid.h`, `hw/thermocouples.h`, `app/controller.h`
with no `srcDir`/`includeDir` settings. Folders at the library root would need
`build.srcDir` and an include path pointing at the root, where `library.json`
and `build_filter.py` also sit.

Host tests: `croaster-control/build_filter.py` (`library.json`
`build.extraScript`) sets the library's source filter to `+<core/>` when the
platform is `native`. PlatformIO provides this hook: its library builder
clears `SRC_FILTER` and then runs the extra script so the script can set it.
The native env compiles only `src/core`; `app/` and `hw/` never reach the host
compiler.

Placement rules: a file goes in `core/` when it compiles without Arduino or
ESP-IDF headers, whatever it is about (the command protocol parser is pure,
so it is in `core/` and host-tested; `app/ControlPlugin` registers it with
Croaster). `hw/` holds drivers that touch pins, SPI or timers. `app/` holds
what ties the parts together on the device: the FreeRTOS controller, NVS
settings (`Preferences`, used only by the plugin and `main.cpp`), the plugin
and the probe bridge into `CroasterCore`.

Earlier options, kept for the record (AGENTS.md then listed only `croaster/`,
`boards/`, `builds/`):
1. Top-level `control-core/` + `control-esp32/` libs (implemented until 2026-10-27).
   Clear split: pure logic vs hardware; consumed like `boards/common` via
   `symlink://`. Bends the layout list (two new top-level folders).
2. Put them in `boards/common-control/` (one lib). Matches the "shared board
   code under boards/" precedent, but `build_all.sh` must skip it (it only skips
   `common`), and the pure/hardware split is lost.
3. Fold into `croaster/src/roaster/`. One library, but every board (ESP8266
   too) compiles heater/fan code and it breaks "monitor library, no board
   specifics" intent.
4. Keep everything inside `boards/esp32s3-pid/src/`. Zero layout change,
   but nothing is reusable for a second control board and tests live with one board.

### D2. One master config vs `config.h` + `pins.h` — resolved
Croaster boards use `config.h` (pins/dummy/LED) and sometimes `pins.h`.
Shawn asked for a single file. Implemented: `src/config.h` holds everything
(name keeps the convention), `src/ConfigCheck.h` validates. Options considered:
split pins.h (convention, two files to edit), runtime JSON config (no compile
checks), Kconfig menus (pioarduino only).

### D3. Library feature switches from board config — **DECISION**
`CROASTER_HAS_BLE` must be seen when `croaster/` compiles.
1. **Force-include `config.h` into every unit (`-include`, implemented).** One
   file really is the master; cost: every library sees the `CMON_*`/`CPID_*` macros
   (prefixed, so collisions are unlikely), and a `config.h` edit recompiles
   every unit. SCons does not track `-include` headers;
   `tools/force_include_deps.py` adds the dependency (T47).
2. `-D CROASTER_HAS_BLE=0` in `platformio.ini`. Croaster style, but the master
   config is then split across two files.
3. Runtime switch (`CroasterApp` option to skip BLE). BLE code still linked
   (no flash saving), library API change.
4. Always build BLE. Simplest; ~310 KB flash plus NimBLE heap at runtime (fine on N16R8).
Recommendation: 1.

### D4. Compile-time feature selection in `main.cpp`
AGENTS.md: "no board-specific compile-time guards" (meaning: the library never
`#ifdef`s on a board). Our `#if CPID_FAN == ...` are feature selections inside
one board, which the rule does not forbid, but they are guards. Alternatives:
separate board folders per variant (monitor / control; duplicates main.cpp),
runtime selection (all drivers linked, invalid configs only caught at boot),
or templates (more C++ than the codebase uses). Kept `#if` + `ConfigCheck.h`
because Shawn asked for compile-time selection with static checks.
2026-10-28: the monitor variant is its own folder, `boards/esp32s3-monitor`
(D26). Croaster PID's `#if`s now select only the fan driver and the display.

### D5. Board id — resolved: `esp32s3-flex`, chosen by Shawn 2026-10-26
Ids must be lowercase and match ICRM's `CroasterBoardTypes`
(`esp32s3`/`esp32c3`/`esp8266`/`unknown`). Folder `boards/esp32s3-flex/`,
envs `esp32s3-flex[-debug|-pioarduino]`, board id
`"esp32s3-flex"`. "roaster" was dropped
because the whole project is a roaster, so it told you nothing. Options that
were considered:
1. **`"esp32s3-flex"` (chosen).** Distinct, so the app never offers the
   ES3C28P LCD image as an update. ICRM may show it as unknown until its enum
   is extended.
2. `"esp32s3"`. Matches the enum, but an ICRM OTA would flash the LCD firmware
   over this firmware (heater control lost).
3. `"unknown"`. Safe, but the app learns nothing.
4. Add `esp32s3Flex` to ICRM's enum (needs the app author). Still worth doing later.

2026-10-28: renamed to Croaster PID (`esp32s3-pid`, `CPID_`). The ICRM enum entry would now be `esp32s3Pid` (D25).
The Croaster monitor's id is `"esp32s3-monitor"`; its enum entry would be `esp32s3Monitor` (D26).

### D6. How the watchdog sees client traffic — **DECISION** (changed 2026-10-26)
The heater must stop when the client disappears, so the watchdog needs to see
every message a client sends. Croaster answers `getArtisanData` inside its
command handler, where a board can't see it. Previously option 3 was
implemented; Shawn asked for Croaster's own WebSocket/BLE path instead.
1. **`CroasterCommandHandler::lastCommandMillis()` (implemented).** `handle()`
   records when it last accepted a message. That covers WebSocket and BLE and
   every command, `getArtisanData` included. The plugin reads it each loop.
   Additive, about 10 lines, and any add-on can use it. The `.aset` polls
   Croaster's own `getArtisanData` again.
2. An `onAnyCommand(fn)` callback hook. Same coverage, but a callback per
   message where a timestamp is enough.
3. A `getData` alias in the plugin (the previous version). No core change, but
   only clients that poll `getData` count; the ICRM app's traffic doesn't.
4. WebSocket client count (`connected()`). Catches drops, not a hung Artisan.
Recommendation: 1.

### D7. Control in a FreeRTOS task, not `loop()`
Croaster is loop-driven. A blocking WiFiManager portal, WebSocket burst or BLE
OTA can stall `loop()` for seconds; an SSR left on for seconds is the failure to
avoid. Options: loop-only (simplest, unsafe stalls), hardware-timer ISR (no SPI
reads in ISR), dedicated task (implemented), task on core 0 (shares core with
WiFi). Task on core 1 at high priority chosen.

### D8. yaeger's flat keys need a core hook — resolved 2026-10-26: dropped (option 2)
Shawn chose option 2: nothing we ship sends flat keys, and dropping them
removes a core change. `onOtherJson` and the flat-key half of `parseLegacy`
are gone; `CroasterCommandHandler` matches upstream apart from
`lastCommandMillis()` (D6). The analysis below is kept for the record.

yaeger's clients send two kinds of message. Named commands
(`setBurner`, `setFan`, `setPreferences`, `getPreferences`) carry a string
`"command"`, so they use Croaster's `onCommand` with no core change (done,
`kLegacyBasicNames`). Flat keys (`{"BurnerVal":60}`, `{"FanVal":80}`,
`{"Setpoint":200}`, `{"Mode":"PID"}`, `{"Target":"BT"}`) carry no `"command"`
member at all.

Why flat keys can't use Croaster's mechanism: `CroasterCommandHandler::handle()`
dispatches on the `"command"` member only. A string goes to the `onCommand`
table, keyed by that string; an object goes to the `onJsonCommand` table, keyed
by the members *inside* `"command"`. A message without `"command"` matches
neither branch, and upstream `handle()` returns false before any registered
callback runs. WebSocket and BLE pass the raw text straight to `handle()`, so
no other layer sees it either. No registration call can reach `BurnerVal`.

Who sends flat keys today: nothing of ours. The `.aset` sends nested commands
(`{"command":{"burner":60}}`). Only yaeger's own web UI and older yaeger
`.aset` files do.
1. `onOtherJson(fn)` fallback in `CroasterCommandHandler` (was implemented).
   About 5 lines, additive, runs only for JSON objects without `"command"`,
   over WebSocket and BLE. Parsing stays in `controlproto::parseLegacy`.
2. **Drop flat-key support (implemented).** Remove the hook, the registration and the flat-key
   half of `parseLegacy`; keep the named yaeger commands. No core change for
   this feature. yaeger's web UI and old settings files stop working; ours
   don't notice.
3. Pre-parse in `CroasterWebSocketManager` before the handler. WebSocket only
   (BLE misses out), and it touches transport code.
4. Translate in the plugin by replacing the handler. No upstream change, but it
   duplicates Croaster's routing (two ways to do one thing).
Recommendation: 1 while yaeger-parity is a goal (T15). If Shawn doesn't need
yaeger's web UI or old `.aset` files, take 2: it removes a core change.
Reversible either way.

### D9. What happens when the client goes away — **DECISION** (changed 2026-10-26)
yaeger: on a clean WebSocket disconnect, in Manual with heater > 0, heater 0
and fan 100. No protection in PID, and none for silent WiFi loss.
1. **Traffic watchdog (implemented):** no client traffic for 10 s while
   heating (Manual, PID or Autotune) → heater off, fan to cooldown,
   `trip:"comms"`. Profiles are exempt (they are meant to run unattended).
   Catches clean disconnects and silent drops; worst case 10 s.
2. Core `onClientDisconnected` hook for an instant cut on a clean disconnect,
   plus option 1 for silent loss. Fastest, but `CroasterApp` doesn't expose
   `wsManager`, so it needs a new core API.
3. yaeger's exact rule (disconnect + Manual only). Matches yaeger, but PID
   keeps heating with nobody watching, and silent drops are missed.
4. No cut-off: the firmware PID keeps going and only the over-temperature trip
   protects. That is how a standalone PID controller behaves, but it is unsafe
   for Manual.
Recommendation: 1; add 2 later if a faster cut on clean disconnects is
wanted.

Settings (changed 2026-10-26, Shawn's point 4): `CPID_WATCHDOG_ENABLED` and
`CPID_WATCHDOG_TIMEOUT_S` in `config.h` set the first-boot defaults.
`{"command":{"watchdog":{"enabled":false}}}` turns the cutoff off entirely and
`{"command":{"watchdog":{"timeout":20}}}` changes the timeout (2 to 600 s).
Both are saved in NVS and reported by `getControl`. Usage: board README,
"Comms watchdog".

Clearing (note added 2026-10-26 after a bench test). A comms trip latches
like every other trip. `clearTrip`, or any heater, mode, autotune or profile
command, clears it; fan and setpoint commands don't. Reconnecting doesn't clear
it on purpose: the trip says the roast went unattended, and the operator should
see that before heat comes back. Artisan sends nothing to the firmware when it
switches ON, so after an OFF/ON cycle the firmware still shows the cooldown
state until a command arrives. A fan command applies even while tripped and a
heater command clears the trip, so if neither changes, the firmware never
received them (no `[CMD-SOCKET]` line in its log). yaeger has no
latch: on a clean disconnect in Manual it sets heater 0 and fan 100, and the
next heater command simply applies.

### D10. How the Croaster PID board relates to Croaster's esp32s3 board config — **DECISION (open; sdkconfig part resolved 2026-10-27)**

Resolved part (Shawn, 2026-10-27: "kill ours, keep his"): the Croaster PID board's
`esp32s3-sdkconfig.defaults` is deleted, and every Croaster PID env sets
`board_build.sdkconfig_path = ../esp32s3/esp32s3-sdkconfig.defaults`. The ini
question below is still open.

Shawn asked (2026-10-26) whether the Croaster PID board's `platformio.ini` could `extends = core`,
or otherwise reuse what `boards/esp32s3/platformio.ini` already says. This is a discussion
item. Nothing in the build has changed; the Croaster PID board is still option A.

**First, what "core" is.** Croaster core (`croaster/`) is a **library**, not an ini section.
It has a `library.json`, and each board pulls it in with
`lib_deps = Croaster=symlink://../../croaster`. PlatformIO's `extends` only works between
ini sections (`[env:x]` or a plain `[section]`), so `extends = core` isn't something
PlatformIO can do. The Croaster PID board already gets every line of core code through that same
symlink. The only things two boards could share at the ini level are build *settings*:
board json, platform, framework, flash size, upload and monitor options, and common flags.

**What Croaster PID and his esp32s3 actually have in common:** the ESP32-S3 chip, 16 MB flash,
`ARDUINO_USB_CDC_ON_BOOT=1`, `BOARD_HAS_PSRAM`, the upload and monitor speeds, the partition
table (`default_16MB.csv`) and the sdkconfig file (since 2026-10-27 Croaster PID points at
`../esp32s3/esp32s3-sdkconfig.defaults`, his copy; its own copy is deleted).
**What differs:** the board json (his is PlatformIO's stock `esp32-s3-devkitc-1` for the
ES3C28P; ours is `esp32-s3-devkitm1-n16r8.json` for the Lonely Binary N16R8), lib_deps (his
has ILI9341 + LVGL; ours has Adafruit SSD1306 + CroasterControl), build_flags (his
`LV_CONF_PATH` points into his `src/ui`; ours does `-include src/config.h` and adds
`ARDUINO_USB_MODE=1`), PSRAM mode (`dio_opi` vs `qio_opi`), and the platform (his is unpinned
`espressif32` and needs Arduino-ESP32 2.x; ours is `@ 8.0.0`, Arduino 3.3.12).

Correction 2026-10-26: an earlier version of this section listed the custom board json and
`boards_dir = ..` as shared facts. Those came from Shawn's pre-project test edits, not from
upstream (see "Stock boards and upstream"). His board needs no `boards_dir`. The
throwaway B and C builds below used that edited ini. The A/B/C trade-offs still hold, but a
C base can no longer carry `board` or `platform`, because the two boards differ on both.

| | How | Pros | Cons |
|---|---|---|---|
| **A** (today) | Standalone `boards/esp32s3-pid/platformio.ini` | The AGENTS.md model: each board owns its config, and an edit to one board can't break another. No change to his files. | About 10 chip-level lines are duplicated. A fix to his esp32s3 ini (e.g. upload flags) has to be copied by hand. |
| **B** | The Croaster PID ini has `[platformio] extra_configs = ../esp32s3/platformio.ini` plus `[env:esp32s3-pid] extends = env:esp32s3`, and overrides lib_deps, build_flags, memory_type, sdkconfig_path, partitions and platform | No edit to his files. Reuses his env literally. | Most of his env has to be overridden, so very little is actually reused. His `[env]` section merges into our project too: if we leave out our own lib_deps we silently inherit his (ILI9341, LVGL, ...). His `env:esp32s3` and `env:esp32s3-debug` show up in our project's env list. Any later edit he makes to `env:esp32s3` silently changes our build. It also couples us to his file layout, and his unpinned platform needs Arduino-ESP32 2.x while Croaster PID needs 3.x, so the platform has to be overridden too. |
| **C** | Shared chip-level base `boards/common/esp32s3-base.ini` with an `[esp32s3_base]` section (board, framework, flash size, upload/monitor, PSRAM/USB flags). Both boards do `extra_configs = ../common/esp32s3-base.ini` and `extends = esp32s3_base`, and each keeps its own lib_deps, flags, PSRAM mode and sdkconfig. | Shares only what is truly the same chip. Nothing leaks between boards: no `[env]` merge and no foreign envs. Each board's diffs stay visible in its own ini. Fits Croaster's "board owns its config" rule, with the chip facts in one place. `boards/common` is a library dir without a `platformio.ini`, so `build_all.sh` doesn't pick up the base ini as a board. | Needs a small edit to his `boards/esp32s3/platformio.ini` (extra_configs, extends, and `${esp32s3_base.build_flags}`), which upstream has to accept. An edit to the base affects both boards, though that's intended. The platform pin has to be decided: either the base pins it for everyone, or each board pins its own. |
| **D** | Croaster PID as a second env inside `boards/esp32s3`, separated with `build_src_filter` | One ini and one folder for the "esp32s3 chip". | Awkward: `src_dir`, `include`, `test` and `config.h` are per *project*, so two firmwares end up sharing one `src/` tree, filtered file by file. `-include src/config.h` and his `src/ui` collide. It breaks the AGENTS.md "one board = one folder + config" model and Croaster's env naming (`<board>`, `<board>-debug`). |

**Throwaway test builds (2026-10-26, espressif32 @ 8.0.0; test folders deleted afterwards):**

- **B builds.** A copy of the Croaster PID board with `extra_configs = ../esp32s3/platformio.ini` and
  `extends = env:esp32s3` plus the overrides above built with SUCCESS: RAM 62,332 B,
  flash 1,773,207 B, byte-for-byte the same sizes as standalone A.
  `pio project config` showed sections `env`, `env:esp32s3` and `env:esp32s3-debug` from his
  file inside our project. In a variant without our own lib_deps, the resolved Croaster PID env
  inherited his `[env]` lib_deps (Croaster, WebSockets, ArduinoJson, Adafruit ILI9341,
  MAX6675, WiFiManager, lvgl). That confirms the silent-leak risk.
- **C builds, both sides.** The test used a base ini with `[esp32s3_base]`. The Croaster PID copy with
  `extends = esp32s3_base` gave SUCCESS: RAM 62,332 B, flash 1,773,207 B (same as A).
  The copy of *his* esp32s3 board (as edited before this project) with
  `extends = esp32s3_base` (plus `boards_dir = ..`) also built `-e esp32s3`: SUCCESS, RAM 105,184 B, flash 2,518,962 B. Comparing his resolved
  env against the original, the only differences were build_flags order (same flags;
  `-W no-deprecated-declarations` normalized to `-Wno-...`) and the platform pin, because
  the test base pinned `@ 8.0.0`. If he wants to stay unpinned, leave `platform` out of the
  base and set it per board.

**Recommendation: C**, when Shawn is ready and upstream is OK with the small edit to the
esp32s3 ini. Until then, stay on **A**, which is what's in the tree now and is fully
reversible. C keeps the one useful property Shawn is after (the "esp32s3" facts live in one
place) without B's hidden coupling. Avoid B: it looks like reuse but needs nearly every
option overridden, and it makes our build depend on edits to his file.
**Reversible:** yes. Moving A→C only touches ini files.

### D11. Two MAX31855 implementations — resolved 2026-10-27: one decoder, in `croaster-control`
Shawn: the MAX31855 code is ours, written for this project, and belongs in
`croaster-control`, not in Croaster's library. Done:
- Removed from `croaster/`: `CroasterMAX31855.{h,cpp}`, `CroasterThermocoupleType`,
  the third `CroasterCore` constructor argument, `makeThermocouple`, the
  `library.json` header entry and the MAX31855 mentions in its description and
  keywords. `croaster/` now builds its two `SmoothThermocouple(MAX6675_Thermocouple)`
  exactly as upstream `5445a4d`.
- The monitor build (no heater, `CPID_HEATER_NONE`) builds the same
  `hw/Thermocouples` as the control build and wraps it in
  `app/MonitorProbes`, two `Thermocouple`s passed to the sensor-injection
  constructor. No controller is built or started. Both `CPID_TC_MAX31855` and
  `CPID_TC_MAX6675` work.
- Behaviour change in the monitor build: readings go through `ProbeChannel`
  (4-sample average, NaN after 3 bad reads) instead of Croaster's
  `SmoothThermocouple`, the same filter the control build uses.
- 2026-10-28: the monitor build is now its own board, `boards/esp32s3-monitor`
  (`CMON_TC_MAX31855` or `CMON_TC_MAX6675`); the no-heater option is gone
  from Croaster PID (D26).

Analysis before the decision (2026-10-26). There were two: `croaster/src/CroasterMAX31855.*` (added by us, bit-banged,
decodes inline, used only by the monitor build (no heater, `CPID_HEATER_NONE`) and by
any stock board that picks it) and `croaster-control/src/core/max31855.h`
(pure, host-tested decoder) read by `croaster-control/src/hw/thermocouples.cpp`
on hardware SPI. They decoded the same 32-bit frame twice.

Adafruit_MAX31855 was weighed against the in-house decoder: keep ours. One
32-bit read gives temperature, cold junction and fault bits together, where
Adafruit's `readCelsius()` and `readInternal()` each read the chip again. Ours
is host-tested and needs no Adafruit BusIO.
1. Keep both (until 2026-10-27). Core stays independent of `croaster-control`; the decode is
   written twice.
2. One decoder in core: move `max31855::decode` into `croaster/src`
   (e.g. `CroasterMAX31855::decode(uint32_t)`), have `CroasterMAX31855` and
   `hw/thermocouples` both call it, and point the host tests at it. One copy;
   `croaster-control` then includes a core header.
3. Drop `CroasterMAX31855` and the monitor build (no heater). Core goes back to
   MAX6675-only, removing one of our core changes; a temperature-only board
   then uses Croaster's stock MAX6675 boards.
4. Use Adafruit_MAX31855 in core. Smallest new code, but two reads per sample
   and a new dependency.
Recommendation: 2, after Shawn says whether core should offer MAX31855 to
other boards at all (if not, 3).
Chosen: 3's core change (core back to MAX6675 only), with the monitor build
kept and moved onto `croaster-control`.

### D12. Croaster PID screen uses Croaster's SSD1306 display — **DECISION (new 2026-10-26)**
The Croaster PID board had its own `RoasterOledDisplay` (U8g2), a second SSD1306 class next to
Croaster's `boards/common` one.
1. **Croaster's `CroasterDisplaySSD1306` plus `setHeaderText(fn)` (implemented).**
   The header shows heater %, fan % and mode, or the trip, alternating with the
   IP. One display class; drops U8g2.
2. Keep the U8g2 class. Supports SH1106 1.3" modules too, but duplicates
   Croaster's display.
3. Subclass `CroasterDisplaySSD1306` in the board. Needs its draw methods made
   virtual or protected, a bigger core edit than a hook.
4. No status on screen (plain Croaster screen). No core change; heater, fan and
   trip are visible only in Artisan.
Lost with 1: SH1106 modules and address 0x3D. Shawn's module is a 0x3C
SSD1306, so nothing he has changes. Reversible.

### D13. Firmware updates over Croaster's OTA only — **DECISION (new 2026-10-26)**
ArduinoOTA (espota) came from yaeger. Croaster already updates over WebSocket
and BLE (`OTA_BEGIN:<size>`, then binary chunks; the ICRM app uses it), and the
control task turns the heater off while `Update.isRunning()`. The
ArduinoOTA heater trip and the comms watchdog were separate code; they only
sat next to each other in `ControlPlugin`.
1. **Croaster's OTA only (implemented).** No espota env, no OTA password
   macro. Side effect: ArduinoOTA was also what advertised `croaster-flex.local`
   over mDNS, so the `.aset` host is now Croaster's AP address `192.168.4.1`;
   on home WiFi the user types the IP from the screen, as Croaster's README says.
2. Keep ArduinoOTA for `pio run -t upload` over WiFi. Handy for development,
   but a second update path with its own password.
3. Croaster's OTA plus `ESPmDNS` in the board's `main.cpp` (about 3 lines) so
   `croaster-flex.local` keeps working. A small new system Croaster doesn't
   have.
4. Croaster's OTA plus an mDNS option in core's WiFi setup, for every board.
   Most useful, but a core feature change for upstream to accept.
Recommendation: 1; take 3 if typing the IP into Artisan gets annoying.

### D14. Config wizard for `config.h` — **DECISION (new 2026-10-26, design only)**
Goal: someone builds the Croaster PID board without reading `config.h`: they answer questions
(probe chip, heater, fan driver, display, pins) and get a valid `config.h`.
`ConfigCheck.h` stays the authority: whatever the wizard writes must still
pass it at compile time.
1. **Standalone Python script `tools/cpid-config.py` (recommended).** Asks
   questions in the terminal (or reads answers from a file), validates them
   with the same rules as `ConfigCheck.h` (reserved pins, duplicates, mains
   requires the dimmer), and writes `src/config.h` from a template. Run once,
   commit the result. No PlatformIO coupling; works from any shell.
2. PlatformIO `extra_scripts = pre:` that generates `config.h` from a
   `cpid.ini` or JSON answers file on every build. The answers file becomes the
   master and `config.h` a build product, which reverses D2 (the header is no
   longer the file you edit) and hides errors in build logs.
3. JSON/ini answers file turned into a header by a small generator, run by
   hand. Like 1 with a file instead of questions; good for keeping several
   machines' configs side by side.
4. A web page (static HTML/JS in `tools/`) that renders `config.h` for
   copy-paste. Friendliest; a second implementation of the validation, in
   JavaScript.
Recommendation: 1, with 3's answers file as its non-interactive mode, so one
script serves both. Feasible: the template is today's `config.h` with
placeholders; the validation table is the `ConfigCheck.h` rules (pin list,
`unique()`, mains/dimmer). Ticket T25.

### D15. `MAX6675_Thermocouple` stays a core dependency — **DECISION (new 2026-10-26)**
Shawn asked to remove the MAX6675 library. Core needs it to compile:
`croaster/library.json` lists it, and `CroasterCore.h` includes
`MAX6675_Thermocouple.h`, `SmoothThermocouple.h` and `Thermocouple.h`. The
`Thermocouple` base class that `RoasterProbe` implements comes from it.
1. **Leave it to core (implemented).** Removed from the Croaster PID `lib_deps`;
   PlatformIO still installs it through `croaster/library.json`. No core change.
2. Define a `CroasterThermocouple` interface in core and include the MAX6675
   library only where a MAX6675 is built. Removes
   the dependency from control builds; touches every core sensor path.
3. Vendor the ~100 lines of the MAX6675 library into core. No external
   dependency, but upstream fixes stop flowing in.
Recommendation: 1. The library costs nothing in a control build beyond
compile time (the linker drops unused code).
Update 2026-10-26: still option 1, but the dependency is now Shawn's fork at
`v2.0.3` (D21).

### D16. Temperature unit — **DECISION (new 2026-10-26)**
Shawn wants °F on the screen and in Artisan. Croaster already has a unit:
`CroasterCore::changeTemperatureUnit()` converts BT, ET and RoR, the screen
prints the unit, and `getArtisanData` reports it in `"temp"`. The control
plugin already converts setpoints in and SV/ambient out with
`toCelsius`/`fromCelsius`, and the controller caps and trips in °C.
1. **Boot in a configured unit (implemented).** `CMON_TEMP_UNIT` in `config.h`
   (default "F"); `main.cpp` calls `core.changeTemperatureUnit()` in `setup()`.
   No core change. The `.aset` ships in °F mode with `channel_modes` 2 (°F).
   Since 2026-10-29 (D27) a `tempUnit` command is saved in NVS and applied at
   the next boot; `CMON_TEMP_UNIT` is the unit until one has been saved.
2. Keep the firmware in °C and let Artisan convert (`channel_modes` 1, Artisan
   in °F). Artisan shows °F, but the OLED stays in °C, and Artisan sends
   setpoints in °F, which a °C firmware reads as °C.
3. Persist `tempUnit` in NVS from core. Affects every board. Done instead
   outside core (D27): `app/unit_store` saves it from an `onJsonCommand`
   callback, on our two boards only.
Artisan's mode and the firmware unit must match: Artisan sends slider and
button temperatures in its own mode, and the firmware reads them in its unit.
The header line now says "Fan" ("H50 Fan60 PID") so the fan % isn't read as a
°F value.

### D17. Visible autotune status — resolved 2026-10-26: telemetry and OLED (option 3)
Shawn chose option 3. The controller keeps an `AutotuneStatus` (idle,
running, done, failed, aborted, need setpoint, need probe), a reason and a
counter that bumps on every update, so a repeated refusal counts as a change.
`core/autotune_status.h` writes the text in the display unit:
`autotune` in `getArtisanData` and `getControl` ("failed: timeout",
"aborted: comms", "need setpoint >= 212 F"), and a 20-character OLED form
("AT: SV<212F", "AT fail: small swing") that replaces the header for 10 s;
a trip line takes precedence. The field keeps the last status, so a refusal
stays visible in telemetry. Aborts name the cause when there is one (`stop`,
a trip name, `setpoint changed`). The gate and exit behaviour are unchanged.

Original options:

Autotune refuses to start below a 100 °C (212 °F) setpoint or without a valid
probe, and later ends as done, failed (timeout, over temperature, sensor fault,
oscillation too small) or aborted. The reason is only in `getControl`'s
`autotune` field, so from Artisan a refused start looks like nothing happened.
1. **`autotune` field in `getArtisanData`** (e.g. `"running"`, `"done"`,
   `"need setpoint >= 100 C (212 F)"`). Visible in Artisan's serial log and any
   client; Artisan has no way to show an arbitrary field on screen. ~40 bytes
   per sample.
2. **OLED header** shows the status for ~10 s after it changes ("AT: SV<212F",
   "AT done", "AT timeout"). Visible at the roaster; 20 characters is tight.
3. 1 and 2 together.
4. Map rejection to a trip-like code in `trip` (e.g. `"autotune"`). Reuses an
   existing field, but mixes a refused command with safety trips.
Recommendation: 3. **Implemented.**

### D18. PID input and default gains — resolved 2026-10-26: BT by default (option 2)
Shawn chose option 2. `CPID_PID_INPUT` in `config.h` (`CPID_PID_INPUT_BT`)
is the first-boot default; `{"command":{"pidInput":"ET"}}` still switches at
runtime and saves to NVS. yaeger's gains stay as defaults, with a `config.h`
note that they were chosen for ET and need an autotune on BT.

NVS: the input is stored (key `in`) only when a `pidInput` command (or,
before D8, a flat `Target`) was received. A board that never got one follows
the new default. A board that did keeps its saved input, because that was an
explicit choice; the firmware doesn't overwrite it. Telemetry `Target` and
`getControl` `pidInput` show the active input; `{"command":{"pidInput":"BT"}}`
sets BT and saves it.

Without a BT probe, PID mode trips `sensor` after 2 s and autotune refuses to
start ("valid probe"). Use `{"command":{"pidInput":"ET"}}` until BT is fitted.

Analysis before the decision:
The firmware PID follows `Target` (ET by default, as in yaeger, saved in NVS;
`{"command":{"pidInput":"BT"}}` changes it). Default gains are yaeger's
`Kp 1, Ki 0.1, Kd 0.01` (`CPID_PID_KP/KI/KD`), in % heater per °C, per °C·s
and %·s per °C. They are the controller's live gains, not leftover fields.
They are gentle: a 10 °C error asks for 10 % heater plus 1 % per second from
the integral, so the output moves slowly (the 0.5 %/s decay seen on the bench
is the integral at work). Autotune replaces them.
1. Keep ET, autotune on the real heater. ET responds within
   seconds of a heater change, so the loop is stable; BT targets are reached
   by choosing ET setpoints (profiles or Artisan alarms).
2. **Default to BT (implemented).** Matches roast thinking, but BT lags the heater by tens of
   seconds and sits in the bean mass, so a single PID on BT overshoots and
   oscillates unless tuned very softly.
3. Cascade: an outer BT loop sets the ET setpoint of the inner ET loop. Best
   tracking of BT, but new code and two sets of gains.
4. Raise the defaults (e.g. Kp 4, Ki 0.05, Kd 0) so an untuned controller is
   less sluggish. Guesswork until measured on the hardware.
Recommendation was: 1 now, run autotune on ET once mains and the heater are
connected; consider 3 if BT tracking matters after real roasts.

### D19. Cooldown fan default — resolved 2026-10-26: 65 % after stop, 100 % after a trip (option 3)
Shawn chose option 3. `cooldownFan` is now two settings, `stop` (65 %) and
`trip` (100 %): defaults `CPID_COOLDOWN_FAN_STOP`/`CPID_COOLDOWN_FAN_TRIP`,
runtime `{"command":{"cooldownFan":{"stop":N,"trip":N}}}` (a plain number
still sets `stop`; yaeger's `cooldownFanSpeed` is `stop`), NVS keys `cool`
and `coolTrip`, and `getControl` reports both. The controller applies the trip
speed once when any trip starts (comms, sensor, no mains, OTA), so a fan
command while tripped still works; over-temperature always runs the fan at
100 %. A board that already stored `cool` in NVS keeps that stop value.

Before: `CPID_COOLDOWN_FAN` (100 %) was the fan after `stop` and after a comms trip.
yaeger stores 65 % for its cooldown preference but uses 100 % on disconnect.
1. 100 %. Fastest cooling; can blow light beans out of a fluid-bed
   chamber.
2. 65 % (yaeger's stored default). Gentler; slower cooling.
3. **65 % after `stop`, 100 % after a trip (implemented).** Two settings.
Either way it is a runtime setting (`{"command":{"cooldownFan":N}}`, NVS).
Recommendation was: Shawn picks by testing on the roaster.

### D20. Roast rules and profiles — **DECISION (new 2026-10-26, open)**
Hard requirement: the PID runs on the ESP32; Artisan only sends setpoints and
commands. Example rule: heater 100 % from the start with a BT target of
383 °F; at 90 % of it, heater 65 % and fan from 20 to 50 %.
1. **Artisan alarms.** Conditions on BT/ET/extra curves, time since CHARGE, or
   another alarm ("If Alarm"); actions move a slider (which sends its WebSocket
   command) or press an event button. The example is two alarms
   (BT > 345 °F → Heater slider 65 and Fan slider 50). No firmware change;
   lives in the `.aset`/alarm file. Needs Artisan running; the comms watchdog
   covers a crash.
2. **Artisan background replay.** Load a previous roast as background and turn
   on event playback by time or by BT. Repeats a good roast's slider moves.
   No firmware change.
3. **Firmware profile (exists, from yaeger).** `{"command":{"profile":[...]}}`
   then `startProfile`: setpoint (on the PID input) and fan by elapsed time,
   with ramps. Runs without Artisan (watchdog exempt). Time-based only; no
   "when BT reaches X" rules. Croaster itself has no profiles.
4. Firmware rule steps: extend profile steps with triggers (BT above X) and
   actions (heater %, fan %, setpoint, mode). Standalone and conditional, but
   new code, a new JSON format and a UI to write it.
Artisan's ramp/soak and "follow background" drive Artisan's own software PID,
which can't set our firmware's setpoint over WebSocket; they don't fit the
requirement.
Recommendation: 1 now (no code), 2 once a good roast exists, 4 only if roasts
must run without a laptop.

### D21. `MAX6675_Thermocouple` fixes — resolved 2026-10-26: maintained fork (option 3)
The upstream library (yuriisalimov 2.0.2, last release 2019) had three faults
that reach Croaster's MAX6675 boards and the monitor build (no heater):
- `SmoothThermocouple` latched NAN. One open-probe reading was blended into
  the average, and because the "no average yet" check was `data == 0`, the
  average never reseeded. The probe read `N/A` until reboot.
- `SmoothThermocouple::MIN_SMOOTHING_FACTOR` had no out-of-line definition,
  so -O0 builds failed to link. Croaster carried
  `croaster/src/SmoothThermocoupleFix.cpp` for this.
- `MAX6675_Thermocouple::readCelsius()` waited 1 ms per SCK half period, so
  each read blocked for about 33 ms (two probes: about 66 ms every 250 ms).

Options:
1. Keep upstream and work around it in core. The link fix already lived in
   core; the NAN latch and the read time would need core code around a
   library we don't control.
2. A smoothing class in core that replaces `SmoothThermocouple`. Fixes the
   latch, but not the read time, and duplicates library code in core.
3. **Fork the library and fix it there (chosen by Shawn).**
   <https://github.com/shawns1234/MAX6675_Thermocouple>, tag `v2.0.3`:
   - the average reseeds after a NAN;
   - a smoothed value of 0 no longer restarts the average;
   - the factor clamp no longer odr-uses the constant;
   - the waits are 1 us, citing the MAX6675 datasheet's 100 ns timing
     figures. The bits read are unchanged.
   The fork has host tests (`test/host`).

Core changes: `croaster/library.json` depends on
`https://github.com/shawns1234/MAX6675_Thermocouple.git#v2.0.3`; the stock
boards (`esp32s3`, `esp32c3`, `esp8266`) no longer list the registry
package; `SmoothThermocoupleFix.cpp` is removed. Never list
`yuriisalimov/MAX6675_Thermocouple` next to the fork: PlatformIO installs both
and the link fails with multiple definitions.

Behaviour change on MAX6675 boards: unplugging a probe shows `N/A` and
plugging it back in shows the new temperature on the next read (250 ms),
without a ramp from the old value. The Croaster PID control build is unaffected (its
probes go through `RoasterProbe` and `ProbeChannel`, not `SmoothThermocouple`).

### D22. Correction offsets reach control — resolved 2026-10-26: core getters (option 1)
Croaster adds `correctionBt`/`correctionEt` to every temperature it shows. The
offsets are stored as numbers in the current display unit, and clients send
setpoints on that corrected scale. The control task reads the probes itself,
so before this change PID and autotune ran on uncorrected °C. With
`correctionBt` +10 in °F mode, PID held the uncorrected BT at SV, and the
display read SV + 10 °F.

Options (Shawn: the PID must never control on the wrong scale):
1. **Core getters (chosen).** `CroasterCore::correctionBtCelsius()` and
   `correctionEtCelsius()` return the offsets as °C differences (÷ 1.8 in
   °F). `ControlPlugin::loop()` polls them and sends `SetCorrection` when
   either changes. Two additive const getters; stock boards never call them.
2. Parse `correctionBt`/`correctionEt` again in the plugin (`onJsonCommand`).
   No core change, but it duplicates core's parsing and unit rules, needs
   `tempUnit` parsed as well, and breaks silently if core changes how it
   stores corrections.
3. Core hands corrected values to the controller. Core would have to know
   about a controller, and the SPI bus would no longer belong to the control
   task alone (D7).

Rules, in `core/probe_correction.h` (host-tested) and the controller:
- PID, autotune and the setpoint compare use corrected values.
- The snapshot, and so `RoasterProbe`, stays uncorrected: CroasterCore adds
  the offset itself, and correcting both would count it twice.
- Safety limits use the higher of the raw and the corrected reading.
  Over-temperature trips when either is above `CPID_OVER_TEMP_C`, so a
  negative offset cannot hide a hot probe, and a positive one trips early.
- Autotune's abort above `CPID_OVER_TEMP_C` compares the corrected input; the
  raw over-temperature trip ends autotune as well, so the higher reading wins
  here too.
- `CPID_MAX_SETPOINT_C` caps the setpoint, which is on the corrected scale.
  With a negative offset the probe can run above the cap; the raw
  over-temperature trip still applies.
- The autotune gate (setpoint >= 100 °C / 212 °F) is on the setpoint, so it
  matches what the client sent. The cooldown fan has no temperature
  thresholds. `Amb` is the cold junction, which Croaster doesn't correct.
- A correction change during autotune aborts it ("aborted: correction
  changed", OLED "AT abort: offset"), since the step would distort the
  measured cycle. In PID or Profile mode the PID restarts bumpless, so the
  derivative term doesn't kick.
- The plugin treats a non-finite offset as 0. Corrections are still not saved
  by core and reset at reboot.
- A `tempUnit` change rescales both offsets (Shawn, 2026-10-27). Before, the
  stored number kept its value, so +10 °F became +10 °C (18 °F) after
  `{"command":{"tempUnit":"C"}}`. `CroasterCore::changeTemperatureUnit` now
  converts it as a difference: C→F ×9/5, F→C ×5/9, C↔K unchanged. The same
  unit again changes nothing. The °C getters return the same value before and
  after (to rounding), so the controller sees no correction change.

Stock boards: `esp32c3` and `esp8266` release builds have the same RAM and
flash use and the same warnings with the getters added (2026-10-26).

### D23. Autotune and control review — fixes made, proposals **DECISION (new 2026-10-26, open)**
Review of autotune end to end and of the control code. Research and sources:
`docs/research/autotune.md`; usage: board README, "Autotune".

Verdict: the autotune is a real Åström–Hägglund relay test with consistent
units (heater request % per °C, seconds). yaeger's vendored AutoTunePID
toggles every 2 s whatever the PV does, so its gains are the same on every
roaster. Ours is new code, not yaeger's. On a simulated roaster it found Ku 17–28 % under the
analytic value and Pu within 1–16 %, finishing in 4–14 min. Low Ku means
softer gains. Results don't change with ±1 °C of noise. Every abort path was
traced. After success the gains are live and saved to NVS, and the board goes
to Manual, heater 0, fan 30 %.

Fixed (clear bugs, host-tested):
- **`pidInput` change didn't stop autotune or reset the PID.** The PV steps
  (BT → ET can be 50 °C), so autotune measured the step as part of a cycle,
  and PID took a derivative kick. Now it follows D22's correction rule:
  autotune aborts ("aborted: input changed", OLED "AT abort: input"), and PID
  and Profile restart bumpless. Same input again is a no-op. `onPvStep()` in
  `control_types.h`; `test_pv_step_stops_autotune_and_resets_pid`.
- **Manual heat with both probes dead didn't trip.** The `SensorFault`
  comment and `how.md` said it did, but the "both probes" clause sat behind
  `mode != Manual` and could never fire. With no readable probe the
  over-temperature trip is blind. Now Manual trips `sensor` after 2 s when
  the heater is on and neither probe reads; heater 0 never trips.
  `sensorFault()`; `test_sensor_fault_rules`.
- **False `nomains` trip.** `FanDimmer::mainsDetected()` read the timer, then
  the ISR's last-crossing time. A crossing in between made the age wrap to
  about 2^64 µs: "no mains", a latched trip while heating with
  `CPID_REQUIRE_MAINS 1`. It now reads the crossing first (until two reads
  agree, against a torn 64-bit read) and treats a crossing after "now" as
  recent. `core/zero_cross.h`; `test_zero_cross_age`.
- **Mains grace period** (Shawn, 2026-10-27: "2-3 s is fine"). With
  `CPID_REQUIRE_MAINS 1`, one missed 100 ms check while heating tripped
  `nomains`. It now trips only after the zero-cross has been missing for
  `CPID_MAINS_GRACE_MS` (default 2000, 0..10000; 0 is the old behavior)
  without a break while the heater is on, like the 2 s sensor-fault delay.
  Zero-cross back or heater 0 resets the count. Cost: with AC present but the
  zero-cross signal lost (Z-C wire off, dimmer fault), the heater runs up to
  2 s before the trip. With no AC the heater cannot run at all. `core/mains_watch.h`;
  `test_mains_dropout_under_grace_does_not_trip`,
  `test_mains_absent_past_grace_trips`, `test_mains_heater_off_resets_grace`.

Proposals, highest priority first (no code changed):
1. **Profile end holds the last setpoint forever, unattended.** The
   controller ignores `ProfileTarget::finished`, and Profile mode is exempt
   from the comms watchdog. Options: (a) at the end, stop (heater 0, stop
   cooldown fan); (b) hold, but re-arm the watchdog once finished; (c) keep.
   Recommendation: a.
2. **Relay swing.** The relay is 0/100 %, so PV swings ±10 to ±45 °C around
   SV on the simulated plants. Options: (a) keep, and pick SV with room
   below 275 °C; (b) relay ± a step around a bias (br3ttb `oStep`); (c)
   Marlin's moving bias, which also evens out unequal heat and cool halves.
   Recommendation: a until a hardware run shows the swing; c if it is too
   big.
3. **No convergence check.** It averages 4 cycles after skipping one. If
   the roaster is still heat-soaking, they drift. Option: finish only when
   the last 3 amplitudes and periods agree within 10–20 % (as br3ttb does), or
   else keep going until the timeout. Recommendation: add; it is small.
4. **Fan change during autotune** changed the plant but didn't abort.
   **Done 2026-10-27 (Shawn): abort.** A fan command that changes the speed
   ("aborted: fan changed", OLED "AT abort: fan") or any heater command
   ("aborted: heater changed", OLED "AT abort: heater") ends autotune with the
   heater off, as SV, mode, `pidInput` and correction changes already did.
   The heater value is dropped; the fan value applies. The start rule (fan
   below 30 % → 60 %) is the controller's own write, not a command, so it
   never aborts. `outputCommandStopsAutotune()` in `control_types.h`;
   `test_output_command_stops_autotune`, `test_autotune_status_text`.
5. **Diagnostics.** Ku and Pu are computed but not reported. Option: add
   `autotuneKu`/`autotuneTu` to `getControl`. Recommendation: yes; it lets
   us compare runs and rules without re-tuning.
6. **Rule choice.** Fixed Tyreus–Luyben PID. Option:
   `{"command":{"autotune":{"rule":"TL"|"ZN"|"PI"}}}`. TL PI suits BT's lag;
   ZN overshoots. Recommendation: wait for one hardware run (needs 5).
7. **Repeated `autotune` restarts** a running test silently. Option: ignore it
   while running. Recommendation: ignore.
8. **After success** the board goes to Manual, heater 0, as yaeger does.
   Option: continue in PID at the tuned SV. Recommendation: keep; the
   operator chooses.
9. **Ku formula.** Ours is 4d/(π√(a² − h²)). The textbook form 4d/(πa)
   (br3ttb, Marlin) differs by under 2 % once a ≥ 5h, and the simulated
   swings were 10h to 45h. Recommendation: keep.
10. **Safety commands can be dropped.** `submit()` doesn't block, and a full
    queue (16) drops the command; the plugin ignores that for `stop`. The
    queue drains every 10 ms, so it is unlikely. Option: send Stop,
    EmergencyOff and Trip to the front and retry on failure. Low priority.
11. **Controller is not host-testable** (FreeRTOS, hardware outputs inline),
    so each fix needs a pure helper (`startOutputOnModeChange`, `onPvStep`,
    `sensorFault`). Option: put the heater, fan, probes and clock behind
    small interfaces and keep FreeRTOS in `run()`/`snapshot()` only, so
    `apply()`/`controlStep()` compile on the host and trips and aborts get
    end-to-end tests. Cost: one refactor of `controller.*`. Recommendation:
    do it before more control features.
12. **Debug env warning** (existing): `-D CORE_DEBUG_LEVEL=3` redefines the
    platform's value, one warning per file. Option: `build_unflags =
    -DCORE_DEBUG_LEVEL=0`. Low priority.

### D24. Autotune before PID and profiles — resolved 2026-10-27 (Shawn)
The default gains are yaeger's, chosen for ET on other hardware (D18), so a
PID or profile run on a fresh board controls on guesses. Shawn made autotune
mandatory, behind a switch.

- `CPID_REQUIRE_AUTOTUNE` (`config.h`, default 1; ConfigCheck: 0 or 1). With
  1 and no tuned gains, `mode: "pid"` and `startProfile` are refused: the mode
  stays, `autotune` reads `autotune required`, OLED `Autotune first` for 10 s.
  A loaded profile stays loaded. Manual and autotune are always allowed.
  With 0, nothing changes: the default gains are usable.
- Tuned is stored as NVS key `tuned` (namespace `control`), written by
  `ConfigStore::saveGains`. Gains with no `tuned` key but a saved `kp` (boards
  from before this change) count as tuned.
- **Manually set gains count as tuned** (decision). The `pid` command and
  yaeger's `setPreferences` gains are a deliberate choice by the operator,
  for example gains from an earlier board or another tool; requiring an
  autotune on top would only force a run they don't need. A failed or
  aborted autotune saves nothing and doesn't count.
- The controller holds the flag in the snapshot (`gainsTuned`; `getControl`
  `tuned`). It is set at boot from NVS, on `SetGains`, and when autotune
  completes. The gate itself is `modeNeedsTuning()` in `control_types.h`
  (host test `test_autotune_required_gate`).
- Profile behavior is otherwise unchanged.

### D25. Board renamed to Croaster PID — resolved 2026-10-28 (Shawn)
"flex" said nothing about what the board does. Shawn renamed it to
**Croaster PID**:

- Folder `boards/esp32s3-pid/`, envs `esp32s3-pid`, `esp32s3-pid-debug`,
  `esp32s3-pid-pioarduino`; board id `"esp32s3-pid"` (`CPID_BOARD_NAME`).
- Board macros `FLEX_*` → `CPID_*`. The PID-controller macros keep the
  controller name after the prefix: `CPID_PID_INPUT`, `CPID_PID_KP`,
  `CPID_PID_KI`, `CPID_PID_KD`. 2026-10-28: settings every board has are
  now `CMON_` (`CMON_BOARD_NAME` and others, D26).
- Artisan settings `artisan/croaster-pid.aset`; planned wizard
  `tools/cpid-config.py` with answers file `cpid.ini` (D14).
- Naming rule (see "Names"): "Croaster PID" in prose, `CPID_`/`esp32s3-pid`
  in code; a bare "PID"/`pid` always means the controller. Controller names
  are unchanged: `Pid`, `PidGains`, `PidInput`, `Mode::Pid`, JSON mode `pid`,
  the `pid` and `pidInput` commands, NVS namespace `control` and its keys.
- Earlier records (D5, the "Names" table, done tickets) keep the old name
  with a dated note.
- Open: the name of the monitor build (`CPID_HEATER_NONE`, no heater or fan).
  It is not a PID board, so docs call it "monitor build (no heater)" until
  Shawn decides. Resolved 2026-10-28: its own board, `esp32s3-monitor`
  ("Croaster monitor", D26).

### D26. Croaster monitor board and the `CMON_`/`CPID_` prefix split — resolved 2026-10-28 (Shawn)
The monitor build (`CPID_HEATER_NONE` + `CPID_FAN_NONE` in Croaster PID) was
not a PID board, and D25 left its name open. Shawn's decisions:

- New board `boards/esp32s3-monitor` ("Croaster monitor"): envs
  `esp32s3-monitor` and `esp32s3-monitor-debug` (the pair `build_all.sh` and
  `upload_monitor.sh` expect), board id `"esp32s3-monitor"`, ICRM enum entry
  `esp32s3Monitor` if one is added. Same hardware and build settings as
  Croaster PID: N16R8 board json, `platformio/espressif32 @ 8.0.0`,
  `default_16MB.csv`, `qio_opi`,
  `sdkconfig_path = ../esp32s3/esp32s3-sdkconfig.defaults`, SSD1306, MAX31855
  or MAX6675.
- Monitor only: no heater, fan, mains input or control task. It reuses
  `croaster-control` `hw/thermocouples` and `app/MonitorProbes` (moved from
  the `CPID_HEATER_NONE` branch of Croaster PID's `main.cpp`). The rest of
  the library compiles but is not linked: no `Controller`, `ControlPlugin`,
  `HeaterOutput` or fan symbols in the ELF.
- Files: `platformio.ini`, `src/main.cpp`, `src/config.h`,
  `src/ConfigCheck.h`, `README.md`. `ConfigCheck.h` copied the N16R8 pin
  rules from Croaster PID's. 2026-10-28 (T46): the chip and module rules
  moved to `croaster-control/src/hw/esp32s3_opi_pins.h`, which both boards
  already link; UART0 (43, 44) and the RGB LED (48) stay in each board's
  `ConfigCheck.h` as dev-board wiring.
- Croaster PID always has a heater and a fan: `CPID_HEATER_NONE`,
  `CPID_FAN_NONE` and the monitor branch of `main.cpp` are gone.
  `ConfigCheck.h` requires `CPID_HEATER_SSR` and `CPID_FAN_AC_DIMMER` or
  `CPID_FAN_PWM`.
- Host tests stay in `boards/esp32s3-pid/test`: they test
  `croaster-control/src/core`, mostly control logic.
- Kept as is, not decided: the monitor's OLED header is Croaster's standard
  header (no header text set); no mDNS name (as on every board, D13).

Our config macros say which part of the firmware they belong to (rule in
"Names"). Applies to names we created only.

- `CMON_` (settings every Croaster board has; same names on every board of
  ours): `CMON_BOARD_NAME`, `CMON_DUMMY_DATA`, `CMON_TEMP_UNIT`,
  `CMON_THERMOCOUPLE`, `CMON_TC_MAX31855`, `CMON_TC_MAX6675`,
  `CMON_PIN_TC_SCK`, `CMON_PIN_TC_MISO`, `CMON_PIN_TC_CS_ET`,
  `CMON_PIN_TC_CS_BT`, `CMON_DISPLAY`, `CMON_DISPLAY_NONE`,
  `CMON_DISPLAY_SSD1306`, `CMON_PIN_I2C_SDA`, `CMON_PIN_I2C_SCL`,
  `CMON_PIN_LED`, `CMON_LED_ON_LEVEL`, `CMON_ENABLE_BLE`, `CMON_WIFI_SLEEP`
  (T53).
- `CPID_` (control add-on, unchanged): heater, fan, mains, `CPID_PID_*`,
  `CPID_REQUIRE_AUTOTUNE`, `CPID_OVER_TEMP_C`, `CPID_MAX_SETPOINT_C`,
  `CPID_MIN_FAN_WITH_HEAT`, `CPID_COOLDOWN_*`, `CPID_WATCHDOG_*`,
  `CPID_CONTROL_CORE`.
- Prose: "Croaster PID" and "Croaster monitor" for the boards; a bare "PID"
  is the controller.
- Not renamed: the upstream author's code and names (`croaster/`,
  `boards/common`, `boards/esp32s3`, `boards/esp32c3`, `boards/esp8266`,
  `CROASTER_HAS_BLE`). `config.h` keeps
  `#define CROASTER_HAS_BLE CMON_ENABLE_BLE`.
- Firmware unchanged: the Croaster PID build is the same size
  (RAM 56 844 B, flash 1 724 947 B).

### D27. Browser UI served by the board — resolved 2026-10-29 (Shawn)
Artisan needs a computer with Artisan installed. Shawn wants a page any
phone or laptop on the network can open, hosted on the board itself. A
history database, recipes and sync come later as a separate app on a
computer; they are not part of this.

Decisions:

- **Library `croaster-webui/` (`CroasterWebUi`)**, linked by both boards.
  `CMON_ENABLE_WEBUI` (default 1) and `CMON_WEBUI_PORT` (default 80) in each
  `config.h`; `ConfigCheck.h` rejects port 81 (Croaster's WebSocket) and
  ports above 32767 (the server's control socket uses port + 32768).
- **Server:** ESP-IDF `esp_http_server` in its own task, started from
  `WebUiServer::loop()` once the station is connected and WiFiManager's
  portal is closed (the portal uses port 80; it counts as open while the
  radio is in AP mode). It stops if the portal opens again. Croaster's core
  and command handler are untouched.
- **The page is a WebSocket client** of Croaster on port 81, like Artisan: it
  polls `getArtisanData` every second, `getControl` every 5 s and
  `getDeviceInfo` every 30 s, and sends the same JSON commands. Croaster's
  limit of 5 WebSocket clients applies.
- **Assets:** readable sources in `web/`, gzipped at build time by
  `embed_assets.py` into `src/webui_assets.h` (committed; regenerated only
  when the source hash changes; gzip level 9, no timestamp). Budget 100 KB
  gzipped, enforced by the build; v1 is about 20 KB. No CDN, no web fonts,
  plain JavaScript, a hand-drawn SVG chart.
- **One page for both boards.** `/info.json` reports the board id, the
  WebSocket port and whether to show Controls; the monitor ships the controls
  script too (a few KB) but hides it.
- **Sections**, in tab order: Monitoring (every value the board reports,
  nothing else; the landing view), Live (tiles, chart, heater and fan strips,
  event buttons, roast clock, CSV), Controls (Croaster PID only: every command, replies and errors
  inline, STOP always visible, usable while a roast records). Device RoR by default; an optional 30 s
  least-squares RoR computed in the browser.
- **Roast timer and events live in the browser** (`localStorage`), not on the
  board. Croaster's own roast timer is not used.
- **Recording starts only on a user action** (fix, 2026-10-29). v1 added
  every polled sample to the session from the moment the page connected, so
  opening Live started a preheat clock and a recording. The session now has
  `active` (and `endedAt`): Start roast or Charge sets it, STOP (or End roast
  on the monitor) clears it, and only an active session takes samples or
  events. Connecting, reconnecting, reloading and switching views change
  nothing. On load, a saved active session whose last sample is more than
  10 min old is ended at that sample; sessions saved before this change have
  no `active` and load as ended.
- **Start, events and views** (2026-10-29, Shawn, T52). The page used to
  open on Live, which showed the roast clock and, before T51, a running
  preheat, so opening the URL looked like starting a roast. Monitoring is
  now the first tab and the landing view; a `#live` or `#controls` hash
  opens that view, and the last view is not restored otherwise. A Start
  button next to STOP begins the recording (samples
  from then on, so the preheat is on the chart); it sends nothing to the
  board, because the board has no roast state to start and a heater or fan
  change belongs to the user's own controls. Charge no longer starts a
  recording. Event buttons are all disabled until Start, then only Charge;
  after Charge: Turning point, Dry end, FC start and Drop; FC end and SC
  start need FC start, SC end needs SC start; an event cannot be marked
  once a later one is, and nothing after Drop. The roast clock counts from
  Charge; samples before it keep negative times on the chart and in the CSV.
  After Drop an End roast button ends the recording without a command (the
  monitor has End roast in the top bar in place of STOP). STOP or End roast
  returns the page to Idle with Start enabled; the last roast stays on the
  chart until the next Start. Controls are not gated by Start: preheating
  is done from there. The board does nothing when a client connects (it
  only counts clients), and the polls (`getArtisanData`, `getControl`,
  `getDeviceInfo`) read state without changing it.
- **Connection** (2026-10-29, Shawn, T53). Shawn saw the link dot flap
  between yellow (connecting) and red (closed) with two browsers open and
  the board's USB port held by a program on his PC. Findings:
  - Croaster's WebSocket server writes with blocking calls (WebSockets
    `write()` loops up to `WEBSOCKETS_TCP_TIMEOUT`, 5 s; each
    `NetworkClient::write` waits up to 10 x 1 s in `select`) and has no
    heartbeat, so one client that stops reading freezes `loop()` for
    seconds per message until TCP gives up on it.
  - `Serial` is the native USB port (HWCDC). After a host opens it and stops
    reading, each line waits about 2 s (20 x 100 ms), and Croaster logs every
    command and broadcast. Fixed in the board `setup()` with
    `Serial.setTxTimeoutMs(0)`.
  - The page dropped a silent socket after 5 s and waited for the close
    handshake before reconnecting, so a slow board saw extra sockets.
  - The page built `ws://<host>:81/` even when loaded over https, which the
    browser refuses.
  - WiFi modem sleep was on (Arduino's default, `WIFI_PS_MIN_MODEM`):
    pings from Shawn's PC averaged 148 ms (40–275 ms). `CMON_WIFI_SLEEP 0`
    (default) calls `WiFi.setSleep(false)` after `app.begin()`. ESP-IDF 4.x
    aborted when modem sleep was off with BLE on; ESP-IDF 5.x (this build:
    5.5) allows it. Cost: the radio stays on, a few tens of mA.
  - RSSI is already in the telemetry on Croaster PID (`wifiStrength`, from
    `ControlPlugin`); the Connection card shows it. The monitor does not
    report it.

  Page changes: `Link.socketUrl` (https: `wss://<host>/ws` through the
  proxy), 10 s silence and 8 s handshake limits with an immediate new
  socket, the socket closed while hidden when `Link.mayPause` allows it (no
  recording, heater 0, Manual; the watchdog acts only while heating), and a
  Connection card with the uptime-based restart count. Not done, for
  approval: a heartbeat (`server.enableHeartbeat(...)`) in
  `CroasterWebSocketManager::begin()` (upstream code), the client count and
  reset reason in `getDeviceInfo`, a same-origin `/ws` on the port 80
  server bridging to the command handler (non-blocking sends, no proxy
  rule, but a second command path and cross-task locking), and a settings
  reset command.
- **STOP** (fix, 2026-10-29) is enabled only while a roast is recording and
  always asks first. It sends `{"command":"stop"}`, which `ControlPlugin`
  registers through `onCommand`: the controller sets the heater to 0 and the
  mode to Manual (ending PID, Profile or Autotune), and the plugin then sets
  the fan to `cooldownFan.stop` (65 % by default). The command has no reply,
  so the page checks the next readings for `Mode` Manual and `BurnerVal` 0
  and logs the result, or a warning after 5 s. It is not sent, and the roast
  is not ended, while the link is down. A heater-off action is still needed
  when no roast is recording (heater set from Controls or Artisan), so
  Controls > Safety keeps it as Heater off: the same command behind a
  confirm, not tied to the recording. The firmware has no temperature-based
  cooldown rule (fan until ET is low, minimum or maximum time): the fan stays
  at the cooldown speed until it is changed.
- **Confirm step** for `erase`, `restartesp`, `dummyToggle` and `wifiConnect`.
- **No password.** Same exposure as the WebSocket on port 81, which already
  accepts any command from the network. Documented in both READMEs and the
  library README.
- **Not in v1:** OTA upload from the page (use Croaster's OTA), a password,
  a roast timer on the board.
- **Temperature unit saved (D16 option 3, outside core).** The page changes
  the unit with `tempUnit`; it now survives a reboot. `app/unit_store`
  reads NVS namespace `croaster`, key `unit`, at boot (falls back to
  `CMON_TEMP_UNIT`) and saves from an `onJsonCommand("tempUnit")` callback
  only when the board applied the request and the unit changed.
  `core/temp_unit.h` holds the decision and has host tests.
- **Hidden tabs and the comms watchdog.** The WebSocket and poll timer run
  in a dedicated Web Worker, whose timers browsers do not slow down in
  hidden tabs; the page polls at once when it becomes visible. Remaining
  cases where polls stop and the watchdog trips, as intended: tab or browser
  closed, computer asleep or lid closed, WiFi lost, iOS/iPadOS Safari in the
  background.

Loop timing. The HTTP task runs on core 0 at priority 3 on Croaster PID
(core opposite `CPID_CONTROL_CORE`), so it never competes with the control
task (core 1, `configMAX_PRIORITIES - 3`) or the Arduino loop (core 1,
priority 1). It only copies constant flash data to a socket; a full page load
is about 20 KB once, then `304` replies. The cost on the loop task is
Croaster's WebSocket handling of one more client: one `getArtisanData` per
second, one `getControl` every 5 s, one `getDeviceInfo` every 30 s, the same
traffic as a second Artisan. NVS is written only when the unit changes.
Sockets: the HTTP server uses at most 4 client sockets plus its listen and
control sockets; with the WebSocket server's 5 clients that stays under
LwIP's 16.

Cost (2026-10-29, against the build before D27):

| Env | RAM | Flash |
|:---|---:|---:|
| `esp32s3-pid` | 56 908 B (+64) | 1 782 163 B (+57 216) |
| `esp32s3-pid-debug` | 56 676 B (+56) | 1 489 555 B (+52 996) |
| `esp32s3-pid-pioarduino` | 59 336 B (+64) | 1 467 047 B (+52 712) |
| `esp32s3-monitor` | 54 708 B (+56) | 1 721 779 B (+63 088) |
| `esp32s3-monitor-debug` | 54 484 B (+56) | 1 454 847 B (+55 416) |

About 20 KB of that is the page; the rest is `esp_http_server` (and, on the
monitor, the `Preferences` code for the saved unit). The server's
task stack and sockets are allocated at run time, after WiFi connects.

Appearance (2026-10-29, Shawn, T50). The page gets colour schemes and style
options, set in the browser:

- **`web/theme.js`** (a UMD module like `roast.js`, host-tested) defines the
  colour tokens, the presets, the style options with their ranges, and the
  parse, validate, migrate, contrast and CSS functions. It loads in `<head>`
  and applies the saved scheme before first paint, so a light scheme never
  flashes dark.
- **Tokens** are CSS custom properties on `<html>`: `bg`, `panel`,
  `panel-2`, `line`, `grid`, `text`, `muted`, `accent`, `on-accent`, `stop`,
  `on-danger`, `ok`, `warn`, `bt`, `et`, `ror`, `ror-et`, `heat`, `fan`,
  `sv`, `event`. `on-accent` and `on-danger` replace the fixed white text on
  accent and danger buttons; `ok`, `ror-et`, `sv` and `event` are new so
  that the link dot, the ET RoR line, the setpoint line (now drawn) and the
  event markers no longer borrow another series' colour.
- **Style options** become custom properties or attributes on `<html>`:
  radius, text scale (rem sizes against a scaled root size), density
  (`data-density`), chart line width, numeral font, card opacity, glow, and
  reduced motion (`data-motion`, plus `prefers-reduced-motion`).
- **Presets:** `neon` (default), `copper` (the v1 dark theme), `light` (v1
  light), `contrast`, `graphite`. The default is a neon green dark theme:
  near-black background, translucent cards with a backdrop blur, a soft
  accent glow on active controls and on the large numerals, faint grid.
  BT and the accent are green; ET is sky blue, BT RoR amber, ET RoR pink,
  heater red-orange, fan violet, so no series reads as a second green. Each
  preset passes 4.5:1 on its text pairs; the host tests enforce that. In
  `copper` and `light`, text on the accent is dark instead of white, the only
  change from v1, because white on those accents was below 4.5:1.
- **Storage:** `croaster.theme` holds
  `{"format":"croaster-theme","v":1,"preset","colors","style"}`. The v1
  values migrate: `"dark"` (the v1 default) to `neon`, `"light"` to `light`,
  anything unreadable to `neon`. Missing values fill from the named preset,
  numbers clamp to their range, unknown keys drop; import uses the same
  rules and names what it skipped.
- **Contrast** follows WCAG 2 relative luminance. Pairs checked: text on
  background, cards and buttons; secondary text on cards; text on accent;
  text on danger. Cards are compared as drawn, blended over the background
  at the card opacity. Warnings never block a change.
- **Live:** the chart's strokes and line width are CSS properties, so a
  change restyles it without a redraw or a reload.
- **Cost:** the gzipped page grows from 20 121 B to 28 270 B (+8 149 B,
  budget 100 KB); flash grows by 8 204 to 8 208 B per env; RAM is unchanged.

| Env | RAM | Flash |
|:---|---:|---:|
| `esp32s3-pid` | 56 908 B (+0) | 1 790 371 B (+8 208) |
| `esp32s3-pid-debug` | 56 676 B (+0) | 1 497 759 B (+8 204) |
| `esp32s3-pid-pioarduino` | 59 336 B (+0) | 1 475 251 B (+8 204) |
| `esp32s3-monitor` | 54 708 B (+0) | 1 729 987 B (+8 208) |
| `esp32s3-monitor-debug` | 54 484 B (+0) | 1 463 051 B (+8 204) |

### D28. Profile library and recipe steps on the board — resolved 2026-10-29 (Shawn)
Profiles lived only in the page's local storage and were lost with the
browser. Shawn asked for profiles kept on the board, and then for steps the
board applies by itself during a roast (for example: fan to 50 % when BT
reaches 389 °F), with heater power and fan per step, the way most roast
recipes are written.

Decisions (Shawn approved 1 to 3 on 2026-10-29):

- **Storage:** LittleFS on the stock `spiffs` partition of `default_16MB.csv`
  (3.4 MB, unused), mounted at `/littlefs`, files in `/profiles/<id>.json`.
  No partition change, so OTA keeps working and keeps the files (it writes
  only the app slots). A blank partition is formatted once on first boot; a
  partition that mounts is never formatted. `CPID_ENABLE_LIBRARY` (default 1)
  in `config.h`, checked in `ConfigCheck.h`. Croaster PID only.
- **Writes are atomic:** `<id>.json.tmp`, then rename. A `.tmp` found at boot
  is an interrupted save and is removed; the `.json` beside it is the last
  good copy.
- **Limits:** 50 profiles, 8 KB per file, name up to 40 characters, notes up
  to 1 KB, id 1 to 32 of `a-z 0-9 _ -` (made from the name, `-2`, `-3` when
  taken).
- **Format v1:** `{"version":1,"name","notes","unit","steps":[...],"recipe":{...}}`.
  `steps` is the existing timed setpoint schedule (Profile mode, unchanged);
  `recipe` is the new trigger list. Either may be missing, not both. Keys the
  firmware does not know are kept as written; a higher `version` is refused.
  Temperatures are in `unit` and converted when loaded.
- **Busy rule:** save and delete are refused while the heater is above 0 or
  a profile or autotune runs (`busy` with the reason); load is refused while
  a recipe runs. The page disables those buttons and shows why.
- **Recipe step:** one trigger and 1 to 4 actions; up to 32 steps.
  - Triggers: time since CHARGE, BT, ET, RoR (BT) with `gte`, `lte` or
    `near` (± `tol`; default 5 s, 2 °C, 1 °C/min; at most 60 s or 20), or an
    event (CHARGE, TP, DRY_END, FC_START, FC_END, SC_START, SC_END, DROP).
  - A step fires once, on a false-to-true change: a condition already true
    when the step is armed must go false first. Unknown readings (a faulted
    probe) count as neither. Time triggers are false before CHARGE.
  - Steps are armed in order; a step with `anyOrder` is armed from the start.
    Steps that are true in the same tick fire together, in order.
  - Actions: `power` (switches to Manual with that heater %), `fan`,
    `setpoint` (switches to PID; optional `ramp` seconds moves the target
    from BT at the step, linear or shaped, D31, D32), `hold` (stops a ramp where it is),
    `cool` (the same as STOP: heater 0, cooldown fan, recipe ends), `end`
    (recipe ends, outputs stay), `note` (logged only).
  - **(1) The editor's default step is heater % plus fan % (open loop).**
    **(2) Setpoint and setpoint ramps stay available per step.** Power and
    setpoint steps mix in one recipe: each switches the mode it needs.
  - Values outside 0..100 %, 0..350 °C, -50..100 °C/min or 0..3600 s are
    refused when the recipe is loaded, not clamped silently; the controller
    clamps again on submit.
- **Where it runs:** `RecipePlugin` ticks the runner (`core/recipe.h`) at
  4 Hz on the loop task, from the corrected readings the user sees. Every
  action goes through `Controller::submit`, so the interlocks, setpoint caps,
  autotune gate and trips apply as for a user command. The board keeps its
  own roast clock: the page sends `roastEvent` when an event is marked or
  undone.
- **Aborts:** a trip (including the comms watchdog), STOP, emergency off and
  OTA abort the recipe and log the reason. Manual changes by the user are
  allowed and do not abort. The watchdog still applies, so a recipe in
  Manual or PID needs a connected client, as before (only Profile mode is
  exempt).
- **Guided mode:** the board does not apply a fired step; the page shows it
  with Set now and Skip. A prompt not answered before the next one fires is
  logged as missed.
- **Status:** each data payload carries `recipe:{state,next,prompt,fired,seq,follow}`
  once a recipe is loaded; the page fetches `getRecipe` (fired list with
  times, events, the recipe itself) only when `seq` changes.
- **(3) Behind schedule (page):** on Live, when a setpoint or ramp step is
  being followed, BT is more than a threshold below the setpoint (default
  15 °F or 8 °C, set in Controls > Recipe) and the heater has been at 100 %
  for 20 s, the page warns that the roaster cannot follow the curve.
- **Not done:** a heads-up shortly before a step fires; STOP in the top bar
  stays greyed unless a roast is recording (Abort recipe and Controls >
  Heater off work at any time).
- **Cost (`esp32s3-pid`):** RAM 59 912 B (+3 016), flash 1 952 691 B
  (+159 536, of which LittleFS about 110 000). Page 47 166 B gzipped
  (+13 141, budget 100 KB). Monitor: RAM +0, flash +13 204 (page only).
  Mount time is logged at boot (`# Library: LittleFS on "spiffs" in N ms`).

### D29. Multi-point autotune — resolved 2026-10-29 (Shawn)
The relay autotune (D17, D23, `RelayAutotune`) oscillated around the user's
setpoint (at least 212 °F), switched the heater 100/0 % with no bias
correction, averaged 4 cycles with no convergence check, applied
Tyreus–Luyben PID gains and saved them on completion. The page showed only
"running". Shawn asked for a one-click run that picks its own temperatures,
measures at several of them, shows which step it is on, and documents the
method.

- **One command, no setpoint.** `{"command":"autotune"}` runs the
  `CPID_AUTOTUNE_POINT*_C` test points (default 250/300/350 °F), clamped to
  the setpoint and over-temperature limits. First set at 300/350/400 °F,
  lowered the same day (T58): this build bypasses the roaster's own controls,
  thermal cutoff included, so the firmware's limits are the only protection,
  and the run's ceiling (top point + 30 °C overshoot stop) drops from 454 to
  404 °F. Gains from these points still apply through a roast: the
  recommended set is the most cautious of the points, and on the simulated
  drum it is within 5 % of the set measured at 300/350/400 °F. The first
  point's relay bias starts at 30 % (was 50 %) so the first heating half
  does not overshoot 250 °F. The 212 °F setpoint gate is gone
  (`NeedSetpoint` stays in the enum, no longer raised).
- **Per point:** heat at full request, relay with Marlin's bias correction to
  settle, then cycles until the last 3 agree (CV < 10 %, at most 6); Ku, Tu,
  a FOPDT model and gains. Recommended set: lowest Kp, longest Ti, median Td
  over the points marked ok.
- **Rule:** Ziegler–Nichols PI by default (`CPID_AUTOTUNE_RULE`), chosen in
  simulation for ramp tracking with low heater noise; Tyreus–Luyben PI as the
  cautious alternative. No gain scheduling: −10 % IAE in simulation did not
  justify a second control path; per-point gains are reported.
- **Nothing applied until accepted.** Results wait on the board (RAM) until
  `{"command":{"autotune":"apply"}}` (gains to NVS, tuned) or `"discard"`.
  The page opens a results dialog (current vs new gains, Save/Cancel), again
  after a reload while results wait.
- **Safety:** heater 0 on sensor fault, over temperature (below the board's
  own trip), overshoot, no heat-up, no oscillation, a total time limit, STOP,
  trips and the D23 abort commands. A finished run ends in Manual with the
  stop cooldown fan.
- **Progress:** phase, point, cycle and time left in the snapshot every
  control step; status text and OLED show them; `getAutotune` returns the
  full report and a 24-line log; data carries `atTarget`.
- **Recipes:** autotune and a running recipe exclude each other
  (`recipeRunning`, `autotuneRunning`).

Docs: `docs/autotune/` (goal, process, implementation, tuning rules).
New names, for Shawn to confirm: `autotune::Sequence` and its types,
`autotuneproto`, `AutotunePlugin`, `Controller::configureAutotune`,
`autotunePlan`, `autotuneReport`, `ControlPlugin::onAutotuneStart`,
`AutotuneStatus::Pending`/`Discarded`, `Command::Kind::ApplyAutotune`/
`DiscardAutotune`, `Snapshot::autotuneProgress`/`autotunePending`,
`CPID_AUTOTUNE_*`, `getAutotune`, `{"autotune":"apply"|"discard"}`,
`atTarget`, `recipeRunning`, `autotuneRunning`.

### D30. Autotune fan test, feedforward and fan assist **DECISION (new 2026-10-31, open)**
Shawn asked for autotune to measure the fan as well ("fan results - let PID
use fan to shed"), with the focus on what airflow does to the beans. Airflow
carries heat out of the drum by convection: at a fixed heater BT settles
lower with more fan, a fan step dips BT until the PID integrator catches up,
and once the heater is at 0 % the fan is the only way left to take heat out
of beans running ahead of the curve (momentum late in drying and before
first crack).

- **Fan test**, optional, after the PID points (the gains exist by then),
  on by default (`CPID_AUTOTUNE_FAN_TEST`, start dialog checkbox, `fanTest`
  on the start command). At the middle test point the board holds BT with PI
  at fan 30/60/90 % (`CPID_AUTOTUNE_FAN_LEVEL*`) and measures the hold
  heater, the dip and recovery after each step, the fan gain, and a bounded
  heater-off fall per level (`CPID_AUTOTUNE_FAN_SHED_DROP_C`, 30 °C). About
  29 minutes on the simulated drum; 40-minute limit. Method and numbers:
  `docs/autotune/fan-test.md`.
- **Results** show in the existing Save/Cancel dialog and wait in RAM. Save
  stores the gains and a versioned roaster model blob in NVS; a run without
  a fan test keeps the saved one. The model holds two fan channels; only the
  exhaust fan is measured now.
- **A stop or fault during the fan test ends only the fan test**: the gains
  stay pending with the levels measured so far.
- **D23 refined:** only fan changes from outside abort autotune; the fan
  test's own steps do not. Every fan and heater change still goes through the
  controller's limits and interlocks (min fan while heating, trips).
- **Fan feedforward** (`CPID_FAN_FEEDFORWARD`) adds the hold-heater
  difference for the current fan; **ramp feedforward**
  (`CPID_RAMP_FEEDFORWARD`) adds τ·slope/K while the setpoint moves (±40 %).
- **Fan assist** (`CPID_FAN_ASSIST`, page toggle, NVS `fanAs`): in PID and
  Profile, when BT is more than 2 °C above the setpoint and the heater is at
  or near 0 %, the fan rises above the user's or recipe's value, 5 % per °C,
  up to `CPID_FAN_ASSIST_MAX` (20 %), 1 %/s up and 2 %/s down. Never below
  the base, never during cooldown, trips or autotune, off without a saved
  fan test, shown on Live.

To confirm: the fan test length (about 29 min, so a full run is about
66 min); the 30 °C heater-off bound (300 °F down to about 246 °F at most);
keeping the previous fan test when a new run has none; the fan test holds
use the autotune output limit (100 %) while the heater ceiling is
`CPID_HEATER_MAX_PERCENT` (90 %); fan assist on by default.
New names, for Shawn to confirm: `roastermodel` (`RoasterModel`,
`PointModel`, `FanModel`, `FanLevel`, `fromReport`, `merge`, `usable`,
`holdPct`, `fanFeedforward`, `rampFeedforward`, `maxRorCpm`,
`SetpointSlope`, `kMaxFanChannels`, `kMaxRampFfPct`), `fanassist::Assist`
and `Settings`, `autotune::makeFanLevels`, `FanTestResult`,
`FanLevelResult`, `FanTestStatus`, `Phase::FanHold`/`FanShed`,
`Sequence::fanRequest`, `Controller::ModelUse`, `configureModelUse`,
`setRoasterModel`, `roasterModel`, `Command::Kind::SetFanAssist`,
`Snapshot::fanBasePct`/`fanAssistPct`/`fanAssistOn`/`fanTested`,
`ConfigStore::saveModel`/`loadModel`/`saveFanAssist`, the `CPID_AUTOTUNE_FAN_*`,
`CPID_FAN_FEEDFORWARD`, `CPID_RAMP_FEEDFORWARD`, `CPID_FAN_ASSIST*`
settings, NVS keys `model` and `fanAs`, protocol `fanTest`, `fanAssist`,
`FanAssist`, `fanEstimate`, `saved`.

### D31. Recipe ramp shapes, power ramps and charge temperature — new 2026-10-31, open for Shawn
Shawn: "vertical axis mean - rate of climb". A ramp step says how BT should
climb, not only where it ends: a roast is steered by its rate of rise, and a
ramp that is fast early and eases off (a declining RoR) is the usual way to
approach first crack without a flick or a crash.

- **Shape = rate-of-climb curve.** A `setpoint` or `power` action with
  `ramp` > 0 takes `shape`: `linear` (default), `normal`, `front`, `back` or
  `custom` with `curve`, 21 rates 0..100 at 0, 5, ..., 100 % of the ramp
  time. The curve is the relative rate of climb; its normalised area is the
  progress F(u), so the target is `start + (target − start) · F(t/T)` and
  always reaches the target at T. Presets: `linear` constant; `front`
  falling from 100 to 0 (F = 2u − u², steepest at the start); `back` rising
  from 0 to 100 (F = u², steepest at the end); `normal` a bell, peak at mid
  ramp. Peak rate over mean rate: 1, 2.03, 2, 2.
- **Power ramps.** `power` with `ramp` moves the heater from its current %
  to the target over the time, with the same shapes (Manual mode). Power and
  setpoint ramps share one ramp: a later action replaces it, `hold` stops it.
  `fan` refuses `ramp` and `shape` (fan assist and feedforward act on the
  fan; a recipe sets it per step).
- **Charge temperature.** Optional recipe `chargeTemp` (recipe unit,
  0..350 °C): on Start the board switches to PID at that setpoint once, to
  preheat for charge. It is not a followed step, so the behind-schedule
  warning stays off until the first setpoint step.
- **Limits.** Up to 8 custom curves per recipe (stored once in the runner,
  21 bytes each); a curve needs a rate above 0.
- **Feasibility (page).** With a saved roaster model (D30) the editor checks
  each setpoint ramp: the steepest rate needed, (to − from)/T · 60 · peak
  factor, against the model's maximum rate of rise at that temperature and
  fan with the heater at the saved
  model's ceiling, and warns under the step. The start is the
  previous setpoint step, else `chargeTemp`, else the step's BT trigger
  (replaced by D32: the step's BT trigger, else the previous setpoint
  target).
- **Live.** `getRecipe` adds `chargeTemp` and, while a ramp runs, `ramp`
  (`kind`, `from`, `to`, `duration`, `elapsed`, `rates`); Live draws the rest
  of a setpoint ramp as a dashed planned SV and shows the ramp line. The
  behind-schedule warning follows the shaped SV because the board's SV is
  shaped.
- **Editor.** Shape dropdown with a preview; **Edit** opens a curve editor
  (drag points, snap to 5, presets, live path preview, steepest point text),
  sized for a 412 px phone screen and themed.
- **Simulation** (drum model, fan 60 %, 150 → 200 °C over 5 min, ramp and
  fan feedforward, `test_shaped_ramps_on_drum`): BT rate of rise in the
  first/second half of the ramp, against the path's 10/10, 15/5 or 5/15:

  | Shape | RoR 1st / 2nd half °C/min | lag RMS / max °C | overshoot °C | peak need / model max at heater 90 % °C/min |
  |:---|:---|:---|:---|:---|
  | linear | 10.3 / 8.5 | 2.9 / 5.2 | 1.8 | 10 / 28 |
  | normal | 8.8 / 9.2 | 2.5 / 5.2 | 0.3 | 20 / 21 |
  | front | 15.9 / 3.4 | 4.6 / 9.1 | 0.5 | 20 / 28 |
  | back | 4.7 / 13.7 | 1.9 / 4.6 | 5.0 | 20 / 15 |

  `front` produces the declining RoR it asks for; its lag is the dead time
  at the steep start. `back` ends at its fastest rate, so BT carries
  momentum past the target, and here it asks for more than the model allows
  near 200 °C (the editor warns).
- **Cost:** page 62 135 B gzipped (+7 152, new `ramp.js`); recipe runner
  +168 B for the curves. `esp32s3-pid` with D30 and D31: RAM 66 768 B,
  flash 2 037 935 B; monitor flash 1 761 499 B (page only).
- **New names, pending Shawn's confirmation:** `recipe::Shape`,
  `kCurvePoints`, `kMaxCurves`, `parseShape`, `shapeName`, `shapeRates`,
  `rampFraction`, `peakFactor`, `Recipe::chargeC`, `curves`, `curveCount`,
  `rates()`, `Inputs::heaterPct`, `Runner::Ramp`, `Runner::ramp()`,
  `startRamp`; JSON `shape`, `curve`, `chargeTemp`, `getRecipe` `ramp`; page
  `ramp.js` (`RampShape`), `#curve-dialog`, `.sv-plan`.
- **To confirm:** the four presets, the 8-curve limit, preheat not counting
  as a followed step, power ramps allowed for every shape.

### D32. Setpoint ramps start at BT — new 2026-10-31, open for Shawn
Shawn approved: a setpoint ramp in a recipe must start from the current BT,
not from the board's last setpoint. Before this, a ramp after Manual power
steps started from a stale setpoint (0, an old Controls value or the charge
temperature). From 0 the PID saw BT far above the plan and cut the heater
until the plan caught up, so BT lost its momentum at the point of the roast
where it matters most. The workaround was a plain setpoint step before the
first ramp.

- **Start rule** (`recipe::rampStartC`). When a `setpoint` step with `ramp`
  applies, the ramp starts at the PID input the recipe sees (corrected BT
  by default; ET or the higher of the two when `pidInput` says so), whatever
  mode or setpoint came before. The shape is applied over the whole
  BT-to-target change, `from + (target − from) · F(t/T)`, so `front` still
  climbs hard first and eases into the target.
- **BT at or above the target.** No ramp: the step sends the target as a
  plain setpoint step would and the PID holds it. Ramps never go down.
- **PID input unreadable.** The ramp starts at the current setpoint when it
  is above 0 and below the target; otherwise the step sends the target at
  once. Either way the controller's `sensor` trip ends PID within 2 s, since
  PID cannot run without its input. Starting from the old setpoint keeps the
  heater request no higher than the earlier behaviour.
- **Bumpless start.** The ramp's first `SetSetpoint` carries `b = 1`; the
  controller then calls `Pid::reset` at the new setpoint with the present
  heater output and the feedforward, after the switch to PID. The heater
  carries on from its Manual % (or the PID's present output) instead of the
  old restart from 0 % when the setpoint went from 0 to a value. Plain
  setpoint steps, the charge temperature and protocol `setpoint` commands
  send `b = 0` and behave as before. The setpoint slope for ramp
  feedforward treats the jump to BT as a step (over 2 °C) and ignores it.
- **Chained ramps.** Each ramp restarts at BT, so a ramp that BT lagged
  behind starts the next from where the beans are, not from the old plan.
- **Power ramps** are unchanged: they start from the current heater %.
- **Page.** The editor has no BT, so the feasibility check estimates the
  start: the step's BT trigger value, else the previous setpoint step's
  target (`RampShape.rampStartEstimate`); `chargeTemp` no longer counts. A
  ramp whose estimate is at or above its target is not checked. The Live
  dashed plan draws from `getRecipe` `ramp.from`, which is now BT at the
  step, so page and board agree.
- **Simulation** (drum model, fan 60 %, Manual 60 % until BT passes
  150 °C (302 °F), then `front` to 200 °C (392 °F) over 5 min,
  `test_ramp_from_manual_on_drum`): BT RoR 16.5 °C/min (29.7 °F/min) before
  the step. Starting at BT with the bumpless restart, the lowest 30 s RoR in
  the next 2 min is 10.5 °C/min (18.9 °F/min) and the overshoot 0.75 °C
  (1.4 °F). The old start from setpoint 0 drives the heater to 0 and BT falls
  at 32.5 °C/min (58.5 °F/min).
- **Cost:** `esp32s3-pid` RAM 66 768 B (unchanged), flash 2 038 623 B
  (+688); monitor flash 1 761 643 B (+144, page only); page 62 278 B
  gzipped (+143).
- **New names, pending Shawn's confirmation:** `recipe::rampStartC`,
  `recipe::pidInputC`, `Inputs::pidInput`, `Command::SetSetpoint` `b = 1`
  (bumpless restart), `Output::add` third argument, page
  `RampShape.rampStartEstimate`.
- **To confirm:** start at the PID input rather than BT when the PID input
  is ET or MAX; the unreadable-input fallback; no downward ramps.

### D33. Fake readings button shows its state — new 2026-10-31, open for Shawn
Shawn asked for Maintenance > Fake readings to be green while dummy mode is
on, so a board left on fake values is obvious.

- **Source of the state.** Croaster keeps the flag in the private member
  `CroasterCore::useDummyData` and reports it nowhere. A one-line public
  getter `bool dummyDataEnabled() const` returns it unchanged (local change
  to upstream, marked in the header, could be offered upstream; the member
  and `toggleDummyData()` keep their names). Tracking toggles in
  croaster-control instead would miss serial and BLE `dummyToggle` and
  depend on the order of Croaster's command dispatch.
- **Transport.** `ControlPlugin`'s existing `onJsonData` hook adds
  `"dummy": true|false` to every data payload, so `getArtisanData` (polled
  each second) and the broadcasts carry it. No new poll and no change to
  `dummyToggle`, which still replies nothing. Artisan and the ICRM app
  ignore the extra key. Croaster Monitor does not add it: its page has no
  Controls tab, where the button lives.
- **Page.** `Roast.dummyState(data)` gives true, false or null (not
  reported). The page keeps the value from the last data reply and clears it
  when the link drops or reopens, so after a reload or reconnect the button
  shows off until the board's first reply, then the real state within about
  a second. A switch shows within about a second too. Style: `.row-buttons
  button.on`, filled with the theme's `--ok` green, plus `aria-pressed`.
  Monitoring > Telemetry lists the field as "Fake readings".
- **Cost:** `esp32s3-pid` RAM 66 768 B (unchanged), flash 2 039 119 B
  (+496); monitor flash 1 762 027 B (+384, page only); page 62 668 B
  gzipped (+390).
- **New names, pending Shawn's confirmation:** data field `dummy`,
  `CroasterCore::dummyDataEnabled()`, page `dummyState`, CSS class `on`
  on Maintenance buttons, element id `dummy-toggle`.

## Build platform and sdkconfig
- Default: `platformio/espressif32 @ 8.0.0` (Arduino-ESP32 3.3.12, IDF 5.5).
  Arduino builds ignore `board_build.sdkconfig_path`; stock libs already have
  PSRAM malloc on.
- pioarduino 55.03.312 (same Arduino core) builds too; only it can apply
  `custom_sdkconfig` (recompiles IDF libs).
- Croaster PID sets `board_build.sdkconfig_path` like Croaster's esp32s3 board
  (Shawn, 2026-10-26) and, since 2026-10-27, points it at his file:
  `../esp32s3/esp32s3-sdkconfig.defaults` (D10). It has no `custom_sdkconfig`
  env. To apply the file, add
  `custom_sdkconfig = file://../esp32s3/esp32s3-sdkconfig.defaults` to a
  pioarduino env (pioarduino reads a relative `file://` path from the project
  folder); the first build takes about 2 minutes.
- Checked 2026-10-27: no installed platform reads `board_build.sdkconfig_path`.
  espressif32 8.0.0, 6.13.0 and pioarduino 55.03.312 look for
  `board_build.esp-idf.sdkconfig_path`, and only when ESP-IDF itself is
  compiled (`framework = espidf`, or pioarduino's `custom_sdkconfig`). The line
  records which file applies, here and in Croaster's esp32s3 ini; it changes
  nothing in an Arduino build.
- `esp32s3-sdkconfig.defaults` findings (checked in the generated sdkconfig):
  - `MBEDTLS_EXTERNAL_MEM_ALLOC=y`, WiFi/LwIP in PSRAM, NimBLE 1 connection:
    applied.
  - `MBEDTLS_SSL_IN/OUT_CONTENT_LEN=4096` are **not applied**: IDF 5.5 only
    uses them with `CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN=y`; the max stays
    16384. The WSS fix works through PSRAM allocation, not smaller buffers.
  - `MBEDTLS_HARDWARE_AES=n` slows TLS; only useful if a DMA/PSRAM AES issue
    was observed.
  - This firmware uses no TLS (plain `ws://`), so none of it is required.

## Hardware notes driving defaults
See `boards/esp32s3-pid/README.md` (pins, power, probe isolation, IPEX).
- Display **confirmed by Shawn (2026-10-26)**: 0.96" I2C 128×64 SSD1306
  module, address 0x3C. It uses Croaster's shared `CroasterDisplaySSD1306`
  (Adafruit SSD1306). The header line shows heater %, fan % and mode, or the
  trip, alternating with the IP.

## Related docs
`how.md` (walkthrough), `architecture.md` (shape, red flags, open questions),
`control-parity.md` (yaeger vs ours, item by item), `artisan-settings.md`
(every `.aset` setting with its source), `autotune/` (multi-point autotune,
D29).

### D34. Optional fans and box temperature — new 2026-11-02, open for Shawn
Shawn: fan1 is the case fan on the sensors with the ESP as fallback; fan2 is
on the ESP temperature, mounted over the ESP; each with a configurable
turn-on temperature. Wiring: docs/wiring.md.

- **Scroll fan.** Follows the cooldown: stop, emergency off and every safety
  trip start one, so the fan runs after any of them until ET is below the
  cooldown end; with no cooldown it is off. This is the safest reading of
  "off on STOP unless the cooldown is active": the fan only adds cooling.
  Manual on/off is accepted only in Manual mode with the heater off and no
  cooldown, and is dropped when the heater comes on, the mode changes or a
  cooldown starts. Not part of the autotune fan test.
- **FAN1 / FAN2.** On/off at full duty through LEDC (10 kHz, under the
  trigger module's 20 kHz limit), 3 C hysteresis, and on when the reading is
  missing. Box temperature: average of the valid MAX31855 cold-junction
  readings (no NoDevice fault, -40..125 C), else the ESP32-S3 chip
  temperature; MAX6675 boards always use the chip. No upstream change: the
  driver already returns the cold-junction temperature.
- **Settings.** Turn-on temperatures (25..90 C) and the box warning
  (30..100 C, default 50 C) are saved in NVS; the Auto/On/Off overrides are
  not, so a fan forced off does not stay off after a reboot.
- **Names pending confirmation:** `CPID_SCROLL_FAN`, `CPID_FAN1`,
  `CPID_FAN2`, `CPID_PIN_SCROLL_FAN`, `CPID_PIN_FAN1`, `CPID_PIN_FAN2`,
  `CPID_AUX_FAN_PWM_HZ`, `CPID_FAN1_ON_C`, `CPID_FAN2_ON_C`,
  `CPID_BOX_WARN_C`; `AuxFansPlugin`, `core/aux_fans.h`,
  `max31855::boxTemperatureC`, `Snapshot::box`; data object `aux` (`box`,
  `boxSrc`, `chip`, `warn`, `scroll`, `fan1`/`fan2` with `on`, `mode`, `at`);
  command `auxFans` (`scroll`, `fan1`, `fan2`, `fan1On`, `fan2On`,
  `boxWarn`); page ids `tile-box`, `aux-row`, `scroll-fan`, `aux-fan1`,
  `aux-fan2`; NVS namespace `aux` (`f1On`, `f2On`, `boxW`).

### D35. End of roast — new 2026-11-02, open for Shawn
Shawn: Roast End acts like STOP, Drop ends heating, the setpoint resets, and
the save offer opens once, at the end.

- **End roast** (header button, and the one shown after Drop) asks the same
  question as STOP, then sends `stop`: heater off, recipe aborted, Manual,
  cooldown fan (the scroll fan follows the cooldown). Recording ends. With
  no link the recording still ends and the log says STOP was not sent.
- **Drop** (judgement call). In the Behmor the beans cool in the drum, so
  Drop is the end of heating. Drop is marked at once, so its time is exact,
  and then the End roast confirmation opens. Cancel keeps heating and
  recording with Drop marked (Undo last event removes it). It asks instead
  of stopping straight away, so one stray click cannot end a roast.
- **Setpoint.** `stop` sets the setpoint to 0, so a later switch to PID
  starts with the heater off. A recipe Cool step and the safety trips are
  unchanged.
- **Save offer.** At most once per roast (session `saveOffered`): at Drop or
  End roast, else after STOP, else when the cooldown ends. It is not shown
  when the board has no roast storage, so the fallbacks remain.
- **STOP afterwards.** Once the cooldown ends, the board is in Manual with
  the heater off and setpoint 0, so STOP is disabled again (batch B rules).

### D36. Mains trip setting — new 2026-11-02, open for Shawn
Controls > Safety > Mains trip turns the `nomains` trip on or off; saved in
NVS (`control`/`mainsOn`), shown only on AC dimmer builds (`getControl`
`mainsTrip` is absent otherwise). `CPID_REQUIRE_MAINS` is now the default
of that setting and follows `CPID_FAN` (on for the dimmer). Turning it off
asks for a danger confirmation.
