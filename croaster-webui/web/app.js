// Croaster web UI: page logic. The WebSocket and polling live in link.worker.js;
// this file keeps the roast session, draws the chart and wires the controls.
(function () {
  'use strict';
  const R = window.Roast;
  const L = window.Link;
  const TH = window.Theme;
  const P = window.ProfileLibrary;
  const A = window.Autotune;
  const RS = window.RampShape;
  const $ = (sel, el) => (el || document).querySelector(sel);
  const $$ = (sel, el) => Array.from((el || document).querySelectorAll(sel));
  const SVGNS = 'http://www.w3.org/2000/svg';

  const KEY_SESSION = 'croaster.session';
  const KEY_PROFILE = 'croaster.profile';
  const KEY_THEME = 'croaster.theme';
  const KEY_ROR = 'croaster.rorSmooth';
  const KEY_TP = 'croaster.autoTp';
  const KEY_RECIPE = 'croaster.recipe';
  const KEY_LAG = 'croaster.recipeLag'; // + ".F" / ".C": behind-schedule threshold in that unit
  const LIB_TIMEOUT_MS = 4000;
  const BEHIND_HOLD_MS = 20000; // heater at 100 % this long before the behind-schedule warning
  const MAX_SAMPLES = 4 * 3600; // 4 h at 1 Hz
  const PENDING_STALE_MS = 4000;
  const PENDING_DROP_MS = 10000;
  const STOP_CONFIRM_MS = 5000; // the board should report Manual and heater 0 within this after "stop"
  const RESUME_GAP_MS = 10 * 60 * 1000; // a saved roast older than this is not resumed on load
  const INTERPOLATIONS = ['linear', 'ease-in', 'ease-out', 'ease-in-out'];
  const EVENT_SHORT = { CHARGE: 'CH', TP: 'TP', DRY_END: 'DE', FC_START: 'FCs', FC_END: 'FCe',
    SC_START: 'SCs', SC_END: 'SCe', DROP: 'DR' };

  // Labels for the values the board sends. Keys not listed are shown under their own name.
  const LABELS = {
    name: 'Device name', BT: 'Bean temp (BT)', ET: 'Environment temp (ET)', DBT: 'BT rate of rise',
    DET: 'ET rate of rise', cBT: 'BT correction', cET: 'ET correction', temp: 'Unit', interval: 'Broadcast interval (s)',
    timer: 'Uptime (s)', BurnerVal: 'Heater (%)', FanVal: 'Fan (%)', FanAssist: 'Fan assist (%)', SV: 'Setpoint', Mode: 'Mode', trip: 'Trip',
    Amb: 'Cold junction', Target: 'PID input', autotune: 'Autotune', pidKp: 'PID Kp', pidKi: 'PID Ki',
    pidKd: 'PID Kd', wifiStrength: 'WiFi RSSI (dBm)', dummy: 'Fake readings', loopStackFree: 'Loop stack free (bytes)',
    mode: 'Mode', pidInput: 'PID input', setpoint: 'Setpoint', burner: 'Heater (%)', fan: 'Fan (%)',
    'cooldownFan.stop': 'Cooldown fan after stop (%)', 'cooldownFan.trip': 'Cooldown fan after trip (%)',
    'watchdog.enabled': 'Comms watchdog', 'watchdog.timeout': 'Watchdog timeout (s)', 'pid.kp': 'PID Kp',
    'pid.ki': 'PID Ki', 'pid.kd': 'PID Kd', tuned: 'Gains tuned', mainsOk: 'Mains detected',
    profileElapsed: 'Profile elapsed (s)', ambient: 'Cold junction',
    ver: 'Firmware version', ip: 'IP address', ssid: 'WiFi network', board: 'Board', darkMode: 'Dark screen',
    brightness: 'Brightness (%)',
  };
  const ALWAYS_LISTED = ['BT', 'ET', 'DBT', 'DET']; // left out of the telemetry while a probe has no reading

  const S = {
    info: { controls: false, wsPort: 81, board: '' },
    link: 'connecting',
    name: '',
    data: null, dataAt: 0,
    dummy: null, // fake readings state from the last data reply on this connection (null: not reported)
    control: null, controlAt: 0,
    device: null, deviceAt: 0,
    stats: null, // connection figures from the worker (Monitoring > Connection)
    paused: false, // socket closed while the page is hidden
    uptime: null, restarts: 0, // board uptime (telemetry `timer`, s since boot) and restarts seen from it
    session: null,
    rorSmooth: localStorage.getItem(KEY_ROR) === '1',
    autoTp: localStorage.getItem(KEY_TP) !== '0',
    pending: {},
    tab: 'monitoring',
    cursorT: null,
    dirty: true,
    lastSaveMs: 0,
    lastAutotune: null,
    at: null, atAskedAt: 0, atError: '', atShownFor: null, // getAutotune reply; results dialog shown for
    lastTrip: null,
    stopCheck: null, // {at, warned} while waiting for the board to report the stop
    profile: loadJson(KEY_PROFILE) || [{ duration: 60, setpoint: '', interpolation: 'linear', fanValue: '' }],
    lib: { state: 'unknown', list: [], askedAt: 0, used: null, total: null, max: null, base: null, imported: null, getIntent: {} },
    recipeRows: (loadJson(KEY_RECIPE) || {}).rows || [],
    recipeMode: (loadJson(KEY_RECIPE) || {}).mode === 'guided' ? 'guided' : 'auto',
    recipeCharge: (loadJson(KEY_RECIPE) || {}).charge || '', // charge temperature as typed, page unit
    recipeAt: 0, // Date.now() of the last getRecipe reply (ramp elapsed is as of then)
    rampWarn: [], // [{i, k, node}] ramp feasibility notes in the recipe editor
    recipe: null, recipeSeq: null, // the board's getRecipe reply and the data "recipe.seq" it matches
    behind: { state: null, warn: false, lagC: NaN },
  };
  let worker = null;

  // ---------- storage ----------
  function loadJson(key) {
    try { return JSON.parse(localStorage.getItem(key)); } catch (_) { return null; }
  }
  function saveJson(key, value) {
    try { localStorage.setItem(key, JSON.stringify(value)); return true; } catch (_) { return false; }
  }
  function newSession() {
    return { startedAt: Date.now(), unit: null, samples: [], events: [], active: false, endedAt: null };
  }
  function saveSession(force) {
    const now = Date.now();
    if (!force && now - S.lastSaveMs < 10000) return;
    S.lastSaveMs = now;
    if (!saveJson(KEY_SESSION, S.session)) $('#session-info').textContent = 'Browser storage full: download the CSV.';
  }

  // ---------- helpers ----------
  const isNum = (v) => typeof v === 'number' && isFinite(v);
  const unit = () => (S.data && S.data.temp) || S.session.unit || '';
  const unitLabel = () => (unit() === 'K' ? 'K' : unit() ? '°' + unit() : '');
  function fmt(v, digits) {
    return isNum(v) ? v.toFixed(digits === undefined ? 1 : digits) : '—';
  }
  function eventOf(code) {
    return S.session.events.find((e) => e.code === code);
  }
  function chargeT() {
    const e = eventOf('CHARGE');
    return e ? e.t : null;
  }
  function nowT() {
    return (Date.now() - S.session.startedAt) / 1000;
  }
  function endT() {
    return S.session.endedAt ? (S.session.endedAt - S.session.startedAt) / 1000 : nowT();
  }
  const recording = () => R.isRecording(S.session);
  function lastSample() {
    const ss = S.session.samples;
    return ss.length ? ss[ss.length - 1] : null;
  }
  function rorAt(i) {
    const s = S.session.samples[i];
    if (!s) return null;
    if (!S.rorSmooth) return isNum(s.dbt) ? s.dbt : null;
    if (s.sr === undefined) s.sr = R.smoothedRor(S.session.samples, i, 'bt', 30);
    return s.sr;
  }
  function el(tag, attrs, text) {
    const e = document.createElement(tag);
    for (const k in attrs || {}) e.setAttribute(k, attrs[k]);
    if (text !== undefined) e.textContent = text;
    return e;
  }
  function svgEl(tag, attrs) {
    const e = document.createElementNS(SVGNS, tag);
    for (const k in attrs) e.setAttribute(k, attrs[k]);
    return e;
  }

  // ---------- link ----------
  function send(payload, quiet) {
    if (!worker) return;
    worker.postMessage({ type: 'send', payload });
    if (!quiet) logReply('out', '→ ' + JSON.stringify(payload));
  }

  function onWorker(e) {
    const m = e.data;
    if (m.type === 'status') {
      const opened = m.state === 'open' && S.link !== 'open';
      S.link = m.state;
      if (m.state !== 'open' || opened) S.dummy = null; // wait for the board's next data reply
      S.dirty = true;
      if (opened) { requestLibrary(); S.recipeSeq = null; if (S.info.controls) askAutotune(); }
    } else if (m.type === 'stats') {
      S.stats = m;
      if (S.tab === 'monitoring') S.dirty = true;
    } else if (m.type === 'sent' && !m.ok) {
      logReply('err', 'not sent: not connected');
    } else if (m.type === 'message') {
      onMessage(m.msg, m.polled, m.at);
    }
  }

  function onMessage(msg, polled, at) {
    if (!msg || typeof msg !== 'object') return;
    if (msg.text !== undefined) { logReply('in', '← ' + msg.text); return; }
    if (msg.data && msg.data.type === 'preferences') { logReply('in', '← ' + JSON.stringify(msg)); return; }
    if (msg.data && typeof msg.data === 'object') {
      if (msg.name) S.name = msg.name;
      S.data = msg.data;
      S.dummy = R.dummyState(msg.data);
      S.dataAt = at;
      watchUptime(msg.data.timer);
      watchStatus(msg.data);
      checkStop(msg.data);
      if (S.info.controls) watchRecipe(msg.data);
      if (polled) addSample(msg.data, at);
      S.dirty = true;
      return;
    }
    if (msg.command === 'getControl') { S.control = msg.response || {}; S.controlAt = at; S.dirty = true; return; }
    if (msg.command === 'getDeviceInfo') { S.device = msg.response || {}; S.deviceAt = at; S.dirty = true; return; }
    if (typeof msg.command === 'string' && /^library/.test(msg.command)) {
      onLibrary(msg.command, msg.response || {});
      if (msg.command !== 'libraryList') logReply(msg.response && msg.response.ok ? 'in' : 'err', '← ' + msg.command + (msg.response && msg.response.ok ? '' : ': ' + (msg.response || {}).error));
      return;
    }
    if (typeof msg.command === 'string' && (/^recipe/.test(msg.command) || msg.command === 'getRecipe' || msg.command === 'roastEvent')) {
      onRecipe(msg.command, msg.response || {});
      if (msg.command !== 'getRecipe') logReply(msg.response && msg.response.ok ? 'in' : 'err', '← ' + JSON.stringify(msg));
      return;
    }
    if (msg.command === 'getAutotune' || msg.command === 'autotune') {
      onAutotune(msg.command, msg.response || {});
      if (msg.command === 'autotune') logReply(msg.response && msg.response.ok ? 'in' : 'err', '← ' + JSON.stringify(msg));
      return;
    }
    if (msg.command === 'profile') {
      const r = msg.response || {};
      $('#profile-status').textContent = r.loaded ? 'Loaded ' + r.steps + ' steps.' :
        'Not loaded: the board rejected the profile (1 to 32 steps; each needs a duration up to 3600 s, a setpoint, and fan 0 to 100 % if set).';
    }
    logReply('in', '← ' + JSON.stringify(msg));
  }

  // `timer` is the board's uptime; a drop means it restarted (power, reset line, crash or watchdog).
  function watchUptime(t) {
    if (!isNum(t)) return;
    if (S.uptime !== null && t + 2 < S.uptime) {
      S.restarts++;
      logReply('err', 'board restarted: uptime went from ' + R.formatClock(S.uptime) + ' to ' + R.formatClock(t));
    }
    S.uptime = t;
  }

  // Autotune results and trips arrive as telemetry fields; surface their changes in the reply log.
  function watchStatus(d) {
    if (d.autotune !== undefined && d.autotune !== S.lastAutotune) {
      if (S.lastAutotune !== null) {
        const bad = /^(need|failed|aborted|autotune required)/.test(d.autotune);
        logReply(bad ? 'err' : 'in', 'autotune: ' + d.autotune);
      }
      S.lastAutotune = d.autotune;
      if (S.info.controls) askAutotune();
    }
    if (d.trip !== undefined && d.trip !== S.lastTrip) {
      if (d.trip) logReply('err', 'trip: ' + d.trip);
      S.lastTrip = d.trip;
    }
  }

  // "stop" has no reply of its own: report what the next readings show.
  function expectStop() {
    S.stopCheck = { at: Date.now(), warned: false };
  }
  function checkStop(d) {
    const c = S.stopCheck;
    if (!c) return;
    if (R.stopConfirmed(d)) {
      logReply('in', 'stop: board reports heater 0 %, Manual' + (isNum(d.FanVal) ? ', fan ' + d.FanVal + ' %' : ''));
      S.stopCheck = null;
    } else if (!c.warned && Date.now() - c.at > STOP_CONFIRM_MS) {
      c.warned = true;
      logReply('err', 'stop: not confirmed, board reports heater ' + fmt(d.BurnerVal, 0) + ' %, mode ' +
        (d.Mode || '?') + '. Use Heater off in Controls, or cut power.');
      S.dirty = true;
    }
  }

  function logReply(kind, text) {
    const log = $('#reply-log');
    const li = el('li', { class: kind }, new Date().toLocaleTimeString() + ' ' + text);
    log.prepend(li);
    while (log.children.length > 40) log.lastChild.remove();
  }

  // ---------- session ----------
  function addSample(d, at) {
    if (!recording()) return; // readings still update the tiles; only a started roast is recorded
    const ses = S.session;
    if (d.temp && ses.unit && d.temp !== ses.unit) {
      R.convertSession(ses, d.temp);
      ses.samples.forEach((s) => { delete s.sr; });
    }
    if (d.temp) ses.unit = d.temp;
    const s = {
      t: Math.round((at - ses.startedAt) / 100) / 10,
      bt: isNum(d.BT) ? d.BT : null, et: isNum(d.ET) ? d.ET : null,
      dbt: isNum(d.DBT) ? d.DBT : null, det: isNum(d.DET) ? d.DET : null,
      h: isNum(d.BurnerVal) ? d.BurnerVal : null, f: isNum(d.FanVal) ? d.FanVal : null,
      sv: isNum(d.SV) ? d.SV : null,
      at: isNum(d.atTarget) ? d.atTarget : null,
      mode: (d.Mode || '') + (d.trip ? ' trip:' + d.trip : ''),
    };
    ses.samples.push(s);
    if (ses.samples.length > MAX_SAMPLES) ses.samples.shift();

    const charge = chargeT();
    if (S.autoTp && charge !== null && !eventOf('TP') && !eventOf('DROP')) {
      const tp = R.detectTurningPoint(ses.samples, charge, { riseBy: ses.unit === 'F' ? 2 : 1 });
      if (tp) addEvent({ code: 'TP', t: tp.t, bt: tp.bt, auto: true });
    }
    saveSession(false);
  }

  function addEvent(ev) {
    S.session.events.push(ev);
    sendEvent(ev.code); // the board's roast clock and event triggers (recipes)
    S.dirty = true;
    renderEvents();
    saveSession(true);
  }

  function markEvent(code) {
    if (!R.eventAllowed(S.session, code)) return;
    const last = lastSample();
    const live = S.data && isNum(S.data.BT) ? S.data.BT : null;
    addEvent({ code, t: last ? last.t : nowT(), bt: last ? last.bt : live });
  }

  // Start: begins a recording in this page only (samples from now, so the preheat
  // shows; the roast clock starts at Charge). Nothing is sent to the board. Asks
  // first when it would clear an earlier roast.
  function startRoast() {
    const go = () => {
      S.session = newSession();
      S.session.active = true;
      saveSession(true);
      render();
    };
    if (recording()) return;
    if (S.session.samples.length || S.session.events.length) {
      confirmThen('Start a new roast? The previous samples and events are cleared (download the CSV first to keep them).', go);
    } else {
      go();
    }
  }

  // Clear roast: drops the kept samples and events of an ended roast from this browser.
  // Nothing is sent to the board.
  function clearRoast() {
    if (recording() || (!S.session.samples.length && !S.session.events.length)) return;
    confirmThen('Clear the last roast from this browser? Its samples and events are deleted (download the CSV first to keep them). The board is not changed.', () => {
      S.session = newSession();
      saveSession(true);
      render();
    });
  }

  function endRoast() {
    if (R.endSession(S.session, Date.now())) saveSession(true);
    render();
  }

  function stopRoast() {
    if (!recording()) return;
    const c = S.control && S.control.cooldownFan;
    const fan = c && isNum(c.stop) ? ' (' + c.stop + ' %)' : '';
    confirmThen('Stop the roast? The heater turns off, the board switches to Manual (ending PID, profile or autotune), ' +
      'the fan runs at the cooldown speed' + fan + ', and recording ends.', () => {
      if (S.link !== 'open') {
        logReply('err', 'STOP not sent: not connected. The board keeps its current heater and mode.');
        return;
      }
      send({ command: 'stop' });
      expectStop();
      endRoast();
    });
  }

  function renderEvents() {
    const box = $('#event-buttons');
    if (!box.children.length) {
      for (const ev of R.EVENTS) {
        const b = el('button', { type: 'button', 'data-code': ev.code, class: ev.code.toLowerCase() }, ev.label);
        b.addEventListener('click', () => markEvent(ev.code));
        box.append(b);
      }
    }
    for (const b of $$('button', box)) {
      const code = b.dataset.code;
      b.classList.toggle('marked', !!eventOf(code));
      b.disabled = !R.eventAllowed(S.session, code);
    }
    $('#undo-event').disabled = !recording() || !S.session.events.length;
    $('#end-after-drop').hidden = !recording() || !eventOf('DROP');

    const list = $('#event-list');
    list.textContent = '';
    const evs = S.session.events.slice().sort((a, b) => a.t - b.t);
    const t0 = chargeT();
    if (!evs.length) list.append(el('li', { class: 'muted' }, 'No events yet'));
    for (const e of evs) {
      const label = (R.EVENTS.find((x) => x.code === e.code) || { label: e.code }).label;
      const at = t0 === null ? '' : R.formatClock(e.t - t0);
      const temp = isNum(e.bt) ? ' · ' + e.bt.toFixed(1) + ' ' + unitLabel() : '';
      list.append(el('li', {}, label + ' ' + at + temp + (e.auto ? ' (auto)' : '')));
    }
  }

  // ---------- rendering ----------
  function render() {
    S.dirty = false;
    const d = S.data || {};
    $('#device-name').textContent = S.name || (S.link === 'open' ? 'connected' : 'not connected');
    const link = $('#link');
    link.dataset.state = S.link;
    $('#link-text').textContent = S.link;
    for (const u of $$('[data-unit]')) u.textContent = unitLabel();

    // Tiles. Heater, fan, setpoint and mode only show when the board reports them.
    $('#t-bt').textContent = fmt(d.BT);
    $('#t-et').textContent = fmt(d.ET);
    const ss = S.session.samples;
    const ror = S.rorSmooth ? rorAt(ss.length - 1) : d.DBT;
    $('#t-ror').textContent = fmt(ror);
    $('#ror-src').textContent = S.rorSmooth ? 'smoothed' : 'device';
    $('#t-ror-et').textContent = isNum(d.DET) ? 'ET ' + d.DET.toFixed(1) + ' ' + unitLabel() + '/min' : unitLabel() + '/min';
    $('#t-heat').textContent = fmt(d.BurnerVal, 0);
    $('#t-fan').textContent = fmt(d.FanVal, 0);
    const assist = A.assistText(d);
    $('#t-fan-assist').hidden = !assist;
    $('#t-fan-assist').textContent = assist;
    $('#t-sv').textContent = fmt(d.SV);
    $('#t-mode').textContent = d.Mode || '—';
    const stop = $('#stop');
    stop.disabled = !recording();
    stop.title = recording() ? 'End the roast: heater off, Manual, cooldown fan' : 'Available while a roast is recording';
    const start = $('#new-roast');
    start.disabled = recording();
    start.title = recording() ? 'A roast is recording: STOP or End roast ends it' : 'Start recording a roast';
    const end = $('#end-roast');
    end.hidden = S.info.controls;
    end.disabled = !recording();
    end.title = recording() ? 'Stop recording; the board is not changed' : 'Available while a roast is recording';
    const dummy = $('#dummy-toggle');
    const fake = S.link === 'open' && S.dummy === true;
    dummy.classList.toggle('on', fake);
    dummy.setAttribute('aria-pressed', String(fake));
    dummy.title = fake ? 'Fake readings are on: charts and Artisan show fake values' : '';
    $('#t-trip').textContent = d.trip ? 'TRIP ' + d.trip : (d.Target ? 'input ' + d.Target : '');
    $('.tile.mode').classList.toggle('tripped', !!d.trip);
    for (const e of $$('[data-field]')) {
      if (e.classList.contains('stepper')) continue;
      e.hidden = S.data ? d[e.dataset.field] === undefined : !S.info.controls;
    }

    const banner = $('#trip-banner');
    banner.hidden = !d.trip;
    if (d.trip) banner.textContent = 'TRIP: ' + d.trip + '. Heater is off. Fix the cause, then use Clear trip in Controls.';

    renderClock();
    renderEvents();
    if (S.tab === 'live') { renderChart(); }
    if (S.tab === 'monitoring') renderMonitoring();
    if (S.tab === 'controls') { renderControls(); renderLibrary(); renderAutotune(); }
    if (S.tab === 'live') renderRecipeLive();
    let info;
    if (ss.length) {
      const charge = chargeT();
      const drop = eventOf('DROP');
      const last = !recording() && charge !== null ? ', roast ' + R.formatClock((drop ? drop.t : endT()) - charge) : '';
      info = ss.length + ' samples since ' + new Date(S.session.startedAt).toLocaleTimeString() +
        (S.session.unit ? ' (' + unitLabel() + ')' : '') + (recording() ? '' : ', recording ended' + last);
    } else {
      info = recording() ? 'Recording, waiting for the first sample' : 'Not recording. Start begins a recording.';
    }
    $('#session-info').textContent = info;
    $('#clear-roast').disabled = recording() || (!ss.length && !S.session.events.length);
  }

  function renderClock() {
    const st = R.sessionState(S.session);
    const charge = chargeT();
    const drop = eventOf('DROP');
    let label = 'Idle', value = '--:--';
    if (st === 'roasting') {
      label = 'Roast';
      value = R.formatClock(nowT() - charge);
    } else if (st === 'cooling') {
      label = 'Cooling · roast ' + R.formatClock(drop.t - charge);
      value = R.formatClock(nowT() - drop.t);
    } else if (st === 'preheat') {
      label = 'Preheat';
      value = R.formatClock(nowT());
    }
    $('#phase').textContent = label;
    $('#roast-clock').textContent = value;
  }

  function kvRows(table, obj, listed) {
    table.textContent = '';
    if (!obj) {
      const tr = el('tr');
      tr.append(el('td', {}, 'Waiting for the board…'));
      table.append(tr);
      return;
    }
    const flat = [];
    const walk = (o, prefix) => {
      for (const k of Object.keys(o)) {
        const v = o[k];
        if (v && typeof v === 'object' && !Array.isArray(v)) walk(v, prefix + k + '.');
        else flat.push([prefix + k, v]);
      }
    };
    walk(obj, '');
    for (const k of listed || []) if (!(k in obj)) flat.push([k, undefined]);
    for (const [k, v] of flat) {
      const tr = el('tr');
      tr.append(el('td', {}, LABELS[k] || k));
      let text;
      if (v === undefined) text = 'no reading';
      else if (v === null) text = 'none';
      else if (typeof v === 'boolean') text = v ? 'yes' : 'no';
      else if (v === '') text = '(none)';
      else text = String(v);
      tr.append(el('td', {}, text));
      tr.append(el('td', { class: 'key' }, k));
      table.append(tr);
    }
  }

  function age(at) {
    return at ? 'updated ' + ((Date.now() - at) / 1000).toFixed(0) + ' s ago' : '';
  }

  function renderMonitoring() {
    const top = S.data ? Object.assign({ name: S.name }, S.data) : null;
    kvRows($('#mon-data'), top, S.data ? ALWAYS_LISTED : null);
    kvRows($('#mon-control'), S.control);
    kvRows($('#mon-info'), S.device);
    $('#age-data').textContent = age(S.dataAt);
    $('#age-control').textContent = age(S.controlAt);
    $('#age-info').textContent = age(S.deviceAt);
    renderConnection();
  }

  // Connection card: what the worker sees of the WebSocket. The board does not report its client count.
  function renderConnection() {
    const t = $('#mon-link');
    const st = S.stats;
    t.textContent = '';
    const secs = (at) => (at ? ((Date.now() - at) / 1000).toFixed(0) + ' s ago' : '—');
    const rows = [
      ['Page', location.origin],
      ['WebSocket URL', st ? st.url : '—'],
      ['State', S.link + (st && st.detail && st.detail !== st.url ? ' (' + st.detail + ')' : '')],
      ['Last message', st ? secs(st.lastRxAt) : '—'],
      ['Round trip (getArtisanData)', st && isNum(st.rttMs) ? st.rttMs + ' ms' : '—'],
      ['Connected since', st && S.link === 'open' ? secs(st.openedAt) : '—'],
      ['Connections opened', st ? String(st.opens) : '—'],
      ['Connection attempts', st ? String(st.attempts) : '—'],
      ['Last close', st && st.closedAt ? [st.closeCode ? L.closeText(st.closeCode) : '', st.closeReason, secs(st.closedAt)]
        .filter(Boolean).join(', ') : '—'],
      ['Board WiFi signal', S.data && isNum(S.data.wifiStrength) ? S.data.wifiStrength + ' dBm' : 'not reported'],
      ['Board uptime', S.uptime !== null ? R.formatClock(S.uptime) : '—'],
      ['Board restarts seen', String(S.restarts)],
      ['Board clients', 'not reported by the board'],
    ];
    if (location.protocol === 'https:' && !new URLSearchParams(location.search).get('ws')) {
      rows.push(['Note', 'Page loaded over https: the proxy must forward wss://' + location.host + '/ws to port ' +
        S.info.wsPort + ' on the board.']);
    }
    for (const [k, v] of rows) {
      const tr = el('tr');
      tr.append(el('td', {}, k));
      tr.append(el('td', {}, v));
      t.append(tr);
    }
  }

  // ---------- chart ----------
  // The rest of a running setpoint ramp, [{t (session s), v (session unit)}]: drawn dashed ahead of SV.
  function setpointPlan() {
    const rec = S.recipe;
    if (!recording() || !rec || rec.state !== 'running') return [];
    const n = rampNow(rec);
    if (!n || n.rp.kind !== 'setpoint' || n.elapsed >= n.rp.duration) return [];
    const ses = S.session;
    const now = (Date.now() - ses.startedAt) / 1000;
    const u0 = n.elapsed / n.rp.duration, out = [];
    const u = ses.unit || 'C';
    for (let i = 0; i <= 30; i++) {
      const uu = u0 + (1 - u0) * i / 30;
      const c = n.rp.from + (n.rp.to - n.rp.from) * RS.fraction(n.rates, uu);
      out.push({ t: now + (uu - u0) * n.rp.duration, v: P.convert(c, 'C', u) });
    }
    return out;
  }

  function renderChart() {
    const svg = $('#chart');
    const W = svg.clientWidth, H = svg.clientHeight;
    if (!W || !H) return;
    const ss = S.session.samples;
    const charge = chargeT();
    const M = { l: 46, r: 46, t: 20, b: 22 };
    const t0 = charge !== null ? charge : (ss.length ? ss[0].t : 0);
    let xMin = ss.length ? ss[0].t - t0 : 0;
    if (charge !== null) xMin = Math.max(xMin, -180); // keep at most 3 min of preheat in view
    const xLast = ss.length ? ss[ss.length - 1].t - t0 : 0;
    const plan = setpointPlan();
    const xMax = Math.max(xLast, xMin + (charge !== null ? 720 : 300), plan.length ? plan[plan.length - 1].t - t0 : 0);
    const xTicks = R.niceTicks(xMin / 60, xMax / 60, Math.max(3, Math.floor(W / 90)));
    const x = (t) => M.l + ((t - t0) - xMin) / (xMax - xMin) * (W - M.l - M.r);

    const u = unit();
    const base = u === 'C' ? [40, 230] : u === 'K' ? [313, 503] : [100, 450];
    let tLo = base[0], tHi = base[1], rLo = 0, rHi = u === 'F' ? 30 : 18;
    const vis = ss.filter((s) => s.t - t0 >= xMin);
    for (const s of vis) {
      for (const v of [s.bt, s.et, s.sv, s.at]) if (isNum(v)) { tLo = Math.min(tLo, v); tHi = Math.max(tHi, v); }
    }
    for (const p of plan) { tLo = Math.min(tLo, p.v); tHi = Math.max(tHi, p.v); }
    const first = ss.length - vis.length;
    for (let i = first; i < ss.length; i++) {
      const r = rorAt(i);
      if (isNum(r)) { rLo = Math.min(rLo, Math.max(r, -20)); rHi = Math.max(rHi, Math.min(r, 120)); }
    }
    const yT = R.niceTicks(tLo, tHi, Math.max(3, Math.floor(H / 60)));
    const yR = R.niceTicks(rLo, rHi, yT.ticks.length - 1);
    const yt = (v) => H - M.b - (v - yT.lo) / (yT.hi - yT.lo) * (H - M.t - M.b);
    const yr = (v) => H - M.b - (Math.max(yR.lo, Math.min(yR.hi, v)) - yR.lo) / (yR.hi - yR.lo) * (H - M.t - M.b);

    svg.textContent = '';
    svg.setAttribute('viewBox', '0 0 ' + W + ' ' + H);
    for (const v of yT.ticks) {
      svg.append(svgEl('line', { class: 'grid', x1: M.l, x2: W - M.r, y1: yt(v), y2: yt(v) }));
      const tx = svgEl('text', { x: M.l - 6, y: yt(v) + 4, 'text-anchor': 'end' });
      tx.textContent = v;
      svg.append(tx);
    }
    const rStep = (yR.hi - yR.lo) / Math.max(1, yT.ticks.length - 1);
    yT.ticks.forEach((_, i) => {
      const v = Math.round((yR.lo + i * rStep) * 10) / 10;
      const tx = svgEl('text', { x: W - M.r + 6, y: yr(v) + 4 });
      tx.textContent = v;
      svg.append(tx);
    });
    for (const m of xTicks.ticks) {
      const px = x(t0 + m * 60);
      if (px < M.l - 1 || px > W - M.r + 1) continue;
      svg.append(svgEl('line', { class: 'grid', x1: px, x2: px, y1: M.t, y2: H - M.b }));
      const tx = svgEl('text', { x: px, y: H - 6, 'text-anchor': 'middle' });
      tx.textContent = R.formatClock(m * 60);
      svg.append(tx);
    }

    const stepN = Math.max(1, Math.ceil(vis.length / (W * 1.5)));
    const line = (cls, get, ymap) => {
      let d = '', pen = false;
      for (let i = first; i < ss.length; i += stepN) {
        const v = get(i);
        if (!isNum(v)) { pen = false; continue; }
        d += (pen ? 'L' : 'M') + x(ss[i].t).toFixed(1) + ' ' + ymap(v).toFixed(1);
        pen = true;
      }
      if (d) svg.append(svgEl('path', { class: cls, d }));
    };
    line('sv', (i) => ss[i].sv, yt);
    if (plan.length) svg.append(svgEl('path', { class: 'sv-plan', d: plan.map((p, n) => (n ? 'L' : 'M') + x(p.t).toFixed(1) + ' ' + yt(p.v).toFixed(1)).join('') }));
    line('at-target', (i) => ss[i].at, yt);
    line('et', (i) => ss[i].et, yt);
    line('bt', (i) => ss[i].bt, yt);
    line('ror', rorAt, yr);
    if (!S.rorSmooth) line('ror-et', (i) => ss[i].det, yr);
    $('#key-ror-et').hidden = S.rorSmooth;

    // Labels of events close together go on the next row down instead of overlapping.
    const rowEnd = [];
    for (const e of S.session.events.slice().sort((a, b) => a.t - b.t)) {
      const px = x(e.t);
      if (px < M.l) continue;
      const g = svgEl('g', { class: 'event' });
      g.append(svgEl('line', { x1: px, x2: px, y1: M.t, y2: H - M.b }));
      const label = EVENT_SHORT[e.code] + (isNum(e.bt) ? ' ' + Math.round(e.bt) : '');
      let row = rowEnd.findIndex((end) => end < px);
      if (row < 0) row = rowEnd.length;
      rowEnd[row] = px + 3 + label.length * 7;
      const tx = svgEl('text', { x: px + 3, y: M.t - 6 + row * 13 });
      tx.textContent = label;
      g.append(tx);
      svg.append(g);
    }

    if (S.cursorT !== null && ss.length) {
      const px = x(S.cursorT);
      svg.append(svgEl('line', { class: 'cursor', x1: px, x2: px, y1: M.t, y2: H - M.b }));
    }
    renderStrips(x, W, M, first);
    S.chartX = { x, t0, W, M, xMin, xMax };
  }

  function renderStrips(x, W, M, first) {
    const svg = $('#strips');
    if (svg.hidden) return;
    const H = svg.clientHeight;
    if (!H) return;
    const ss = S.session.samples;
    svg.textContent = '';
    svg.setAttribute('viewBox', '0 0 ' + W + ' ' + H);
    const y = (v) => H - 4 - Math.max(0, Math.min(100, v)) / 100 * (H - 8);
    for (const v of [0, 50, 100]) {
      svg.append(svgEl('line', { class: 'grid', x1: M.l, x2: W - M.r, y1: y(v), y2: y(v) }));
      const tx = svgEl('text', { x: M.l - 6, y: y(v) + 4, 'text-anchor': 'end' });
      tx.textContent = v + '%';
      svg.append(tx);
    }
    const step = (cls, key) => {
      let d = '', last = null;
      for (let i = first; i < ss.length; i++) {
        const v = ss[i][key];
        if (!isNum(v)) { last = null; continue; }
        const px = x(ss[i].t).toFixed(1), py = y(v).toFixed(1);
        d += last === null ? 'M' + px + ' ' + py : 'H' + px + (v !== last ? 'V' + py : '');
        last = v;
      }
      if (d) svg.append(svgEl('path', { class: cls, d }));
    };
    step('fan', 'f');
    step('heat', 'h');
  }

  function onChartPointer(ev) {
    const c = S.chartX;
    const ss = S.session.samples;
    if (!c || !ss.length) return;
    const rect = $('#chart').getBoundingClientRect();
    const px = ev.clientX - rect.left;
    const t = c.t0 + c.xMin + (px - c.M.l) / (c.W - c.M.l - c.M.r) * (c.xMax - c.xMin);
    let best = 0;
    for (let i = 0; i < ss.length; i++) if (Math.abs(ss[i].t - t) < Math.abs(ss[best].t - t)) best = i;
    const s = ss[best];
    S.cursorT = s.t;
    const parts = [R.formatClock(s.t - c.t0), 'BT ' + fmt(s.bt), 'ET ' + fmt(s.et), 'RoR ' + fmt(rorAt(best))];
    if (isNum(s.h)) parts.push('heater ' + s.h + '%');
    if (isNum(s.f)) parts.push('fan ' + s.f + '%');
    if (isNum(s.sv)) parts.push('SV ' + fmt(s.sv));
    if (isNum(s.at)) parts.push('test point ' + fmt(s.at));
    $('#readout').textContent = parts.join('  ');
    renderChart();
  }

  // ---------- controls ----------
  function renderControls() {
    const d = S.data || {};
    for (const st of $$('.stepper')) {
      const key = st.dataset.key;
      const p = S.pending[key];
      const now = Date.now();
      const reported = d[st.dataset.field];
      if (p && (reported === p.value || now - p.at > PENDING_DROP_MS)) delete S.pending[key];
      const q = S.pending[key];
      st.classList.toggle('pending', !!q && now - q.at <= PENDING_STALE_MS);
      st.classList.toggle('stale', !!q && now - q.at > PENDING_STALE_MS);
      st.title = q && now - q.at > PENDING_STALE_MS ? 'The board reports ' + reported + ': it may have limited the value' : '';
      $('output', st).textContent = q ? q.value : fmt(reported, 0);
    }
    $('#autotune-status').textContent = d.autotune || '—';

    // Fill the forms from the board's values, except fields the user is editing.
    const c = S.control || {};
    fill('setpoint', { value: d.SV });
    fill('pidInput', { value: d.Target });
    fill('pid', { kp: d.pidKp, ki: d.pidKi, kd: d.pidKd });
    if (c.cooldownFan) fill('cooldownFan', c.cooldownFan);
    if (c.watchdog) fill('watchdog', c.watchdog);
    const fa = c.fanAssist;
    $('form[data-send="fanAssist"]').hidden = !fa;
    if (fa) {
      fill('fanAssist', { enabled: fa.enabled });
      $('#fan-assist-note').textContent = 'Raises the fan above your setting, by up to ' + (isNum(fa.max) ? fa.max : 20) +
        ' %, when BT runs above the setpoint with the heater already off, to shed heat from the beans. ' +
        (fa.available ? 'Uses the saved autotune fan test.' : 'Inactive until an autotune fan test is saved.');
    }
    fill('tempUnit', { value: d.temp });
    fill('interval', { value: d.interval });
    fill('correctionBt', { value: d.cBT });
    fill('correctionEt', { value: d.cET });
  }

  function fill(name, values) {
    const form = $('form[data-send="' + name + '"]');
    if (!form || form.dataset.touched) return;
    for (const k in values) {
      const input = form.elements[k];
      const v = values[k];
      if (!input || v === undefined || v === null || document.activeElement === input) continue;
      if (input.type === 'checkbox') input.checked = !!v;
      else input.value = typeof v === 'number' ? Math.round(v * 1000) / 1000 : v;
    }
  }

  function formPayload(form) {
    const f = form.elements;
    const n = (k) => Number(f[k].value);
    switch (form.dataset.send) {
      case 'setpoint': return { setpoint: n('value') };
      case 'pidInput': return { pidInput: f.value.value };
      case 'pid': return { pid: { kp: n('kp'), ki: n('ki'), kd: n('kd') } };
      case 'cooldownFan': return { cooldownFan: { stop: n('stop'), trip: n('trip') } };
      case 'watchdog': return { watchdog: { enabled: f.enabled.checked, timeout: Math.round(n('timeout')) } };
      case 'fanAssist': return { fanAssist: f.enabled.checked };
      case 'tempUnit': return { tempUnit: f.value.value };
      case 'interval': return { interval: Math.round(n('value')) };
      case 'correctionBt': return { correctionBt: n('value') };
      case 'correctionEt': return { correctionEt: n('value') };
      case 'wifiConnect': return { wifiConnect: { ssid: f.ssid.value, pass: f.pass.value } };
    }
    return null;
  }

  function confirmThen(text, action, okLabel) {
    if (!text) { action(); return; }
    const dlg = $('#confirm');
    $('#confirm-text').textContent = text;
    $('#confirm button[value="ok"]').textContent = okLabel || 'Confirm';
    dlg.returnValue = '';
    dlg.onclose = () => { if (dlg.returnValue === 'ok') action(); };
    dlg.showModal();
  }

  function stepperClick(st, step) {
    const key = st.dataset.key;
    const reported = (S.data || {})[st.dataset.field];
    const base = S.pending[key] ? S.pending[key].value : (isNum(reported) ? reported : 0);
    const value = Math.max(Number(st.dataset.min), Math.min(Number(st.dataset.max), Math.round(base + step)));
    S.pending[key] = { value, at: Date.now() };
    send({ command: { [key]: value } }); // one command per click
    renderControls();
  }

  // ---------- profile editor ----------
  function renderProfile() {
    const body = $('#profile tbody');
    body.textContent = '';
    S.profile.forEach((step, i) => {
      const tr = el('tr');
      tr.append(el('td', { class: 'num' }, String(i + 1)));
      const cell = (input) => { const td = el('td'); td.append(input); tr.append(td); return input; };
      const num = (key, attrs) => {
        const inp = el('input', Object.assign({ type: 'number', step: 'any' }, attrs));
        inp.value = step[key];
        inp.addEventListener('input', () => { step[key] = inp.value; saveJson(KEY_PROFILE, S.profile); });
        return cell(inp);
      };
      num('duration', { min: '1' });
      num('setpoint', {});
      const sel = el('select');
      for (const o of INTERPOLATIONS) sel.append(el('option', { value: o }, o));
      sel.value = step.interpolation || 'linear';
      sel.addEventListener('change', () => { step.interpolation = sel.value; saveJson(KEY_PROFILE, S.profile); });
      cell(sel);
      num('fanValue', { min: '0', max: '100', placeholder: 'keep' });
      const td = el('td', { class: 'row-buttons' });
      const btn = (label, title, fn) => {
        const b = el('button', { type: 'button', class: 'ghost', title }, label);
        b.addEventListener('click', () => { fn(); saveJson(KEY_PROFILE, S.profile); renderProfile(); });
        td.append(b);
      };
      btn('↑', 'Move up', () => { if (i > 0) S.profile.splice(i - 1, 0, S.profile.splice(i, 1)[0]); });
      btn('↓', 'Move down', () => { if (i < S.profile.length - 1) S.profile.splice(i + 1, 0, S.profile.splice(i, 1)[0]); });
      btn('✕', 'Remove', () => { S.profile.splice(i, 1); });
      tr.append(td);
      body.append(tr);
    });
    $('#profile-add').disabled = S.profile.length >= 32;
  }

  function loadProfile() {
    const steps = [];
    for (const [i, s] of S.profile.entries()) {
      const duration = Number(s.duration), setpoint = Number(s.setpoint);
      if (!(duration > 0 && duration <= 3600) || s.setpoint === '' || !isFinite(setpoint)) {
        $('#profile-status').textContent = 'Step ' + (i + 1) + ': needs a duration above 0 and up to 3600 s, and a setpoint.';
        return;
      }
      const step = { duration, setpoint, interpolation: s.interpolation || 'linear' };
      if (s.fanValue !== '' && s.fanValue !== undefined) {
        const fan = Number(s.fanValue);
        if (!(fan >= 0 && fan <= 100)) {
          $('#profile-status').textContent = 'Step ' + (i + 1) + ': fan must be 0 to 100 %.';
          return;
        }
        step.fanValue = fan;
      }
      steps.push(step);
    }
    if (!steps.length || steps.length > 32) {
      $('#profile-status').textContent = 'A profile has 1 to 32 steps.';
      return;
    }
    $('#profile-status').textContent = 'Sent ' + steps.length + ' steps, waiting for the board…';
    send({ command: { profile: steps } });
  }

  // ---------- device library ----------
  // Controls > Device library: profiles kept on the board (library.js has the
  // format). The board answers libraryList; no answer within LIB_TIMEOUT_MS
  // (monitor firmware, or a build without the library) shows it as unavailable.
  const edUnit = () => unit() || 'C';

  function libStatus(text, isError) {
    const p = $('#lib-status');
    p.textContent = text;
    p.classList.toggle('err', !!isError);
  }

  function requestLibrary() {
    if (!S.info.controls || S.link !== 'open') return;
    if (S.lib.state !== 'ready') S.lib.state = 'waiting';
    S.lib.askedAt = Date.now();
    send({ command: 'libraryList' });
    setTimeout(() => { S.dirty = true; }, LIB_TIMEOUT_MS + 100); // shows "unavailable" without new data
  }

  // Schedule rows, or null when every setpoint is blank (the profile is a recipe only).
  function scheduleRows() {
    return S.profile.every((s) => s.setpoint === '' || s.setpoint === undefined || s.setpoint === null) ? null : S.profile;
  }

  function editorProfile() {
    const name = $('#lib-name').value;
    const rows = scheduleRows();
    const recipe = S.recipeRows.length ? P.recipeFromEditor(S.recipeRows, S.recipeMode, S.recipeCharge) : null;
    if (!rows && !recipe) return { error: 'empty' };
    return P.fromEditor(rows, name, $('#lib-notes').value, edUnit(), S.lib.base, recipe);
  }

  // Saves `profile`, asking first when a profile with its name exists.
  function saveToDevice(profile, done) {
    const same = P.findByName(S.lib.list, profile.name);
    const go = (extra) => {
      send(Object.assign({ command: 'librarySave', profile }, extra));
      libStatus('Saving "' + profile.name + '"…');
      if (done) done();
    };
    if (same) confirmThen('Replace "' + same.name + '" on the board with this version?', () => go({ id: same.id, overwrite: true }));
    else go({});
  }

  function openInEditors(p) {
    S.profile = Array.isArray(p.steps) ? P.toEditor(p, edUnit()) : [{ duration: 60, setpoint: '', interpolation: 'linear', fanValue: '' }];
    S.recipeRows = p.recipe ? P.recipeToEditor(p.recipe, p.unit, edUnit()) : [];
    S.recipeMode = p.recipe && p.recipe.mode === 'guided' ? 'guided' : 'auto';
    S.recipeCharge = p.recipe && isNum(p.recipe.chargeTemp) ? P.convert(p.recipe.chargeTemp, p.unit, edUnit()) : '';
    S.lib.base = JSON.parse(JSON.stringify(p));
    $('#lib-name').value = p.name || '';
    $('#lib-notes').value = p.notes || '';
    saveJson(KEY_PROFILE, S.profile);
    saveRecipeRows();
    renderProfile();
    renderRecipe();
  }

  function downloadJson(name, obj) {
    const a = el('a', { download: P.fileName(name) });
    a.href = URL.createObjectURL(new Blob([JSON.stringify(obj, null, 2) + '\n'], { type: 'application/json' }));
    document.body.append(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(a.href), 1000);
  }

  function onLibrary(cmd, r) {
    const fail = (what) => libStatus(what + ': ' + P.message(r.error, r.reason) + (r.error === 'badRecipe' && r.reason ? '' : '.'), true);
    if (cmd === 'libraryList') {
      if (!r.ok) { S.lib.state = 'unavailable'; S.lib.list = []; return; }
      S.lib.state = 'ready';
      S.lib.list = r.profiles || [];
      S.lib.used = r.usedBytes; S.lib.total = r.totalBytes; S.lib.max = r.max;
      libStatus(S.lib.list.length ? '' : 'No profiles on the board yet. Save the editors or import a file.');
    } else if (cmd === 'libraryGet') {
      const intent = S.lib.getIntent[r.id];
      delete S.lib.getIntent[r.id];
      if (!r.ok) { fail('Not read'); return; }
      if (intent === 'export') { downloadJson(r.profile.name, r.profile); libStatus('Exported "' + r.profile.name + '".'); }
      else { openInEditors(r.profile); libStatus('Opened "' + r.profile.name + '" in the editors.'); }
    } else if (cmd === 'librarySave') {
      if (!r.ok) { fail('Not saved'); return; }
      libStatus(r.created ? 'Saved as a new profile.' : 'Saved over the existing profile.');
      requestLibrary();
    } else if (cmd === 'libraryDelete') {
      if (!r.ok) { fail('Not deleted'); return; }
      libStatus('Deleted.');
      requestLibrary();
    } else if (cmd === 'libraryLoad') {
      if (!r.ok) { fail('Not loaded'); return; }
      const parts = [];
      if (r.loaded) parts.push('schedule of ' + r.steps + ' steps (Start profile runs it)');
      if (r.recipe) parts.push('recipe (Run recipe on Live runs it)');
      libStatus(parts.length ? 'Loaded the ' + parts.join(' and the ') + '.' : 'The board did not load it.', !parts.length);
      if (r.recipe) send({ command: 'getRecipe' });
    }
    S.dirty = true;
  }

  function renderLibrary() {
    const lib = S.lib;
    if (lib.state === 'waiting' && Date.now() - lib.askedAt > LIB_TIMEOUT_MS) lib.state = 'unavailable';
    const ready = lib.state === 'ready';
    $('#lib-table').hidden = !ready || !lib.list.length;
    $('#lib-tools').hidden = !ready;
    if (lib.state === 'unavailable') libStatus('Device library unavailable: this firmware has none, or its storage did not mount.');
    else if (lib.state !== 'ready' && S.link !== 'open') libStatus('Not connected.');
    $('#lib-count').textContent = ready ? lib.list.length + ' of ' + (lib.max || P.MAX_PROFILES) +
      (isNum(lib.used) && isNum(lib.total) && lib.total ? ' · ' + Math.round(lib.used / 1024) + ' of ' + Math.round(lib.total / 1024) + ' KB' : '') : '';
    const busy = P.busyReason(S.data);
    for (const b of [$('#lib-save'), $('#lib-import-save')]) {
      b.disabled = !!busy;
      b.title = busy ? 'Not while ' + busy : '';
    }
    const body = $('#lib-list');
    const key = JSON.stringify(lib.list) + busy;
    if (body.dataset.key === key) return; // rebuild only on change, so focus and clicks survive
    body.dataset.key = key;
    body.textContent = '';
    for (const m of lib.list) {
      const tr = el('tr');
      tr.append(el('td', {}, m.name + (m.valid ? '' : ' (not valid)')));
      tr.append(el('td', {}, m.unit));
      tr.append(el('td', {}, m.steps ? m.steps + ' steps · ' + R.formatClock(m.duration) : '—'));
      tr.append(el('td', {}, m.recipeSteps ? m.recipeSteps + ' steps' : '—'));
      const td = el('td', { class: 'row-buttons' });
      const btn = (label, cls, fn, disabled, title) => {
        const b = el('button', { type: 'button', class: cls }, label);
        if (disabled) b.disabled = true;
        if (title) b.title = title;
        b.addEventListener('click', fn);
        td.append(b);
      };
      btn('Load to board', '', () => { send({ command: 'libraryLoad', id: m.id }); libStatus('Loading "' + m.name + '"…'); }, !m.valid);
      btn('Edit', 'ghost', () => { lib.getIntent[m.id] = 'edit'; send({ command: 'libraryGet', id: m.id }); });
      btn('Export', 'ghost', () => { lib.getIntent[m.id] = 'export'; send({ command: 'libraryGet', id: m.id }); });
      btn('Delete', 'ghost', () => confirmThen('Delete "' + m.name + '" from the board? Export it first to keep a copy.', () => {
        send({ command: 'libraryDelete', id: m.id });
      }), !!busy, busy ? 'Not while ' + busy : '');
      tr.append(td);
      body.append(tr);
    }
  }

  function importFile(file) {
    if (!file) return;
    if (file.size > 64 * 1024) { libStatus('Not imported: the file is larger than 64 KB.', true); return; }
    file.text().then((text) => {
      const r = P.parseFile(text);
      if (r.error) {
        const why = r.error === 'badRecipe' ? ' (' + P.recipeReason(JSON.parse(text).recipe, JSON.parse(text).unit) + ')' : '';
        libStatus('Not imported: ' + P.message(r.error) + why + '.', true);
        return;
      }
      S.lib.imported = r.profile;
      const p = r.profile;
      const parts = [];
      if (p.steps) parts.push(p.steps.length + ' schedule steps');
      if (p.recipe) parts.push(p.recipe.steps.length + ' recipe steps');
      $('#lib-imported-text').textContent = '"' + p.name + '": ' + parts.join(', ') + '.';
      $('#lib-imported').hidden = false;
    }, () => libStatus('Could not read the file.', true));
  }

  // ---------- recipe ----------
  // Controls > Recipe edits the steps; Live shows the board's run (getRecipe).
  const TRIGGER_LABELS = { time: 'Time', bt: 'BT', et: 'ET', ror: 'RoR', event: 'Event' };
  const OP_LABELS = { gte: '\u2265', lte: '\u2264', near: '\u2248' };
  const ACTION_LABELS = { power: 'Heater %', fan: 'Fan %', setpoint: 'Setpoint (PID)', hold: 'Hold ramp',
    cool: 'Cool (stop)', end: 'End recipe', note: 'Note' };

  function saveRecipeRows() {
    saveJson(KEY_RECIPE, { mode: S.recipeMode, rows: S.recipeRows, charge: S.recipeCharge });
  }

  function recipeStatus(text, isError) {
    const p = $('#recipe-status');
    p.textContent = text;
    p.classList.toggle('err', !!isError);
  }

  function renderRecipe() {
    const body = $('#recipe-table tbody');
    body.textContent = '';
    $('#recipe-mode').value = S.recipeMode;
    const charge = $('#recipe-charge');
    if (document.activeElement !== charge) charge.value = S.recipeCharge;
    S.rampWarn = [];
    const changed = () => { saveRecipeRows(); checkRamps(); };
    const select = (options, labels, value, onChange) => {
      const s = el('select');
      for (const o of options) s.append(el('option', { value: o }, labels[o] || o));
      s.value = value;
      s.addEventListener('change', () => { onChange(s.value); changed(); });
      return s;
    };
    const input = (attrs, value, onInput) => {
      const i = el('input', attrs);
      i.value = value === undefined || value === null ? '' : value;
      i.addEventListener('input', () => { onInput(i.value); changed(); });
      return i;
    };
    S.recipeRows.forEach((row, i) => {
      const tr = el('tr');
      tr.append(el('td', { class: 'num' }, String(i + 1)));
      const when = el('div', { class: 'when' });
      when.append(select(P.TRIGGERS, TRIGGER_LABELS, row.type, (v) => { row.type = v; renderRecipe(); }));
      if (row.type === 'event') {
        when.append(select(P.EVENTS, P.EVENT_LABELS, row.event || 'CHARGE', (v) => { row.event = v; }));
      } else {
        when.append(select(P.OPS, OP_LABELS, row.op || 'gte', (v) => { row.op = v; renderRecipe(); }));
        const isTime = row.type === 'time';
        when.append(input(isTime ? { type: 'text', placeholder: 'm:ss', 'aria-label': 'Time since Charge' } :
          { type: 'number', step: 'any', 'aria-label': 'Value' }, row.value, (v) => { row.value = v; }));
        when.append(el('span', { class: 'small muted' }, isTime ? 'since Charge' : row.type === 'ror' ? unitLabel() + '/min' : unitLabel()));
        if (row.op === 'near') {
          when.append(el('span', { class: 'small muted' }, '\u00b1'));
          when.append(input({ type: 'number', step: 'any', min: '0', placeholder: isTime ? '5' : '2', 'aria-label': 'Tolerance' },
            row.tol, (v) => { row.tol = v; }));
        }
      }
      const any = el('label', { class: 'small toggle', title: 'Armed from the start instead of after the step before it' });
      const cb = el('input', { type: 'checkbox' });
      cb.checked = !!row.anyOrder;
      cb.addEventListener('change', () => { row.anyOrder = cb.checked; changed(); });
      any.append(cb, ' any order');
      when.append(any);
      when.append(input({ type: 'text', maxlength: '40', placeholder: 'label', 'aria-label': 'Label' }, row.label, (v) => { row.label = v; }));
      const tdWhen = el('td');
      tdWhen.append(when);
      tr.append(tdWhen);

      const then = el('div', { class: 'then' });
      row.actions.forEach((a, k) => {
        const act = el('div', { class: 'act' });
        act.append(select(P.ACTIONS, ACTION_LABELS, a.type, (v) => { a.type = v; renderRecipe(); }));
        if (a.type === 'fan') {
          act.append(input({ type: 'number', min: '0', max: '100', step: '1', 'aria-label': ACTION_LABELS[a.type] }, a.value, (v) => { a.value = v; }));
        } else if (a.type === 'power' || a.type === 'setpoint') {
          const power = a.type === 'power';
          act.append(power ? input({ type: 'number', min: '0', max: '100', step: '1', 'aria-label': ACTION_LABELS[a.type] }, a.value, (v) => { a.value = v; }) :
            input({ type: 'number', step: 'any', 'aria-label': 'Setpoint' }, a.value, (v) => { a.value = v; }));
          act.append(el('span', { class: 'small muted' }, (power ? '%' : unitLabel()) + ', ramp s'));
          act.append(input({ type: 'number', min: '0', max: '3600', step: '1', placeholder: '0', 'aria-label': 'Ramp seconds' }, a.ramp, (v) => { a.ramp = v; }));
          act.append(rampShapeControl(a, power, changed));
        } else if (a.type === 'note') {
          act.append(input({ type: 'text', class: 'note', maxlength: '80', 'aria-label': 'Note' }, a.text, (v) => { a.text = v; }));
        }
        const rm = el('button', { type: 'button', class: 'ghost', title: 'Remove action' }, '\u2715');
        rm.disabled = row.actions.length <= 1;
        rm.addEventListener('click', () => { row.actions.splice(k, 1); changed(); renderRecipe(); });
        act.append(rm);
        then.append(act);
        if (a.type === 'setpoint') {
          const note = el('p', { class: 'small ramp-warn', role: 'status', hidden: '' });
          then.append(note);
          S.rampWarn.push({ i, k, node: note });
        }
      });
      const add = el('button', { type: 'button', class: 'ghost' }, '+ action');
      add.disabled = row.actions.length >= P.RECIPE.MAX_ACTIONS;
      add.addEventListener('click', () => { row.actions.push({ type: 'fan', value: '' }); changed(); renderRecipe(); });
      then.append(add);
      const tdThen = el('td');
      tdThen.append(then);
      tr.append(tdThen);

      const td = el('td', { class: 'row-buttons' });
      const btn = (label, title, fn) => {
        const b = el('button', { type: 'button', class: 'ghost', title }, label);
        b.addEventListener('click', () => { fn(); changed(); renderRecipe(); });
        td.append(b);
      };
      const rows = S.recipeRows;
      btn('\u2191', 'Move up', () => { if (i > 0) rows.splice(i - 1, 0, rows.splice(i, 1)[0]); });
      btn('\u2193', 'Move down', () => { if (i < rows.length - 1) rows.splice(i + 1, 0, rows.splice(i, 1)[0]); });
      btn('\u2715', 'Remove', () => { rows.splice(i, 1); });
      tr.append(td);
      body.append(tr);
    });
    $('#recipe-table').hidden = !S.recipeRows.length;
    $('#recipe-add').disabled = S.recipeRows.length >= P.RECIPE.MAX_STEPS;
    const empty = 'No recipe steps. Add step starts one with heater and fan.';
    if (!S.recipeRows.length) recipeStatus(empty);
    else if ($('#recipe-status').textContent === empty) recipeStatus('');
    const lag = $('#recipe-lag');
    if (document.activeElement !== lag) lag.value = lagSetting();
    checkRamps();
  }

  // Shape picker for a ramp: the shape list, a small preview of the path, and
  // the curve editor for a custom shape.
  function rampShapeControl(a, power, changed) {
    const box = el('span', { class: 'ramp-shape' });
    const sel = el('select', { 'aria-label': 'Ramp shape', title: 'How the rate of climb is spread over the ramp' });
    for (const sh of RS.SHAPES) sel.append(el('option', { value: sh }, RS.LABELS[sh]));
    sel.value = RS.SHAPES.includes(a.shape) ? a.shape : 'linear';
    const spark = svgEl('svg', { class: 'ramp-spark', viewBox: '0 0 48 20', 'aria-hidden': 'true' });
    const draw = () => {
      spark.textContent = '';
      const pts = RS.path(0, 1, 1, RS.rates(a.shape, a.curve), 24);
      spark.append(svgEl('path', { d: pts.map((p, n) => (n ? 'L' : 'M') + (2 + p.t * 44).toFixed(1) + ' ' + (18 - p.v * 16).toFixed(1)).join('') }));
    };
    const edit = el('button', { type: 'button', class: 'ghost', title: 'Edit the rate-of-climb curve' }, 'Edit');
    edit.hidden = sel.value !== 'custom';
    const open = () => openCurveEditor(a, power, () => { sel.value = a.shape; edit.hidden = a.shape !== 'custom'; draw(); changed(); });
    sel.addEventListener('change', () => {
      const before = a.shape || 'linear';
      a.shape = sel.value;
      if (a.shape === 'custom' && !RS.validCurve(a.curve)) a.curve = RS.rates(before === 'custom' ? 'linear' : before);
      edit.hidden = a.shape !== 'custom';
      draw();
      changed();
      if (a.shape === 'custom') open();
    });
    edit.addEventListener('click', open);
    draw();
    box.append(sel, spark, edit);
    return box;
  }

  // Touch editor for a custom ramp: 21 rates (0..100 in steps of 5) at 0, 5,
  // ... 100 % of the ramp time, drawn by dragging; the path it gives below.
  function openCurveEditor(a, power, done) {
    const dlg = $('#curve-dialog');
    let rates = RS.rates('custom', a.curve);
    const svg = $('#curve-edit');
    const prev = $('#curve-path');
    const W = 400, H = 180, M = { l: 30, r: 10, t: 10, b: 20 };
    const px = (i) => M.l + i / (RS.POINTS - 1) * (W - M.l - M.r);
    const py = (v) => H - M.b - v / 100 * (H - M.t - M.b);
    const to = Number(a.value);
    const seconds = Number(a.ramp) > 0 ? Number(a.ramp) : 0;
    const draw = () => {
      svg.textContent = '';
      for (const v of [0, 50, 100]) {
        svg.append(svgEl('line', { class: 'grid', x1: M.l, x2: W - M.r, y1: py(v), y2: py(v) }));
        const tx = svgEl('text', { x: M.l - 4, y: py(v) + 4, 'text-anchor': 'end' });
        tx.textContent = v;
        svg.append(tx);
      }
      for (const f of [0, 0.5, 1]) {
        const tx = svgEl('text', { x: px(f * (RS.POINTS - 1)), y: H - 4, 'text-anchor': f === 0 ? 'start' : f === 1 ? 'end' : 'middle' });
        tx.textContent = seconds ? P.clock(f * seconds) : Math.round(f * 100) + ' %';
        svg.append(tx);
      }
      svg.append(svgEl('path', { class: 'curve-line', d: rates.map((v, i) => (i ? 'L' : 'M') + px(i).toFixed(1) + ' ' + py(v).toFixed(1)).join('') }));
      rates.forEach((v, i) => svg.append(svgEl('circle', { class: 'curve-dot', cx: px(i), cy: py(v), r: 5 })));
      // Path preview, start to target: the start is the heater or setpoint when the step fires.
      prev.textContent = '';
      const pts = RS.path(0, 1, 1, rates, 40);
      prev.append(svgEl('line', { class: 'grid', x1: M.l, x2: W - M.r, y1: 70, y2: 70 }));
      prev.append(svgEl('line', { class: 'grid', x1: M.l, x2: W - M.r, y1: 10, y2: 10 }));
      prev.append(svgEl('path', { class: 'curve-path', d: pts.map((p, n) => (n ? 'L' : 'M') + (M.l + p.t * (W - M.l - M.r)).toFixed(1) + ' ' + (70 - p.v * 60).toFixed(1)).join('') }));
      const lo = svgEl('text', { x: M.l - 4, y: 74, 'text-anchor': 'end' });
      lo.textContent = power ? 'now' : 'start';
      const hi = svgEl('text', { x: M.l - 4, y: 14, 'text-anchor': 'end' });
      hi.textContent = isNum(to) && String(a.value).trim() !== '' ? to + (power ? ' %' : ' ' + unitLabel()) : 'target';
      prev.append(lo, hi);
      const pk = RS.peak(rates);
      $('#curve-text').textContent = 'Steepest at ' + Math.round(pk.at * 100) + ' % of the ramp, ' + pk.factor.toFixed(1) +
        '\u00d7 a linear ramp.';
    };
    const setAt = (ev) => {
      const r = svg.getBoundingClientRect();
      const x = (ev.clientX - r.left) / r.width * W, y = (ev.clientY - r.top) / r.height * H;
      const i = Math.max(0, Math.min(RS.POINTS - 1, Math.round((x - M.l) / (W - M.l - M.r) * (RS.POINTS - 1))));
      const v = RS.snap((H - M.b - y) / (H - M.t - M.b) * 100);
      // A fast drag skips points: fill the ones between in a straight line.
      const j = last === null ? i : last, lo = Math.min(i, j), hi = Math.max(i, j);
      const vj = last === null ? v : rates[j];
      for (let n = lo; n <= hi; n++) rates[n] = hi === lo ? v : RS.snap(vj + (v - vj) * (n - j) / (i - j));
      last = i;
      draw();
    };
    let drag = false, last = null;
    svg.onpointerdown = (ev) => { drag = true; last = null; svg.setPointerCapture(ev.pointerId); setAt(ev); ev.preventDefault(); };
    svg.onpointermove = (ev) => { if (drag) setAt(ev); };
    svg.onpointerup = svg.onpointercancel = () => { drag = false; };
    const presets = $('#curve-presets');
    presets.textContent = '';
    for (const sh of ['linear', 'normal', 'front', 'back']) {
      const b = el('button', { type: 'button', class: 'ghost', title: RS.HINTS[sh] }, RS.LABELS[sh]);
      b.addEventListener('click', () => { rates = RS.rates(sh); draw(); });
      presets.append(b);
    }
    $('#curve-title').textContent = (power ? 'Heater' : 'Setpoint') + ' ramp shape';
    draw();
    dlg.onclose = () => {
      if (dlg.returnValue !== 'ok') return;
      if (!rates.some((v) => v > 0)) rates = RS.rates('linear');
      a.shape = 'custom';
      a.curve = rates.slice();
      done();
    };
    dlg.returnValue = '';
    dlg.showModal();
  }

  // Feasibility notes under setpoint ramps, from the saved autotune model.
  function checkRamps() {
    if (!S.rampWarn.length) return;
    const saved = S.at && S.at.saved;
    const u = edUnit();
    const recipe = P.recipeFromEditor(S.recipeRows, S.recipeMode, S.recipeCharge);
    const found = saved ? RS.checkRecipe(recipe, u, saved, S.at.unit || u) : [];
    for (const w of S.rampWarn) {
      const hit = found.find((f) => f.step === w.i && f.action === w.k);
      w.node.hidden = !hit;
      w.node.textContent = hit ? RS.checkText(hit, u) : '';
    }
  }

  // "step 3: " for the first recipe row the board would refuse, so the message points at it.
  function stepHint(reason) {
    if (!reason) return '';
    for (let i = 0; i < S.recipeRows.length; i++) {
      const one = P.recipeFromEditor([S.recipeRows[i]], S.recipeMode);
      if (P.recipeReason(one, edUnit())) return 'step ' + (i + 1) + ': ';
    }
    return '';
  }

  function loadRecipe() {
    if (!S.recipeRows.length) { recipeStatus('Add a step first.', true); return; }
    const name = $('#lib-name').value.trim() || 'Recipe';
    const res = P.fromEditor(null, name, '', edUnit(), null, P.recipeFromEditor(S.recipeRows, S.recipeMode, S.recipeCharge));
    if (res.error) { recipeStatus('Not sent: ' + stepHint(res.reason) + (res.reason || P.message(res.error)) + '.', true); return; }
    send({ command: 'recipeLoad', profile: res.profile });
    recipeStatus('Sent ' + S.recipeRows.length + ' steps, waiting for the board…');
  }

  // Behind-schedule threshold, in the page unit (default 15 °F or 8 °C).
  function lagSetting() {
    const v = Number(localStorage.getItem(KEY_LAG + '.' + edUnit()));
    return v > 0 ? v : edUnit() === 'F' ? 15 : 8;
  }

  function onRecipe(cmd, r) {
    if (cmd === 'getRecipe') {
      S.recipe = r;
      S.recipeAt = Date.now();
    } else if (cmd === 'recipeLoad') {
      if (r.ok) recipeStatus('Loaded on the board' + (r.schedule ? ', with its schedule' : '') + '. Run recipe starts it.');
      else recipeStatus('Not loaded: ' + (r.reason || P.message(r.error)) + '.', true);
      send({ command: 'getRecipe' });
    } else if (!r.ok) {
      if (r.error === 'autotuneRunning') recipeStatus(A.refusal(r.error), true);
      const why = { autotuneRunning: 'autotune is running', empty: 'no recipe is loaded', running: 'a recipe is already running', notRunning: 'no recipe is running',
        noPrompt: 'that step is not waiting', badEvent: 'unknown event' }[r.error] || r.error;
      logReply('err', cmd + ': ' + why);
    }
    S.dirty = true;
  }

  // Data payloads carry a short recipe status; fetch the full one when it changes.
  function watchRecipe(d) {
    const r = d.recipe;
    if (!r) { if (S.recipe && S.recipe.state !== 'empty') S.recipe = null; S.recipeSeq = null; }
    else if (r.seq !== S.recipeSeq) { S.recipeSeq = r.seq; send({ command: 'getRecipe' }); }
    const u = d.temp || 'C';
    const b = P.behindSchedule(S.behind.state, {
      nowMs: Date.now(), follow: !!(r && r.state === 'running' && r.follow), heater: d.BurnerVal,
      btC: isNum(d.BT) ? P.toCelsius(d.BT, u) : NaN, setpointC: isNum(d.SV) ? P.toCelsius(d.SV, u) : NaN,
    }, { lagC: lagSetting() * (u === 'F' ? 5 / 9 : 1), holdMs: BEHIND_HOLD_MS });
    S.behind = b;
  }

  function sendEvent(code, undo) {
    if (!S.info.controls || S.link !== 'open') return;
    send(undo ? { command: 'roastEvent', event: code, undo: true } : { command: 'roastEvent', event: code });
  }

  function firedText(rec, f) {
    const u = edUnit();
    const step = P.describeStep(rec.recipe, f.step, rec.unit, u);
    const head = 'Step ' + (f.step + 1) + ' ' + (f.how === 'skipped' ? 'skipped' : f.how === 'missed' ? 'missed' : 'fired') +
      (isNum(f.at) ? ' at ' + P.clock(f.at) : ' before Charge');
    return head + ':' + step.slice(step.indexOf(':') + 1) + (f.how === 'prompted' ? ' (waiting)' : f.how === 'confirmed' ? ' (set)' : '');
  }

  // The ramp in progress, from getRecipe "ramp" (setpoint values in C).
  function rampNow(rec) {
    const rp = rec && rec.ramp;
    if (!rp || !isNum(rp.duration) || !(rp.duration > 0)) return null;
    const elapsed = Math.min(rp.duration, (isNum(rp.elapsed) ? rp.elapsed : 0) + (Date.now() - S.recipeAt) / 1000);
    return { rp, elapsed, rates: Array.isArray(rp.rates) && rp.rates.length === RS.POINTS ? rp.rates : RS.rates('linear') };
  }
  function rampLiveText(rec) {
    const n = rampNow(rec);
    if (!n) return '';
    const u = edUnit();
    const v = (x) => (n.rp.kind === 'power' ? Math.round(x) + ' %' : Math.round(P.convert(x, 'C', u)) + ' ' + unitLabel());
    const shape = RS.SHAPES.find((sh) => sh !== 'custom' && RS.PRESETS[sh].join() === n.rates.join()) || 'custom';
    return (n.rp.kind === 'power' ? 'Heater' : 'Setpoint') + ' ramp ' + v(n.rp.from) + ' \u2192 ' + v(n.rp.to) + ', ' +
      RS.LABELS[shape].toLowerCase() + ', ' + P.clock(n.elapsed) + ' of ' + P.clock(n.rp.duration) + '.';
  }

  function renderRecipeLive() {
    const box = $('#recipe-live');
    const rec = S.recipe;
    const show = S.info.controls && !!rec && rec.ok && rec.state !== 'empty';
    box.hidden = !show;
    if (!show) return;
    const u = edUnit();
    $('#rl-name').textContent = rec.name ? '· ' + rec.name : '';
    const states = { ready: 'loaded', running: 'running', done: 'finished', aborted: 'aborted' };
    $('#rl-state').textContent = (states[rec.state] || rec.state) +
      ((rec.state === 'aborted' || rec.state === 'done') && rec.reason ? ' (' + rec.reason + ')' : '') +
      (rec.mode === 'guided' ? ', guided' : '') +
      (isNum(rec.sinceCharge) ? '' : rec.state === 'running' ? ' · time steps wait for Charge' : '');
    const running = rec.state === 'running';
    $('#rl-start').disabled = running;
    $('#rl-skip').disabled = !running || rec.next < 0;
    $('#rl-abort').disabled = !running;
    $('#rl-next').textContent = running && rec.next >= 0 ? 'Next: ' + P.describeStep(rec.recipe, rec.next, rec.unit, u) :
      running ? 'No steps left in order.' : '';
    const prompt = $('#rl-prompt');
    prompt.hidden = !(running && rec.prompt >= 0);
    if (!prompt.hidden) $('#rl-prompt-text').textContent = P.describeStep(rec.recipe, rec.prompt, rec.unit, u);
    $('#rl-ramp').textContent = running ? rampLiveText(rec) : '';
    const list = $('#rl-fired');
    list.textContent = '';
    for (const f of (rec.fired || []).slice().reverse()) list.append(el('li', { class: f.how }, firedText(rec, f)));
    const warn = $('#rl-behind');
    warn.hidden = !(running && S.behind.warn);
    if (!warn.hidden) {
      const lag = S.behind.lagC * (u === 'F' ? 9 / 5 : 1);
      warn.textContent = 'Behind schedule: BT is ' + lag.toFixed(0) + ' ' + unitLabel() +
        ' under the setpoint with the heater at 100 %. The roaster cannot follow this curve.';
    }
  }

  // ---------- autotune ----------
  // getAutotune is read-only: asked on connect, when the telemetry status changes,
  // and on a timer while a run is going or its results wait for a choice.
  function askAutotune() {
    S.atAskedAt = Date.now();
    send({ command: 'getAutotune' }, true); // polled: kept out of the reply log
  }

  function onAutotune(cmd, r) {
    if (cmd === 'getAutotune') {
      S.at = r;
      checkRamps();
      const key = r.pending && r.recommended ? JSON.stringify(r.recommended) + (r.results || []).length : null;
      // Pending results open the dialog once per page load, including after a reload.
      if (key && key !== S.atShownFor) { S.atShownFor = key; showAutotuneDialog(); }
      if (!r.pending && $('#at-dialog').open) $('#at-dialog').close('closed');
    } else if (r.error) {
      S.atError = A.refusal(r.error);
    } else if (r.action) {
      S.atError = r.ok ? '' : 'No results waiting: they were already saved or discarded.';
      if (r.ok) logReply('in', r.action === 'apply' ? 'autotune: new gains saved' : 'autotune: results discarded');
      askAutotune();
    }
    S.dirty = true;
  }

  function startAutotune() {
    S.atError = '';
    if (S.recipe && S.recipe.state === 'running') { S.atError = A.refusal('recipeRunning'); S.dirty = true; return; }
    const dlg = $('#at-start-dialog');
    const box = $('#at-fan-test');
    const fanText = A.fanTestText(S.at);
    $('#at-fan-option').hidden = !fanText;
    $('#at-fan-text').textContent = fanText;
    box.checked = !!fanText;
    const text = () => { $('#at-start-text').textContent = A.confirmText(S.at, box.checked); };
    box.onchange = text;
    text();
    dlg.returnValue = '';
    dlg.onclose = () => {
      if (dlg.returnValue !== 'ok') return;
      send(fanText ? { command: 'autotune', fanTest: box.checked } : { command: 'autotune' });
    };
    dlg.showModal();
  }

  function fanTables(rep, wrap, summary, body, full) {
    const rows = A.fanRows(rep);
    $(wrap).hidden = !rows.length && !A.fanSummary(rep);
    $(summary).textContent = A.fanSummary(rep);
    const b = $(body);
    b.textContent = '';
    for (const r of rows) b.append(cells(el('tr'), full ? [r.fan, r.hold, r.dip, r.recovery, r.gain, r.shed] : [r.fan, r.hold, r.gain, r.shed]));
  }

  function decideAutotune(choice) {
    send({ command: { autotune: choice } });
  }

  function cells(tr, values) {
    for (const v of values) tr.append(el('td', {}, v));
    return tr;
  }

  function showAutotuneDialog() {
    const rep = S.at;
    const gains = A.gainRows(rep);
    if (!gains) return;
    const dlg = $('#at-dialog');
    const rows = A.resultRows(rep);
    const rec = rep.recommended;
    $('#at-dialog-summary').textContent = 'Recommended from ' + rec.used + ' of ' + rows.length + ' test points (' +
      A.ruleName(rec.rule) + ', the most cautious gains of the points that measured cleanly).';
    const pts = $('#at-dialog-points tbody');
    pts.textContent = '';
    for (const r of rows) pts.append(cells(el('tr'), [r.target, r.status, r.kp, r.ki, r.kd]));
    const g = $('#at-dialog-gains tbody');
    g.textContent = '';
    for (const r of gains) g.append(cells(el('tr'), [r.name, r.current, r.next]));
    fanTables(rep, '#at-dialog-fan', '#at-dialog-fan-summary', '#at-dialog-fan-table tbody', false);
    dlg.returnValue = '';
    dlg.onclose = () => {
      if (dlg.returnValue === 'save') decideAutotune('apply');
      else if (dlg.returnValue === 'cancel') decideAutotune('discard');
      // Escape leaves the results in the panel.
    };
    if (!dlg.open) dlg.showModal();
  }

  function renderAutotune() {
    const rep = S.at;
    const d = S.data || {};
    const running = !!(rep && rep.running) || d.Mode === 'Tuning';
    const pts = rep && rep.plan ? A.pointList(rep.plan.points, rep.unit) : '';
    const n = rep && rep.plan ? A.minutes(rep.plan.estimate) : null;
    $('#at-plan').textContent = 'Heats to ' + (pts || 'a few test temperatures') + ', measures the response at each and ' +
      'calculates PID gains' + (n ? '. About ' + n + ' min.' : '.') +
      (rep && rep.plan && rep.plan.fanTest ? ' An optional fan test then measures how airflow removes heat from the beans.' : '') +
      ' Nothing changes until you save the results.';
    $('#at-start').disabled = running || S.link !== 'open';
    $('#at-start').hidden = running;
    $('#at-stop').hidden = !running;
    $('#at-error').textContent = S.atError;
    const ended = rep && (rep.running || ['done', 'failed', 'aborted'].includes(rep.phase));
    $('#at-run').hidden = !ended;
    if (ended) {
      $('#at-progress').value = A.fraction(rep);
      $('#at-progress-text').textContent = A.progressText(rep);
      const list = $('#at-steps');
      list.textContent = '';
      for (const st of A.steps(rep)) {
        const li = el('li', { class: st.state }, st.label);
        if (st.detail) li.append(el('span', { class: 'muted' }, ' · ' + st.detail));
        list.append(li);
      }
    }
    const rows = A.resultRows(rep);
    $('#at-results').hidden = !rows.length;
    const body = $('#at-table tbody');
    body.textContent = '';
    for (const r of rows) body.append(cells(el('tr'), [r.target, r.status, r.cycles, r.swing, r.period, r.ku, r.kp, r.ki, r.kd]));
    fanTables(rep, '#at-fan', '#at-fan-summary', '#at-fan-table tbody', true);
    const gains = A.gainRows(rep);
    $('#at-choice').hidden = !gains;
    $('#at-recommended').textContent = gains ? 'Recommended: ' + gains.map((g) => g.name + ' ' + g.next).join(', ') +
      ' (now ' + gains.map((g) => g.current).join(' / ') + ').' : '';
    const log = $('#at-log');
    log.textContent = '';
    for (const e of (rep && rep.log) || []) log.append(el('li', {}, A.clock(e.t) + '  ' + e.text));
    $('#at-log-box').hidden = !(rep && rep.log && rep.log.length);
  }

  // ---------- wiring ----------
  // Monitoring is the landing view; a #live or #controls in the URL opens that view.
  // Opening a view only changes what is drawn: it sends nothing and records nothing.
  function setTab(tab) {
    if (tab === 'controls' && !S.info.controls) tab = 'monitoring';
    if (!['live', 'monitoring', 'controls'].includes(tab)) tab = 'monitoring';
    S.tab = tab;
    for (const p of $$('[data-panel]')) p.hidden = p.dataset.panel !== tab;
    for (const a of $$('[data-tab]')) a.classList.toggle('active', a.dataset.tab === tab);
    if (tab === 'controls' && S.lib.state !== 'ready') requestLibrary();
    render();
  }

  // ---------- appearance ----------
  // The scheme lives in croaster.theme as JSON (theme.js documents the format).
  // Every change is applied to <html> at once and saved, so the chart and the
  // rest of the page restyle without a reload.
  const AP = { theme: null, inputs: {} };

  function saveTheme() {
    try { localStorage.setItem(KEY_THEME, TH.serialize(AP.theme)); } catch (_) { /* storage full or blocked */ }
  }

  function setTheme(theme) {
    AP.theme = theme;
    TH.apply(theme, document.documentElement);
    saveTheme();
    syncAppearance();
  }

  function apStatus(text, isError) {
    const p = $('#ap-status');
    p.textContent = text;
    p.classList.toggle('err', !!isError);
  }

  function syncAppearance() {
    const t = AP.theme;
    for (const k in AP.inputs) {
      const i = AP.inputs[k];
      if (i.hex) {
        i.color.value = t.colors[k];
        if (document.activeElement !== i.hex) { i.hex.value = t.colors[k]; i.hex.removeAttribute('aria-invalid'); }
      } else if (i.type === 'bool') i.input.checked = t.style[k];
      else {
        i.input.value = t.style[k];
        if (i.out) i.out.textContent = t.style[k] + (i.unit || '');
      }
    }
    const current = TH.serialize(t);
    for (const b of $$('#ap-presets button')) {
      b.setAttribute('aria-pressed', String(TH.serialize(TH.fromPreset(b.dataset.preset)) === current));
    }
    const warn = TH.contrastWarnings(t);
    const ul = $('#ap-warnings');
    ul.hidden = !warn.length;
    ul.textContent = '';
    for (const w of warn) {
      ul.append(el('li', {}, w.label + ': contrast ' + w.ratio.toFixed(2) + ':1, below the ' + TH.AA_MIN +
        ':1 WCAG AA minimum for text.'));
    }
  }

  function buildAppearance() {
    const presets = $('#ap-presets');
    for (const name of Object.keys(TH.PRESETS)) {
      const b = el('button', { type: 'button', 'data-preset': name }, TH.PRESETS[name].label);
      b.addEventListener('click', () => { setTheme(TH.fromPreset(name)); apStatus(''); });
      presets.append(b);
    }

    const colors = $('#ap-colors');
    let group = null;
    for (const tok of TH.TOKENS) {
      if (tok.group !== group) {
        group = tok.group;
        colors.append(el('h4', { class: 'small muted' }, group));
      }
      const id = 'ap-c-' + tok.key;
      const row = el('div', { class: 'swatch-row' });
      const color = el('input', { type: 'color', id, 'aria-label': tok.label + ' colour picker' });
      const hex = el('input', { type: 'text', maxlength: '7', spellcheck: 'false', autocomplete: 'off',
        'aria-label': tok.label + ' hex value' });
      row.append(el('label', { for: id }, tok.label), color, hex);
      colors.append(row);
      color.addEventListener('input', () => {
        AP.theme.colors[tok.key] = color.value.toLowerCase();
        setTheme(AP.theme);
      });
      hex.addEventListener('input', () => {
        const v = TH.normalizeHex(hex.value);
        hex.setAttribute('aria-invalid', String(!v));
        if (v) { AP.theme.colors[tok.key] = v; setTheme(AP.theme); }
      });
      hex.addEventListener('change', () => { hex.value = AP.theme.colors[tok.key]; hex.removeAttribute('aria-invalid'); });
      AP.inputs[tok.key] = { color, hex };
    }

    const style = $('#ap-style');
    for (const key of Object.keys(TH.STYLE_FIELDS)) {
      const f = TH.STYLE_FIELDS[key];
      const id = 'ap-s-' + key;
      const row = el('div', { class: 'style-row' });
      const label = el('label', { for: id }, f.label);
      let input, out = null;
      if (f.type === 'number') {
        input = el('input', { type: 'range', id, min: f.min, max: f.max, step: f.step });
        out = el('output', { for: id });
        const wrap = el('span');
        wrap.append(input, ' ', out);
        row.append(label, wrap);
      } else if (f.type === 'choice') {
        input = el('select', { id });
        for (const o of f.options) input.append(el('option', { value: o }, o[0].toUpperCase() + o.slice(1)));
        row.append(label, input);
      } else {
        input = el('input', { type: 'checkbox', id });
        row.append(label, input);
      }
      style.append(row);
      input.addEventListener(f.type === 'number' ? 'input' : 'change', () => {
        const raw = f.type === 'bool' ? input.checked : f.type === 'number' ? Number(input.value) : input.value;
        const r = TH.normalize(Object.assign({}, AP.theme, { style: Object.assign({}, AP.theme.style, { [key]: raw }) }));
        setTheme(r.theme);
      });
      AP.inputs[key] = { input, out, type: f.type, unit: f.unit === '%' ? '%' : f.unit ? ' ' + f.unit : '' };
    }

    const dlg = $('#appearance');
    $('#appearance-open').addEventListener('click', () => {
      $('#ap-json').value = TH.serialize(AP.theme, true);
      apStatus('');
      syncAppearance();
      dlg.showModal();
    });
    $('#ap-close').addEventListener('click', () => dlg.close());
    $('#ap-reset').addEventListener('click', () =>
      confirmThen('Reset all colours and style options to the default scheme?', () => {
        setTheme(TH.fromPreset(TH.DEFAULT_PRESET));
        $('#ap-json').value = TH.serialize(AP.theme, true);
        apStatus('Reset to the default scheme.');
      }));
    $('#ap-export').addEventListener('click', () => {
      const json = TH.serialize(AP.theme, true);
      $('#ap-json').value = json;
      const a = el('a', { download: 'croaster-theme.json' });
      a.href = URL.createObjectURL(new Blob([json + '\n'], { type: 'application/json' }));
      document.body.append(a);
      a.click();
      a.remove();
      setTimeout(() => URL.revokeObjectURL(a.href), 1000);
    });
    $('#ap-copy').addEventListener('click', () => {
      const ta = $('#ap-json');
      ta.value = TH.serialize(AP.theme, true);
      const done = () => apStatus('Copied.');
      if (navigator.clipboard && window.isSecureContext) {
        navigator.clipboard.writeText(ta.value).then(done, () => apStatus('Copy failed. Select the text and copy it.', true));
      } else {
        // The board serves plain http, where the async clipboard API is not available.
        ta.select();
        if (document.execCommand('copy')) done();
        else apStatus('Select the text and copy it.', true);
      }
    });
    const importText = (text) => {
      const r = TH.parseImport(text);
      if (!r.theme) { apStatus('Not loaded: ' + r.errors.join('; ') + '.', true); return; }
      setTheme(r.theme);
      $('#ap-json').value = TH.serialize(AP.theme, true);
      apStatus(r.errors.length ? 'Loaded. Skipped: ' + r.errors.join('; ') + '.' : 'Loaded.', r.errors.length > 0);
    };
    $('#ap-import').addEventListener('click', () => importText($('#ap-json').value));
    $('#ap-import-file').addEventListener('change', (e) => {
      const file = e.target.files[0];
      if (!file) return;
      if (file.size > 64 * 1024) { apStatus('Not loaded: file is larger than 64 KB.', true); return; }
      file.text().then(importText, () => apStatus('Could not read the file.', true));
      e.target.value = '';
    });
  }

  function initAppearance() {
    let raw = null;
    try { raw = localStorage.getItem(KEY_THEME); } catch (_) { /* blocked: defaults */ }
    const m = TH.migrate(raw);
    AP.theme = m.theme;
    buildAppearance();
    TH.apply(AP.theme, document.documentElement);
    if (m.migrated || m.errors.length) saveTheme();
    syncAppearance();
  }

  function downloadCsv() {
    const csv = R.buildCsv(S.session, rorAt);
    const a = el('a');
    const d = new Date(S.session.startedAt);
    const pad = (v) => String(v).padStart(2, '0');
    a.download = 'roast-' + d.getFullYear() + pad(d.getMonth() + 1) + pad(d.getDate()) + '-' + pad(d.getHours()) +
      pad(d.getMinutes()) + '.csv';
    a.href = URL.createObjectURL(new Blob([csv], { type: 'text/csv' }));
    document.body.append(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(a.href), 1000);
  }

  function wire() {
    initAppearance();
    window.addEventListener('hashchange', () => setTab(location.hash.slice(1)));
    $('#stop').addEventListener('click', stopRoast);
    const askEnd = () => {
      if (recording()) confirmThen('End the roast? Recording stops; the board is not changed. The samples stay until the next Start.', endRoast);
    };
    $('#end-roast').addEventListener('click', askEnd);
    $('#end-after-drop').addEventListener('click', askEnd);

    $('#ror-smooth').checked = S.rorSmooth;
    $('#ror-smooth').addEventListener('change', (e) => {
      S.rorSmooth = e.target.checked;
      localStorage.setItem(KEY_ROR, S.rorSmooth ? '1' : '0');
      render();
    });
    $('#auto-tp').checked = S.autoTp;
    $('#auto-tp').addEventListener('change', (e) => {
      S.autoTp = e.target.checked;
      localStorage.setItem(KEY_TP, S.autoTp ? '1' : '0');
    });
    $('#undo-event').addEventListener('click', () => {
      if (!recording()) return;
      const gone = S.session.events.pop();
      if (gone) sendEvent(gone.code, true);
      saveSession(true);
      render();
    });
    $('#new-roast').addEventListener('click', () => startRoast());
    $('#csv').addEventListener('click', downloadCsv);
    $('#clear-roast').addEventListener('click', clearRoast);

    const chart = $('#chart');
    chart.addEventListener('pointermove', onChartPointer);
    chart.addEventListener('pointerleave', () => { S.cursorT = null; $('#readout').textContent = ''; renderChart(); });

    for (const b of $$('button[data-cmd]')) {
      b.addEventListener('click', () => confirmThen(b.dataset.confirm, () => {
        const cmd = JSON.parse(b.dataset.cmd);
        send(cmd);
        if (cmd.command === 'stop') expectStop();
      }));
    }
    for (const st of $$('.stepper')) {
      for (const b of $$('button[data-step]', st)) b.addEventListener('click', () => stepperClick(st, Number(b.dataset.step)));
    }
    for (const form of $$('form[data-send]')) {
      form.addEventListener('input', () => { form.dataset.touched = '1'; });
      form.addEventListener('submit', (ev) => {
        ev.preventDefault();
        const payload = formPayload(form);
        if (!payload) return;
        confirmThen(form.dataset.confirm, () => {
          send({ command: payload });
          delete form.dataset.touched;
          if (form.dataset.send === 'wifiConnect') form.reset();
        });
      });
    }
    $('#profile-add').addEventListener('click', () => {
      const last = S.profile[S.profile.length - 1];
      S.profile.push({ duration: 60, setpoint: last ? last.setpoint : '', interpolation: 'linear', fanValue: '' });
      saveJson(KEY_PROFILE, S.profile);
      renderProfile();
    });
    $('#profile-load').addEventListener('click', loadProfile);

    // Device library
    $('#at-start').addEventListener('click', startAutotune);
    $('#at-review').addEventListener('click', showAutotuneDialog);
    $('#at-discard').addEventListener('click', () => confirmThen('Discard the autotune results? The current gains stay.', () => decideAutotune('discard')));

    $('#lib-refresh').addEventListener('click', requestLibrary);
    $('#lib-save').addEventListener('click', () => {
      const res = editorProfile();
      if (res.error === 'empty') { libStatus('Nothing to save: fill the profile setpoints or add recipe steps.', true); return; }
      if (res.error) { libStatus('Not saved: ' + stepHint(res.reason) + (res.reason || P.message(res.error)) + '.', true); return; }
      saveToDevice(res.profile);
    });
    $('#lib-import').addEventListener('click', () => $('#lib-file').click());
    $('#lib-file').addEventListener('change', (e) => { importFile(e.target.files[0]); e.target.value = ''; });
    const closeImport = () => { S.lib.imported = null; $('#lib-imported').hidden = true; };
    $('#lib-import-save').addEventListener('click', () => { if (S.lib.imported) saveToDevice(S.lib.imported, closeImport); });
    $('#lib-import-edit').addEventListener('click', () => {
      if (S.lib.imported) { openInEditors(S.lib.imported); libStatus('Opened "' + S.lib.imported.name + '" in the editors; Save editors to device keeps it.'); }
      closeImport();
    });
    $('#lib-import-cancel').addEventListener('click', closeImport);

    // Recipe
    $('#recipe-mode').addEventListener('change', (e) => { S.recipeMode = e.target.value; saveRecipeRows(); });
    $('#recipe-charge').addEventListener('input', (e) => { S.recipeCharge = e.target.value; saveRecipeRows(); checkRamps(); });
    $('#recipe-lag').addEventListener('change', (e) => {
      const v = Number(e.target.value);
      if (v > 0 && v <= 50) localStorage.setItem(KEY_LAG + '.' + edUnit(), String(v));
      e.target.value = lagSetting();
    });
    $('#recipe-add').addEventListener('click', () => {
      S.recipeRows.push(P.newRecipeRow());
      saveRecipeRows();
      renderRecipe();
    });
    $('#recipe-load').addEventListener('click', loadRecipe);
    const runRecipe = () => send({ command: 'recipeStart' });
    $('#recipe-run').addEventListener('click', runRecipe);
    $('#rl-start').addEventListener('click', runRecipe);
    $('#rl-abort').addEventListener('click', () => confirmThen('Abort the recipe? No further steps run; the heater and fan stay as they are.', () => send({ command: 'recipeAbort' })));
    $('#rl-skip').addEventListener('click', () => send({ command: 'recipeSkip' }));
    $('#rl-apply').addEventListener('click', () => { if (S.recipe) send({ command: 'recipeConfirm', step: S.recipe.prompt }); });
    $('#rl-dismiss').addEventListener('click', () => { if (S.recipe) send({ command: 'recipeSkip', step: S.recipe.prompt }); });

    // A phone suspends a hidden page without closing its socket, and the board then blocks
    // on writes to it. So the socket is closed while hidden when nothing needs it (see
    // Link.mayPause) and on pagehide, and reopened when the page is shown again.
    const pause = () => {
      if (worker && !S.paused) { S.paused = true; worker.postMessage({ type: 'pause' }); }
    };
    const resume = () => {
      if (worker && S.paused) { S.paused = false; worker.postMessage({ type: 'resume' }); }
    };
    document.addEventListener('visibilitychange', () => {
      if (document.hidden) {
        saveSession(true);
        if (L.mayPause(recording(), S.info.controls, S.data)) pause();
        return;
      }
      resume();
      if (worker) worker.postMessage({ type: 'visible' }); // poll at once; the board has been fed by the worker meanwhile
      render();
    });
    window.addEventListener('pagehide', () => { saveSession(true); pause(); });
    window.addEventListener('pageshow', (e) => { if (e.persisted) resume(); });
    window.addEventListener('resize', () => { S.dirty = true; });
    // The top bar wraps to two rows on narrow screens; the sticky tabs sit below it.
    const bar = $('.topbar');
    const barHeight = () => document.documentElement.style.setProperty('--topbar-h', bar.offsetHeight + 'px');
    if (window.ResizeObserver) new ResizeObserver(barHeight).observe(bar);
    barHeight();

    // Redraw at most once per frame when something changed, and tick the clocks once a second.
    const frame = () => { if (S.dirty && !document.hidden) render(); requestAnimationFrame(frame); };
    requestAnimationFrame(frame);
    setInterval(() => { if (!document.hidden) { renderClock(); if (S.tab !== 'live') S.dirty = true; } }, 1000);
    setInterval(() => {
      const every = A.pollMs(S.at);
      if (S.info.controls && S.link === 'open' && every && Date.now() - S.atAskedAt >= every) askAutotune();
    }, 1000);
  }

  async function start() {
    S.session = loadJson(KEY_SESSION) || newSession();
    if (!Array.isArray(S.session.samples) || !Array.isArray(S.session.events)) S.session = newSession();
    const wasRecording = R.isRecording(S.session);
    R.resumeSession(S.session, Date.now(), RESUME_GAP_MS);
    if (wasRecording && !R.isRecording(S.session)) saveSession(true);
    try {
      const r = await fetch('info.json', { cache: 'no-store' });
      if (r.ok) Object.assign(S.info, await r.json());
    } catch (_) { /* not served by the board: see the ws and controls parameters below */ }
    // ?ws=ws://192.168.1.50:81&controls=1 points the page at a board when web/ is served from a computer.
    const query = new URLSearchParams(location.search);
    if (query.get('controls') === '1') S.info.controls = true;
    // Over https the page uses wss://<host>/ws, which the proxy forwards to the board (see Link.socketUrl).
    const url = L.socketUrl(location, S.info.wsPort, query.get('ws'));

    for (const e of $$('[data-needs="controls"]')) e.hidden = !S.info.controls;
    wire();
    renderProfile();
    renderRecipe();
    setTab(location.hash.slice(1));

    worker = new Worker('link.worker.js');
    worker.onmessage = onWorker;
    worker.onerror = () => logReply('err', 'connection worker failed to start');
    worker.postMessage({ type: 'open', url, controls: S.info.controls });
  }

  start();
})();
