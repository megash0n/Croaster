// Static checks of croaster-webui/web: syntax, no external resources, and every
// element id the scripts look up exists in index.html. Run: node --test croaster-webui/test/*.test.js
'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const WEB = path.join(__dirname, '..', 'web');
const read = (name) => fs.readFileSync(path.join(WEB, name), 'utf8');
const files = fs.readdirSync(WEB);
const scripts = files.filter((f) => f.endsWith('.js'));
const html = read('index.html');

test('every script parses', () => {
  for (const f of scripts) assert.doesNotThrow(() => new vm.Script(read(f), { filename: f }), f);
});

test('no external resources (no CDN, no web fonts)', () => {
  for (const f of files) {
    const text = read(f).replace(/http:\/\/www\.w3\.org\/2000\/svg/g, '');
    assert.doesNotMatch(text, /https?:\/\/(?!<)/, f + ' references an external URL');
    assert.doesNotMatch(text, /@import|@font-face|url\(/, f + ' loads a stylesheet, font or image');
  }
});

test('files referenced by index.html and app.js exist', () => {
  const refs = [...html.matchAll(/(?:src|href)="(?!data:)([^"#?]+)"/g)].map((m) => m[1]);
  refs.push(...[...read('app.js').matchAll(/new Worker\('([^']+)'\)/g)].map((m) => m[1]));
  assert.ok(refs.length >= 4);
  for (const r of refs) assert.ok(files.includes(r), r + ' is missing from web/');
});

test('ids used by app.js exist in index.html', () => {
  const ids = new Set([...html.matchAll(/\sid="([^"]+)"/g)].map((m) => m[1]));
  const used = [...read('app.js').matchAll(/\$\('#([A-Za-z0-9_-]+)/g)].map((m) => m[1]);
  assert.ok(used.length > 20);
  for (const id of used) assert.ok(ids.has(id), '#' + id + ' is not in index.html');
});

test('every form command in index.html has a payload in app.js', () => {
  const app = read('app.js');
  const sends = [...html.matchAll(/data-send="([^"]+)"/g)].map((m) => m[1]);
  for (const s of sends) assert.match(app, new RegExp("case '" + s + "':"), s);
});

test('data-cmd buttons carry valid JSON commands', () => {
  for (const m of html.matchAll(/data-cmd='([^']+)'/g)) {
    const cmd = JSON.parse(m[1]);
    assert.ok('command' in cmd, m[1]);
  }
});

test('risky commands sit behind a confirm step', () => {
  for (const name of ['restartesp', 'erase', 'dummyToggle']) {
    const re = new RegExp('data-cmd=\'\\{"command":"' + name + '"\\}\' data-confirm="');
    assert.match(html, re, name);
  }
  assert.match(html, /data-send="wifiConnect" data-confirm="/);
  assert.match(html, /data-cmd='\{"command":"stop"\}' data-confirm="[^"]+">Heater off</);
  assert.match(html, /data-cmd='\{"command":"stop"\}' data-confirm="[^"]+">Stop autotune</);
});

test('STOP starts disabled and acts only through a confirm', () => {
  const app = read('app.js');
  assert.match(html, /<button [^>]*id="stop"[^>]* disabled[ >]/);
  assert.match(app, /\$\('#stop'\)\.addEventListener\('click', stopRoast\)/);
  const body = app.slice(app.indexOf('function stopRoast()'), app.indexOf('function renderEvents('));
  assert.match(body, /if \(!R\.stopAvailable\(recording\(\), S\.data, S\.recipe\)\) return;/);
  assert.ok(body.indexOf('confirmThen(') >= 0 && body.indexOf('confirmThen(') < body.indexOf("send({ command: 'stop' })"),
    'stop is sent from inside the confirm callback');
  assert.match(app, /stop\.disabled = !R\.stopAvailable\(recording\(\), S\.data, S\.recipe\);/);
});

test('samples are recorded only after an explicit start', () => {
  const app = read('app.js');
  const add = app.slice(app.indexOf('function addSample('), app.indexOf('function addEvent('));
  assert.match(add, /^function addSample\(d, at\) \{\s+if \(!recording\(\)\) return;/);
  assert.match(read('app.js'), /active: false/); // a new or reset session does not record
});

test('Monitoring is the first tab and the landing view; Start sits beside STOP', () => {
  const app = read('app.js');
  const tabs = [...html.matchAll(/data-tab="(\w+)"/g)].map((m) => m[1]);
  assert.deepEqual(tabs, ['monitoring', 'live', 'controls', 'roasts', 'beans']);
  assert.match(html, /<section id="tab-monitoring" data-panel="monitoring">/);
  assert.match(html, /<section id="tab-live" data-panel="live" hidden>/);
  assert.match(app, /tab: 'monitoring',/);
  assert.match(app, /tab = 'monitoring';\n\s+S\.tab = tab;/); // unknown or missing hash
  const bar = html.slice(html.indexOf('<div class="top-actions">'), html.indexOf('</header>'));
  assert.ok(bar.indexOf('id="new-roast"') >= 0 && bar.indexOf('id="new-roast"') < bar.indexOf('id="stop"'));
  assert.match(bar, /id="end-roast" hidden disabled/);
});

test('Start sends nothing to the board; event marks send only roastEvent', () => {
  const app = read('app.js');
  const start = app.slice(app.indexOf('function startRoast('), app.indexOf('function endRoast('));
  const mark = app.slice(app.indexOf('function markEvent('), app.indexOf('function startRoast('));
  for (const body of [start, mark]) assert.doesNotMatch(body, /send\(|postMessage\(/);
  assert.match(mark, /^function markEvent\(code\) \{\s+if \(!R\.eventAllowed\(S\.session, code\)\) return;/);
  // addEvent tells the board's roast clock (recipe event and time triggers) and nothing else.
  const add = app.slice(app.indexOf('function addEvent('), app.indexOf('}', app.indexOf('function addEvent(')));
  assert.match(add, /sendEvent\(ev\.code\)/);
  const ev = app.slice(app.indexOf('function sendEvent('), app.indexOf('function firedText('));
  assert.deepEqual([...ev.matchAll(/command: '(\w+)'/g)].map((m) => m[1]), ['roastEvent', 'roastEvent']);
});

test('the page sends commands only from user actions', () => {
  // Every send() call site. Loading the page or switching views reaches none of
  // them, except the read-only libraryList (link open, Controls tab) and roastList
  // (link open, Roasts tab), getRecipe
  // (the data payload's recipe.seq changed) and getAutotune (link open, status
  // change, timer while a run or its results are pending); the worker's polls (getArtisanData,
  // getControl, getDeviceInfo) are read-only too. A new command needs an entry here.
  const app = read('app.js');
  const calls = app.split('\n').filter((l) => /\bsend\(/.test(l) && !/function send\(/.test(l)).map((l) => l.trim());
  assert.deepEqual(calls, [
    "send({ command: 'recipeStart' });",
    "send({ command: 'stop' });",
    'send({ command: { auxFans: values } });',
    'send({ command: { [key]: value } }); // one command per click',
    'send({ command: { profile: steps } });',
    "send({ command: 'roastList' }, true);",
    "send({ command: 'roastSave', id, roast: rec }, true);",
    "send({ command: 'roastSave', roast: rec }, true);",
    "const fetch = (intent) => () => { rec.getIntent[m.id] = intent; send({ command: 'roastGet', id: m.id }, true); };",
    "send({ command: 'roastDelete', id: m.id });",
    "send({ command: 'beanList' }, true);",
    "btn('Edit', () => { bs.getIntent[b.id] = 'edit'; send({ command: 'beanGet', id: b.id }, true); }, !!busy, no);",
    "send({ command: 'beanDelete', id: b.id });",
    "send(id ? { command: 'beanSave', id, bean: rec } : { command: 'beanSave', bean: rec }, true);",
    "send({ command: 'roastGet', id }, true);",
    "send({ command: 'libraryList' });",
    "send(Object.assign({ command: 'librarySave', profile }, extra));",
    "if (r.recipe) send({ command: 'getRecipe' });",
    `btn('Load to board', '', () => { send({ command: 'libraryLoad', id: m.id }); libStatus('Loading "' + m.name + '"…'); }, !m.valid);`,
    "btn('Edit', 'ghost', () => { lib.getIntent[m.id] = 'edit'; send({ command: 'libraryGet', id: m.id }); });",
    "btn('Export', 'ghost', () => { lib.getIntent[m.id] = 'export'; send({ command: 'libraryGet', id: m.id }); });",
    "send({ command: 'libraryDelete', id: m.id });",
    "send({ command: 'recipeLoad', profile: res.profile });",
    "send({ command: 'getRecipe' });",
    "else if (r.seq !== S.recipeSeq) { S.recipeSeq = r.seq; send({ command: 'getRecipe' }); }",
    "send(undo ? { command: 'roastEvent', event: code, undo: true } : { command: 'roastEvent', event: code });",
    "send({ command: 'getAutotune' }, true); // polled: kept out of the reply log",
    "send(fanText ? { command: 'autotune', fanTest: box.checked } : { command: 'autotune' });",
    'send({ command: { autotune: choice } });',
    'send(cmd);',
    'send({ command: payload });',
    "$('#rl-abort').addEventListener('click', () => confirmThen('Abort the recipe? No further steps run; the heater and fan stay as they are.', () => send({ command: 'recipeAbort' })));",
    "$('#rl-skip').addEventListener('click', () => send({ command: 'recipeSkip' }));",
    "$('#rl-apply').addEventListener('click', () => { if (S.recipe) send({ command: 'recipeConfirm', step: S.recipe.prompt }); });",
    "$('#rl-dismiss').addEventListener('click', () => { if (S.recipe) send({ command: 'recipeSkip', step: S.recipe.prompt }); });",
  ]);
  const worker = read('link.worker.js');
  const sent = [...worker.matchAll(/send\(\{ command: '(\w+)'/g)].map((m) => m[1]);
  assert.deepEqual(sent, ['getArtisanData', 'getControl', 'getDeviceInfo']);
});

test('the shared modules app.js uses load before it', () => {
  const app = read('app.js');
  for (const [global, file] of [['Roast', 'roast.js'], ['ProfileLibrary', 'library.js'], ['Autotune', 'autotune.js'], ['RampShape', 'ramp.js'], ['Records', 'records.js']]) {
    assert.ok(app.includes('window.' + global), file);
    assert.ok(html.indexOf('<script src="' + file + '"></script>') >= 0 && html.indexOf(file) < html.indexOf('app.js'), file);
  }
});

test('theme.js loads in <head>, before first paint and before app.js', () => {
  const head = html.slice(0, html.indexOf('</head>'));
  assert.match(head, /<script src="theme\.js"><\/script>/);
  assert.ok(html.indexOf('theme.js') < html.indexOf('app.js'));
});

test('app.css fallbacks match the default scheme, and every colour token is used', () => {
  const T = require(path.join(WEB, 'theme.js'));
  const css = read('app.css');
  const rootBlock = css.slice(css.indexOf(':root {'), css.indexOf('}', css.indexOf(':root {')));
  const def = T.fromPreset(T.DEFAULT_PRESET);
  const vars = T.cssVars(def).vars;
  for (const k of Object.keys(vars)) {
    const m = rootBlock.match(new RegExp('\\s' + k.replace(/[-]/g, '\\-') + ':\\s*([^;]+);'));
    assert.ok(m, k + ' has no fallback in :root');
    assert.equal(m[1].trim(), vars[k], k);
  }
  const body = css.slice(css.indexOf('}', css.indexOf(':root {')));
  for (const t of T.TOKENS) assert.match(body, new RegExp('var\\(--' + t.key + '\\)'), '--' + t.key + ' is never used');
});

test('the page takes its WebSocket URL from Link.socketUrl (wss://<host>/ws over https)', () => {
  const app = read('app.js');
  assert.ok(html.indexOf('src="link.js"') > 0 && html.indexOf('src="link.js"') < html.indexOf('src="app.js"'));
  assert.match(app, /L\.socketUrl\(location, S\.info\.wsPort, query\.get\('ws'\)\)/);
  assert.doesNotMatch(app, /'wss?:\/\/'/);
});

test('the socket is paused only through Link.mayPause or on pagehide, and Clear roast sends nothing', () => {
  const app = read('app.js');
  assert.match(app, /if \(L\.mayPause\(recording\(\), S\.info\.controls, S\.data\)\) pause\(\);/);
  assert.equal((app.match(/\bpause\(\);/g) || []).length, 2);
  const clear = app.slice(app.indexOf('function clearRoast('), app.indexOf('function endRoast('));
  assert.doesNotMatch(clear, /send\(|postMessage/);
});

test('autotune starts only from the start dialog, and results change only from Save, Cancel or Discard', () => {
  const app = read('app.js');
  const start = app.slice(app.indexOf('function startAutotune('), app.indexOf('function decideAutotune('));
  assert.match(start, /dlg\.onclose = \(\) => \{\s+if \(dlg\.returnValue !== 'ok'\) return;\s+send\(fanText \? \{ command: 'autotune', fanTest: box\.checked \} : \{ command: 'autotune' \}\);/);
  assert.equal((start.match(/\bsend\(/g) || []).length, 1);
  assert.match(html, /<input type="checkbox" id="at-fan-test" checked>/); // fan test on by default
  assert.doesNotMatch(html, /data-cmd='\{"command":"autotune"\}'/);
  const dlg = app.slice(app.indexOf('function showAutotuneDialog('), app.indexOf('function renderAutotune('));
  assert.match(dlg, /returnValue === 'save'\) decideAutotune\('apply'\)/);
  assert.match(dlg, /returnValue === 'cancel'\) decideAutotune\('discard'\)/);
  const calls = [...app.matchAll(/decideAutotune\('(\w+)'\)/g)].map((m) => m[1]);
  assert.deepEqual(calls, ['apply', 'discard', 'discard']);
  assert.match(html, /<button value="save" autofocus>Save<\/button>/);
});

test('Fake readings button turns green from the data flag', () => {
  assert.match(html, /<button[^>]*id="dummy-toggle"[^>]*data-cmd='\{"command":"dummyToggle"\}'/);
  const app = read('app.js');
  assert.match(app, /S\.dummy = R\.dummyState\(msg\.data\)/);
  assert.match(app, /\$\('#dummy-toggle'\)/);
  assert.match(app, /classList\.toggle\('on', fake\)/);
  assert.match(read('app.css'), /\.row-buttons button\.on \{[^}]*background: var\(--ok\)/);
});

test('Ref overlays a saved roast: read with roastGet, drawn dashed from its charge', () => {
  const app = read('app.js');
  const css = read('app.css');
  assert.match(html, /id="ref-btn" data-needs="controls" hidden/);
  assert.match(app, /intent === 'ref'\) setReference\(/);
  assert.match(app, /class: 'ref-bt'/);
  assert.match(app, /x\(t0 \+ p\.t\)/); // reference t counts from its charge
  assert.match(css, /\.chart \.ref-bt \{[^}]*stroke-dasharray/);
});

test('coffee-ref.json: ISO country codes, regions of listed countries, 12 described processes', () => {
  const ref = JSON.parse(read('coffee-ref.json'));
  const names = ref.countries.map(([n]) => n);
  assert.equal(new Set(names).size, names.length);
  for (const [, iso] of ref.countries) assert.match(iso, /^[A-Z]{2}$/);
  for (const c of Object.keys(ref.regions)) assert.ok(names.includes(c), c);
  assert.equal(ref.processes.length, 12);
  for (const [n, d] of ref.processes) assert.ok(n && d && d.length < 100, n);
  assert.match(html, /id="bf-name" maxlength="40" required/);
});
