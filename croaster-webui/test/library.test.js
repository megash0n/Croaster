// Host tests for web/library.js (device library format and recipes).
// Run: node --test croaster-webui/test/*.test.js
'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const L = require('../web/library.js');

const good = () => ({ version: 1, name: 'City roast', notes: 'Kenya AA', unit: 'F',
  steps: [{ duration: 60, setpoint: 300, interpolation: 'linear' }, { duration: 120, setpoint: 400, fanValue: 60 }] });
const recipe = () => ({ steps: [{ trigger: { type: 'bt', value: 389 },
  actions: [{ type: 'fan', value: 50 }, { type: 'power', value: 60 }] }] });

test('validate follows format v1', () => {
  assert.equal(L.validate(good()), '');
  assert.equal(L.validate(Object.assign(good(), { version: 2 })), 'newerVersion');
  assert.equal(L.validate(Object.assign(good(), { version: '1' })), 'badVersion');
  assert.equal(L.validate(Object.assign(good(), { name: ' ' })), 'badName');
  assert.equal(L.validate(Object.assign(good(), { name: 'x'.repeat(41) })), 'badName');
  assert.equal(L.validate(Object.assign(good(), { notes: 'n'.repeat(1025) })), 'badNotes');
  assert.equal(L.validate(Object.assign(good(), { unit: 'X' })), 'badUnit');
  assert.equal(L.validate(Object.assign(good(), { steps: [] })), 'badSteps');
  assert.equal(L.validate(Object.assign(good(), { steps: [{ duration: 60, setpoint: 300, fanValue: 101 }] })), 'badSteps');
  assert.equal(L.validate(Object.assign(good(), { future: { a: 1 } })), ''); // unknown keys are kept
  assert.equal(L.validate(Object.assign(good(), { extra: 'x'.repeat(8192) })), 'tooLarge');
});

test('a profile needs a schedule, a recipe or both', () => {
  const p = good();
  delete p.steps;
  assert.equal(L.validate(p), 'badSteps');
  p.recipe = recipe();
  assert.equal(L.validate(p), '');
  assert.equal(L.validate(Object.assign(good(), { recipe: recipe() })), '');
  p.recipe.steps[0].trigger.value = 700; // F: 371 C
  assert.equal(L.validate(p), 'badRecipe');
  assert.equal(L.recipeReason(p.recipe, 'F'), 'temperature trigger must be 0..350 C');
});

test('recipeReason matches the board limits', () => {
  const r = (trigger, actions, extra) => ({ steps: [Object.assign({ trigger, actions: actions || [{ type: 'end' }] }, extra)] });
  assert.equal(L.recipeReason(r({ type: 'time', value: 3600 }), 'C'), '');
  assert.equal(L.recipeReason(r({ type: 'ror', op: 'near', value: 18, tol: 9 }), 'F'), '');
  assert.equal(L.recipeReason(r({ type: 'event', value: 'FC_START' }), 'C'), '');
  const bad = [
    [r({ type: 'time', value: 3601 }), 'time trigger must be 0..3600 s'],
    [r({ type: 'bt', value: 351 }), 'temperature trigger must be 0..350 C'],
    [r({ type: 'ror', value: 101 }), 'RoR trigger must be -50..100 C/min'],
    [r({ type: 'bt', op: 'eq', value: 200 }), 'trigger op must be gte, lte or near'],
    [r({ type: 'bt', op: 'near', value: 200, tol: 21 }), 'tolerance out of range'],
    [r({ type: 'event', value: 'CRACK' }), 'unknown event'],
    [r({ type: 'drum', value: 1 }), 'trigger type must be time, bt, et, ror or event'],
    [r({ type: 'time', value: 1 }, []), 'a step has 1 to 4 actions'],
    [r({ type: 'time', value: 1 }, Array(5).fill({ type: 'end' })), 'a step has 1 to 4 actions'],
    [r({ type: 'time', value: 1 }, [{ type: 'power', value: 101 }]), 'power and fan must be 0..100 %'],
    [r({ type: 'time', value: 1 }, [{ type: 'setpoint', value: 400 }]), 'setpoint must be 0..350 C'],
    [r({ type: 'time', value: 1 }, [{ type: 'setpoint', value: 200, ramp: 3601 }]), 'ramp must be 0..3600 s'],
    [r({ type: 'time', value: 1 }, [{ type: 'drum', value: 50 }]), 'action type must be power, fan, setpoint, hold, cool, end or note'],
    [r({ type: 'time', value: 1 }, [{ type: 'note', text: 'x'.repeat(81) }]), 'note text must be up to 80 characters'],
    [r({ type: 'time', value: 1 }, null, { anyOrder: 1 }), 'anyOrder must be true or false'],
    [{ mode: 'fast', steps: [] }, 'mode must be auto or guided'],
    [{ steps: [] }, 'a recipe has 1 to 32 steps'],
  ];
  for (const [rec, why] of bad) assert.equal(L.recipeReason(rec, 'C'), why, JSON.stringify(rec));
});

test('editor rows round trip, with unit conversion', () => {
  const rows = [L.newRecipeRow()];
  assert.deepEqual(rows[0].actions.map((a) => a.type), ['power', 'fan']); // open loop by default
  rows[0].value = '0:00';
  rows[0].actions[0].value = 80;
  rows[0].actions[1].value = 40;
  rows.push({ type: 'bt', op: 'gte', value: 389, tol: '', event: 'CHARGE', anyOrder: false, label: 'Push',
    actions: [{ type: 'setpoint', value: 410, ramp: 90 }] });
  const rec = L.recipeFromEditor(rows, 'guided');
  assert.equal(rec.mode, 'guided');
  assert.equal(L.recipeReason(rec, 'F'), '');
  assert.deepEqual(rec.steps[1], { trigger: { type: 'bt', op: 'gte', value: 389 }, label: 'Push',
    actions: [{ type: 'setpoint', value: 410, ramp: 90 }] });
  assert.equal(rec.steps[0].trigger.value, 0);
  const back = L.recipeToEditor(rec, 'F', 'C');
  assert.equal(back[0].value, '0:00');
  assert.equal(back[1].value, 198.3);
  assert.equal(back[1].actions[0].value, 210);
  assert.equal(L.recipeToEditor({ steps: [{ trigger: { type: 'ror', value: 18, op: 'near', tol: 9 }, actions: [] }] }, 'F', 'C')[0].tol, 5);
  // a blank value is named by validation
  rows[0].actions[0].value = '';
  assert.equal(L.recipeReason(L.recipeFromEditor(rows, 'auto'), 'F'), 'power and fan must be 0..100 %');
  // fromEditor without a schedule
  const out = L.fromEditor(null, 'Recipe only', '', 'F', { future: 1 }, rec);
  assert.equal(out.error, undefined);
  assert.equal(out.profile.steps, undefined);
  assert.equal(out.profile.future, 1);
  const badOut = L.fromEditor(null, 'x', '', 'F', null, L.recipeFromEditor(rows, 'auto'));
  assert.equal(badOut.error, 'badRecipe');
  assert.equal(badOut.reason, 'power and fan must be 0..100 %');
});

test('describeStep reads like the Live list', () => {
  const rec = recipe();
  assert.equal(L.describeStep(rec, 0, 'F', 'F'), 'Step 1 (BT \u2265 389 \u00b0F): fan 50 %, power 60 %');
  const t = { steps: [{ trigger: { type: 'time', value: 462 }, label: 'Late', actions: [{ type: 'setpoint', value: 220, ramp: 90 }, { type: 'cool' }] }] };
  assert.equal(L.describeStep(t, 0, 'C', 'C'), 'Step 1 Late (time \u2265 7:42): setpoint 220 \u00b0C over 1:30, cool');
  assert.equal(L.describeStep({ steps: [{ trigger: { type: 'event', value: 'FC_START' }, actions: [{ type: 'note', text: 'hi' }] }] }, 0, 'C', 'C'),
    'Step 1 (First crack): "hi"');
});

test('behindSchedule warns after the heater stays saturated', () => {
  let st = null;
  const run = (ms, bt, heater, follow) => {
    const r = L.behindSchedule(st, { nowMs: ms, btC: bt, setpointC: 200, heater, follow }, { lagC: 8, holdMs: 20000 });
    st = r.state;
    return r.warn;
  };
  assert.equal(run(0, 190, 100, true), false);
  assert.equal(run(19000, 191, 100, true), false);
  assert.equal(run(20000, 191, 100, true), true);
  assert.equal(run(21000, 191, 90, true), false); // heater not saturated: resets
  assert.equal(run(22000, 191, 100, true), false);
  assert.equal(run(50000, 195, 100, true), false); // lag 5 C
  assert.equal(run(80000, 150, 100, false), false); // power step: not following a setpoint
});

test('parseClock reads m:ss or seconds', () => {
  assert.equal(L.parseClock('7:42'), 462);
  assert.equal(L.parseClock('462'), 462);
  assert.equal(L.parseClock(' 0:05 '), 5);
  assert.ok(Number.isNaN(L.parseClock('7:61')));
  assert.ok(Number.isNaN(L.parseClock('')));
  assert.ok(Number.isNaN(L.parseClock('abc')));
});

test('busyReason and helpers', () => {
  assert.equal(L.busyReason({ Mode: 'Profile' }), 'a profile is running');
  assert.equal(L.busyReason({ Mode: 'Manual', BurnerVal: 10 }), 'the heater is on');
  assert.equal(L.busyReason({ Mode: 'Manual', BurnerVal: 0 }), '');
  assert.equal(L.fileName('City roast #2'), 'city-roast-2.json');
  assert.equal(L.findByName([{ name: 'City' }], ' city ').name, 'City');
  assert.equal(L.parseFile('{').error, 'badFile');
  assert.equal(L.message('busy', 'heater on'), 'refused while the board reports heater on');
});

test('behindSchedule ignores the charge preheat', () => {
  // The board reports follow false while it preheats to chargeTemp, so a
  // saturated heater during preheat neither warns nor starts the hold timer.
  let st = null;
  for (let ms = 0; ms <= 60000; ms += 1000) {
    const r = L.behindSchedule(st, { nowMs: ms, btC: 120, setpointC: 200, heater: 100, follow: false }, { lagC: 8, holdMs: 20000 });
    st = r.state;
    assert.equal(r.warn, false);
  }
  const r = L.behindSchedule(st, { nowMs: 61000, btC: 120, setpointC: 200, heater: 100, follow: true }, { lagC: 8, holdMs: 20000 });
  assert.equal(r.warn, false); // first setpoint step: the hold starts now
});
