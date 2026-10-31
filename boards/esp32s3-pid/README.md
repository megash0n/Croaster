# Croaster PID — ESP32-S3 heater + fan control board

Board `esp32s3-pid` ("Croaster PID"): a Croaster board that runs the
roaster's heater under PID control. Standalone **PlatformIO** project, like every `boards/<board>/` folder. It is a
Croaster board that also **controls** the roaster: PID heater through an SSR,
AC-dimmer (or PWM) fan, roast profiles, multi-point autotune, safety trips, and
Artisan control over Croaster's WebSocket. It consumes:

| lib_dep | What |
|:---|:---|
| `Croaster=symlink://../../croaster` | Croaster core: WiFiManager, WebSocket :81, BLE, OTA, RoR, roast timer |
| `CroasterDisplaySSD1306=symlink://../common` | Croaster's shared OLED screen; its header line shows heater %, fan % and mode (or the trip, or the autotune step while it runs and for 10 s after a change) |
| `CroasterControl=symlink://../../croaster-control` | Control add-on. `src/core`: PID, autotune, profiles, MAX31855/MAX6675 decode, command protocol (pure, host-tested). `src/hw`: probe SPI reads, SSR, dimmer/PWM fan. `src/app`: control task, NVS, `ControlPlugin`, `AutotunePlugin` |

Design notes and decisions: [`docs/design.md`](../../docs/design.md).
Whether this ini should share chip-level settings with `boards/esp32s3` (standalone,
`extends`, or a shared base ini) is open decision D10 there; today it is standalone.

---

## 📋 Hardware (defaults in `src/config.h`)

| Part | Item | Notes |
|:---|:---|:---|
| MCU | Lonely Binary ESP32-S3 **Gold Edition N16R8**, IPEX | 16 MB QIO flash, 8 MB **octal** PSRAM (`qio_opi`). **Attach the IPEX antenna before powering up.** |
| ET / BT | 2 × MAX31855 breakout | shared SCK/MISO, one CS each, 3V3 |
| BT probe | PerfectPrime TL1918SL (K, 3.2 × 300 mm, ungrounded) | ungrounded = right for MAX31855 |
| ET probe | K probe from the Inkbird kit | **check grounded vs ungrounded** (see below) |
| Heater | SSR-40DA from the Inkbird PID kit (kit PID unused) | input 3–32 VDC, on > 2.4 V: 3.3 V GPIO drives it directly |
| Fan | RobotDyn 1-ch AC dimmer (BTA16 triac, Z-C output) | VCC from **3V3** so Z-C swings 0–3.3 V |
| Display | 0.96" 128×64 I2C OLED, SSD1306 (**confirmed**), yellow/blue | address 0x3C; top 16 rows are yellow: the IP header sits there |

All modules run from the board's 3V3 pin (AMS1117, ~500 mA budget: OLED ~20 mA,
2 × MAX31855 < 3 mA, dimmer logic < 10 mA, SSR input ~7.5 mA).

## 🔌 Wiring (default pins)

| Signal | GPIO | Notes |
|:---|:---:|:---|
| Thermocouple SCK | 12 | HSPI SCK |
| Thermocouple MISO (DO) | 13 | HSPI MISO |
| ET CS | 10 | |
| BT CS | 11 | |
| SSR + | 4 | SSR − to GND |
| Dimmer PSM/DIM | 5 | gate; must be < 32 |
| Dimmer Z-C | 6 | |
| OLED SDA | 8 | Arduino default I2C |
| OLED SCL | 9 | |
| Status LED | 48 | on-board WS2812 (`LED_BUILTIN`) |

Never use: 0/3/45/46 (strapping), 19/20 (native USB), 26–34 (flash, not on the
header), **35–37 (octal PSRAM — they are on the header)**, 43/44 (UART0 → USB-UART
port), 48 (RGB LED). `src/ConfigCheck.h` rejects these at compile time.

Yaeger's original devkit map (heater 3, fan 8, Z-C 7, TC 5/6/15/16, I2C 41/42)
also passes the check except heater on GPIO3 (strapping): move it if you reuse
that wiring.

---

## ⚙️ Configure: `src/config.h`

The one file to edit: thermocouple chip, heater, fan driver, display, pins,
features (BLE), PID defaults, safety limits, control core. It is
force-included into every translation unit (`-include src/config.h`) so it can
also switch Croaster library features (`CROASTER_HAS_BLE`). Editing it rebuilds
every unit (`tools/force_include_deps.py`).

Invalid choices fail the build with a message naming the setting:

```
src/ConfigCheck.h: error: static assertion failed: config.h: two CMON_PIN_*/CPID_PIN_* settings use the same GPIO
src/ConfigCheck.h: error: #error "config.h: CPID_REQUIRE_MAINS needs CPID_FAN_AC_DIMMER ..."
```

Setting names: `CMON_` for what every Croaster board has (probes, screen,
identity, BLE; the same names as `boards/esp32s3-monitor`), `CPID_` for what
the control add-on adds (heater, fan, mains, PID, safety). This board always
has a heater (SSR) and a fan (AC dimmer or PWM). For probes only, with no
heater or fan, build [`boards/esp32s3-monitor`](../esp32s3-monitor/README.md).

---

## 🔧 Build, upload, test

```bash
cd boards/esp32s3-pid
pio run -e esp32s3-pid                    # release (what ../../build_all.sh builds)
pio run -e esp32s3-pid -t upload          # flash over USB
../../upload_monitor.sh esp32s3-pid       # debug build + upload + monitor
pio test -e native                            # host unit tests (croaster-control/src/core)
```

| Env | Platform | Use |
|:---|:---|:---|
| `esp32s3-pid` | `platformio/espressif32 @ 8.0.0` (Arduino 3.3.12 / IDF 5.5) | release |
| `esp32s3-pid-debug` | same | `CORE_DEBUG_LEVEL=3` |
| `esp32s3-pid-pioarduino` | pioarduino 55.03.312 (same Arduino core) | alternative platform |

Every env sets `board_build.sdkconfig_path = ../esp32s3/esp32s3-sdkconfig.defaults`,
Croaster's own file in `boards/esp32s3` (this board keeps no copy). Arduino builds on these platforms compile
against prebuilt IDF libraries, so the file documents the intended Kconfig
rather than changing the build (see `docs/design.md`, "Build platform and
sdkconfig").

Serial logs: native USB-C port (`ARDUINO_USB_CDC_ON_BOOT=1`). Using the other
(USB-UART) port? Set `ARDUINO_USB_CDC_ON_BOOT=0` in `platformio.ini`.

---

## 🌐 Browser UI

Once the board is on WiFi, open `http://<board ip>/` (the IP is on the OLED)
on any phone or computer on the same network. Live shows the readings, a
BT/ET/RoR chart with heater and fan, event buttons, a roast clock and CSV
export; Monitoring lists every value the board reports; Controls has every
command below, with the board's reply shown, and STOP is always in the top
bar. Details: [`croaster-webui/README.md`](../../croaster-webui/README.md).

- **No password.** Anyone on the network can open the page and run the
  heater, just as anyone can send commands to the WebSocket on port 81. Use
  a network you trust, or set `CMON_ENABLE_WEBUI 0` in `config.h`.
- An open page polls every second, so it keeps the comms watchdog fed even
  with Artisan closed. Closing the tab, a sleeping laptop or iOS Safari in
  the background stops the polls, and the watchdog trips after its timeout.
- The roast clock and events are kept in that browser only.
- Not on the page yet: firmware upload (use Croaster's OTA).
- Settings: `CMON_ENABLE_WEBUI` (1), `CMON_WEBUI_PORT` (80; the page needs
  the setup portal closed, which also uses port 80).
- WiFi modem sleep: `CMON_WIFI_SLEEP` (0 = off, the default; replies in a
  few ms instead of 100–300 ms, a few tens of mA more). If the board
  resets at boot after this setting, set it to 1.
- Over https (a reverse proxy) the page connects to `wss://<host>/ws`; the
  proxy must forward `/ws` to port 81 with the WebSocket upgrade headers.

---

## 📡 Commands (WebSocket :81 and BLE, Croaster JSON)

Croaster's own commands work unchanged (`getArtisanData`, `getDeviceInfo`,
`tempUnit`, OTA...). Temperatures are in Croaster's current unit: °F
(`CMON_TEMP_UNIT` in `config.h`) until `{"command":{"tempUnit":"C"}}` switches
it. The change is saved, so the board boots in that unit from then on. Limits and the PID still work in °C. The Croaster PID board adds:

| JSON | Effect |
|:---|:---|
| `{"command":{"burner":0..100}}` | heater %, manual mode |
| `{"command":{"fan":0..100}}` | fan % |
| `{"command":{"setpoint":T}}` | PID setpoint |
| `{"command":{"mode":"manual"\|"pid"}}` | mode (`pid` needs tuned gains, see Autotune) |
| `{"command":{"pidInput":"ET"\|"BT"\|"MAX"}}` | PID input (saved; default BT, `CPID_PID_INPUT`; a saved value wins over the default) |
| `{"command":{"pid":{"kp":..,"ki":..,"kd":..}}}` | gains (saved; counts as tuned) |
| `{"command":{"cooldownFan":{"stop":0..100,"trip":0..100}}}` | fan after `stop` (default 65) and when any safety trip starts (default 100); either field alone works; a plain number sets `stop` (saved) |
| `{"command":{"watchdog":{"enabled":bool,"timeout":s}}}` | comms watchdog on/off and timeout, 2..600 s (saved; either field alone works) |
| `{"command":{"profile":[{"duration":s,"setpoint":T,"interpolation":"ease-out","fanValue":%}]}}` | load profile: 1 to 32 steps, duration above 0 and up to 3600 s, setpoint -50 to 1000 °C (in the current unit), `fanValue` 0..100 optional; replies `{"command":"profile","response":{"loaded":bool,"steps":n}}` |
| `{"command":"startProfile"}` / `"stop"` / `"autotune"` / `"clearTrip"` / `"getControl"` | actions / query (autotune: see Autotune below; `getAutotune`, `{"autotune":"apply"\|"discard"}`) |

Profile library (`CPID_ENABLE_LIBRARY`, design D28). Files are format v1:
`{"version":1,"name":..,"notes":..,"unit":"F","steps":[...],"recipe":{...}}`,
`steps` and/or `recipe`. Save and delete reply `busy` while the heater is on or
a profile or autotune runs. Errors: `{"ok":false,"error":"badName"|"notFound"|"exists"|"full"|"busy"|"badRecipe"|"unavailable"|...,"reason":..}`.

| JSON | Reply `response` |
|:---|:---|
| `{"command":"libraryList"}` | `{ok,version,max,maxBytes,usedBytes,totalBytes,profiles:[{id,name,unit,steps,recipeSteps,duration,bytes,valid}]}` |
| `{"command":"libraryGet","id":"city"}` | `{ok,id,profile}` |
| `{"command":"librarySave","profile":{..},"id":"city","overwrite":true}` | `{ok,id,created}`; without `id` a new id is made from the name |
| `{"command":"libraryDelete","id":"city"}` | `{ok,id}` |
| `{"command":"libraryLoad","id":"city"}` | `{ok,id,loaded,steps,recipe}`: the schedule goes to Profile mode, the recipe to the runner; refused while a recipe runs |

Recipe steps run on the board (design D28). A step is
`{"trigger":{"type":"time"|"bt"|"et"|"ror"|"event","op":"gte"|"lte"|"near","value":v,"tol":t},"anyOrder":bool,"label":..,"actions":[...]}`;
actions are `{"type":"power","value":%}`, `fan`, `{"type":"setpoint","value":T,"ramp":s}`, `hold`, `cool`, `end`, `{"type":"note","text":..}`.
Time is seconds since CHARGE; event values are `CHARGE`, `TP`, `DRY_END`, `FC_START`, `FC_END`, `SC_START`, `SC_END`, `DROP`.

| JSON | Reply `response` |
|:---|:---|
| `{"command":"recipeLoad","profile":{"version":1,"name":..,"unit":"F","recipe":{"mode":"auto"\|"guided","steps":[..]}}}` | `{ok,recipe,schedule}`; refused (`busy`) while one runs, `badRecipe` for a unit other than C, F or K |
| `{"command":"recipeStart"}` / `"recipeAbort"` | `{ok}`; errors `empty`, `running`, `notRunning`, `autotuneRunning` |
| `{"command":"recipeConfirm","step":n}` | guided: apply the prompted step; error `noPrompt` |
| `{"command":"recipeSkip"}` / `{"command":"recipeSkip","step":n}` | skip the next step, or dismiss the prompt for step n |
| `{"command":"roastEvent","event":"CHARGE","undo":false}` | `{ok,event,sinceCharge}`: the board's roast clock (the page sends it on each event mark and undo) |
| `{"command":"getRecipe"}` | `{ok,state,reason,seq,name,unit,mode,next,prompt,follow,sinceCharge,events:{..},fired:[{step,at,how}],recipe}` |

Data samples carry `"recipe":{"state","next","prompt","fired","seq","follow"}` once a recipe is loaded.
`state` is `ready`, `running`, `done` or `aborted`; `reason` is `complete`, `end`, `cool`, `user`, `stop`, `ota` or the trip name.

yaeger's named commands also work (same parser, see `docs/control-parity.md`).
yaeger's flat keys (`{"BurnerVal":v}` without `"command"`) are not accepted.

| JSON | Effect |
|:---|:---|
| `{"command":"setBurner","value":v}` / `setFan` | heater / fan % |
| `{"command":"setPreferences","pidKp":..,"pidKi":..,"pidKd":..,"cooldownFanSpeed":..}` | gains + cooldown fan (saved); replies with preferences |
| `{"command":"getPreferences","id":N}` | `{"id":N,"data":{"type":"preferences","pidKp",...,"cooldownFanSpeed"}}` |

Data broadcasts gain `BurnerVal`, `FanVal`, `SV`, `Mode`, `trip`, `autotune`,
`Amb`, `Target`, `pidKp`/`pidKi`/`pidKd`, `wifiStrength`, `dummy` (fake
readings on), and `atTarget` (the
autotune test point) during autotune. `autotune` holds the last autotune
status in the display unit: `idle`, `running: heating to 300 F (1/3)`,
`running: cycle 3/6 at 350 F (2/3)`, `results ready`, `done`, `discarded`,
`failed: no heat-up`, `aborted: stop`, `need a valid probe`,
`autotune required`. While it runs, and for 10 s after it changes, the OLED
header shows a short form instead ("AT2/3 cyc3/6 350F", "AT results ready").

The PID runs on the ESP32. Clients (Artisan, ICRM, a browser) only send targets
and commands.

Safety: over-temperature, probe fault, no mains (with `CPID_REQUIRE_MAINS 1`,
the dimmer sees no zero-cross for `CPID_MAINS_GRACE_MS`, 2 s, while heating),
OTA in progress, and comms lost (below) all turn the heater off and latch a
trip. `clearTrip`, or any heater, mode, autotune or profile command, clears it;
the firmware trips again at once if the cause is still there. Fan and setpoint
changes don't clear a trip.

### Comms watchdog

While the heater is on (Manual, PID or Autotune; profile runs are exempt), the
firmware expects a client message at least every timeout. Any message counts,
over WebSocket or BLE, including Artisan's once-a-second `getArtisanData`. If
none arrives, it trips `comms`: heater off, fan to the trip speed (100 % by
default, `cooldownFan.trip`).

| Setting | Default | Change at build time | Change at runtime (saved in NVS) |
|:---|:---|:---|:---|
| On/off | on | `CPID_WATCHDOG_ENABLED` (1/0) | `{"command":{"watchdog":{"enabled":false}}}` |
| Timeout | 10 s | `CPID_WATCHDOG_TIMEOUT_S` (2..600) | `{"command":{"watchdog":{"timeout":20}}}` |

`config.h` values apply until a runtime command saves new ones; after that NVS
wins. `{"command":"getControl"}` shows the current values under `watchdog`.
Turn it off only when something else watches the roast: with it off, a crashed
laptop leaves the heater running until the over-temperature trip.

### Autotune

Autotune measures the roaster and picks PID gains for it. It replaces the
default gains (`CPID_PID_KP/KI/KD`, yaeger's 1 / 0.1 / 0.01, never tuned for
this hardware). Run it once the heater, fan and the PID input probe (BT by
default) are installed, and again after changing any of them. Run it empty, or
with the load you roast with, at the fan speed you roast with.

**Required before PID and profiles** (`CPID_REQUIRE_AUTOTUNE 1`, the default)
- Until the board has tuned gains, PID mode and `startProfile` are refused:
  the mode stays as it is, `autotune` reads `autotune required` in the data
  and `getControl`, and the OLED header shows `Autotune first` for 10 s.
  Manual mode and autotune always work. `getControl` `tuned` says whether the
  board has tuned gains.
- Tuned means saved in NVS by a completed autotune, or set with the `pid`
  command (or yaeger's `setPreferences` gains). Gains you set yourself count:
  if you already know good gains for this roaster, send them and skip the
  autotune. A failed or aborted autotune doesn't count.
- A fresh board, or one whose flash was erased (`pio run -t erase`), has no
  tuned gains. A board that saved gains before this setting existed counts as
  tuned. Uploading new firmware keeps NVS, so it stays tuned.
- `CPID_REQUIRE_AUTOTUNE 0` in `src/config.h` turns the check off: PID and
  profiles run on the default gains at once.
- Rerun autotune now and then, and after changing the heater, fan, probe or
  PID input. A new completed run replaces the saved gains.

How it works (design D29; full description in
[`docs/autotune/`](../../docs/autotune/README.md)): one command, no setpoint.
The board heats to each test point in turn (`CPID_AUTOTUNE_POINT1_C` ..
`POINT4_C`, default 250, 300 and 350 °F), switches the heater between two
levels around it (a relay test, with the levels re-centred each cycle so the
heating and cooling halves even out), and records cycles until the last three
agree. From the swing and the period it computes the ultimate gain Ku and
period Tu at each point, then Ziegler–Nichols PI gains (`CPID_AUTOTUNE_RULE`),
and recommends the most cautious set over the points. Marlin and Klipper tune
at one target; this runs several because BT sweeps the whole roast range.

**Start**
1. Empty drum, drum turning, roaster cold or cooled (BT under about 300 °F). Set the fan
   to your roasting speed first (a fan change during the run stops it); below
   30 % the firmware raises it to 60 %.
2. Page: Controls > Autotune > **Start autotune**, then OK in the confirm
   (it lists the points and the expected minutes). Or send
   `{"command":"autotune"}` from any client (Artisan's **Autotune** button,
   WebSocket :81, BLE). Keep a client polling: the comms watchdog stays
   active during autotune.
3. Leave the controls alone until it finishes. Touching any of them (fan,
   heater, SV, mode, PID input, correction) ends it with the heater off.

It refuses to start without a reading on the PID input probe
(`need a valid probe`), while a recipe runs (reply error `recipeRunning`), and
when the roaster is already more than 30 °C above the first point
(`failed: too hot to start`).

**While it runs**
- `Mode` reads `Tuning`; `autotune` reads e.g. `running: cycle 3/6 at 350 F
  (2/3)`; the OLED header shows `AT2/3 cyc3/6 350F`; data carries `atTarget`.
- The page shows a progress bar with the time left, the steps (heat, settle,
  measure per point, then calculate) and the board's log, one line per cycle.
  `{"command":"getAutotune"}` returns the same report.
- Duration: about 13 minutes per point on a simulated Behmor-like drum
  (39 min for three points); expect 45 to 65 minutes on the roaster. The
  limit is `CPID_AUTOTUNE_TIME_LIMIT_MIN` (90).
- BT swings a few degrees around each point (the relay step is 40 %).

**Stop**: `{"command":"stop"}` (STOP, Heater off, Stop autotune) ends it with
the heater off and the stop cooldown fan. Any other control change also ends
it with the heater off: switching mode (`mode`, `startProfile`), or changing
SV, `pidInput`, a correction offset, the fan (`aborted: fan changed`) or the
heater (`aborted: heater changed`). The heater command's value is not applied;
a fan command is.

**Safety**: fails with the heater off on a probe fault, BT above
`min(260 °C, CPID_OVER_TEMP_C − 5 °C)`, BT 30 °C over a test point, under 3 °C
rise in 180 s while heating (`no heat-up`), no relay switch in 15 minutes,
and the total time limit. Every trip ends it as well; the status names it.

**Result**
- Done: `autotune` reads `results ready` (OLED `AT results ready`), Manual,
  heater 0 %, stop cooldown fan. Nothing is applied yet. The page opens a
  dialog with each point's result and the current versus new Kp, Ki and Kd:
  **Save** sends `{"command":{"autotune":"apply"}}` (gains live and saved in
  NVS like the `pid` command, `tuned` set), **Cancel** sends `"discard"`. The
  results wait on the board until one is chosen, and are lost on reboot.
- Failed or aborted: `failed: <reason>` or `aborted: <reason>`, Manual,
  heater 0 %. The old gains stay.
- The gains are in heater % per °C (`kp`), per °C·s (`ki`) and %·s per °C
  (`kd`). To go back, send the old values with
  `{"command":{"pid":{"kp":..,"ki":..,"kd":..}}}`.

| JSON | Reply `response` |
|:---|:---|
| `{"command":"autotune"}` | starts; refused `{ok:false,error:"recipeRunning"}` while a recipe runs |
| `{"command":"getAutotune"}` | `{status,unit,running,pending,phase,failure,plan,progress,current,results,recommended,log}` (`docs/autotune/implementation.md`) |
| `{"command":{"autotune":"apply"\|"discard"}}` | `{ok,action}`; `ok` false when no results were waiting |

**Credit**
- Method: K. J. Åström and T. Hägglund, relay feedback autotuning (Automatica,
  1984); bias correction as in Marlin's
  [`M303`](https://marlinfw.org/docs/gcode/M303.html). Tuning rules: J. G.
  Ziegler and N. B. Nichols (Trans. ASME, 1942), B. D. Tyreus and W. L. Luyben
  (Ind. Eng. Chem. Res., 1992), S. Skogestad (J. Process Control, 2003).
  Sources and simulation: `docs/autotune/tuning-rules.md`.
- The command and the fan rule (below 30 % → 60 %) follow
  [yaeger](https://github.com/tadelv/yaeger) by Vid Tadel, whose PID and
  autotune were added by RobTS in
  [PR #21](https://github.com/tadelv/yaeger/pull/21) on top of
  [lily-osp/AutoTunePID](https://github.com/lily-osp/AutoTunePID).
- The autotune here (`croaster-control/src/core/autotune_sequence.cpp`) is new
  code written for this project, not taken from yaeger, AutoTunePID, Marlin or
  Klipper.

## ☕ Artisan

Load `artisan/croaster-pid.aset` (Help → Load Settings). Host
`192.168.4.1` on Croaster's access point (or the IP on the OLED), port 81, path `WebSocket`,
one `getArtisanData` per second. Curves: BT, ET, Heater, Fan, SV. Sliders: Fan,
Setpoint, Heater. Buttons: heater 50–70 %/Off, PID, Manual, Autotune, Stop,
Clear trip. Artisan's own PID ("Control") is off, because ours runs on the
ESP32. Every setting and its source: [`docs/artisan-settings.md`](../../docs/artisan-settings.md).

## ⚠️ Check before first power-up
- IPEX antenna attached (transmitting without it can damage the RF stage).
- ET probe isolation: MAX31855 reports a short-to-GND/VCC fault on grounded
  probes or a probe touching the chassis. Ungrounded probes only.
- Dimmer VCC on 3V3, mains 60 Hz (`CPID_MAINS_HZ`), fan motor suits triac
  phase control (universal / shaded-pole; not capacitor-run or EC fans).
