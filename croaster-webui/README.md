# CroasterWebUi: browser UI served by the board

Open `http://<board ip>/` once the board is on WiFi (the IP is on the OLED).
The page has three sections:

- **Live**: BT, ET, RoR, heater, fan, setpoint and mode tiles; a BT/ET/RoR
  chart with heater and fan strips under it; event buttons (Charge, Turning
  point, Dry end, FC start/end, SC start/end, Drop) with a roast clock; CSV
  export. RoR is the device's (`DBT`/`DET`) or, with "Smoothed RoR", a 30 s
  least-squares slope of BT computed in the browser.
- **Monitoring**: every value the board reports (`getArtisanData`,
  `getControl`, `getDeviceInfo`) with its key and age. Nothing else.
- **Controls** (Croaster PID only): every control command, with the board's
  reply or error shown inline. STOP stays in the top bar on every section.
  `restartesp`, `erase`, `dummyToggle` and `wifiConnect` ask for confirmation.
  The Autotune panel starts a run after a confirm, shows its progress, steps,
  log and per-point results, and asks to save or discard the new gains
  (`docs/autotune/`); Live draws the test point while it runs.

## Appearance

The **Appearance** button in the top bar opens the colour and style settings
(a side panel on a computer, full screen on a phone). Changes apply as you
make them, the chart included, and are saved in this browser.

- **Presets:** Neon (default: a neon green dark theme with translucent cards
  and a soft glow), Copper dark, Light, High contrast and Graphite. "Reset to
  default" returns to Neon.
- **Colours:** a picker and a hex field (`#rgb` or `#rrggbb`) for each of
  background, cards, elevated surfaces (buttons, inputs), border, grid
  lines, text, secondary text, accent, text on accent, danger (STOP and trip
  banner), text on danger, success, warning, and the chart's BT, ET, RoR
  (BT), RoR (ET), heater, fan, setpoint and event markers. An invalid hex
  is outlined and ignored.
- **Style:** corner radius, text size (80 to 140 %), density (comfortable or
  compact), chart line width, numerals (monospace or the system font), card
  opacity, glow, and reduce motion (also on when the system asks for it).
- **Contrast:** text pairs below the WCAG AA minimum of 4.5:1 are listed
  under the settings. It is a warning; the scheme still applies.
- **Share:** "Download JSON" saves `croaster-theme.json`; "Load file" or
  "Apply pasted JSON" reads one back. Missing values come from the preset
  named in the file; unknown keys are dropped and invalid values are named.

The scheme is stored under `croaster.theme` as
`{"format":"croaster-theme","v":1,"preset":...,"colors":{...},"style":{...}}`.
Pages from before this setting stored `"dark"` or `"light"`; `"dark"` moves
to Neon and `"light"` to Light.

The roast clock, events and samples live in the browser (`localStorage`), not
on the board. A reload keeps them; another browser does not see them. The
Drop/Cooldown name is a board setting (`dropLabel`), so every browser on
that board shows the same one. The page removes the old
`croaster.dropLabel` browser value and does not copy it onto the board.

## Security

There is no password. Anyone on the same network can open the page and, on
Croaster PID, run the heater. This matches Croaster's WebSocket on port 81,
which already accepts commands from anyone on the network. Use it on a
network you trust, or build with `CMON_ENABLE_WEBUI 0`.

## How it is built

| Path | What |
|:---|:---|
| `web/` | Page sources: `index.html`, `app.css`, `app.js` (UI), `theme.js` (colour schemes, validation and contrast; also loaded by the tests), `roast.js` (pure roast math, also loaded by the tests), `library.js` (profile file format, recipe editor rows, behind-schedule check; also loaded by the tests), `autotune.js` (autotune panel texts, step list and result rows; also loaded by the tests), `link.worker.js` (WebSocket and polling) |
| `embed_assets.py` | Library extra script: gzips `web/` into `src/webui_assets.h`. Runs on every build and rewrites the header only when a source changes. Output is reproducible (gzip level 9, no timestamp, sorted files). Fails the build above 100 KB gzipped |
| `src/webui_assets.h` | Generated, committed. Its first lines carry the SHA-256 of the sources |
| `src/WebUiServer.*` | ESP-IDF `esp_http_server` serving the assets and `/info.json` |
| `test/` | Host tests (below) |

`python3 croaster-webui/embed_assets.py` regenerates the header by hand;
`--check` exits 1 if it is stale.

The server starts once the station is connected and WiFiManager's portal is
closed (the portal owns port 80 while it runs) and stops if the portal opens
again. It runs in its own task (core 0 on Croaster PID, away from the control
task; priority 3) and only hands out constant data: it never calls Croaster's
core or command handler. The page talks to the board through Croaster's
WebSocket on port 81, like Artisan does.

`/info.json` tells the page the board id, whether to show Controls, and the
WebSocket port. Both boards ship the same page; the monitor hides Controls.

Responses carry an ETag (hash of the gzipped asset) and `Cache-Control:
no-cache`, so the browser revalidates and gets `304` until the firmware
changes.

## Polling and the comms watchdog

The page polls `getArtisanData` every second, `getControl` every 5 s (Croaster
PID only) and `getDeviceInfo` every 30 s. Like any client, those polls feed
Croaster PID's comms watchdog: while the page is open, the watchdog does not
trip, even with Artisan closed.

The WebSocket and the poll timer run in a dedicated Web Worker. Browsers slow
timers on a hidden tab's page (Chrome to once a minute after five minutes),
which would trip the watchdog during a roast watched from another tab; worker
timers keep running.
When the tab becomes visible again the page asks for an immediate poll.

What still stops the polls, and so trips the watchdog: closing the tab or the
browser, the computer sleeping or its lid closing, losing WiFi, and iOS or
iPadOS Safari, which suspends the whole page in the background. Those trips
are the watchdog doing its job.

## Development

Open `web/index.html` from any static server and point it at a board:
`http://localhost:8000/?ws=ws://192.168.1.50:81&controls=1`.

## Tests

```bash
node --test croaster-webui/test/*.test.js          # roast math, CSV, themes, library and recipes, autotune panel, page checks
python3 -m unittest discover -s croaster-webui/test # asset pipeline
```

`roast.test.js` covers the least-squares RoR, turning-point detection, unit
conversion, axis ticks, clock format and CSV. `theme.test.js` covers colour
parsing, presets, validation, migration of saved values, import and export,
and WCAG contrast. `library.test.js` covers the profile format (limits, unknown
keys kept, newer versions refused, unit conversion), recipe validation with
the board's reasons, editor round trips and the behind-schedule check.
`autotune.test.js` covers the start confirm text, the step list, progress,
result and gain rows, refusal messages and the polling rate.
`web_check.test.js` parses every
script and checks that the page loads nothing from outside the board, that
every element id the script uses exists, that every form has a command, and
that risky commands ask first. `test_embed_assets.py` checks the output is
reproducible, the committed header is current and the page is under budget.
