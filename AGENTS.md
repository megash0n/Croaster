# AGENTS.md — Working rules for this repo

## Read first
Start every session by reading **`CONTEXT.md`** — the single source of truth
for the current state of the Croaster firmware (architecture, git state, WIP,
gotchas, next steps). If it's missing, recreate it from the codebase before
doing work.

## Keep `CONTEXT.md` current
Update `CONTEXT.md` in the **same change** as the work that makes it stale:

- Change architecture, file layout, build commands, or the firmware version.
- Add/remove a board implementation or a library feature.
- Land a user-visible firmware/UI change.
- Add a gotcha that took effort to learn.
- Change the git state (new commits, branch, WIP direction).
- Complete or drop a "Next steps" item.

Rules: keep it concise (short bullets/tables, not prose); don't duplicate what
the code, READMEs, or CHANGELOG already say; commit it with the change it
describes.

## Repository layout
- `croaster/` — the **Croaster library core** (display/pin-agnostic):
  `croaster/src/` + `croaster/library.json`. Consumed by every board via
  `lib_deps = ../../croaster`. Board specifics never go here.
- `boards/<board>/` — a standalone PlatformIO project per board (`esp32c3`,
  `esp8266`, `esp32s3`). Each owns its `platformio.ini`, `src/main.cpp` +
  `src/config.h` (pins/dummy/LED), board-specific display code, and a per-board
  README. `boards/common/` holds the shared SSD1306 display used by the
  non-LVGL boards.
- `croaster-control/` — heater and fan control add-on (`src/core` pure and
  host-tested, `src/hw` ESP32 drivers, `src/app` control task and
  `ControlPlugin`). Used by `boards/esp32s3-pid` (Croaster PID) and
  `boards/esp32s3-monitor` (Croaster monitor, probe path only) via
  `CroasterControl=symlink://../../croaster-control`. Board config never goes
  here either. Rules: `croaster-control/AGENTS.md`.
- `croaster-webui/` — the browser page served by the S3 boards (`web/`
  sources, gzipped into `src/webui_assets.h`). Rules: `croaster-webui/AGENTS.md`.
- `boards/esp32s3-pid` (Croaster PID: heater, fan, PID, recipes, autotune)
  and `boards/esp32s3-monitor` (probes and page, no outputs). Host tests for
  `croaster-control/src/core` live in `boards/esp32s3-pid/test/`.
- `docs/` — `design.md` (decisions D1…), `tickets.md` (T1…), `test-plan.md`,
  `how.md`, `autotune/`.
- `tools/` — build helpers shared by boards (`force_include_deps.py`: makes the
  force-included `config.h` a rebuild dependency).
- `builds/` — gitignored; firmware output of `build_all.sh`.

## Build & release
- `./build_all.sh [board...]` — builds every board (or the listed ones) and
  copies each `firmware.bin` into `builds/`. Per-board manual build:
  `cd boards/<board> && pio run -e <env> -t upload`.
- `./upload_monitor.sh <board> [port]` — build + upload a **DEBUG** build of
  one board to the connected device, then open the serial monitor
  (`pio run -e <board>-debug -t upload -t monitor`). Release firmware comes
  only from `build_all.sh`: each board env is pinned to `build_type = release`,
  and the `<board>-debug` env `extends` it.
- PlatformIO is **not on PATH** on macOS — the script uses
  `~/.platformio/penv/bin/pio`; set `PIO=...` to override.
- Boards consume the library via **`symlink://`** lib_deps
  (`Croaster=symlink://../../croaster`, `CroasterDisplaySSD1306=symlink://../common`);
  PlatformIO links the live sources into `.pio/libdeps/<env>/`, so edits to
  `croaster/src/` are picked up with no `.pio/libdeps` clearing.
- Each board hardcodes its id via `CroasterDeviceIdentity::setBoardName("...")`
  in its `main.cpp` (read by
  `CroasterDeviceIdentity::boardName()`; reported to the app as `board`).
  There are **no board-specific compile-time guards** — each board builds its
  own folder unconditionally.

## Conventions
- Board config lives in the board (`config.h`, `pins.h`, `platformio.ini`,
  README) — never in the library.
- 4-space indent, Allman braces, ~120 col width (matches the library sources).
- No Arduino IDE — PlatformIO is the only workflow.
- Board ids are lowercase: `esp32s3` / `esp32c3` / `esp8266` / `unknown`
  (match the ICRM app's `CroasterBoardTypes` enum).
- Config macro prefixes for our boards (docs/design.md "Names", D26): `CMON_`
  for settings every Croaster board has (probes, screen, identity, radio),
  `CPID_` for what the control add-on adds (heater, fan, mains, PID, safety).
- Never rename the upstream author's names or code: everything in `croaster/`,
  `boards/common`, `boards/esp32s3`, `boards/esp32c3`, `boards/esp8266`, and
  his macros such as `CROASTER_HAS_BLE`. Our `config.h` sets them from our
  macros (`#define CROASTER_HAS_BLE CMON_ENABLE_BLE`).

## Changes on `rework/roaster`
- Each change lands with its docs in the same commit: `CONTEXT.md`,
  `CHANGELOG.md`, a decision in `docs/design.md` (next free D number, listed
  in the "open for Shawn" table), a ticket in `docs/tickets.md` (next free T
  number; fetch first, numbers are shared), `docs/test-plan.md` (host checks
  and bench steps), and `docs/how.md` or `docs/autotune/` when they describe
  the code touched.
- New public names (types, functions, JSON keys, config macros, page ids) are
  listed in the decision as pending Shawn's confirmation.
- Fast-forward pushes only, to `rework/roaster`; `main` is left alone.
- Docs and comments: professional, concise, about the code and the roasting
  behaviour it controls (BT, rate of rise, charge, first crack). Name no
  commercial roasting products and no outside code beyond the upstream
  Croaster project and the libraries in `platformio.ini`.
- Local changes inside `croaster/` stay minimal, are marked in a comment, and
  are recorded in a ticket as "could be offered upstream".

## Checks before a commit
- Host: `cd boards/esp32s3-pid && pio test -e native` (all pass).
- Page: `node --test croaster-webui/test/*.test.js` and, in `croaster-webui/`,
  `python3 embed_assets.py --check` and `python3 test/test_embed_assets.py`.
- Firmware: `pio run -e esp32s3-pid` and `pio run` in
  `boards/esp32s3-monitor`, with no warnings beyond the WiFiManager ones
  already there; note RAM, flash and page size changes in the decision.

## Before you finish a session
- Working tree clean (or leftovers explicitly noted in CONTEXT.md → Next steps).
- `CONTEXT.md` reflects exactly what is committed and what is pending.
