# Architecture — roaster control on Croaster

Written with the `/architect` method, applied to what's already built: ground
it (see `how.md`), set out the shape and the alternatives, screen it against
the design red flags, and list open decisions. Decision details with options
are in `design.md` (D1–D15).

## Problem

Shawn's roaster needs yaeger's level of control (firmware PID on the ESP32,
autotune, heater/fan commands, Artisan) on top of Croaster's platform (WiFi
portal, WebSocket, BLE/ICRM app, OTA, display), without breaking Croaster's
rules:
- `croaster/` stays a pin- and display-agnostic thermometer library.
- Board config lives in the board.
- `CONTEXT.md` is the source of truth.

Constraints found while grounding:
- Croaster's loop can stall for seconds (WiFi portal, OTA, BLE). A heater
  must never wait on it.
- Croaster owns the WebSocket/BLE command parser. We may only add hooks.
- Artisan matches replies by `id`, polls with one request, and can only *send*
  commands (it can't run our PID).

## Usage (caller's view)

A board wires it up in `main.cpp`. This is the whole public surface a board sees:

```cpp
Thermocouples probes(TcChip::MAX31855, SCK, MISO, CS_ET, CS_BT);
HeaterOutput heater(CPID_PIN_HEATER, CPID_HEATER_WINDOW_MS, 90, true);
FanDimmer fan(...);                                     // or FanPwm, chosen in config.h
Controller controller(probes, heater, fan, limits()); // control task
RoasterProbe btProbe(roaster, RoasterProbe::Channel::BT); // Croaster reads the snapshot
CroasterCore core(false, &btProbe, &etProbe);
ControlPlugin plugin(roaster, pluginConfig());          // Croaster glue

void setup() {
    heater.begin(); fan.begin(); probes.begin();        // outputs off first
    auto s = ConfigStore::load(defaults());             // NVS
    controller.begin(s.gains, s.pidInput, s.cooldownFan.trip, core, prio);
    plugin.attach(app, s);                              // registers commands + data hook
    app.begin();                                        // Croaster: WiFi, WS, BLE, display
    plugin.begin();                                     // starts the watchdog clock
}
void loop() { app.loop(); plugin.loop(); }              // plugin.loop = watchdog
```

A client sees JSON only:

```json
{"command":{"setpoint":200}}      {"command":{"mode":"pid"}}
{"command":"autotune"}            {"command":"getArtisanData","id":7}
{"command":"setBurner","value":60} {"command":"setPreferences","pidKp":5}
```

## Shape

```mermaid
flowchart TB
    subgraph board["boards/esp32s3-pid (board)"]
        CFG[config.h + ConfigCheck.h]
        MAIN[main.cpp]
        OLED[CroasterDisplaySSD1306]
    end
    subgraph rc["croaster-control/src/app + src/hw (ESP32)"]
        PLUG[ControlPlugin]
        R[Controller task]
        HW[hw: SSR, dimmer/PWM fan, thermocouples]
        NVS[ConfigStore NVS]
    end
    subgraph core["croaster-control/src/core (pure C++, host-tested)"]
        PID[Pid]
        AT[RelayAutotune]
        PROF[RoastProfile]
        PROTO[controlproto: parse/format JSON]
        DEC[MAX31855/MAX6675 decode]
    end
    subgraph cl["croaster (library)"]
        APP[CroasterApp]
        CMD[CroasterCommandHandler]
        CORE[CroasterCore: RoR, data JSON]
        WS[WebSocket :81]
        BLE[BLE]
    end
    MAIN --> PLUG & R & OLED
    CFG -.compile-time.-> MAIN
    WS & BLE --> CMD
    CMD -- onCommand / onJsonCommand --> PLUG
    PLUG --> PROTO
    PLUG -- Command queue --> R
    R --> PID & AT & PROF & HW
    HW --> DEC
    R -- Snapshot --> PLUG & OLED
    R -- Snapshot via RoasterProbe --> CORE
    CORE -- onJsonData --> PLUG
    PLUG --> NVS
```

Load-bearing decisions:
- **State has one owner.** The control task alone owns the heater, fan,
  SPI bus, mode and PID. Everyone else gets a queue for writes and a
  snapshot for reads. No locks spread through the code, and a stalled
  WebSocket can't hold the heater on (*split ownership* avoided).
- **The wire format lives in one module.** `controlproto` turns JSON into
  `Command`s and formats data fields. The control task never sees JSON, and
  Croaster never sees `Command` (*information leakage* avoided). It is pure,
  so it is unit-tested on the PC.
- **Deep control interface.** `Controller` exposes `begin`, `submit(Command)`,
  `snapshot()`, `loadProfile`. Mode logic, bumpless transfer, anti-windup,
  interlocks, the SSR window and autotune all sit behind those four calls.
- **Croaster changes are small additive hooks** (sensor-injection constructor,
  `onJsonData`, `lastCommandMillis`, correction getters, BLE switch). Every existing
  board builds unchanged.
- **Board config is compile-time** (`config.h` + `static_assert`s), so a bad
  pin or a conflicting choice fails the build, not the roast.

What it deliberately does **not** do:
- Run Artisan's PC-side PID.
- Accept WiFi credentials over the wire.
- Serve its own web UI (T14).

## Synthesis decision

Three whole shapes were weighed when the skeleton was designed (see
`analysis.md`):
1. **Port yaeger onto Croaster** (chosen base: Croaster). Croaster's platform
   (portal, BLE, ICRM, OTA, multi-board) is broader. yaeger's control features
   were rebuilt as libraries, keeping its wire format for compatibility (D8).
2. **Extend yaeger with Croaster's features.** Rejected: yaeger is one
   project with globals and a loop-driven heater, so it would mean rewriting
   Croaster's platform anyway.
3. **Fork Croaster's core and put control inside it.** Rejected: every
   Croaster board would carry heater code, and upstream merges would conflict.

Taken from yaeger: command names and fields, telemetry fields, autotune fan
rules, heater 90 % cap, fan floor, PID input choice and persisted gains.
Taken from Croaster: everything about connectivity, display and boards.

## Red-flag screen (honest)

| Flag | Where | Verdict |
|:---|:---|:---|
| Two ways to do one task | Nested commands (`{"command":{"burner":..}}`) **and** yaeger's named commands (`{"command":"setBurner","value":..}`) | Accepted on purpose for yaeger compatibility. Both end in `controlproto` → the same `Command`, so the behaviour can't diverge. New clients should use nested. yaeger's flat keys were dropped (D8). |
| Hand-synced list | `.aset` command strings vs `controlproto` keys; `kNestedKeys` vs `parseNested` | Inside the firmware, registration loops over `kNestedKeys`/`kBasicNames`, so it's derived. The `.aset` is a separate file Artisan reads. `artisan-settings.md` documents every key. A future test could parse the `.aset` and run each command through `controlproto` (ticket T16). |
| Pass-through | `RoasterProbe` (Croaster `Thermocouple` → snapshot) | An adapter that adds policy (NaN on fault, unit), so not a plain pass-through |
| Importable internals | Boards can include `app/controller.h`, `hw/*` | Needed: the board constructs the drivers. Not hidden. |
| Shallow module | `ControlPlugin` public API: `attach`/`begin`/`loop` | Deep: hides all JSON routing, NVS and the watchdog |

## Tradeoffs accepted
- A dedicated control task, in exchange for one more concept (queue/snapshot)
  than Croaster's loop-only style. It is the only way a WiFi stall can't
  freeze the heater.
- Two new top-level library folders (D1), in exchange for host-testable pure
  logic and reuse by a future second control board.
- A force-included `config.h` (D3), in exchange for one master config file.
- A comms watchdog on traffic, not on disconnect events (D9), in exchange for
  also catching silent WiFi loss. The cost is a timeout (10 s default,
  2–600 s, can be switched off) instead of yaeger's instant cut on a clean
  disconnect.

## Alternatives considered
- **Control in `loop()`** (Croaster's style). Simpler, but heater safety
  would depend on the slowest thing in the loop (D7).
- **Hardware-timer ISR for control.** Can't read SPI or run floats safely in
  an ISR.
- **One combined library in `croaster/`.** Fewer folders, but every board
  (ESP8266 too) would compile heater code (D1).

## Open questions (DECISIONs for Shawn)
1. D1: resolved 2026-10-27, one library `croaster-control/` with
   `src/core`, `src/hw`, `src/app`.
2. D3: force-include `config.h` into every unit. OK?
3. D6/D9: is the comms watchdog (heater off, fan to cooldown; 10 s default,
   configurable, can be off) the behaviour you want, or yaeger's instant cut
   on disconnect as well?
4. D8: resolved 2026-10-26, flat keys and the `onOtherJson` hook dropped.
5. Cooldown fan default: resolved by D19. Exhaust Fan 65 % after stop,
   100 % when a trip starts. Cooldown ends when ET is below the saved end
   temperature (60 °C / 140 °F).
6. D10: how the Croaster PID board's `platformio.ini` relates to Croaster's
   `boards/esp32s3` (standalone, `extends` his env, or a shared chip-level base
   ini). See design.md.
7. D11: resolved 2026-10-27, one decoder in `croaster-control/src/core`;
   `croaster/` reads MAX6675 only, as upstream.
8. D13: is typing the IP into Artisan fine, or add `ESPmDNS`? No hostname
   is chosen. The old `croaster-flex` name is retired (D25).
9. D14: config wizard as a Python script (recommended) or another option?

Resolved: D5, board id `esp32s3-flex` (Shawn, 2026-10-26); D25, renamed to
`esp32s3-pid` ("Croaster PID", Shawn, 2026-10-28); D26, the no-heater build
became its own board `boards/esp32s3-monitor` ("Croaster monitor"), and
config macros split into `CMON_` (every board) and `CPID_` (control),
Shawn, 2026-10-28.

## Next implementation step
Bench bring-up (T2): flash, confirm the OLED, both probes and `getArtisanData` in
Artisan with the new `.aset`.
