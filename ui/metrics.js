export function summarize(rows) {
  const usable = rows.filter(
    (r) => !r.aborted && Number.isFinite(r.movementTimeMs) && r.movementTimeMs > 0,
  );
  const hits = usable.filter((r) => r.hit);
  const sumTime = hits.reduce((s, r) => s + r.movementTimeMs / 1000, 0);
  return {
    attempts: usable.length,
    hits: hits.length,
    errors: usable.length - hits.length,
    accuracy: usable.length ? hits.length / usable.length : null,
    meanTimeMs: usable.length
      ? usable.reduce((s, r) => s + r.movementTimeMs, 0) / usable.length
      : null,
    nominalRate: sumTime
      ? hits.reduce((s, r) => s + Math.log2(r.distance / r.width + 1), 0) / sumTime
      : null,
  };
}
export function csv(rows) {
  if (!rows.length) return '';
  const keys = [...new Set(rows.flatMap(Object.keys))];
  const cell = (value) =>
    '"' +
    String(
      value == null ? '' : typeof value === 'object' ? JSON.stringify(value) : value,
    ).replaceAll('"', '""') +
    '"';
  return [
    keys.map(cell).join(','),
    ...rows.map((row) => keys.map((key) => cell(row[key])).join(',')),
  ].join('\n');
}
