(function (root, factory) {
  const api = factory();
  if (typeof module === "object" && module.exports) module.exports = api;
  if (root) root.ThermalDisplay = api;
}(typeof globalThis !== "undefined" ? globalThis : this, function () {
  "use strict";

  function finiteValues(values) {
    return (values || []).map(Number).filter(Number.isFinite);
  }

  function percentile(values, fraction) {
    const ordered = finiteValues(values).sort((left, right) => left - right);
    if (!ordered.length) return null;
    const bounded = Math.min(1, Math.max(0, Number(fraction)));
    return ordered[Math.floor((ordered.length - 1) * bounded)];
  }

  function robustRange(values, lowFraction = 0.02, highFraction = 0.98) {
    const minimum = percentile(values, lowFraction);
    const maximum = percentile(values, highFraction);
    if (!Number.isFinite(minimum) || !Number.isFinite(maximum)) return null;
    if (maximum > minimum) return {minimum, maximum};
    return {minimum: minimum - 0.5, maximum: maximum + 0.5};
  }

  function smoothRange(previous, current, expandAlpha = 0.65, contractAlpha = 0.2) {
    if (!current || !Number.isFinite(current.minimum) || !Number.isFinite(current.maximum)) {
      return previous || null;
    }
    if (!previous || !Number.isFinite(previous.minimum) || !Number.isFinite(previous.maximum)) {
      return {...current};
    }
    const minimumAlpha = current.minimum < previous.minimum ? expandAlpha : contractAlpha;
    const maximumAlpha = current.maximum > previous.maximum ? expandAlpha : contractAlpha;
    let minimum = previous.minimum + (current.minimum - previous.minimum) * minimumAlpha;
    let maximum = previous.maximum + (current.maximum - previous.maximum) * maximumAlpha;
    if (maximum - minimum < 1) {
      const center = (minimum + maximum) / 2;
      minimum = center - 0.5;
      maximum = center + 0.5;
    }
    return {minimum, maximum};
  }

  function median3(values) {
    return (values || []).map((value, index, source) => {
      if (!Number.isFinite(value)) return Number.NaN;
      const window = source.slice(Math.max(0, index - 1), index + 2)
        .filter(Number.isFinite)
        .sort((left, right) => left - right);
      return window[Math.floor(window.length / 2)];
    });
  }

  return {percentile, robustRange, smoothRange, median3};
}));
