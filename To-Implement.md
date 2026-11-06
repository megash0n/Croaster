# To-Implement: Build-time hardware selection

Superseded for thermocouples. Croaster PID and Croaster monitor choose
MAX31855 or MAX6675 in each board's `config.h` (`docs/design.md` D11, D21,
D26). `croaster/` still builds the upstream MAX6675 path. The note below is
the earlier proposal and is not the current layout.

This note describes a small refactor for the Croaster fork. The goal is to let a board choose its thermocouple (and later other hardware) at build time without scattering `#ifdef` blocks through the library.

## Current layout

Each board lives in its own folder (`boards/esp32s3`, etc.). It supplies:

- `config.h` / `pins.h` for pins, dummy mode, and LED
- `platformio.ini` with a release env and a debug env that extends it
- `main.cpp` that constructs `CroasterCore`, a display subclass, and `CroasterApp`
- A hard-coded board name via `CroasterDeviceIdentity::setBoardName`

The shared library is pulled in with `symlink://` so edits appear immediately. Board-specific choices stay out of the library.

## Proposed change for thermocouples

Add a single config header that the library includes, and let each board’s `platformio.ini` set the type with a build flag.

### 1. platformio.ini (per board)

```ini
[env:esp32s3]
build_flags = -DTHERMOCOUPLE=MAX31855
lib_deps =
    Croaster=symlink://../../croaster
    adafruit/Adafruit MAX31855 library

[env:esp32s3-max6675]
extends = env:esp32s3
build_flags = -DTHERMOCOUPLE=MAX6675
lib_deps =
    Croaster=symlink://../../croaster
    Yurii Salimov/MAX6675 Thermocouple
```

A board can keep its existing release/debug pair and simply add another env if it wants to support both chips.

### 2. HardwareConfig.h (new file in the library)

```cpp
#pragma once

#if !defined(THERMOCOUPLE)
  #define THERMOCOUPLE MAX31855
#endif

#if THERMOCOUPLE == MAX31855
  #include <Adafruit_MAX31855.h>
  using ThermocoupleType = Adafruit_MAX31855;
#elif THERMOCOUPLE == MAX6675
  #include <MAX6675_Thermocouple.h>
  using ThermocoupleType = Thermocouple;
#else
  #error "Set -DTHERMOCOUPLE=MAX31855 or MAX6675"
#endif
```

### 3. Library change

In `CroasterCore` replace the concrete pointer type with the alias:

```cpp
ThermocoupleType* thermocoupleBT;
ThermocoupleType* thermocoupleET;
```

Construction and method calls stay the same if both libraries expose a compatible interface. If they do not, a thin adapter can sit behind the alias.

## Where the same pattern helps

- **Display type.** A board already picks its display subclass in `main.cpp`. A build flag can select which subclass is compiled, or which set of sources is included, without touching the core library.
- **Pin sets.** `pins.h` already lives in the board folder. A second env can point at a different pin header or inject `-DPIN_SET=ALTERNATE` if a board has two common wiring layouts.
- **Optional features.** Features that are only useful on some boards (extra sensors, different LED drivers) can be gated the same way so the library binary stays small.
- **Debug vs release.** The existing `<board>-debug` env that extends the release env already demonstrates the pattern. Adding a hardware choice is the same mechanism.

## What stays unchanged

- Board folders still own their `config.h`, `pins.h`, and `main.cpp`.
- The library continues to be consumed via `symlink://`.
- `build_all.sh` and `upload_monitor.sh` keep working. They just gain extra envs they can target if needed.
- No runtime cost. The unused thermocouple code is never compiled.

## Implementation order

1. Add `HardwareConfig.h` and the type alias in the library.
2. Update one board’s `platformio.ini` with the two envs and verify both build.
3. Move any remaining thermocouple-specific includes out of the core and into the config header.
4. Apply the same flag pattern to the next hardware choice that differs across boards.

This keeps the library free of board knowledge while giving each board a one-line way to declare its hardware.
