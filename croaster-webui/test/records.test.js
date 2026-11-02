'use strict';
const test = require('node:test');
const assert = require('node:assert');
const RC = require('../web/records.js');

function session(n, step, extra) {
  const samples = [];
  for (let i = 0; i < n; i++) samples.push({ t: i * step, bt: 200 + i * 0.5, et: 300 + i * 0.25 });
  return Object.assign({ unit: 'F', samples, events: [{ code: 'CHARGE', t: 30, bt: 215 }, { code: 'DROP', t: 600, bt: 410 }] }, extra);
}

test('buildRoast: times from CHARGE, weights and bean kept, readings to 0.1', () => {
  const r = RC.buildRoast(session(661, 1), { name: ' Kenya ', dateS: 1761900000.4, bean: 'kenya-aa', beanName: 'Kenya AA', greenG: 250, roastedG: 212.04 });
  assert.strictEqual(r.version, 1);
  assert.strictEqual(r.name, 'Kenya');
  assert.strictEqual(r.date, 1761900000);
  assert.strictEqual(r.start, -30);
  assert.strictEqual(r.dt, 1);
  assert.strictEqual(r.bt.length, 661);
  assert.strictEqual(r.bt[30], 215);
  assert.strictEqual(r.et.length, r.bt.length);
  assert.strictEqual(r.roastedG, 212);
  assert.deepStrictEqual(r.events[0], { code: 'CHARGE', t: 0, bt: 215 });
  assert.strictEqual(RC.duration(r), 570);
  assert.ok(JSON.stringify(r).length <= RC.MAX_BYTES);
  assert.strictEqual(r.recipe, undefined);
});

test('buildRoast: the recipe that ran is kept by name, cut to 40 characters', () => {
  assert.strictEqual(RC.buildRoast(session(10, 1), { name: 'x', dateS: 1, recipe: 'City+' }).recipe, 'City+');
  assert.strictEqual(RC.buildRoast(session(10, 1), { name: 'x', dateS: 1, recipe: 'é'.repeat(50) }).recipe, 'é'.repeat(40));
  assert.match(RC.roastCsv(RC.buildRoast(session(10, 1), { name: 'x', dateS: 1, recipe: 'City+' })), /\nrecipe,City\+\n/);
});

test('buildRoast: long or dense roasts are thinned to fit the board', () => {
  const r = RC.buildRoast(session(3600, 0.5), { name: 'x', dateS: 1 });
  assert.ok(r.bt.length <= RC.MAX_POINTS);
  assert.ok(JSON.stringify(r).length <= RC.MAX_BYTES - 200);
  assert.ok(r.dt >= 2);
  const gaps = session(10, 1);
  gaps.samples[4].bt = null;
  gaps.samples[5].bt = null;
  const g = RC.buildRoast(gaps, { name: 'x', dateS: 1 });
  assert.strictEqual(g.bt[5], null); // no reading either side: a gap
  assert.strictEqual(g.bt[3], 201.5);
});

test('lossPct, CSV and the reference curve', () => {
  assert.strictEqual(RC.lossPct(250, 212), 15.2);
  assert.strictEqual(RC.lossPct(0, 212), null);
  const r = RC.buildRoast(session(40, 1), { name: 'A, "b"', dateS: 1761900000, greenG: 100, roastedG: 85 });
  const csv = RC.roastCsv(r);
  assert.match(csv, /^name,"A, ""b"""\n/);
  assert.match(csv, /loss_pct,15\n/);
  assert.match(csv, /\n0,215,307.5,CHARGE\n/);
  const ref = RC.refCurve(r, 'C');
  assert.strictEqual(ref.points[0].t, -30);
  assert.ok(Math.abs(ref.points[0].v - (200 - 32) * 5 / 9) < 1e-9);
  assert.deepStrictEqual(ref.events[0], { code: 'CHARGE', t: 0 });
});

test('pruneCandidate: the oldest once the board keeps 25', () => {
  const list = Array.from({ length: 25 }, (_, i) => ({ id: 'r' + i, date: 100 - i }));
  assert.strictEqual(RC.pruneCandidate(list.slice(0, 24)), null);
  assert.strictEqual(RC.pruneCandidate(list).id, 'r24');
});

test('beans: form text becomes a record, purchases add stock', () => {
  const b = RC.beanFromForm({ name: ' Nyeri AA ', origin: 'Kenya', process: 'washed', region: '', altitudeMin: '1700',
    altitudeMax: '', density: 'High', flavours: 'blackcurrant, grapefruit,', score: '87.5', favourite: true,
    blend: false, components: [{ origin: 'Brazil' }] }, { stockG: 100, purchases: [] });
  assert.deepEqual(b, { version: 1, name: 'Nyeri AA', origin: 'Kenya', process: 'washed', altitudeMin: 1700,
    score: 87.5, density: 'high', flavours: ['blackcurrant', 'grapefruit'], favourite: true, stockG: 100, purchases: [] });
  assert.equal(RC.beanFromForm({ name: 'x', origin: 'y', process: 'z', density: '720' }).density, 720);
  const blend = RC.beanFromForm({ name: 'House', origin: 'Blend', process: 'mixed', blend: true,
    components: [{ origin: 'Brazil', process: 'natural', percent: '60' }, { origin: '', process: '' }] });
  assert.deepEqual(blend.components, [{ origin: 'Brazil', process: 'natural', percent: 60 }]);
  let s = RC.addPurchase({ name: 'x' }, { amount: '2', unit: 'lb', price: '18', date: '2026-02-10' });
  assert.equal(s.stockG, 907.2);
  assert.deepEqual(s.purchases, [{ g: 907.2, price: 18, date: '2026-02-10' }]);
  s = RC.addPurchase(s, { amount: '1', unit: 'kg' });
  assert.equal(s.stockG, 1907.2);
  assert.equal(RC.addPurchase(s, { amount: '', unit: 'kg' }), s);
  assert.equal(RC.stockText(1907.2), '1.91 kg');
  assert.equal(RC.stockText(850), '850 g');
  assert.equal(RC.stockText(undefined), '—');
});
