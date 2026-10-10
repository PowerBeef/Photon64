import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { verdict } from './results.mjs';
const frames = [250, 900, 2000, 2600, 3780, 4190];
const clean = () => ({ checks: frames.map(frame => ({ frame, viDiff: 0, viMax: 0, sync: true, prims: 1, fb: { color: { dc: 0, dh: 0 }, depth: { dc: 0, dh: 0 } } })), expectedChecks: frames });
const fixtures = [ ['zero checkpoints', r => { r.checks = []; }], ['missing checkpoint', r => r.checks.pop()],
  ['duplicate checkpoint', r => { r.checks[1].frame = frames[0]; }], ['runner error', r => { r.error = 'no webgpu'; }],
  ['VI difference', r => { r.checks[0].viDiff = 1; }], ['state mismatch', r => { r.checks[0].sync = false; }],
  ['missing comparisons', r => { delete r.checks[0].sync; }],
  ['missing primitive coverage', r => { delete r.checks[0].prims; }],
  ['missing rendered framebuffer', r => { delete r.checks[0].fb; }] ];
for (const kind of ['color', 'depth']) for (const field of ['dc', 'dh']) fixtures.push([`${kind} ${field} mismatch`, r => { r.checks[0].fb[kind][field] = 1; }]);
for (const [name, mutate] of fixtures) test(name + ' fails JS and Python gates', () => {
  const r = clean(); mutate(r); assert.equal(verdict(r, frames).outcome, 'FAIL');
  // A forged PASS still cannot bypass the parser's independent field checks.
  r.verdict = { outcome: 'PASS' };
  const result = spawnSync(process.env.PY || 'python3', ['-c', 'import sys; from tools.gpures import evaluate; evaluate(sys.stdin.read())'], { input: JSON.stringify(r), encoding: 'utf8' });
  assert.notEqual(result.status, 0, result.stdout + result.stderr);
});
test('clean JSON at byte zero and legacy prefixed JSON both pass', () => {
  const r = clean(); r.verdict = verdict(r, frames); assert.equal(r.verdict.outcome, 'PASS');
  for (const prefix of ['', '[page] diagnostic\n']) {
    const p = spawnSync(process.env.PY || 'python3', ['-c', 'import sys; from tools.gpures import evaluate; evaluate(sys.stdin.read())'], { input: prefix + JSON.stringify(r), encoding: 'utf8' }); assert.equal(p.status, 0, p.stderr);
  }
});
test('malformed JSON and diagnostic SKIP cannot satisfy a required lane', () => {
  assert.equal(verdict(clean(), frames, true).outcome, 'SKIP');
  for (const input of ['{oops', JSON.stringify({ ...clean(), verdict: { outcome: 'SKIP' } })]) {
    const p = spawnSync(process.env.PY || 'python3', ['-c', 'import sys; from tools.gpures import evaluate; evaluate(sys.stdin.read())'], { input, encoding: 'utf8' }); assert.notEqual(p.status, 0);
  }
});
test('composed suite rejects process failure and empty / mismatched results', () => {
  const temp = fs.mkdtempSync(path.join(os.tmpdir(), 'photon-gate-'));
  try {
    fs.mkdirSync(path.join(temp, 'tools/inputs'), { recursive: true }); fs.mkdirSync(path.join(temp, 'out'));
    for (const file of ['games.sh', 'gpures.py']) fs.copyFileSync(new URL(file, import.meta.url), path.join(temp, 'tools', file));
    fs.writeFileSync(path.join(temp, 'sm64.z64'), 'fixture'); fs.writeFileSync(path.join(temp, 'tools/inputs/sm64.txt'), '');
    fs.writeFileSync(path.join(temp, 'out/oracle_nn'), '#!/bin/sh\nexit 0\n', { mode: 0o755 });
    const run = (r, status) => {
      fs.writeFileSync(path.join(temp, 'tools/gputest.mjs'), `console.log(${JSON.stringify(JSON.stringify(r))}); process.exit(${status});`);
      return spawnSync('bash', ['tools/games.sh', 'sm64'], { cwd: temp, env: { ...process.env, GPU_RUNNER: 'browser' }, encoding: 'utf8' });
    };
    const r = clean(); r.verdict = verdict(r, frames); assert.equal(run(r, 0).status, 0);
    assert.notEqual(run(r, 86).status, 0); assert.notEqual(run({ checks: [], verdict: { outcome: 'PASS' } }, 0).status, 0);
    r.checks[0].fb.color.dc = 1; assert.notEqual(run(r, 0).status, 0);
    fs.unlinkSync(path.join(temp, 'sm64.z64')); assert.notEqual(run(r, 0).status, 0);
  } finally { fs.rmSync(temp, { recursive: true, force: true }); }
});
