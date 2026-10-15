"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");
const {
  HISTORY_WINDOW_MS,
  mergeTelemetryHistory,
  historyTimeDomain,
  timeToRatio,
} = require("../static/telemetry-history.js");

const BASE = Date.parse("2026-07-30T12:00:00.000Z");
const row = (seconds, sequence, extra = {}) => ({
  received_at: new Date(BASE + seconds * 1000).toISOString(),
  sequence,
  ...extra,
});

test("history is sorted and clipped to the latest 60 seconds", () => {
  const result = mergeTelemetryHistory([row(90, 90), row(0, 0), row(60, 60), row(30, 30)]);
  assert.deepEqual(result.map(item => item.sequence), [30, 60, 90]);
});

test("a delayed database response cannot erase the live tail", () => {
  const live = [row(101, 101), row(102, 102)];
  const database = [row(98, 98), row(99, 99), row(100, 100)];
  const result = mergeTelemetryHistory(database, live);
  assert.equal(result.at(-1).sequence, 102);
});

test("the live form replaces a persisted duplicate", () => {
  const persisted = row(10, 7, {source: "database"});
  const live = row(10, 7, {source: "live", health: {ra8_online: true}});
  const result = mergeTelemetryHistory([persisted], [live]);
  assert.equal(result.length, 1);
  assert.equal(result[0].source, "live");
});

test("sequence reset does not rewind server time", () => {
  const result = mergeTelemetryHistory([
    row(10, 0xffffffff),
    row(11, 1),
  ]);
  assert.deepEqual(result.map(item => item.sequence), [0xffffffff, 1]);
});

test("repeated history backfill is idempotent", () => {
  const database = Array.from({length: 61}, (_, index) => row(index, index));
  let result = mergeTelemetryHistory(database);
  for (let pass = 0; pass < 5; pass++) {
    result = mergeTelemetryHistory(database, result, [row(61 + pass, 100 + pass)]);
  }
  assert.equal(result.at(-1).sequence, 104);
  assert.equal(new Set(result.map(item => `${item.received_at}/${item.sequence}`)).size, result.length);
  assert.ok(Date.parse(result.at(-1).received_at) - Date.parse(result[0].received_at) <= HISTORY_WINDOW_MS);
});

test("a short history keeps its position in a fixed time window", () => {
  const rows = [row(50, 1), row(60, 2)];
  const domain = historyTimeDomain(rows);
  assert.equal(timeToRatio(Date.parse(rows[0].received_at), domain.end), 5 / 6);
  assert.equal(timeToRatio(Date.parse(rows[1].received_at), domain.end), 1);
});

test("chart domains also accept pre-parsed numeric timestamps", () => {
  const domain = historyTimeDomain([
    {received_at: BASE + 50_000},
    {received_at: BASE + 60_000},
  ]);
  assert.deepEqual(domain, {
    start: BASE,
    end: BASE + 60_000,
  });
});
