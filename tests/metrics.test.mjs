import test from "node:test";
import assert from "node:assert/strict";
import { summarize, csv } from "../ui/metrics.js";
test("empty trials never invent metrics", () => {
  const m = summarize([]);
  assert.equal(m.accuracy, null);
  assert.equal(m.nominalRate, null);
});
test("misses affect accuracy and time but not nominal successful rate", () => {
  const m = summarize([
    { distance: 100, width: 100, movementTimeMs: 1000, hit: true },
    { distance: 100, width: 100, movementTimeMs: 3000, hit: false },
  ]);
  assert.equal(m.accuracy, 0.5);
  assert.equal(m.meanTimeMs, 2000);
  assert.equal(m.nominalRate, 1);
  assert.equal(m.errors, 1);
});
test("aborted trials are retained raw but excluded from metrics", () => {
  const m = summarize([
    {
      distance: 100,
      width: 100,
      movementTimeMs: 1000,
      hit: false,
      aborted: true,
    },
  ]);
  assert.equal(m.attempts, 0);
});
test("aggregate uses summed ID over summed time, not average trial rate", () => {
  const m = summarize([
    { distance: 100, width: 100, movementTimeMs: 1000, hit: true },
    { distance: 100, width: 100, movementTimeMs: 3000, hit: true },
  ]);
  assert.equal(m.nominalRate, 0.5);
});
test("CSV quotes nested profile and multiline text", () => {
  const s = csv([{ note: 'a,"b"\nc', profile: { alpha: 0.35 } }]);
  assert.ok(s.includes('"a,""b""\nc"'));
  assert.ok(s.includes('{""alpha"":0.35}'));
});
