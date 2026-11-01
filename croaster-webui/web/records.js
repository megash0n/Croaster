// Saved roasts on the board (croaster-control/src/core/records.h has the
// format and the board's checks): building a record from a recorded session,
// its CSV, and its BT curve for the reference overlay.
// Pure functions only: no DOM, no network.
(function (root, factory) {
  'use strict';
  const api = factory();
  if (typeof module === 'object' && module.exports) module.exports = api;
  else root.Records = api;
})(typeof self !== 'undefined' ? self : this, function () {
  'use strict';

  const VERSION = 1;
  const MAX_ROASTS = 25;
  const MAX_BYTES = 13000; // board limit, one WebSocket message
  const MAX_POINTS = 900;

  const isNum = (v) => typeof v === 'number' && isFinite(v);
  const r1 = (v) => (isNum(v) ? Math.round(v * 10) / 10 : null);

  // Typical spacing of the samples (median), at least 0.5 s.
  function sampleStep(samples) {
    const d = [];
    for (let i = 1; i < samples.length; i++) d.push(samples[i].t - samples[i - 1].t);
    if (!d.length) return 1;
    d.sort((a, b) => a - b);
    return Math.max(0.5, Math.round(d[Math.floor(d.length / 2)] * 10) / 10);
  }

  // Value of `key` at time t: linear between the samples either side when both
  // have a reading, else the nearer one within `gap` seconds (half a step), else null.
  function valueAt(samples, key, t, from, gap) {
    let i = from;
    while (i < samples.length - 1 && samples[i + 1].t <= t) i++;
    const a = samples[i], b = samples[i + 1];
    if (b && isNum(a[key]) && isNum(b[key]) && b.t > a.t) return { i, v: a[key] + (b[key] - a[key]) * (t - a.t) / (b.t - a.t) };
    const near = [a, b].filter((s) => s && isNum(s[key]) && Math.abs(s.t - t) <= gap)
      .sort((x, y) => Math.abs(x.t - t) - Math.abs(y.t - t))[0];
    return { i, v: near ? near[key] : null };
  }

  // A roast record from a session ({unit, samples:[{t,bt,et}], events:[{code,t,bt}]}).
  // `o`: {name, dateS, bean, beanName, greenG, roastedG, notes}. The curve is
  // resampled every dt seconds from CHARGE (from the first sample without
  // one); dt grows until the record fits MAX_POINTS and MAX_BYTES.
  function buildRoast(session, o) {
    const ss = (session.samples || []).filter((s) => isNum(s.t));
    const charge = (session.events || []).find((e) => e.code === 'CHARGE');
    const t0 = charge ? charge.t : (ss.length ? ss[0].t : 0);
    const base = {
      version: VERSION, name: String(o.name || '').trim(), date: Math.round(o.dateS), unit: session.unit || 'C',
    };
    if (o.bean) { base.bean = o.bean; if (o.beanName) base.beanName = o.beanName; }
    if (isNum(o.greenG)) base.greenG = r1(o.greenG);
    if (isNum(o.roastedG)) base.roastedG = r1(o.roastedG);
    if (o.notes) base.notes = String(o.notes);
    const first = ss.length ? ss[0].t : t0, last = ss.length ? ss[ss.length - 1].t : t0;
    const span = Math.max(0, last - first);
    let dt = sampleStep(ss);
    if (span / dt + 1 > MAX_POINTS) dt = Math.ceil(span / (MAX_POINTS - 1) * 10) / 10;
    for (;;) {
      const rec = Object.assign({}, base, { start: r1(first - t0), dt, bt: [], et: [] });
      let ib = 0, ie = 0;
      for (let t = first; t <= last + 1e-6 && ss.length; t += dt) {
        const b = valueAt(ss, 'bt', t, ib, dt / 2), e = valueAt(ss, 'et', t, ie, dt / 2);
        ib = b.i; ie = e.i;
        rec.bt.push(r1(b.v));
        rec.et.push(r1(e.v));
      }
      rec.events = (session.events || []).slice(0, 16).map((e) => {
        const x = { code: e.code, t: r1(e.t - t0) };
        if (isNum(e.bt)) x.bt = r1(e.bt);
        return x;
      });
      if (JSON.stringify(rec).length <= MAX_BYTES - 200 || dt >= 60) return rec;
      dt = Math.min(60, Math.round(dt * 1.5 * 10) / 10);
    }
  }

  // Weight loss in %, or null without both weights.
  function lossPct(greenG, roastedG) {
    return isNum(greenG) && isNum(roastedG) && greenG > 0 ? Math.round((greenG - roastedG) / greenG * 1000) / 10 : null;
  }

  // Charge to drop (or to the last point) in seconds.
  function duration(rec) {
    const drop = (rec.events || []).find((e) => e.code === 'DROP');
    if (drop) return drop.t;
    return isNum(rec.start) && isNum(rec.dt) && Array.isArray(rec.bt) ? rec.start + rec.dt * Math.max(0, rec.bt.length - 1) : null;
  }

  function csvCell(v) {
    if (v === null || v === undefined) return '';
    const s = String(v);
    return /[",\n]/.test(s) ? '"' + s.replace(/"/g, '""') + '"' : s;
  }

  // CSV of a saved roast: a header block, then one row per curve point with
  // seconds from CHARGE and the events at or before each point.
  function roastCsv(rec) {
    const u = rec.unit || '';
    const head = [['name', rec.name], ['date', new Date(rec.date * 1000).toISOString()], ['bean', rec.beanName || ''],
      ['green_g', rec.greenG], ['roasted_g', rec.roastedG], ['loss_pct', lossPct(rec.greenG, rec.roastedG)],
      ['notes', rec.notes || '']].map((r) => r.map(csvCell).join(','));
    const events = (rec.events || []).slice().sort((a, b) => a.t - b.t);
    let next = 0;
    const rows = [['seconds', 'BT_' + u, 'ET_' + u, 'event'].join(',')];
    (rec.bt || []).forEach((bt, i) => {
      const t = Math.round((rec.start + i * rec.dt) * 10) / 10;
      const marks = [];
      while (next < events.length && events[next].t <= t + 1e-6) marks.push(events[next++].code);
      rows.push([t, csvCell(bt), csvCell((rec.et || [])[i]), marks.join(' ')].join(','));
    });
    return head.join('\n') + '\n\n' + rows.join('\n') + '\n';
  }

  function convert(v, from, to) {
    if (!isNum(v) || from === to) return v;
    let c = from === 'F' ? (v - 32) * 5 / 9 : from === 'K' ? v - 273.15 : v;
    return to === 'F' ? c * 9 / 5 + 32 : to === 'K' ? c + 273.15 : c;
  }

  // Reference overlay: the BT curve as [{t, v}] in seconds from CHARGE and in
  // `unit`, gaps left out, and the events [{code, t}].
  function refCurve(rec, unit) {
    const pts = [];
    (rec.bt || []).forEach((v, i) => { if (isNum(v)) pts.push({ t: rec.start + i * rec.dt, v: convert(v, rec.unit, unit || rec.unit) }); });
    return { name: rec.name, points: pts, events: (rec.events || []).map((e) => ({ code: e.code, t: e.t })) };
  }

  // The roast a new save deletes to make room: the oldest when `max` are kept.
  function pruneCandidate(list, max) {
    if (!Array.isArray(list) || list.length < (max || MAX_ROASTS)) return null;
    return list.slice().sort((a, b) => (a.date || 0) - (b.date || 0))[0];
  }

  return { VERSION, MAX_ROASTS, MAX_BYTES, MAX_POINTS, sampleStep, buildRoast, lossPct, duration, roastCsv,
    refCurve, pruneCandidate };
});
