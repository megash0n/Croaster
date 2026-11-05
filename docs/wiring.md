# Wiring: ESP32-S3 boards

GPIO use on the ESP32-S3 DevKitC (N16R8). The pins are set in each board's
`src/config.h`; `src/ConfigCheck.h` refuses at compile time a pin that is
reserved or used twice. All logic runs at 3.3 V; module grounds are tied to
the ESP32 GND.

## PID board (`boards/esp32s3-pid`)

| GPIO | Macro | Connects to | Notes |
|---:|:---|:---|:---|
| 12 | `CMON_PIN_TC_SCK` | both MAX31855 SCK/CLK | HSPI clock, shared |
| 13 | `CMON_PIN_TC_MISO` | both MAX31855 DO/SO | shared; MOSI unused |
| 10 | `CMON_PIN_TC_CS_ET` | ET MAX31855 CS | one CS per chip |
| 11 | `CMON_PIN_TC_CS_BT` | BT MAX31855 CS | |
| 4 | `CPID_PIN_HEATER` | SSR-40DA input + | input - to GND; on above 2.4 V |
| 5 | `CPID_PIN_FAN` | AC dimmer PSM/DIM | must be below GPIO 32 |
| 6 | `CPID_PIN_ZERO_CROSS` | AC dimmer Z-C | AC dimmer only |
| 8 | `CMON_PIN_I2C_SDA` | SSD1306 SDA | OLED, address 0x3C |
| 9 | `CMON_PIN_I2C_SCL` | SSD1306 SCL | |
| 48 | `CMON_PIN_LED` | on-board RGB LED | some DevKitC revisions use 38 |
| 19, 20 | (USB) | native USB-C port | USB D-/D+, serial log |
| 7 | `CPID_PIN_SCROLL_FAN` | relay module IN | optional, `CPID_SCROLL_FAN` |
| 15 | `CPID_PIN_FAN1` | FAN1 MOSFET module trigger PWM | optional, `CPID_FAN1` (case fan) |
| 16 | `CPID_PIN_FAN2` | FAN2 MOSFET module trigger PWM | optional, `CPID_FAN2` (ESP fan) |

Thermocouple modules, the OLED and the dimmer's logic side take 3.3 V from
the 3V3 pin. 43/44 (UART0 to the USB-UART bridge) and 48 are reserved.

### Cooling Fan (scroll relay, `CPID_SCROLL_FAN`)

AC scroll / Cooling Fan switched by a 1-channel JQC3F-5VDC-C relay module.

- Trigger jumper on **HIGH** (the board drives HIGH for on, LOW at boot).
- DC+ to 5V (VBUS/5V pin), DC- to GND, IN to GPIO 7.
- Fan hot (line) wire through COM and NO; NC unused. Neutral goes straight
  to the fan.
- The relay contacts carry mains: use a module with proper creepage, fuse
  the line and keep the mains side enclosed and away from the logic wiring.
- Some 5 V modules need more than 3.3 V on IN to switch reliably; check that
  the relay clicks with IN at 3.3 V before closing the box.

Shown on the Live page as Cooling Fan. Runs while the cooldown runs (after
STOP, End roast, emergency off or any safety trip, including `nomains`) and
stops with the Exhaust Fan when ET falls below the cooldown end. Never during
preheat. A saved enable setting (Controls > Safety, default on) turns the
output off for builds that leave the pin free. The Live page can turn it on
or off in Manual mode with the heater off and no cooldown when enabled.

### 12 V fans (MOSFET modules, `CPID_FAN1`, `CPID_FAN2`)

3-pin 12 V fans, each on one channel of a dual AOD4184A trigger-switch
module (3.3-20 V trigger, 5-36 V load, PWM input 0-20 kHz).

- Module VIN+ / VIN- to the roaster's 12 V supply.
- OUT+ / OUT- to the fan's red / black wires.
- Trigger PWM to the GPIO (15 for FAN1, 16 for FAN2), trigger GND to ESP32 GND.
- Yellow tach wire unused (leave it insulated).
- The board drives LEDC PWM at 10 kHz (`CPID_AUX_FAN_PWM_HZ`), inside the
  module's 20 kHz limit. Keep it at or below 20 kHz if changed.

FAN1 (case fan) follows the box temperature: the average cold-junction
temperature of the MAX31855 chips that report one, else the ESP32-S3 chip
temperature. FAN2 (ESP fan) follows the ESP32-S3 chip temperature; mount it
to blow on the ESP32 module. Each turns on at its set temperature (defaults
45 C and 60 C) and off 3 C below; the Live page can force it on or off.

### Free pins

1, 2, 14, 17, 18, 21, 38 (unless the LED is on 38), 39, 40, 41, 42, 47.
Avoid 0, 3, 45, 46 (strapping), 26-37 (flash and PSRAM), 43, 44 and 48.

## Monitor board (`boards/esp32s3-monitor`)

Same thermocouple, I2C and LED pins as the PID board (12, 13, 10, 11, 8, 9,
48). It has no heater, fan or optional fan outputs, so 4, 5, 6, 7, 15 and 16
are free there too.
