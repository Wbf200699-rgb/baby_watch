"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");
const {
  buildPressureBaseline,
  pressureDisplay,
  pressurePeakHold,
  thermalCandidateMask,
} = require("../static/matrix-display.js");

test("pressure baseline uses a per-cell median and preserves raw values", () => {
  const baseline = buildPressureBaseline([[10, 20], [12, 18], [11, 19]]);
  assert.deepEqual(baseline, [11, 19]);
  const display = pressureDisplay([15, 20], baseline, 1);
  assert.deepEqual(display.raw, [15, 20]);
  assert.deepEqual(display.corrected, [3, 0]);
  assert.equal(display.maximum, 3);
  assert.equal(display.hasBaseline, true);
});

test("pressure display keeps a single light contact visible", () => {
  const values = Array(256).fill(0);
  values[93] = 3;
  const display = pressureDisplay(values, null, 1);
  assert.equal(display.corrected[93], 2);
  assert.equal(display.maximum, 2);
});

test("pressure peak hold keeps a brief light contact visible without changing raw data", () => {
  const held = pressurePeakHold([0, 2], [10, 0], 500, 1000);
  assert.deepEqual(held, [5, 2]);
  assert.deepEqual(pressurePeakHold([0, 2], held, 1000, 1000), [0, 2]);
  assert.deepEqual(pressurePeakHold([0, 2], held, 10, 0), [0, 2]);
});

test("thermal candidate requires allowed, adjacent hot cells", () => {
  const values = Array(16).fill(200);
  values[5] = 300;
  values[6] = 305;
  values[15] = 330;
  const allowed = Array(16).fill(false);
  allowed[5] = true;
  allowed[6] = true;
  allowed[15] = true;
  const result = thermalCandidateMask(values, 4, 4, allowed, {minimumComponent: 2});
  assert.equal(result.count, 2);
  assert.equal(result.mask[5], true);
  assert.equal(result.mask[6], true);
  assert.equal(result.mask[15], false);
});

test("thermal candidate rejects heat outside the video baby region", () => {
  const values = Array(16).fill(200);
  values[5] = 300;
  values[6] = 305;
  const result = thermalCandidateMask(values, 4, 4, Array(16).fill(false));
  assert.equal(result.count, 0);
});
