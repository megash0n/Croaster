// Host tests for web/roast.js. Run: node --test croaster-webui/test/*.test.js
'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const R = require('../web/roast.js');

const close = (a, b, eps) => assert.ok(Math.abs(a - b) < (eps || 1e-9), a + ' != ' + b);

test('lsSlope fits a straight line exactly', () => {
  close(R.lsSlope([0, 1, 2, 3], [10, 12, 14, 16]), 2);
  close(R.lsSlope([5, 6, 8], [1, 1, 1]), 0);
  assert.equal(R.lsSlope([1], [1]), null);
  assert.equal(R.lsSlope([2, 2, 2], [1, 2, 3]), null); // no time spread
});

test('lsSlope averages noise instead of following the last two points', () => {
  const ts = [], vs = [];
  for (let t = 0; t <= 30; t++) { ts.push(t); vs.push(200 + 0.25 * t + (t % 2 ? 0.5 : -0.5)); }
  close(R.lsSlope(ts, vs), 0.25, 0.01);
});

test('smoothedRor is the 30 s least-squares slope times 60', () => {
  const samples = [];
  for (let t = 0; t <= 120; t++) samples.push({ t, bt: 150 + t * 0.2 }); // 12 degrees per minute
  close(R.smoothedRor(samples, 120, 'bt', 30), 12, 1e-6);
  // Only the window counts: a ramp change 40 s ago does not leak in.
  const bent = samples.map((s) => ({ t: s.t, bt: s.t < 80 ? 150 : 150 + (s.t - 80) * 0.1 }));
  close(R.smoothedRor(bent, 120, 'bt', 30), 6, 1e-6);
});

test('smoothedRor needs enough points and skips gaps', () => {
  const samples = [{ t: 0, bt: 100 }, { t: 1, bt: 101 }, { t: 2, bt: 102 }];
  assert.equal(R.smoothedRor(samples, 2, 'bt', 30), null);
  const gappy = [];
  for (let t = 0; t <= 30; t++) gappy.push({ t, bt: t % 5 === 0 ? null : 100 + t });
  close(R.smoothedRor(gappy, 30, 'bt', 30), 60, 1e-6);
});

test('detectTurningPoint finds the BT minimum after CHARGE once BT has risen', () => {
  const samples = [];
  const bt = [390, 395, 400, 300, 250, 220, 205, 200, 201, 203, 205, 207, 210];
  bt.forEach((v, i) => samples.push({ t: 100 + i, bt: v }));
  assert.equal(R.detectTurningPoint(samples.slice(0, 10), 103), null); // not confirmed yet
  assert.deepEqual(R.detectTurningPoint(samples, 103), { t: 107, bt: 200 });
  assert.equal(R.detectTurningPoint(samples, 103, { maxAfterS: 3 }), null);
  const rising = [0, 1, 2, 3, 4, 5].map((i) => ({ t: i, bt: 300 + 3 * i }));
  assert.equal(R.detectTurningPoint(rising, 0), null); // BT never dipped after CHARGE
});

test('formatClock', () => {
  assert.equal(R.formatClock(0), '0:00');
  assert.equal(R.formatClock(61.9), '1:01');
  assert.equal(R.formatClock(-75), '-1:15');
  assert.equal(R.formatClock(NaN), '--:--');
});

test('phase follows CHARGE and DROP', () => {
  assert.equal(R.phase([]), 'idle');
  assert.equal(R.phase([{ code: 'CHARGE' }]), 'roasting');
  assert.equal(R.phase([{ code: 'CHARGE' }, { code: 'DROP' }]), 'cooling');
});

test('niceTicks covers the range with round steps', () => {
  const t = R.niceTicks(103, 447, 5);
  assert.equal(t.step, 100);
  assert.deepEqual(t.ticks, [100, 200, 300, 400, 500]);
  assert.ok(R.niceTicks(5, 5, 4).ticks.length >= 2);
});

test('convertTemp handles absolute values and deltas', () => {
  close(R.convertTemp(100, 'C', 'F'), 212);
  close(R.convertTemp(212, 'F', 'K'), 373.15);
  close(R.convertTemp(18, 'F', 'C', true), 10);
  close(R.convertTemp(10, 'C', 'K', true), 10);
});

test('convertSession rewrites samples and events', () => {
  const s = { unit: 'F', samples: [{ t: 0, bt: 212, et: 32, dbt: 18, det: null, sv: 392 }],
    events: [{ code: 'CHARGE', t: 0, bt: 212 }] };
  R.convertSession(s, 'C');
  assert.equal(s.unit, 'C');
  close(s.samples[0].bt, 100);
  close(s.samples[0].et, 0);
  close(s.samples[0].dbt, 10);
  assert.equal(s.samples[0].det, null);
  close(s.samples[0].sv, 200);
  close(s.events[0].bt, 100);
});

test('buildCsv writes one row per sample with events and CHARGE-relative time', () => {
  const session = {
    unit: 'F',
    samples: [
      { t: 10, bt: 400, et: 450, dbt: 0, h: 50, f: 60, sv: null, mode: 'Manual' },
      { t: 11, bt: 380.123, et: 449, dbt: 0, h: 50, f: 60, sv: null, mode: 'Manual' },
      { t: 12, bt: 360, et: 448, dbt: 0, h: 55, f: 60, sv: 400, mode: 'PID trip:comms' },
    ],
    events: [{ code: 'CHARGE', t: 11, bt: 380 }, { code: 'TP', t: 12, bt: 360 }],
  };
  const lines = R.buildCsv(session, (i) => [null, 1.5, -2][i]).trim().split('\n');
  assert.equal(lines[0], 'time,seconds,BT_F,ET_F,RoR_F_per_min,heater_pct,fan_pct,SV_F,mode,event');
  assert.equal(lines[1], '-0:01,-1.0,400.00,450.00,,50,60,,Manual,');
  assert.equal(lines[2], '0:00,0.0,380.12,449.00,1.50,50,60,,Manual,CHARGE');
  assert.equal(lines[3], '0:01,1.0,360.00,448.00,-2.00,55,60,400.0,PID trip:comms,TP');
});

test('a session records only while active; saved sessions without the field count as ended', () => {
  assert.equal(R.isRecording({ samples: [], events: [] }), false);
  assert.equal(R.isRecording({ samples: [], events: [], active: false }), false);
  assert.equal(R.isRecording({ samples: [], events: [], active: true }), true);
  assert.equal(R.isRecording(null), false);
});

test('endSession stops recording once and records the end time', () => {
  const s = { startedAt: 0, samples: [], events: [], active: true, endedAt: null };
  assert.equal(R.endSession(s, 5000), true);
  assert.equal(s.active, false);
  assert.equal(s.endedAt, 5000);
  assert.equal(R.endSession(s, 9000), false); // already ended: end time kept
  assert.equal(s.endedAt, 5000);
});

test('resumeSession keeps a recent roast and ends a stale one at its last sample', () => {
  const recent = { startedAt: 1000, samples: [{ t: 60 }], events: [], active: true, endedAt: null };
  R.resumeSession(recent, 1000 + 60000 + 30000, 600000);
  assert.equal(recent.active, true);
  const stale = { startedAt: 1000, samples: [{ t: 60 }], events: [], active: true, endedAt: null };
  R.resumeSession(stale, 1000 + 60000 + 700000, 600000);
  assert.equal(stale.active, false);
  assert.equal(stale.endedAt, 61000);
  const empty = { startedAt: 1000, samples: [], events: [], active: true, endedAt: null };
  R.resumeSession(empty, 1000 + 700000, 600000); // started, never sampled
  assert.equal(empty.active, false);
  const old = { startedAt: 1000, samples: [{ t: 1 }], events: [] }; // no active field: untouched
  R.resumeSession(old, 1e12, 600000);
  assert.equal(old.active, undefined);
});

test('sessionState: idle until started, then preheat, roasting, cooling, ended', () => {
  const s = { startedAt: 0, samples: [], events: [], active: false, endedAt: null };
  assert.equal(R.sessionState(s), 'idle');
  s.samples.push({ t: 0 }); // samples alone (an old saved session) do not mean a roast
  assert.equal(R.sessionState(s), 'ended');
  s.samples = [];
  s.active = true;
  assert.equal(R.sessionState(s), 'preheat');
  s.events.push({ code: 'CHARGE', t: 10 });
  assert.equal(R.sessionState(s), 'roasting');
  s.events.push({ code: 'DROP', t: 600 });
  assert.equal(R.sessionState(s), 'cooling');
  R.endSession(s, 700000);
  assert.equal(R.sessionState(s), 'ended');
});

test('stopConfirmed: Manual with the heater at 0 %', () => {
  assert.equal(R.stopConfirmed({ Mode: 'Manual', BurnerVal: 0, FanVal: 65 }), true);
  assert.equal(R.stopConfirmed({ Mode: 'PID', BurnerVal: 0 }), false);
  assert.equal(R.stopConfirmed({ Mode: 'Manual', BurnerVal: 20 }), false);
  assert.equal(R.stopConfirmed({ Mode: 'Manual' }), false); // a board without controls never confirms
  assert.equal(R.stopConfirmed(null), false);
});

test('eventAllowed: nothing before Start, Charge first, later events in order, Drop last', () => {
  const ses = (active, codes) => ({ active, samples: [], events: codes.map((code, i) => ({ code, t: i })) });
  const allowed = (s) => R.EVENTS.map((e) => e.code).filter((c) => R.eventAllowed(s, c));
  assert.deepEqual(allowed(ses(false, [])), []); // idle: all disabled
  assert.deepEqual(allowed(ses(true, [])), ['CHARGE']); // after Start
  assert.deepEqual(allowed(ses(true, ['CHARGE'])), ['TP', 'DRY_END', 'FC_START', 'DROP']);
  assert.deepEqual(allowed(ses(true, ['CHARGE', 'TP', 'DRY_END'])), ['FC_START', 'DROP']);
  assert.deepEqual(allowed(ses(true, ['CHARGE', 'DRY_END', 'FC_START'])), ['FC_END', 'SC_START', 'DROP']);
  assert.deepEqual(allowed(ses(true, ['CHARGE', 'FC_START', 'FC_END', 'SC_START'])), ['SC_END', 'DROP']);
  assert.deepEqual(allowed(ses(true, ['CHARGE', 'FC_START', 'DROP'])), []); // after Drop
  assert.deepEqual(allowed(ses(false, ['CHARGE', 'FC_START'])), []); // ended
  assert.equal(R.eventAllowed(ses(true, ['CHARGE']), 'NOPE'), false);
});

test('dummyState reads the fake readings flag from a data payload', () => {
  assert.equal(R.dummyState({ BT: 20, dummy: true }), true);
  assert.equal(R.dummyState({ BT: 20, dummy: false }), false);
  assert.equal(R.dummyState({ BT: 20 }), null); // Croaster Monitor and older firmware
  assert.equal(R.dummyState({ dummy: 1 }), null);
  assert.equal(R.dummyState(null), null);
});
