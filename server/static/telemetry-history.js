(function (root, factory) {
  const api = factory();
  if (typeof module === "object" && module.exports) module.exports = api;
  if (root) root.TelemetryHistory = api;
}(typeof globalThis !== "undefined" ? globalThis : this, function () {
  "use strict";

  const HISTORY_WINDOW_MS = 60_000;
  const HISTORY_MAX_POINTS = 480;

  function receivedAtMs(row) {
    const raw = row?.received_at;
    if (typeof raw === "number") return Number.isFinite(raw) ? raw : null;
    const value = Date.parse(raw || "");
    return Number.isFinite(value) ? value : null;
  }

  function mergeTelemetryHistory(...sources) {
    const unique = new Map();
    let order = 0;
    for (const rows of sources) {
      for (const row of rows || []) {
        const time = receivedAtMs(row);
        if (time == null) continue;
        const key = `${row.received_at}\u001f${row.sequence ?? ""}`;
        unique.set(key, {row, time, order: order++});
      }
    }
    const sorted = [...unique.values()]
      .sort((left, right) => left.time - right.time || left.order - right.order);
    if (!sorted.length) return [];
    const end = sorted.at(-1).time;
    return sorted
      .filter(item => item.time >= end - HISTORY_WINDOW_MS)
      .slice(-HISTORY_MAX_POINTS)
      .map(item => item.row);
  }

  function historyTimeDomain(rows, windowMs = HISTORY_WINDOW_MS) {
    const times = (rows || []).map(receivedAtMs).filter(Number.isFinite);
    if (!times.length) return null;
    const end = Math.max(...times);
    return {start: end - windowMs, end};
  }

  function timeToRatio(timestamp, end, windowMs = HISTORY_WINDOW_MS) {
    if (!Number.isFinite(timestamp) || !Number.isFinite(end) || windowMs <= 0) return null;
    return (timestamp - (end - windowMs)) / windowMs;
  }

  return {
    HISTORY_WINDOW_MS,
    HISTORY_MAX_POINTS,
    receivedAtMs,
    mergeTelemetryHistory,
    historyTimeDomain,
    timeToRatio,
  };
}));
