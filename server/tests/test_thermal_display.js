"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");
const {robustRange, smoothRange, median3} = require("../static/thermal-display.js");

test("isolated extreme pixels do not control the thermal display range", () => {
  const values = Array.from({length: 100}, (_, index) => 295 + (index % 11));
  values[0] = -922;
  values[99] = 3000;
  assert.deepEqual(robustRange(values), {minimum: 295, maximum: 305});
});

test("display range expands quickly and contracts slowly", () => {
  const expanded = smoothRange({minimum: 280, maximum: 320}, {minimum: 200, maximum: 420});
  assert.equal(expanded.minimum, 228);
  assert.equal(expanded.maximum, 385);
  const contracted = smoothRange(expanded, {minimum: 290, maximum: 310});
  assert.equal(contracted.minimum, 240.4);
  assert.equal(contracted.maximum, 370);
});

test("three-point median removes an isolated trend spike but keeps gaps", () => {
  const values = median3([300, 301, -922, 302, Number.NaN, 303]);
  assert.deepEqual(values.slice(0, 4), [301, 300, 301, 302]);
  assert.ok(Number.isNaN(values[4]));
});
