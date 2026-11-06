# Analysis — yaeger + Croaster → one roaster firmware

Companion to [`design.md`](design.md) (architecture + decisions) and
[`tickets.md`](tickets.md) (remaining work). Date: 2026-10-26. The fork
has since added `boards/esp32s3-pid`, `boards/esp32s3-monitor`,
`croaster-control` and `croaster-webui`. This file is the starting
comparison; current layout is `CONTEXT.md` and `docs/how.md`.

## 1. The two repos

### yaeger (shawns1234/yaeger, `legacy/` on branch rework/skeleton)
Single PlatformIO project, ~1 650 lines.

| Module | Role |
|:---|:---|
| `main.cpp` | ESPAsyncWebServer :80, AsyncWebSocket `/ws`, ElegantOTA, LittleFS static web UI, NeoPixel status |
| `CommandLoop.cpp` | WebSocket JSON: Artisan `getData` (id echo), `BurnerVal`/`FanVal`/`Setpoint`/`Mode`/`Target`, `setBurner`/`setFan`/`autotune`/`set/getPreferences` |
| `Control.cpp` | manual / PID heater, fan, autotune (vendored `AutoTunePID`) |
| `pwm.cpp` + `vendor/ESP32_EnhancedPWM.h` | heater PWM 50 Hz, fan PWM 20 kHz (DC fan) |
| `sensor.cpp` | 2 × Adafruit MAX31855, moving average, fault logging |
| `display.cpp` | 16×2 LCD (LiquidCrystal_I2C) showing IP |
| `wifi_setup.cpp` | STA from Preferences, AP fallback |
| `web/` | browser UI served from LittleFS |

Data flow: everything runs in async-server callbacks plus `loop()`; control
state is shared without locks. No safety trips beyond `MAX_HEATER_POWER 90`.

### Croaster (IiemB/Croaster, local branch rework/roaster)
Library `croaster/` + per-board projects `boards/{esp32c3,esp8266,esp32s3}` +
`boards/common` (SSD1306). Monitor only: 2 × MAX6675, RoR, roast timer,
WebSocket :81 (links2004), BLE (ICRM app), OTA over WebSocket/BLE,
WiFiManager portal. See design.md "Croaster's model".

**Author's intent** (README, CONTEXT.md, AGENTS.md, CHANGELOG): a reusable,
display- and pin-agnostic library that any board can consume like an external
project; boards own pins/display/id; CONTEXT.md is the single source of truth;
PlatformIO only; release envs for `build_all.sh`, `-debug` envs for
`upload_monitor.sh`.

**Extension points:** `CroasterPinConfig`, `CroasterDisplay` subclass,
`CroasterApp(core, display, led, level)`, `commands().onCommand()` /
`onJsonCommand()`, `CroasterDeviceIdentity::setBoardName()`,
`CROASTER_HAS_BLE`, plus (added here) the sensor-injection constructor and
`CroasterCore::onJsonData()`.

## 2. Which implementation wins, per function

| Function | Winner | Why |
|:---|:---|:---|
| Project structure, build scripts | Croaster | library/board split, `build_all.sh`, docs discipline |
| WiFi provisioning | Croaster (WiFiManager) | captive portal; yaeger needed creds over WS |
| Network transport | Croaster WebSocket :81 | already speaks Artisan; sync server, no AsyncTCP |
| Bluetooth | Croaster (ICRM app) | yaeger has none |
| OTA | Croaster (WS/BLE, ICRM app) | ElegantOTA needs an HTTP server; ArduinoOTA was yaeger carry-over (D13) |
| RoR | Croaster, as upstream (falling temperature reads 0) | see design.md core changes |
| Thermocouple | new MAX31855 decode (both repos' ideas) | control task owns the bus; fault bits; filter reset |
| Heater | rewritten from yaeger | SSR time-proportional window, not 50 Hz PWM (zero-cross SSR cannot follow 50 Hz PWM) |
| Fan | new AC dimmer driver (+ PWM option from yaeger) | Shawn's fan is mains AC |
| PID, autotune, profiles | yaeger ideas, rewritten pure + tested | host tests, no globals |
| Display | Croaster's shared `CroasterDisplaySSD1306` with a header-text hook | yaeger's 16×2 LCD isn't Shawn's hardware |
| Safety | new | over-temp, sensor fault, no mains, OTA, comms-lost trips |
| Web UI | deferred (ticket T14) | Artisan + ICRM cover control |

## 3. Dependencies

| Lib | Used by | Note |
|:---|:---|:---|
| links2004/WebSockets 2.7.3 | Croaster | |
| bblanchon/ArduinoJson 7.4.3 | all | |
| tzapu/WiFiManager 2.0.17 | Croaster | `-Wformat` warnings only |
| MAX6675_Thermocouple 2.0.3 (shawns1234 fork, git tag `v2.0.3`) | Croaster | provides the `Thermocouple` interface we implement; fork fixes in design D21 |
| adafruit/Adafruit SSD1306 2.5.17 | boards/common | the Croaster PID screen |
| Arduino BLE (NimBLE in Arduino 3.3) | Croaster | |
Dropped from yaeger: ESPAsyncWebServer/AsyncTCP, ElegantOTA, WebSerial,
Adafruit MAX31855, nexgen_filter, Moving-Average lib, LiquidCrystal_I2C,
NeoPixel (Arduino 3.x drives the WS2812 via `digitalWrite(LED_BUILTIN)`).

## 4. Pin maps

| Signal | yaeger devkit | yaeger S3-mini | Croaster esp32c3 | **new default (Lonely Binary)** |
|:---|:---:|:---:|:---:|:---:|
| Heater | 3 (strap!) | 18 | — | 4 |
| Fan | 8 (PWM) | 21 | — | 5 (dimmer gate) |
| Zero-cross | — | — | — | 6 |
| TC SCK / MISO | 6 / 5 | 33 / 35 (PSRAM!) | 4 / 5 | 12 / 13 |
| CS ET / BT | 15 / 16 | 37 / 39 | 6 / 7 | 10 / 11 |
| I2C SDA / SCL | 41 / 42 | 41 / 42 | 8 / 9 | 8 / 9 |
| LED | 48 (NeoPixel) | | | 48 (`LED_BUILTIN`) |

## 5. Artisan protocol
- Transport: WebSocket `ws://<host>:81/` (Croaster). yaeger used `:80/ws`.
- Poll: Croaster's `{"command":"getArtisanData","id":N}` →
  `{"id":N,"name":..,"data":{"BT","ET","DBT","DET","timer","temp",...,"BurnerVal","FanVal","SV","Mode","trip"}}`.
- Control: yaeger's flat `{"BurnerVal":v}` is not routable by Croaster
  (no `command` key), so the `.aset` sends `{"command":{"burner":v}}` /
  `{"command":{"fan":v}}`. Full list: board README.
- `boards/esp32s3-pid/artisan/croaster-pid.aset` = yaeger's settings
  with port 81, empty path, host `192.168.4.1`, new slider/button
  commands, personal paths cleared.

## 6. Bluetooth
yaeger: none. Croaster: BLE GATT server for the ICRM Android app (same JSON
commands, OTA over BLE). Kept, on by default (`CMON_ENABLE_BLE`); control
commands work over BLE too because they register in the shared command handler.
Artisan has no BLE path for this protocol; use WiFi.

## 7. Master config (`boards/esp32s3-pid/src/config.h`)
- Only `#define`s; force-included into every unit so it also sets Croaster's
  `CROASTER_HAS_BLE`.
- Selects: converter (MAX31855/MAX6675), heater (NONE/SSR), fan
  (NONE/AC_DIMMER/PWM), display (NONE/SSD1306),
  pins, BLE, PID defaults, limits, control core.
- `ConfigCheck.h` (included first by `main.cpp`): `#error` for invalid enums
  and combinations (heater without fan, `REQUIRE_MAINS` without dimmer, mains
  not 50/60); `static_assert` for reserved/duplicate pins, dimmer gate < 32,
  ranges. Verified: duplicate pin, PSRAM pin 36, PWM fan + REQUIRE_MAINS all
  fail with the intended message; monitor config builds.
- `main.cpp` only builds what is selected (`#if`), so unused drivers are not
  linked.
- 2026-10-28: heater is SSR only and fan AC dimmer or PWM only; the monitor
  config is now its own board, `boards/esp32s3-monitor`, and shared settings
  are `CMON_` (D26).

## 8. Shawn's 7 questions

1. **esp_http_server vs async (ESPAsyncWebServer)?** Neither is needed now:
   Croaster's links2004 WebSocket server on :81 already carries Artisan and the
   app, and it runs in `loop()` without AsyncTCP's extra task and its
   use-after-free history. When the web UI comes back (T14), use
   `esp_http_server` (in IDF, maintained, works on any core) rather than
   reintroducing AsyncTCP.
2. **ElegantOTA vs plain OTA?** Plain. Croaster already does OTA over WebSocket
   and BLE (ICRM app); ElegantOTA would require an HTTP server and a
   third-party UI. The control task turns the heater off while an update runs
   (`Update.isRunning()`).
3. **Moving average?** Keep a short one (4 samples) on raw readings for control
   and reset it after 3 consecutive faults so a stale average never feeds the
   PID. RoR smoothing stays Croaster's; heavier smoothing belongs in Artisan, not
   in the control path (it adds lag the PID sees).
4. **EnhancedPWM?** Drop it. A zero-cross SSR cannot follow 50 Hz PWM; heat is
   a 1 s time-proportional window in software, the AC fan uses phase control
   (zero-cross ISR + GPTimer), and a DC fan uses plain LEDC (`ledcAttach`) at
   25 kHz.
5. **Autotune quality?** The vendored AutoTunePID mixed tuning and control and
   had no output limits or timeout. Replaced by a relay (Åström–Hägglund)
   autotune with hysteresis, warm-up + 4 measured cycles, 30 min timeout, an over-temp abort and Tyreus–Luyben gains by default (Ziegler–Nichols selectable),
   host-tested on a simulated plant; still validate on the real roaster (T8)
   and treat results as a starting point.
6. **WiFi best practice?** WiFiManager portal for first setup (no creds in
   code), STA with auto-reconnect, IP shown on the OLED, WiFi on core 0
   while control runs on core 1 at higher priority, and attach the IPEX antenna
   before power-up. Never let the network path gate the heater: the control
   task keeps running and the comms watchdog turns heat off if the client is gone.
7. **LittleFS?** Not needed now: settings are a few numbers in NVS
   (`Preferences`, wear-levelled, atomic per key). Use LittleFS only when the web
   UI or stored profiles return; the 16 MB partition table already has room.

## 9. Build results (2026-10-26, America/Chicago)

| Build | Result | RAM | Flash |
|:---|:---|---:|---:|
| `esp32s3-flex` (espressif32 8.0.0) | ok | 62 316 B (19.0 %) | 1 765 159 B (26.9 %) |
| `esp32s3-flex-pioarduino` (55.03.312) | ok | 62 656 B | 1 425 327 B |
| `esp32s3-flex-pioarduino-sdkconfig` | ok (clean, 2 min) | 62 448 B | 1 431 815 B |
| variant MAX6675 + PWM + SH1106, BLE off | ok | 60 008 B | 1 451 014 B |
| monitor (no heater/fan, Croaster SSD1306) | ok | 55 716 B | 1 669 131 B |
| negative config tests (3) | fail as intended | | |
| `pio test -e native` | 15/15 pass | | |
| `boards/esp32c3` with core changes (8.0.0) | ok | 44 680 B | 1 741 489 B (91.6 %) |
| `./build_all.sh esp32s3-flex` | ok | | `builds/Croaster_esp32s3-flex_0.62.bin` |

Platform note: 8.0.0 and pioarduino link the same Arduino core; pioarduino's
images are ~340 KB smaller (different prebuilt IDF lib config). Both are fine.
Unpinned `platform = espressif32` (esp32c3/esp32s3 boards) resolves to whatever
is installed; pin it (T13).

## 10. Hardware findings (defaults come from these)
- **Board:** Lonely Binary ESP32-S3 Gold N16R8, IPEX (external antenna module;
  no firmware setting). Octal PSRAM uses GPIO35–37, which are on the header.
  WS2812 on 48. Two USB-C: native (19/20) and USB-UART (43/44).
- **Fan dimmer (B071X19VL1):** RobotDyn 1-ch, BTA16-600B, Z-C + PSM pins,
  3.3/5 V logic → 3V3 VCC. Phase control suits universal/shaded-pole motors.
- **OLED (B09C5K91H7):** Hosyond 0.96" SSD1306 128×64 I2C, yellow top 16 rows.
- **B0873WTGS5:** PerfectPrime TL1918SL K probe, ungrounded, 3.2 × 300 mm.
- **Inkbird kit (B08Y8GX1WT):** SSR-40DA (3–32 VDC in, zero-cross) + K probe
  (junction type unknown) + heatsink; kit PID unused.
- **MAX31855:** faults on grounded probes (SCG/SCV) → use ungrounded probes.

## 11. Open questions for Shawn
See design.md **DECISION** items (D1, D3, D5, D6) and the final report.
