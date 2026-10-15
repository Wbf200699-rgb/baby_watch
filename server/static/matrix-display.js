(function (root, factory) {
  const api = factory();
  if (typeof module === "object" && module.exports) module.exports = api;
  else root.MatrixDisplay = api;
}(typeof globalThis !== "undefined" ? globalThis : this, function () {
  function finiteValues(values) {
    return (values || []).map(Number).filter(Number.isFinite);
  }

  function percentile(values, ratio) {
    const sorted = finiteValues(values).sort((left, right) => left - right);
    if (!sorted.length) return 0;
    const position = Math.max(0, Math.min(1, Number(ratio))) * (sorted.length - 1);
    const lower = Math.floor(position);
    const upper = Math.ceil(position);
    if (lower === upper) return sorted[lower];
    const weight = position - lower;
    return sorted[lower] * (1 - weight) + sorted[upper] * weight;
  }

  function buildPressureBaseline(frames) {
    const valid = (frames || []).filter(frame => Array.isArray(frame) && frame.length);
    if (!valid.length) return [];
    const length = valid[0].length;
    if (valid.some(frame => frame.length !== length)) return [];
    return Array.from({length}, (_, index) => percentile(valid.map(frame => frame[index]), 0.5));
  }

  function pressureDisplay(values, baseline, threshold = 0) {
    const raw = (values || []).map(value => Number(value) || 0);
    const hasBaseline = Array.isArray(baseline) && baseline.length === raw.length;
    const floor = Math.max(0, Number(threshold) || 0);
    const corrected = raw.map((value, index) => Math.max(
      0,
      value - (hasBaseline ? Number(baseline[index]) || 0 : 0) - floor,
    ));
    const maximum = Math.max(1, ...corrected);
    return {raw, corrected, minimum: 0, maximum, hasBaseline, threshold: floor};
  }

  function pressurePeakHold(values, previous, elapsedMs, holdMs) {
    const current = (values || []).map(value => Math.max(0, Number(value) || 0));
    const prior = Array.isArray(previous) && previous.length === current.length ? previous : null;
    const duration = Math.max(0, Number(holdMs) || 0);
    if (!prior || duration === 0) return current;
    const decay = Math.max(0, 1 - Math.max(0, Number(elapsedMs) || 0) / duration);
    return current.map((value, index) => Math.max(value, (Number(prior[index]) || 0) * decay));
  }

  function thermalCandidateMask(values, rows, cols, allowed, options = {}) {
    const raw = (values || []).map(Number);
    const count = Number(rows) * Number(cols);
    if (!Number.isInteger(count) || count <= 0 || raw.length !== count) {
      return {mask: [], count: 0, threshold: 0};
    }
    const p50 = percentile(raw, 0.50);
    const p70 = percentile(raw, options.percentile ?? 0.70);
    const p98 = percentile(raw, 0.98);
    const threshold = Math.max(
      p70,
      p50 + Math.max(Number(options.minimumDelta ?? 5), (p98 - p50) * 0.30),
    );
    const permitted = Array.isArray(allowed) && allowed.length === count
      ? allowed : Array(count).fill(true);
    const initial = raw.map((value, index) => permitted[index] && value >= threshold);
    const visited = Array(count).fill(false);
    const mask = Array(count).fill(false);
    const minimumComponent = Math.max(1, Number(options.minimumComponent ?? 2));
    let kept = 0;

    for (let start = 0; start < count; start++) {
      if (!initial[start] || visited[start]) continue;
      const component = [];
      const pending = [start];
      visited[start] = true;
      while (pending.length) {
        const index = pending.pop();
        component.push(index);
        const row = Math.floor(index / cols);
        const col = index % cols;
        const neighbors = [];
        if (row > 0) neighbors.push(index - cols);
        if (row + 1 < rows) neighbors.push(index + cols);
        if (col > 0) neighbors.push(index - 1);
        if (col + 1 < cols) neighbors.push(index + 1);
        neighbors.forEach(next => {
          if (initial[next] && !visited[next]) {
            visited[next] = true;
            pending.push(next);
          }
        });
      }
      if (component.length >= minimumComponent) component.forEach(index => {
        mask[index] = true;
        kept++;
      });
    }
    return {mask, count: kept, threshold, p50, p98};
  }

  return {percentile, buildPressureBaseline, pressureDisplay, pressurePeakHold, thermalCandidateMask};
}));
