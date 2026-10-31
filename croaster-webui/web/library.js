// Device profile library: the file format and its checks, shared by the page and
// the host tests (node croaster-webui/test). Same rules as the board
// (croaster-control/src/core/profile_library.h and RoastProfile::loadJson).
// Pure functions only: no DOM, no network.
(function (root, factory) {
  'use strict';
  const api = factory();
  if (typeof module === 'object' && module.exports) module.exports = api;
  else root.ProfileLibrary = api;
})(typeof self !== 'undefined' ? self : this, function () {
  'use strict';

  const VERSION = 1; // highest format version this page and the board understand
  const MAX_PROFILES = 50;
  const MAX_BYTES = 8192;
  const MAX_NAME = 40; // characters (code points)
  const MAX_NOTES = 1024;
  const MAX_STEPS = 32;
  const MAX_STEP_S = 3600;
  const MIN_SETPOINT_C = -50;
  const MAX_SETPOINT_C = 1000;
  const UNITS = ['C', 'F', 'K'];
  // Recipe limits, as core/recipe.h.
  const RECIPE = { MAX_STEPS: 32, MAX_ACTIONS: 4, MAX_NOTE: 80, MAX_LABEL: 40, MAX_TIME_S: 3600,
    MIN_TEMP_C: 0, MAX_TEMP_C: 350, MIN_ROR_C: -50, MAX_ROR_C: 100, MAX_RAMP_S: 3600, CURVE_POINTS: 21, MAX_CURVES: 8 };
  const SHAPES = ['linear', 'normal', 'front', 'back', 'custom'];
  const EVENTS = ['CHARGE', 'TP', 'DRY_END', 'FC_START', 'FC_END', 'SC_START', 'SC_END', 'DROP'];
  const TRIGGERS = ['time', 'bt', 'et', 'ror', 'event'];
  const OPS = ['gte', 'lte', 'near'];
  const ACTIONS = ['power', 'fan', 'setpoint', 'hold', 'cool', 'end', 'note'];

  const MESSAGES = {
    badRequest: 'the request was incomplete',
    badId: 'the profile id is not valid',
    badVersion: 'the file has no valid "version"',
    newerVersion: 'the file was written by a newer firmware or page',
    badName: 'the name must be 1 to ' + MAX_NAME + ' characters',
    badNotes: 'notes must be text up to ' + MAX_NOTES + ' characters',
    badUnit: 'the unit must be C, F or K',
    badSteps: '1 to ' + MAX_STEPS + ' steps; each needs a duration above 0 and up to ' + MAX_STEP_S +
      ' s, a setpoint, and fan 0 to 100 % if set',
    badRecipe: 'the recipe is not valid',
    tooLarge: 'the profile is over ' + MAX_BYTES + ' bytes',
    notFound: 'the profile is not on the board',
    exists: 'a profile with that id already exists',
    full: 'the library is full (' + MAX_PROFILES + ' profiles)',
    busy: 'the board is busy',
    storage: 'the board could not read or write its storage',
    unavailable: 'the board has no device library',
    badFile: 'the file is not JSON',
  };

  const isNum = (v) => typeof v === 'number' && isFinite(v);
  const chars = (s) => [...s].length;

  function toCelsius(v, unit) {
    return unit === 'F' ? (v - 32) * 5 / 9 : unit === 'K' ? v - 273.15 : v;
  }
  function fromCelsius(c, unit) {
    return unit === 'F' ? c * 9 / 5 + 32 : unit === 'K' ? c + 273.15 : c;
  }
  // Rounded to 0.1 so a round trip through another unit does not grow digits.
  function convert(v, from, to) {
    if (from === to) return v;
    return Math.round(fromCelsius(toCelsius(v, from), to) * 10) / 10;
  }

  // A temperature difference in `unit` to C (RoR, tolerances).
  function deltaToC(d, unit) {
    return unit === 'F' ? d * 5 / 9 : d;
  }
  function convertDelta(d, from, to) {
    if (from === to) return d;
    const c = deltaToC(d, from);
    return Math.round((to === 'F' ? c * 9 / 5 : c) * 10) / 10;
  }

  function byteLength(text) {
    return new TextEncoder().encode(text).length;
  }

  function validStep(s, unit) {
    if (!s || typeof s !== 'object' || Array.isArray(s)) return false;
    if (!isNum(s.duration) || !(s.duration > 0 && s.duration <= MAX_STEP_S)) return false;
    if (!isNum(s.setpoint)) return false;
    const c = toCelsius(s.setpoint, unit);
    if (!(c >= MIN_SETPOINT_C && c <= MAX_SETPOINT_C)) return false;
    if (s.fanValue !== undefined && s.fanValue !== null && !(isNum(s.fanValue) && s.fanValue >= 0 && s.fanValue <= 100)) return false;
    return true;
  }

  const isObj = (v) => v && typeof v === 'object' && !Array.isArray(v);
  const present = (v) => v !== undefined && v !== null;

  function triggerReason(t, unit) {
    if (!isObj(t)) return 'trigger missing';
    const op = present(t.op) ? t.op : 'gte';
    if (!OPS.includes(op)) return 'trigger op must be gte, lte or near';
    if (t.type === 'event') return EVENTS.includes(t.value) ? '' : 'unknown event';
    if (!isNum(t.value)) return 'trigger value missing';
    let maxTol;
    if (t.type === 'time') {
      if (!(t.value >= 0 && t.value <= RECIPE.MAX_TIME_S)) return 'time trigger must be 0..3600 s';
      if (present(t.tol) && !(isNum(t.tol) && t.tol > 0 && t.tol <= 60)) return 'tolerance out of range';
      return '';
    }
    if (t.type === 'bt' || t.type === 'et') {
      const c = toCelsius(t.value, unit);
      if (!(c >= RECIPE.MIN_TEMP_C && c <= RECIPE.MAX_TEMP_C)) return 'temperature trigger must be 0..350 C';
      maxTol = 20;
    } else if (t.type === 'ror') {
      const c = deltaToC(t.value, unit);
      if (!(c >= RECIPE.MIN_ROR_C && c <= RECIPE.MAX_ROR_C)) return 'RoR trigger must be -50..100 C/min';
      maxTol = 20;
    } else {
      return 'trigger type must be time, bt, et, ror or event';
    }
    if (present(t.tol)) {
      const c = isNum(t.tol) ? deltaToC(t.tol, unit) : NaN;
      if (!(c > 0 && c <= maxTol)) return 'tolerance out of range';
    }
    return '';
  }

  // "ramp", "shape" and "curve" of a power or setpoint action.
  function rampReason(a) {
    if (present(a.ramp) && !(isNum(a.ramp) && a.ramp >= 0 && a.ramp <= RECIPE.MAX_RAMP_S)) return 'ramp must be 0..3600 s';
    const shape = present(a.shape) ? a.shape : 'linear';
    if (!SHAPES.includes(shape)) return 'shape must be linear, normal, front, back or custom';
    if (shape !== 'custom') return present(a.curve) ? 'curve needs shape custom' : '';
    if (!Array.isArray(a.curve) || a.curve.length !== RECIPE.CURVE_POINTS) return 'curve must have 21 rates';
    if (!a.curve.every((v) => isNum(v) && v >= 0 && v <= 100)) return 'curve rates must be 0..100';
    if (!a.curve.some((v) => Math.round(v) > 0)) return 'curve needs a rate above 0';
    return '';
  }

  function actionReason(a, unit) {
    if (!isObj(a)) return 'action must be an object';
    if (a.type === 'power' || a.type === 'fan') {
      if (!(isNum(a.value) && a.value >= 0 && a.value <= 100)) return 'power and fan must be 0..100 %';
      if (a.type === 'fan') return present(a.ramp) || present(a.shape) ? 'fan does not ramp' : '';
      return rampReason(a);
    }
    if (a.type === 'setpoint') {
      if (!isNum(a.value)) return 'setpoint value missing';
      const c = toCelsius(a.value, unit);
      if (!(c >= RECIPE.MIN_TEMP_C && c <= RECIPE.MAX_TEMP_C)) return 'setpoint must be 0..350 C';
      return rampReason(a);
    }
    if (a.type === 'hold' || a.type === 'cool' || a.type === 'end') return '';
    if (a.type === 'note')
      return typeof a.text === 'string' && chars(a.text) <= RECIPE.MAX_NOTE ? '' : 'note text must be up to 80 characters';
    return 'action type must be power, fan, setpoint, hold, cool, end or note';
  }

  // '' when `r` is a valid recipe (temperatures in `unit`), else the reason,
  // worded as the board's recipe::parse.
  function recipeReason(r, unit) {
    if (!isObj(r)) return 'recipe must be an object';
    const mode = present(r.mode) ? r.mode : 'auto';
    if (mode !== 'auto' && mode !== 'guided') return 'mode must be auto or guided';
    if (present(r.chargeTemp)) {
      if (!isNum(r.chargeTemp)) return 'chargeTemp must be a number';
      const c = toCelsius(r.chargeTemp, unit);
      if (!(c >= RECIPE.MIN_TEMP_C && c <= RECIPE.MAX_TEMP_C)) return 'chargeTemp must be 0..350 C';
    }
    if (!Array.isArray(r.steps) || r.steps.length < 1 || r.steps.length > RECIPE.MAX_STEPS) return 'a recipe has 1 to 32 steps';
    let curves = 0;
    for (const s of r.steps) {
      if (!isObj(s)) return 'step must be an object';
      let e = triggerReason(s.trigger, unit);
      if (e) return e;
      if (present(s.anyOrder) && typeof s.anyOrder !== 'boolean') return 'anyOrder must be true or false';
      if (present(s.label) && (typeof s.label !== 'string' || chars(s.label) > RECIPE.MAX_LABEL)) return 'label must be up to 40 characters';
      if (!Array.isArray(s.actions) || s.actions.length < 1 || s.actions.length > RECIPE.MAX_ACTIONS) return 'a step has 1 to 4 actions';
      for (const a of s.actions) {
        e = actionReason(a, unit);
        if (e) return e;
        if (a.shape === 'custom' && ++curves > RECIPE.MAX_CURVES) return 'a recipe has up to 8 custom curves';
      }
    }
    return '';
  }

  // Returns '' when `p` is a valid profile, else an error code (see MESSAGES).
  // Keys this version does not know are allowed and kept.
  function validate(p) {
    if (!p || typeof p !== 'object' || Array.isArray(p)) return 'badRequest';
    if (!Number.isInteger(p.version) || p.version < 1) return 'badVersion';
    if (p.version > VERSION) return 'newerVersion';
    if (typeof p.name !== 'string') return 'badName';
    const n = chars(p.name);
    if (n === 0 || n > MAX_NAME || /[\x00-\x1f]/.test(p.name) || !/[^ ]/.test(p.name)) return 'badName';
    if (p.notes !== undefined && p.notes !== null && (typeof p.notes !== 'string' || chars(p.notes) > MAX_NOTES)) return 'badNotes';
    if (!UNITS.includes(p.unit)) return 'badUnit';
    if (!present(p.steps) && !present(p.recipe)) return 'badSteps';
    if (present(p.steps)) {
      if (!Array.isArray(p.steps) || p.steps.length < 1 || p.steps.length > MAX_STEPS) return 'badSteps';
      if (!p.steps.every((s) => validStep(s, p.unit))) return 'badSteps';
    }
    if (present(p.recipe) && recipeReason(p.recipe, p.unit)) return 'badRecipe';
    if (byteLength(JSON.stringify(p)) > MAX_BYTES) return 'tooLarge';
    return '';
  }

  // Reads a file's text. {profile} or {error}.
  function parseFile(text) {
    let p;
    try { p = JSON.parse(text); } catch (_) { return { error: 'badFile' }; }
    const error = validate(p);
    return error ? { error } : { profile: p };
  }

  // Builds a profile from the editor rows (setpoints in `unit`). `base` is the
  // profile the editor was opened from: its other keys are kept. `rows` null
  // leaves the schedule out; `recipe` (recipeFromEditor) null leaves the recipe out.
  function fromEditor(rows, name, notes, unit, base, recipe) {
    const p = Object.assign({}, base || {});
    p.version = VERSION;
    p.name = String(name || '').trim();
    if (notes) p.notes = String(notes); else delete p.notes;
    p.unit = unit;
    if (recipe) p.recipe = recipe; else delete p.recipe;
    if (!rows) {
      delete p.steps;
      const error = validate(p);
      return error ? { error, reason: error === 'badRecipe' ? recipeReason(p.recipe, unit) : '' } : { profile: p };
    }
    p.steps = rows.map((s) => {
      const step = { duration: Number(s.duration), setpoint: s.setpoint === '' ? NaN : Number(s.setpoint),
        interpolation: s.interpolation || 'linear' };
      if (s.fanValue !== '' && s.fanValue !== undefined && s.fanValue !== null) step.fanValue = Number(s.fanValue);
      return step;
    });
    const error = validate(p);
    return error ? { error, reason: error === 'badRecipe' ? recipeReason(p.recipe, unit) : '' } : { profile: p };
  }

  const blank = (v) => v === '' || v === undefined || v === null;
  const num = (v) => (blank(v) ? NaN : Number(v));
  // "7:42" or "462" (seconds) to 462; NaN when it is neither.
  function parseClock(v) {
    if (isNum(v)) return v;
    const t = String(v === undefined || v === null ? '' : v).trim();
    const m = /^(\d+):([0-5]?\d)$/.exec(t);
    if (m) return Number(m[1]) * 60 + Number(m[2]);
    return /^\d+(\.\d+)?$/.test(t) ? Number(t) : NaN;
  }

  // A new recipe step for the editor: at 0:00, heater power and fan (open loop).
  function newRecipeRow() {
    return { type: 'time', op: 'gte', value: '', tol: '', event: 'CHARGE', anyOrder: false, label: '',
      actions: [{ type: 'power', value: '' }, { type: 'fan', value: '' }] };
  }

  // Recipe JSON from editor rows (values as typed, in the page unit). Blank
  // fields become NaN so validation names them. `chargeTemp` blank leaves it out.
  function recipeFromEditor(rows, mode, chargeTemp) {
    const out = { mode: mode === 'guided' ? 'guided' : 'auto' };
    if (!blank(chargeTemp)) out.chargeTemp = num(chargeTemp);
    out.steps = rows.map((r) => {
        const trigger = { type: r.type, op: r.op || 'gte' };
        trigger.value = r.type === 'event' ? r.event : r.type === 'time' ? parseClock(r.value) : num(r.value);
        if (r.type !== 'event' && trigger.op === 'near' && !blank(r.tol)) trigger.tol = num(r.tol);
        const step = { trigger };
        if (r.label && String(r.label).trim()) step.label = String(r.label).trim();
        if (r.anyOrder) step.anyOrder = true;
        step.actions = r.actions.map((a) => {
          const out = { type: a.type };
          if (a.type === 'power' || a.type === 'fan' || a.type === 'setpoint') out.value = num(a.value);
          if ((a.type === 'setpoint' || a.type === 'power') && !blank(a.ramp) && Number(a.ramp) > 0) {
            out.ramp = num(a.ramp);
            if (a.shape && a.shape !== 'linear') out.shape = a.shape;
            if (a.shape === 'custom') out.curve = Array.isArray(a.curve) ? a.curve.slice() : [];
          }
          if (a.type === 'note') out.text = String(a.text || '');
          return out;
        });
        return step;
      });
    return out;
  }

  // Editor rows from a recipe written in `from`, temperatures shown in `to`.
  function recipeToEditor(recipe, from, to) {
    return (recipe.steps || []).map((s) => {
      const t = s.trigger || {};
      const temp = t.type === 'bt' || t.type === 'et';
      const row = { type: t.type, op: t.op || 'gte', value: '', tol: '', event: 'CHARGE', anyOrder: !!s.anyOrder,
        label: s.label || '', actions: [] };
      if (t.type === 'event') row.event = t.value;
      else row.value = temp ? convert(t.value, from, to) : t.type === 'ror' ? convertDelta(t.value, from, to) : clock(t.value);
      if (isNum(t.tol)) row.tol = t.type === 'time' ? t.tol : convertDelta(t.tol, from, to);
      row.actions = (s.actions || []).map((a) => ({ type: a.type,
        value: a.type === 'setpoint' ? convert(a.value, from, to) : isNum(a.value) ? a.value : '',
        ramp: isNum(a.ramp) ? a.ramp : '', shape: a.shape || 'linear',
        curve: Array.isArray(a.curve) ? a.curve.slice() : undefined, text: a.text || '' }));
      return row;
    });
  }

  function clock(s) {
    if (!isNum(s)) return '';
    const m = Math.floor(s / 60);
    const r = Math.floor(s - m * 60);
    return m + ':' + String(r).padStart(2, '0');
  }

  const EVENT_LABELS = { CHARGE: 'Charge', TP: 'Turning point', DRY_END: 'Dry end', FC_START: 'First crack',
    FC_END: 'First crack end', SC_START: 'Second crack', SC_END: 'Second crack end', DROP: 'Drop' };
  const OP_TEXT = { gte: '\u2265', lte: '\u2264', near: '\u2248' };

  // "BT >= 389 °F" (with the math sign) for a step written in `from`, shown in `to`.
  function describeTrigger(t, from, to) {
    if (!t) return '';
    if (t.type === 'event') return EVENT_LABELS[t.value] || t.value;
    const op = OP_TEXT[t.op || 'gte'] || '';
    if (t.type === 'time') return 'time ' + op + ' ' + clock(t.value);
    if (t.type === 'ror') return 'RoR ' + op + ' ' + convertDelta(t.value, from, to) + ' \u00b0' + to + '/min';
    return t.type.toUpperCase() + ' ' + op + ' ' + convert(t.value, from, to) + ' \u00b0' + to;
  }
  // " over 2:00" or " over 2:00, front"
  function rampText(a) {
    if (!(isNum(a.ramp) && a.ramp > 0)) return '';
    return ' over ' + clock(a.ramp) + (a.shape && a.shape !== 'linear' ? ', ' + a.shape : '');
  }
  function describeAction(a, from, to) {
    switch (a.type) {
      case 'power': return 'power ' + a.value + ' %' + rampText(a);
      case 'fan': return 'fan ' + a.value + ' %';
      case 'setpoint':
        return 'setpoint ' + convert(a.value, from, to) + ' \u00b0' + to + rampText(a);
      case 'hold': return 'hold';
      case 'cool': return 'cool';
      case 'end': return 'end recipe';
      case 'note': return '"' + a.text + '"';
      default: return a.type;
    }
  }
  // "Step 3 (BT >= 389 °F): fan 50 %, power 60 %"
  function describeStep(recipe, i, from, to) {
    const s = recipe && recipe.steps && recipe.steps[i];
    if (!s) return 'Step ' + (i + 1);
    const head = 'Step ' + (i + 1) + (s.label ? ' ' + s.label : '') + ' (' + describeTrigger(s.trigger, from, to) + ')';
    return head + ': ' + (s.actions || []).map((a) => describeAction(a, from, to)).join(', ');
  }

  // Behind-schedule check for a setpoint step: BT more than `lagC` below the
  // setpoint while the heater sits at 100 % for `holdMs`. Feed it every sample;
  // `state` carries the time saturation started. Returns {state, warn, lagC}.
  function behindSchedule(state, sample, opt) {
    const lagLimit = opt && isNum(opt.lagC) ? opt.lagC : 8;
    const holdMs = opt && isNum(opt.holdMs) ? opt.holdMs : 20000;
    const now = sample.nowMs;
    const lag = isNum(sample.setpointC) && isNum(sample.btC) ? sample.setpointC - sample.btC : NaN;
    const behind = !!sample.follow && isNum(sample.heater) && sample.heater >= 100 && lag > lagLimit;
    const since = behind ? (state && isNum(state.since) ? state.since : now) : null;
    return { state: { since }, warn: behind && now - since >= holdMs, lagC: lag };
  }

  // Editor rows from a profile, setpoints converted to `unit`.
  function toEditor(p, unit) {
    return p.steps.map((s) => ({
      duration: s.duration,
      setpoint: convert(s.setpoint, p.unit, unit),
      interpolation: s.interpolation || 'linear',
      fanValue: isNum(s.fanValue) ? s.fanValue : '',
    }));
  }

  // Download name: "City roast #2" -> "city-roast-2.json".
  function fileName(name) {
    const slug = String(name || '').toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-+|-+$/g, '').slice(0, 40);
    return (slug || 'profile') + '.json';
  }

  // Why save and delete are off, from the telemetry; '' when the board is idle.
  // Same rule as the board (proflib::busyReason).
  function busyReason(d) {
    if (!d) return '';
    if (d.Mode === 'Profile') return 'a profile is running';
    if (d.Mode === 'Tuning') return 'autotune is running';
    if (isNum(d.BurnerVal) && d.BurnerVal > 0) return 'the heater is on';
    return '';
  }

  // The library entry whose name matches (case-insensitive), or null.
  function findByName(list, name) {
    const key = String(name || '').trim().toLowerCase();
    return (list || []).find((m) => String(m.name).toLowerCase() === key) || null;
  }

  function message(code, reason) {
    if (code === 'busy' && reason) return 'refused while the board reports ' + reason;
    return MESSAGES[code] || code;
  }

  return { VERSION, MAX_PROFILES, MAX_BYTES, MAX_NAME, MAX_NOTES, MAX_STEPS, RECIPE, EVENTS, EVENT_LABELS,
    TRIGGERS, OPS, ACTIONS, validate, recipeReason, parseFile, fromEditor, toEditor, newRecipeRow,
    SHAPES, recipeFromEditor, recipeToEditor, parseClock, describeTrigger, describeStep, behindSchedule, clock, convert, convertDelta,
    toCelsius, fileName, busyReason, findByName, message };
});
