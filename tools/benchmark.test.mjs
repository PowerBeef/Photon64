import test from 'node:test';
import assert from 'node:assert/strict';
import { parseInputs, summarize } from './benchmark.mjs';
test('replay input supports all controller buttons and signed analog axes', () => {
  assert.deepEqual(parseInputs('0:START:4,4:A+CU+X=-80+Y=127:20'), [
    { frame: 0, duration: 4, buttons: 4096 }, { frame: 4, duration: 20, buttons: 32776, x: -80, y: 127 }
  ]);
});
test('mistyped or unbounded replay inputs fail instead of silently changing gameplay', () => {
  for (const input of ['1:STAR:3', '-1:A', '2:A:0', '3:X=128', '3:Y=-129', '4:A:2:3', ':A']) assert.throws(() => parseInputs(input));
});
test('timing summaries retain tail latency and reject empty or invalid evidence', () => {
  assert.deepEqual(summarize([1, 3, 2, 100]), { count: 4, meanMs: 26.5, p50Ms: 2, p95Ms: 100, p99Ms: 100, maxMs: 100 });
  for (const values of [[], [NaN], [Infinity], [-1]]) assert.throws(() => summarize(values));
});
