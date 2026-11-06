# Croaster monitor — ESP32-S3 temperature monitor board

Board `esp32s3-monitor` ("Croaster monitor"): the Croaster PID hardware
(`boards/esp32s3-pid`) without the heater and fan. It reads ET and BT and
serves them like every Croaster board: OLED, RoR, roast timer, WebSocket :81,
BLE and OTA. There is no control task, no heater, fan or mains input, and no
control commands. The page hides Controls, so the Drop label stays "Drop".
Standalone **PlatformIO** project, like every
`boards/<board>/` folder. It consumes:

| lib_dep | What |
|:---|:---|
| `Croaster=symlink://../../croaster` | Croaster core: WiFiManager, WebSocket :81, BLE, OTA, RoR, roast timer |
| `CroasterDisplaySSD1306=symlink://../common` | Croaster's shared OLED screen, with its standard header |
| `CroasterControl=symlink://../../croaster-control` | Only the probe path is used: `hw/thermocouples` (MAX31855 or MAX6675 SPI reader, decoded by `core/`) and `app/MonitorProbes` (hands the readings to `CroasterCore`). The control code is not linked in. |

Design notes and decisions: [`docs/design.md`](../../docs/design.md) (D26).

---

## 📋 Hardware (defaults in `src/config.h`)

| Part | Item | Notes |
|:---|:---|:---|
| MCU | Lonely Binary ESP32-S3 **Gold Edition N16R8**, IPEX | 16 MB QIO flash, 8 MB **octal** PSRAM (`qio_opi`). **Attach the IPEX antenna before powering up.** |
| ET / BT | 2 × MAX31855 or 2 × MAX6675 breakout | shared SCK/MISO, one CS each, 3V3 |
| Display | 0.96" 128×64 I2C OLED, SSD1306 | address 0x3C |

## 🔌 Wiring (default pins)

Same pins as Croaster PID.

| Signal | GPIO | Notes |
|:---|:---:|:---|
| Thermocouple SCK | 12 | HSPI SCK |
| Thermocouple MISO (DO) | 13 | HSPI MISO |
| ET CS | 10 | |
| BT CS | 11 | |
| OLED SDA | 8 | Arduino default I2C |
| OLED SCL | 9 | |
| Status LED | 48 | on-board WS2812 (`LED_BUILTIN`) |

Never use: 0/3/45/46 (strapping), 19/20 (native USB), 26–34 (flash, not on the
header), **35–37 (octal PSRAM — they are on the header)**, 43/44 (UART0 → USB-UART
port), 48 (RGB LED). `src/ConfigCheck.h` rejects these at compile time.

---

## ⚙️ Configure: `src/config.h`

The one file to edit: thermocouple chip (`CMON_THERMOCOUPLE`: `CMON_TC_MAX31855`
or `CMON_TC_MAX6675`), display, pins, temperature unit (`CMON_TEMP_UNIT`,
default "F", used until a `tempUnit` command saves another one), BLE
(`CMON_ENABLE_BLE`), WiFi modem sleep (`CMON_WIFI_SLEEP`, 0 = off by
default) and the browser UI (`CMON_ENABLE_WEBUI`,
`CMON_WEBUI_PORT`). Every setting uses the `CMON_`
prefix, the same names Croaster PID uses for the same settings. The file is
force-included into every translation unit (`-include src/config.h`) so it can
also switch Croaster library features (`CROASTER_HAS_BLE`). Editing it rebuilds
every unit (`tools/force_include_deps.py`).

The board id is `"esp32s3-monitor"` (`CMON_BOARD_NAME`). Do not set it to
`"esp32s3"`: that id belongs to the ES3C28P LCD image, and an app OTA would
replace this firmware.

---

## 🔧 Build, upload

```bash
cd boards/esp32s3-monitor
pio run -e esp32s3-monitor                 # release (what ../../build_all.sh builds)
pio run -e esp32s3-monitor -t upload       # flash over USB
../../upload_monitor.sh esp32s3-monitor    # debug build + upload + monitor
```

| Env | Platform | Use |
|:---|:---|:---|
| `esp32s3-monitor` | `platformio/espressif32 @ 8.0.0` (Arduino 3.3.12 / IDF 5.5) | release |
| `esp32s3-monitor-debug` | same | `CORE_DEBUG_LEVEL=3` |

Like Croaster PID, every env sets
`board_build.sdkconfig_path = ../esp32s3/esp32s3-sdkconfig.defaults`
(Croaster's own file in `boards/esp32s3`). Host unit tests for the shared
decoders live with Croaster PID (`cd ../esp32s3-pid && pio test -e native`).

Serial logs: native USB-C port (`ARDUINO_USB_CDC_ON_BOOT=1`). Using the other
(USB-UART) port? Set `ARDUINO_USB_CDC_ON_BOOT=0` in `platformio.ini`.

---

## 📡 Commands and Artisan

Croaster's own commands only (`getArtisanData`, `getDeviceInfo`, `tempUnit`,
corrections, OTA...), over WebSocket :81 and BLE. For Artisan, use Croaster's
standard setup (["How to Connect Croaster with Artisan"](../../README.md#-how-to-connect-croaster-with-artisan));
`boards/esp32s3-pid/artisan/croaster-pid.aset` adds heater and fan controls
this board does not have.

## 🌐 Browser UI

Once the board is on WiFi, open `http://<board ip>/` (the IP is on the OLED).
Live shows BT, ET and RoR with a chart, event buttons, a roast clock and CSV
export; Monitoring lists every value the board reports. This board has no
Controls section. No password: anyone on the network can open it. Details:
[`croaster-webui/README.md`](../../croaster-webui/README.md).

## ⚠️ Check before first power-up
- IPEX antenna attached (transmitting without it can damage the RF stage).
- MAX31855: grounded probes, or a probe touching the chassis, read as a
  short-to-GND/VCC fault. Use ungrounded probes.
