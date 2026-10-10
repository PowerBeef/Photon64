import { test } from 'node:test';
import assert from 'node:assert/strict';
import gate from './release-gate.cjs';
const jobs = () => gate.requiredJobs.map(name => ({ name, conclusion: 'success' }));
test('release requires every exact named job including GPU and branded Safari', () => {
  gate.verifyJobs(jobs());
  for (const name of gate.requiredJobs) {
    assert.throws(() => gate.verifyJobs(jobs().filter(j => j.name !== name)), /Required job/);
    assert.throws(() => gate.verifyJobs(jobs().map(j => j.name === name ? { ...j, conclusion: 'skipped' } : j)), /Required job/);
  }
  assert.throws(() => gate.verifyJobs([...jobs(), jobs()[0]]), /exactly once/);
});
