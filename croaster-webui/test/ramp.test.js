// Host tests for web/ramp.js (ramp shapes and the saved-model check).
'use strict';
const test = require('node:test');
const assert = require('node:assert');
const RS = require('../web/ramp.js');
const L = require('../web/library.js');

const near = (a, b, tol) => assert.ok(Math.abs(a - b) <= tol, a + ' not within ' + tol + ' of ' + b);

test('preset curves match the board (core/recipe.cpp) and sit on the 5-step grid', () => {
  assert.deepStrictEqual(RS.PRESETS.normal, [5, 10, 15, 20, 30, 45, 60, 75, 90, 95, 100, 95, 90, 75, 60, 45, 30, 20, 15, 10, 5]);
  assert.deepStrictEqual(RS.PRESETS.front, [100, 95, 90, 85, 80, 75, 70, 65, 60, 55, 50, 45, 40, 35, 30, 25, 20, 15, 10, 5, 0]);
  assert.deepStrictEqual(RS.PRESETS.back, RS.PRESETS.front.slice().reverse());
  assert.ok(RS.PRESETS.linear.every((v) => v === 50));
  for (const sh of ['linear', 'normal', 'front', 'back']) {
    assert.strictEqual(RS.PRESETS[sh].length, RS.POINTS);
    assert.ok(RS.PRESETS[sh].every((v) => v % RS.STEP === 0 && v >= 0 && v <= 100));
  }
  assert.deepStrictEqual(RS.SHAPES, L.SHAPES);
});

test('fraction is the normalised area under the rate curve', () => {
  near(RS.fraction(RS.PRESETS.linear, 0.3), 0.3, 1e-9);
  near(RS.fraction(RS.PRESETS.front, 0.5), 0.75, 1e-9); // 2u - u^2
  near(RS.fraction(RS.PRESETS.back, 0.5), 0.25, 1e-9);  // u^2
  near(RS.fraction(RS.PRESETS.normal, 0.5), 0.5, 1e-9);
  assert.ok(RS.fraction(RS.PRESETS.normal, 0.2) < 0.2);
  assert.strictEqual(RS.fraction(RS.PRESETS.front, -1), 0);
  assert.strictEqual(RS.fraction(RS.PRESETS.front, 2), 1);
  const p = RS.path(150, 250, 10, RS.PRESETS.front, 2);
  assert.deepStrictEqual(p.map((x) => x.t), [0, 5, 10]);
  near(p[1].v, 225, 1e-9);
  near(RS.peak(RS.PRESETS.front).factor, 2, 1e-9);
  assert.strictEqual(RS.peak(RS.PRESETS.back).at, 1);
  assert.strictEqual(RS.peak(RS.PRESETS.linear).factor, 1);
  assert.deepStrictEqual(RS.peak(RS.PRESETS.back).spots, [{ from: 1, to: 1 }]);
  assert.deepStrictEqual(RS.peak(RS.PRESETS.linear).spots, [{ from: 0, to: 1 }]);
});

test('peak lists every steepest place; normalize scales the tallest to 100', () => {
  const both = RS.peak(RS.fromKnots([100, 0, 0, 0, 100]));
  assert.deepStrictEqual(both.spots, [{ from: 0, to: 0 }, { from: 1, to: 1 }]);
  assert.strictEqual(both.at, 0);
  assert.deepStrictEqual(RS.normalize([0, 0, 0, 0, 5]), [0, 0, 0, 0, 100]);
  assert.deepStrictEqual(RS.normalize([10, 20, 5]), [50, 100, 25]);
  assert.deepStrictEqual(RS.normalize([0, 0]), [0, 0]);
  // Small points, once scaled, give the full curve (whole-number rates lose detail when small).
  assert.deepStrictEqual(RS.fromKnots(RS.normalize([0, 0, 0, 0, 5])), RS.fromKnots([0, 0, 0, 0, 100]));
});

test('snap and custom curves', () => {
  assert.strictEqual(RS.snap(47), 45);
  assert.strictEqual(RS.snap(48), 50);
  assert.strictEqual(RS.snap(-3), 0);
  assert.strictEqual(RS.snap(140), 100);
  assert.ok(!RS.validCurve(new Array(21).fill(0)));
  assert.ok(!RS.validCurve(new Array(20).fill(5)));
  assert.deepStrictEqual(RS.rates('custom', [1, 2]), RS.PRESETS.linear); // invalid: linear
  const mine = RS.PRESETS.back.slice();
  assert.deepStrictEqual(RS.rates('custom', mine), mine);
  assert.notStrictEqual(RS.rates('front'), RS.PRESETS.front); // a copy
});

// Two points 150 and 220 C, ambient 25, heater ceiling 90.
const saved = { ambient: 25, runFan: 50, heaterMax: 90, fan: null,
  points: [{ target: 150, gain: 2.8, tau: 120, hold: 30 }, { target: 220, gain: 2.6, tau: 140, hold: 45 }] };

test('maxRorC follows core/roaster_model.cpp maxRorCpm', () => {
  const m = RS.model(saved, 'C');
  // at 150: k = 125 / 30, (90 - 30) / 120 s * 60
  near(RS.maxRorC(m, 150, 50), 125 / 30 * 60 / 120 * 60, 1e-9);
  // fan levels: hold scales with the fan conductance at the test temperature
  const withFan = Object.assign({}, saved, { fan: { target: 150, levels: [{ fan: 30, hold: 24 }, { fan: 90, hold: 36 }] } });
  const mf = RS.model(withFan, 'C');
  near(RS.maxRorC(mf, 150, 60), 125 / 30 * (90 - 30) / 120 * 60, 1e-9); // 60 % fan: hold 30
  assert.ok(RS.maxRorC(mf, 150, 90) < RS.maxRorC(mf, 150, 30)); // more air, less headroom
  assert.strictEqual(RS.model(null, 'C'), null);
  assert.strictEqual(RS.model({ points: [] }, 'C'), null);
});

test('checkRecipe flags a ramp steeper than the model allows, from the target before it', () => {
  const recipe = { chargeTemp: 200, steps: [
    { trigger: { type: 'event', value: 'TP' }, actions: [{ type: 'setpoint', value: 150 }] },
    { trigger: { type: 'time', value: 60 }, actions: [{ type: 'fan', value: 50 }, { type: 'setpoint', value: 210, ramp: 30, shape: 'front' }] },
    { trigger: { type: 'time', value: 300 }, actions: [{ type: 'setpoint', value: 215, ramp: 600 }] },
  ] };
  const w = RS.checkRecipe(recipe, 'C', saved, 'C');
  assert.strictEqual(w.length, 1);
  assert.strictEqual(w[0].step, 1);
  assert.strictEqual(w[0].action, 1);
  near(w[0].needC, 120 * 2, 1e-9); // 60 C in 30 s, twice as steep at the start
  near(w[0].atC, 150, 1e-9);
  assert.match(RS.checkText(w[0], 'F'), /^Needs 432 °F\/min near 302 °F; the saved autotune model gives about \d+ °F\/min at fan 50 %/);
  // Same ramp, linear and slow enough: nothing.
  recipe.steps[1].actions[1] = { type: 'setpoint', value: 210, ramp: 600 };
  assert.deepStrictEqual(RS.checkRecipe(recipe, 'C', saved, 'C'), []);
  // No saved model: nothing to check.
  assert.deepStrictEqual(RS.checkRecipe(recipe, 'C', null), []);
  // Model saved in F, recipe in C.
  // The start is the BT estimate, not "chargeTemp": with no BT trigger and no
  // setpoint before it, nothing to check.
  assert.deepStrictEqual(RS.checkRecipe({ chargeTemp: 200, steps: [
    { trigger: { type: 'event', value: 'TP' }, actions: [{ type: 'setpoint', value: 210, ramp: 30, shape: 'front' }] }] }, 'C', saved, 'C'), []);
  const savedF = { ambient: 77, runFan: 50, heaterMax: 90, points: [{ target: 302, tau: 120, hold: 30 }, { target: 428, tau: 140, hold: 45 }] };
  const m = RS.model(savedF, 'F');
  near(m.points[0].t, 150, 1e-9);
});

test('library checks and round-trips ramp shapes, power ramps and chargeTemp', () => {
  const step = (a) => ({ steps: [{ trigger: { type: 'bt', value: 100 }, actions: [a] }] });
  assert.strictEqual(L.recipeReason(step({ type: 'setpoint', value: 200, ramp: 60, shape: 's' }), 'C'), 'shape must be linear, normal, front, back or custom');
  assert.strictEqual(L.recipeReason(step({ type: 'setpoint', value: 200, ramp: 60, shape: 'custom' }), 'C'), 'curve must have 21 rates');
  assert.strictEqual(L.recipeReason(step({ type: 'power', value: 60, ramp: 60, shape: 'custom', curve: new Array(21).fill(0) }), 'C'), 'curve needs a rate above 0');
  assert.strictEqual(L.recipeReason(step({ type: 'power', value: 60, ramp: 60, shape: 'custom', curve: new Array(21).fill(101) }), 'C'), 'curve rates must be 0..100');
  assert.strictEqual(L.recipeReason(step({ type: 'setpoint', value: 200, curve: RS.PRESETS.back }), 'C'), 'curve needs shape custom');
  assert.strictEqual(L.recipeReason(step({ type: 'fan', value: 60, ramp: 30 }), 'C'), 'fan does not ramp');
  assert.strictEqual(L.recipeReason(step({ type: 'power', value: 60, ramp: 30, shape: 'back' }), 'C'), '');
  assert.strictEqual(L.recipeReason(Object.assign({ chargeTemp: 700 }, step({ type: 'fan', value: 1 })), 'F'), 'chargeTemp must be 0..350 C');
  assert.strictEqual(L.recipeReason(Object.assign({ chargeTemp: 392 }, step({ type: 'fan', value: 1 })), 'F'), '');
  const many = { steps: Array.from({ length: 9 }, () => ({ trigger: { type: 'bt', value: 100 },
    actions: [{ type: 'setpoint', value: 200, ramp: 60, shape: 'custom', curve: RS.PRESETS.normal }] })) };
  assert.strictEqual(L.recipeReason(many, 'C'), 'a recipe has up to 8 custom curves');
  many.steps.pop();
  assert.strictEqual(L.recipeReason(many, 'C'), '');

  const rows = [{ type: 'bt', op: 'gte', value: '300', tol: '', event: 'CHARGE', anyOrder: false, label: '',
    actions: [{ type: 'setpoint', value: '400', ramp: '120', shape: 'custom', curve: RS.PRESETS.back.slice() },
      { type: 'power', value: '70', ramp: '30', shape: 'front' }, { type: 'power', value: '50', ramp: '', shape: 'front' }] }];
  const rec = L.recipeFromEditor(rows, 'auto', '390');
  assert.strictEqual(rec.chargeTemp, 390);
  assert.deepStrictEqual(rec.steps[0].actions[0], { type: 'setpoint', value: 400, ramp: 120, shape: 'custom', curve: RS.PRESETS.back });
  assert.deepStrictEqual(rec.steps[0].actions[1], { type: 'power', value: 70, ramp: 30, shape: 'front' });
  assert.deepStrictEqual(rec.steps[0].actions[2], { type: 'power', value: 50 }); // no ramp: no shape
  assert.strictEqual(L.recipeReason(rec, 'F'), '');
  assert.strictEqual(L.recipeFromEditor(rows, 'auto', '').chargeTemp, undefined);
  const back = L.recipeToEditor(rec, 'F', 'F');
  assert.strictEqual(back[0].actions[0].shape, 'custom');
  assert.deepStrictEqual(back[0].actions[0].curve, RS.PRESETS.back);
  assert.strictEqual(back[0].actions[1].shape, 'front');
  assert.match(L.describeStep(rec, 0, 'F', 'F'), /setpoint 400 °F over 2:00, custom, power 70 % over 0:30, front/);
});

test('rampStartEstimate matches the board: BT at the step, estimated from the trigger or the target before', () => {
  near(RS.rampStartEstimate({ type: 'bt', value: 302 }, 100, 'F'), 150, 1e-9); // BT trigger wins
  assert.strictEqual(RS.rampStartEstimate({ type: 'event', value: 'TP' }, 149, 'F'), 149); // previous target, C
  assert.ok(Number.isNaN(RS.rampStartEstimate({ type: 'time', value: 60 }, NaN, 'C')));
  assert.ok(Number.isNaN(RS.rampStartEstimate(undefined, undefined, 'C')));
});

test('checkRecipe starts a ramp at its BT trigger and skips one that starts at or above the target', () => {
  const steep = { type: 'setpoint', value: 210, ramp: 30, shape: 'front' };
  // Setpoint 150 before, but the ramp fires at BT 120: the climb is 120 -> 210.
  const w = RS.checkRecipe({ steps: [
    { trigger: { type: 'event', value: 'TP' }, actions: [{ type: 'setpoint', value: 150 }] },
    { trigger: { type: 'bt', value: 120 }, actions: [steep] }] }, 'C', saved, 'C');
  assert.strictEqual(w.length, 1);
  near(w[0].needC, 90 / 30 * 60 * 2, 1e-9);
  near(w[0].atC, 120, 1e-9);
  // BT trigger above the target: the board holds the target, no ramp to check.
  assert.deepStrictEqual(RS.checkRecipe({ steps: [{ trigger: { type: 'bt', value: 215 }, actions: [steep] }] }, 'C', saved, 'C'), []);
});

test('rateIcon draws the rate of climb: Front falls, Back rises, Normal is a hump, Linear is flat', () => {
  const ys = (sh) => RS.rateIcon(RS.rates(sh), 0, 0, 20, 100).split(/[ML]/).filter(Boolean).map((p) => Number(p.split(' ')[1]));
  const front = ys('front'), back = ys('back'), normal = ys('normal'), linear = ys('linear');
  assert.ok(front[0] < front[front.length - 1]); // y grows downward: high rate first
  assert.ok(back[0] > back[back.length - 1]);
  const mid = normal[(normal.length - 1) / 2];
  assert.ok(mid < normal[0] && mid < normal[normal.length - 1]);
  assert.ok(linear.every((y) => y === linear[0]));
  assert.deepStrictEqual(ys('custom'), linear); // no curve: linear
  assert.strictEqual(RS.rateIcon(RS.rates('custom', back.map((_, i) => i * 5)), 0, 0, 20, 100), RS.rateIcon(RS.rates('back'), 0, 0, 20, 100));
});

test('custom curves: 5 editor points, smooth 21 rates through them, no overshoot', () => {
  assert.deepStrictEqual(RS.knotsOf(RS.PRESETS.back), [0, 25, 50, 75, 100]);
  const lin = RS.fromKnots([0, 25, 50, 75, 100]);
  assert.deepStrictEqual(lin, RS.PRESETS.back); // straight points give a straight line
  const hump = RS.fromKnots([0, 0, 100, 0, 0]);
  assert.strictEqual(hump.length, RS.POINTS);
  assert.ok(hump.every((v) => v >= 0 && v <= 100 && Number.isInteger(v)));
  assert.deepStrictEqual(RS.knotsOf(hump), [0, 0, 100, 0, 0]); // passes through the points
  assert.ok(hump.slice(0, 6).every((v) => v === 0)); // flat stays flat: no dip below 0
  assert.ok(hump[7] > 0 && hump[7] < hump[9]);
  assert.ok(RS.validCurve(RS.fromKnots([100, 40, 60, 10, 0])));
  // A 21-rate curve from an older recipe still loads unchanged.
  const old = Array.from({ length: 21 }, (_, i) => (i % 2 ? 30 : 70));
  assert.ok(RS.validCurve(old));
  assert.deepStrictEqual(RS.rates('custom', old), old);
});
