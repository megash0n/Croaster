// Ramp shapes for recipe power and setpoint ramps: the rate-of-climb curves,
// the path a ramp follows, and a check of setpoint ramps against the saved
// autotune model. Same curves and maths as croaster-control/src/core/recipe.h
// (rampFraction, peakFactor) and core/roaster_model.h (maxRorCpm).
// Pure functions only: no DOM, no network.
(function (root, factory) {
  'use strict';
  const api = factory();
  if (typeof module === 'object' && module.exports) module.exports = api;
  else root.RampShape = api;
})(typeof self !== 'undefined' ? self : this, function () {
  'use strict';

  const POINTS = 21; // rate at 0, 5, ... 100 % of the ramp time
  const STEP = 5;    // rates snap to steps of 5 (0..100)
  const KNOTS = 5;   // custom editor points at 0, 25, 50, 75, 100 % of the ramp time
  const SHAPES = ['linear', 'normal', 'front', 'back', 'custom'];
  const LABELS = { linear: 'Linear', normal: 'Normal', front: 'Front', back: 'Back', custom: 'Custom' };
  const HINTS = {
    linear: 'Constant rate of climb.',
    normal: 'Slow start, fastest in the middle, slow finish.',
    front: 'Fastest at the start, easing into the target.',
    back: 'Slow start, fastest at the end.',
    custom: 'Your own rate of climb.',
  };
  const PRESETS = {
    linear: new Array(POINTS).fill(50),
    normal: [5, 10, 15, 20, 30, 45, 60, 75, 90, 95, 100, 95, 90, 75, 60, 45, 30, 20, 15, 10, 5],
    front: Array.from({ length: POINTS }, (_, i) => 100 - STEP * i),
    back: Array.from({ length: POINTS }, (_, i) => STEP * i),
  };

  const isNum = (v) => typeof v === 'number' && isFinite(v);
  const toC = (v, u) => (u === 'F' ? (v - 32) * 5 / 9 : u === 'K' ? v - 273.15 : v);
  const fromC = (c, u) => (u === 'F' ? c * 9 / 5 + 32 : u === 'K' ? c + 273.15 : c);
  const deltaFromC = (d, u) => (u === 'F' ? d * 9 / 5 : d);

  function snap(v) {
    const n = Number(v);
    if (!isFinite(n)) return 0;
    return Math.max(0, Math.min(100, Math.round(n / STEP) * STEP));
  }

  // A valid custom curve: 21 numbers 0..100, at least one above 0.
  function validCurve(c) {
    return Array.isArray(c) && c.length === POINTS && c.every((v) => isNum(v) && v >= 0 && v <= 100) && c.some((v) => v > 0);
  }

  // The editor's 5 points of a 21-rate curve (its rates at 0, 25 ... 100 %).
  function knotsOf(c) {
    const step = (POINTS - 1) / (KNOTS - 1);
    return Array.from({ length: KNOTS }, (_, k) => c[k * step]);
  }

  // The 21 rates through 5 points, by monotone cubic interpolation
  // (Fritsch-Carlson): smooth, and never above or below the points either side,
  // so no rate goes under 0 or over 100. Rounded to whole percent.
  function fromKnots(k) {
    const n = k.length - 1, d = [], m = [];
    for (let i = 0; i < n; i++) d.push(k[i + 1] - k[i]);
    m[0] = d[0];
    m[n] = d[n - 1];
    for (let i = 1; i < n; i++) m[i] = d[i - 1] * d[i] <= 0 ? 0 : 2 / (1 / d[i - 1] + 1 / d[i]);
    return Array.from({ length: POINTS }, (_, j) => {
      const x = j / (POINTS - 1) * n, i = Math.min(n - 1, Math.floor(x)), t = x - i;
      const t2 = t * t, t3 = t2 * t;
      const v = (2 * t3 - 3 * t2 + 1) * k[i] + (t3 - 2 * t2 + t) * m[i] + (-2 * t3 + 3 * t2) * k[i + 1] + (t3 - t2) * m[i + 1];
      return Math.max(0, Math.min(100, Math.round(v)));
    });
  }

  // The rate curve of `shape` (a custom one from `curve`); linear when unknown.
  function rates(shape, curve) {
    if (shape === 'custom') return validCurve(curve) ? curve.slice() : PRESETS.linear.slice();
    return (PRESETS[shape] || PRESETS.linear).slice();
  }

  // Fraction of the change done at u (0..1) of the ramp time.
  function fraction(r, u) {
    if (!(u > 0)) return 0;
    if (u >= 1) return 1;
    const n = POINTS - 1, pos = u * n;
    let total = 0, done = 0;
    for (let i = 0; i < n; i++) {
      const a = r[i], b = r[i + 1];
      total += (a + b) / 2;
      if (pos >= i + 1) done += (a + b) / 2;
      else if (pos > i) { const x = pos - i; done += a * x + (b - a) * x * x / 2; }
    }
    return total > 0 ? Math.min(1, Math.max(0, done / total)) : u;
  }

  // Steepest rate over the mean rate, and where it is (0..1): `at` is the first place,
  // `spots` every run of points at that rate ({from, to}, 0..1; rates are whole numbers).
  function peak(r) {
    const n = POINTS - 1;
    let total = 0, at = 0;
    for (let i = 0; i < n; i++) total += (r[i] + r[i + 1]) / 2;
    for (let i = 1; i < POINTS; i++) if (r[i] > r[at]) at = i;
    const spots = [];
    for (let i = 0; i < POINTS; i++) {
      if (Math.abs(r[i] - r[at]) >= 0.5) continue;
      const prev = spots[spots.length - 1];
      if (prev && prev.to === (i - 1) / n) prev.to = i / n;
      else spots.push({ from: i / n, to: i / n });
    }
    return { factor: total > 0 ? r[at] / (total / n) : 1, at: at / n, spots };
  }

  // `v` scaled so the largest is 100 (only the proportions of a rate curve matter).
  // All zero: unchanged.
  function normalize(v) {
    const top = Math.max(...v);
    return top > 0 ? v.map((x) => Math.round(x / top * 100)) : v.slice();
  }

  // SVG path of the rate curve `r` itself (not the ramp path) in a box at
  // (x, y) of size w x h: rate 0 at the bottom, 100 at the top. Used for the
  // shape icons: Front falls, Back rises, Normal is a hump, Linear is flat.
  function rateIcon(r, x, y, w, h) {
    const n = r.length - 1;
    return r.map((v, i) => (i ? 'L' : 'M') + (x + i / n * w).toFixed(1) + ' ' + (y + h - v / 100 * h).toFixed(1)).join('');
  }

  // `count` + 1 points {t, v} of a ramp from `from` to `to` over `seconds`.
  function path(from, to, seconds, r, count) {
    const n = count || 40, out = [];
    for (let i = 0; i <= n; i++) {
      const u = i / n;
      out.push({ t: u * seconds, v: from + (to - from) * fraction(r, u) });
    }
    return out;
  }

  // ---- saved autotune model (getAutotune "saved"), as core/roaster_model.cpp ----
  // Temperatures converted to C once; `unit` is the reply's unit.
  function model(saved, unit) {
    if (!saved || !Array.isArray(saved.points)) return null;
    const amb = isNum(saved.ambient) ? toC(saved.ambient, unit) : 25;
    const points = saved.points
      .map((p) => ({ t: toC(p.target, unit), tau: p.tau, hold: p.hold }))
      .filter((p) => isNum(p.t) && isNum(p.tau) && p.tau > 0 && isNum(p.hold) && p.hold > 0 && p.t - amb > 1)
      .sort((a, b) => a.t - b.t);
    if (!points.length) return null;
    let fan = null;
    if (saved.fan && Array.isArray(saved.fan.levels)) {
      const levels = saved.fan.levels.filter((l) => isNum(l.fan) && isNum(l.hold));
      if (levels.length >= 2) fan = { t: toC(saved.fan.target, unit), levels };
    }
    return { amb, points, fan, runFan: saved.runFan, heaterMax: isNum(saved.heaterMax) ? saved.heaterMax : 100 };
  }

  function along(m, t, f) {
    const p = m.points;
    if (t <= p[0].t) return f(p[0]);
    for (let i = 1; i < p.length; i++)
      if (t <= p[i].t) {
        const x = (t - p[i - 1].t) / (p[i].t - p[i - 1].t);
        return f(p[i - 1]) + x * (f(p[i]) - f(p[i - 1]));
      }
    return f(p[p.length - 1]);
  }

  function holdPct(m, t, fanPct) {
    const rise = Math.max(0, t - m.amb);
    const perDeg = (p) => p.hold / (p.t - m.amb);
    if (m.fan && isNum(fanPct) && m.fan.t - m.amb > 1) {
      const lv = m.fan.levels;
      let h = lv[lv.length - 1].hold;
      if (fanPct <= lv[0].fan) h = lv[0].hold;
      else for (let i = 1; i < lv.length; i++)
        if (fanPct <= lv[i].fan) { const x = (fanPct - lv[i - 1].fan) / (lv[i].fan - lv[i - 1].fan); h = lv[i - 1].hold + x * (lv[i].hold - lv[i - 1].hold); break; }
      const g = h / (m.fan.t - m.amb);
      const at = along(m, t, perDeg), ref = along(m, m.fan.t, perDeg);
      return g * (ref > 0 ? at / ref : 1) * rise;
    }
    return along(m, t, perDeg) * rise;
  }

  // Highest BT rate of rise (C/min) at `tC` and `fanPct` with the heater at its ceiling.
  function maxRorC(m, tC, fanPct) {
    if (!m || !isNum(tC)) return NaN;
    const tau = along(m, tC, (p) => p.tau);
    const h = holdPct(m, tC, isNum(fanPct) ? fanPct : m.runFan);
    const rise = tC - m.amb;
    if (!(h > 0) || !(rise > 1)) return NaN;
    return Math.max(0, (rise / h) * (m.heaterMax - h) / tau * 60);
  }

  // Where the board starts a setpoint ramp: BT when the step fires
  // (core/recipe.cpp rampStartC). The editor has no BT, so it estimates it:
  // the step's BT trigger value, else the target of the setpoint action
  // before it. NaN when there is no estimate.
  function rampStartEstimate(trigger, prevTargetC, unit) {
    const t = trigger || {};
    if (t.type === 'bt' && isNum(t.value)) return toC(t.value, unit);
    return isNum(prevTargetC) ? prevTargetC : NaN;
  }

  // Setpoint ramps in a recipe (temperatures in `unit`) the saved model says
  // the roaster cannot follow at their steepest part. Each ramp starts from
  // rampStartEstimate; one that starts at or above its target does not ramp
  // on the board and is not checked. The fan is the last fan action before it
  // (else the autotune run fan). Returns [{step, action, needC, maxC, atC, fan}] in C.
  function checkRecipe(recipe, unit, saved, savedUnit) {
    const m = model(saved, savedUnit || unit);
    const out = [];
    if (!m || !recipe || !Array.isArray(recipe.steps)) return out;
    let sp = NaN;
    let fan = m.runFan;
    recipe.steps.forEach((s, i) => {
      const t = s.trigger || {};
      (s.actions || []).forEach((a, k) => {
        if (a.type === 'fan' && isNum(a.value)) fan = a.value;
        if (a.type !== 'setpoint' || !isNum(a.value)) return;
        const to = toC(a.value, unit);
        const from = rampStartEstimate(t, sp, unit);
        sp = to;
        if (!isNum(a.ramp) || !(a.ramp > 0) || !isNum(from) || !(to > from)) return;
        const r = rates(a.shape, a.curve);
        const pk = peak(r);
        const needC = (to - from) / a.ramp * 60 * pk.factor;
        const atC = from + (to - from) * fraction(r, pk.at);
        const maxC = maxRorC(m, atC, fan);
        if (isNum(maxC) && needC > maxC) out.push({ step: i, action: k, needC, maxC, atC, fan });
      });
    });
    return out;
  }

  // "Needs 30 °F/min near 380 °F; the saved autotune model gives about 22 °F/min at Exhaust Fan 40 %."
  function checkText(w, unit) {
    const d = unit === 'K' ? ' K' : ' \u00b0' + unit;
    return 'Needs ' + Math.round(deltaFromC(w.needC, unit)) + d + '/min near ' + Math.round(fromC(w.atC, unit)) + d +
      '; the saved autotune model gives about ' + Math.round(deltaFromC(w.maxC, unit)) + d + '/min' +
      (isNum(w.fan) ? ' at Exhaust Fan ' + Math.round(w.fan) + ' %' : '') + '. BT will fall behind: lengthen the ramp or use a flatter shape.';
  }

  return { POINTS, STEP, KNOTS, SHAPES, LABELS, HINTS, PRESETS, snap, validCurve, knotsOf, fromKnots, rates, fraction, peak, normalize, rateIcon, path,
    model, maxRorC, rampStartEstimate, checkRecipe, checkText };
});
