// Roast math shared by the page and the host tests (node croaster-webui/test).
// Pure functions only: no DOM, no network.
(function (root, factory) {
  'use strict';
  const api = factory();
  if (typeof module === 'object' && module.exports) module.exports = api;
  else root.Roast = api;
})(typeof self !== 'undefined' ? self : this, function () {
  'use strict';

  // Roast events in button order. TP can also be found automatically after CHARGE.
  const EVENTS = [
    { code: 'CHARGE', label: 'Charge' },
    { code: 'TP', label: 'Turning point' },
    { code: 'DRY_END', label: 'Dry end' },
    { code: 'FC_START', label: 'FC start' },
    { code: 'FC_END', label: 'FC end' },
    { code: 'SC_START', label: 'SC start' },
    { code: 'SC_END', label: 'SC end' },
    { code: 'DROP', label: 'Drop' },
  ];

  // Least-squares slope of v over t (units per second); null with fewer than 2 points or no time spread.
  function lsSlope(ts, vs) {
    const n = ts.length;
    if (n < 2 || vs.length !== n) return null;
    let st = 0, sv = 0;
    for (let i = 0; i < n; i++) { st += ts[i]; sv += vs[i]; }
    const mt = st / n, mv = sv / n;
    let num = 0, den = 0;
    for (let i = 0; i < n; i++) {
      const dt = ts[i] - mt;
      num += dt * (vs[i] - mv);
      den += dt * dt;
    }
    return den > 0 ? num / den : null;
  }

  // Rate of rise in degrees per minute at samples[index]: least-squares slope of the
  // samples in the last windowS seconds, times 60. Samples without a value are skipped.
  // Needs at least minPoints readings spread over at least half the window.
  function smoothedRor(samples, index, key, windowS, minPoints) {
    const w = windowS || 30;
    const need = minPoints || 5;
    const end = samples[index];
    if (!end) return null;
    const ts = [], vs = [];
    for (let i = index; i >= 0; i--) {
      const s = samples[i];
      if (end.t - s.t > w) break;
      if (typeof s[key] === 'number' && isFinite(s[key])) { ts.push(s.t); vs.push(s[key]); }
    }
    if (ts.length < need || ts[0] - ts[ts.length - 1] < w / 2) return null;
    const slope = lsSlope(ts, vs);
    return slope === null ? null : slope * 60;
  }

  // Turning point: the lowest BT after CHARGE, confirmed once BT has stayed at least
  // riseBy above it for confirmCount samples. Searched only in the first maxAfterS seconds.
  function detectTurningPoint(samples, chargeT, opts) {
    const o = Object.assign({ riseBy: 2, confirmCount: 3, maxAfterS: 300 }, opts || {});
    let first = null, min = null, above = 0;
    for (const s of samples) {
      if (s.t < chargeT || typeof s.bt !== 'number') continue;
      if (s.t - chargeT > o.maxAfterS) break;
      if (first === null) first = s;
      if (min === null || s.bt < min.bt) { min = { t: s.t, bt: s.bt }; above = 0; continue; }
      if (s.bt >= min.bt + o.riseBy) {
        // A minimum at the CHARGE sample itself means BT never dipped: no turning point.
        if (++above >= o.confirmCount) return min.t > first.t ? min : null;
      } else {
        above = 0;
      }
    }
    return null;
  }

  // "m:ss"; negative values (before CHARGE) get a leading minus.
  function formatClock(seconds) {
    if (typeof seconds !== 'number' || !isFinite(seconds)) return '--:--';
    const neg = seconds < 0;
    const s = Math.floor(Math.abs(seconds));
    const m = Math.floor(s / 60);
    const r = s % 60;
    return (neg ? '-' : '') + m + ':' + (r < 10 ? '0' : '') + r;
  }

  // Phase from the marked events: idle until CHARGE, roasting until DROP, then cooling.
  function phase(events) {
    const has = (c) => events.some((e) => e.code === c);
    if (has('DROP')) return 'cooling';
    if (has('CHARGE')) return 'roasting';
    return 'idle';
  }

  // A session records samples only while it is active: from Start until STOP or
  // End roast. Sessions saved before
  // this field existed have no `active` and count as ended.
  function isRecording(session) {
    return !!session && session.active === true;
  }

  // Marks an active session ended at nowMs. Returns true when it was recording.
  function endSession(session, nowMs) {
    if (!isRecording(session)) return false;
    session.active = false;
    session.endedAt = nowMs;
    return true;
  }

  // On page load: a saved active session keeps recording only if its last sample
  // (or its start, with no samples) is at most maxGapMs old. An older one is ended
  // at that last sample, so a stale session never resumes when the page connects.
  function resumeSession(session, nowMs, maxGapMs) {
    if (!isRecording(session)) return session;
    const ss = session.samples || [];
    const lastMs = session.startedAt + (ss.length ? ss[ss.length - 1].t * 1000 : 0);
    if (nowMs - lastMs > (maxGapMs === undefined ? 600000 : maxGapMs)) endSession(session, lastMs);
    return session;
  }

  // State shown by the clock: idle (nothing recorded), preheat (recording, no
  // CHARGE), roasting, cooling (after DROP), or ended (recording stopped).
  function sessionState(session) {
    if (!isRecording(session)) {
      return session && (session.samples.length || session.events.length) ? 'ended' : 'idle';
    }
    const ph = phase(session.events);
    return ph === 'idle' ? 'preheat' : ph;
  }

  // Event buttons follow the roast: nothing before Start, then Charge only, then
  // the later events in order, each once, until Drop. An event is refused once a
  // later one is marked; FC end needs FC start, SC start needs FC start and SC
  // end needs SC start. Undo removes the last event and re-enables it.
  const NEEDS = { FC_END: 'FC_START', SC_START: 'FC_START', SC_END: 'SC_START' };
  function eventAllowed(session, code) {
    if (!isRecording(session)) return false;
    const has = (c) => session.events.some((e) => e.code === c);
    const order = EVENTS.findIndex((e) => e.code === code);
    if (order < 0 || has(code)) return false;
    if (code === 'CHARGE') return true;
    if (!has('CHARGE') || has('DROP')) return false;
    if (NEEDS[code] && !has(NEEDS[code])) return false;
    if (code === 'DROP') return true;
    return !EVENTS.slice(order + 1).some((e) => e.code !== 'DROP' && has(e.code));
  }

  // The board has acted on "stop": Manual mode with the heater at 0 %.
  function stopConfirmed(data) {
    return !!data && data.Mode === 'Manual' && data.BurnerVal === 0;
  }

  // Round axis ticks: about `count` steps of 1, 2 or 5 x 10^n covering [min, max].
  function niceTicks(min, max, count) {
    if (!(max > min)) max = min + 1;
    const raw = (max - min) / Math.max(1, count || 5);
    const mag = Math.pow(10, Math.floor(Math.log10(raw)));
    const norm = raw / mag;
    const step = (norm <= 1 ? 1 : norm <= 2 ? 2 : norm <= 5 ? 5 : 10) * mag;
    const lo = Math.floor(min / step) * step;
    const hi = Math.ceil(max / step) * step;
    const ticks = [];
    for (let v = lo; v <= hi + step / 2; v += step) ticks.push(Math.round(v * 1e6) / 1e6);
    return { lo, hi, step, ticks };
  }

  // Temperature conversions between Croaster's units. Rates of rise and offsets
  // scale only (deltaOnly), absolute temperatures also shift.
  function convertTemp(value, from, to, deltaOnly) {
    if (typeof value !== 'number' || from === to) return value;
    let c = value;
    if (from === 'F') c = deltaOnly ? value * 5 / 9 : (value - 32) * 5 / 9;
    else if (from === 'K') c = deltaOnly ? value : value - 273.15;
    if (to === 'F') return deltaOnly ? c * 9 / 5 : c * 9 / 5 + 32;
    if (to === 'K') return deltaOnly ? c : c + 273.15;
    return c;
  }

  // Converts a recorded session in place when the device unit changes mid-session.
  function convertSession(session, to) {
    const from = session.unit;
    if (!from || from === to) { session.unit = to; return session; }
    const abs = ['bt', 'et', 'sv'];
    const rel = ['dbt', 'det'];
    for (const s of session.samples) {
      for (const k of abs) if (typeof s[k] === 'number') s[k] = convertTemp(s[k], from, to, false);
      for (const k of rel) if (typeof s[k] === 'number') s[k] = convertTemp(s[k], from, to, true);
    }
    for (const e of session.events) if (typeof e.bt === 'number') e.bt = convertTemp(e.bt, from, to, false);
    session.unit = to;
    return session;
  }

  function csvCell(v, digits) {
    if (v === null || v === undefined || (typeof v === 'number' && !isFinite(v))) return '';
    if (typeof v === 'number') return digits === undefined ? String(v) : v.toFixed(digits);
    const s = String(v);
    return /[",\n]/.test(s) ? '"' + s.replace(/"/g, '""') + '"' : s;
  }

  // CSV of a session: one row per sample, time relative to CHARGE (to the first
  // sample when there is no CHARGE). Each event is written on the first sample at
  // or after it. ror(i) returns the RoR shown in the page for sample i.
  function buildCsv(session, ror) {
    const u = session.unit || '';
    const charge = session.events.find((e) => e.code === 'CHARGE');
    const t0 = charge ? charge.t : (session.samples.length ? session.samples[0].t : 0);
    const events = session.events.slice().sort((a, b) => a.t - b.t);
    let next = 0;
    const lines = [['time', 'seconds', 'BT_' + u, 'ET_' + u, 'RoR_' + u + '_per_min', 'heater_pct', 'fan_pct',
      'SV_' + u, 'mode', 'event'].join(',')];
    session.samples.forEach((s, i) => {
      const marks = [];
      while (next < events.length && events[next].t <= s.t) marks.push(events[next++].code);
      lines.push([
        formatClock(s.t - t0), csvCell(s.t - t0, 1), csvCell(s.bt, 2), csvCell(s.et, 2), csvCell(ror(i), 2),
        csvCell(s.h), csvCell(s.f), csvCell(s.sv, 1), csvCell(s.mode), marks.join(' '),
      ].join(','));
    });
    return lines.join('\n') + '\n';
  }

  // Fake readings state from a data payload: true or false when the board reports
  // "dummy", null when it does not (Croaster Monitor, older firmware, no data yet).
  function dummyState(data) {
    return data && typeof data.dummy === 'boolean' ? data.dummy : null;
  }

  return { EVENTS, lsSlope, smoothedRor, detectTurningPoint, formatClock, phase, niceTicks, convertTemp,
    convertSession, buildCsv, isRecording, endSession, resumeSession, sessionState, stopConfirmed, eventAllowed,
    dummyState };
});
