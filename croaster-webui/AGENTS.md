# croaster-webui — rules for the browser page

The page is served by the board from `src/webui_assets.h`, which
`embed_assets.py` generates from `web/`. Read `README.md` here for the file
list and the asset pipeline.

## Editing `web/`
- After any change in `web/`, run `python3 embed_assets.py` and commit the
  regenerated `src/webui_assets.h` with the sources. `--check` fails when they
  differ. The page must stay under the 100 KB gzipped budget; note the new
  size in the decision.
- Plain scripts, no build step or framework. Shared logic lives in its own
  file as a UMD module (`roast.js`, `library.js`, `autotune.js`, `ramp.js`):
  pure functions, no DOM, loaded by `index.html` before `app.js` and
  required by the node tests. `app.js` owns the DOM and the connection.
- Rules the board also enforces (recipe limits, error reasons, ramp shapes,
  the roaster model's rate-of-rise limit) are ported from
  `croaster-control/src/core`; change both sides together and keep the
  board's wording for reasons.
- Styling uses the theme custom properties set by `theme.js` (`--bg`, `--bt`,
  ...); no fixed colours. Dialogs and editors fit a 412 px wide phone screen
  and work with touch (pointer events).
- Temperatures inside the modules are °C; convert at the edges with the
  recipe's or the display's unit.

## Tests
- `node --test croaster-webui/test/*.test.js` from the repo root: one test
  file per module, `web_check.test.js` for page-wide rules (script order,
  command call sites).
- `python3 test/test_embed_assets.py` for the asset pipeline.
- Visual changes: open `web/index.html` from a local HTTP server at 412 px
  wide and check the browser console is clean (the board-only
  `info.json` request fails off the board).
