import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import zlib from 'node:zlib';
import { spawnSync } from 'node:child_process';
test('interactive core preserves a live machine, rejects invalid steps, and captures its VI', () => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'photon-coreplay-'));
  try {
    const requests = [{ op: 'step', fields: 120 }, { op: 'snapshot' }, { op: 'step', fields: 3, input: 'A+X=80' },
      { op: 'step', fields: -1 }, { op: 'inspect' }, { op: 'quit' }];
    const run = spawnSync(process.execPath, ['tools/coreplay.mjs', 'testroms/RSPCP2VRCP.N64', directory], {
      input: requests.map(r => JSON.stringify(r)).join('\n') + '\n', encoding: 'utf8', timeout: 30000
    });
    assert.equal(run.status, 0, run.stderr || run.error?.message);
    const result = run.stdout.trim().split('\n').map(JSON.parse);
    assert.equal(result[0].ready, true);
    assert.equal(result[1].fields, 120);
    assert.equal(result[2].ok, true);
    assert.equal(result[3].fields, 123);
    assert.equal(result[4].ok, false);
    assert.equal(result[5].fields, 123);
    const png = fs.readFileSync(result[2].file);
    assert.deepEqual([...png.subarray(0, 8)], [137, 80, 78, 71, 13, 10, 26, 10]);
    const width = png.readUInt32BE(16), height = png.readUInt32BE(20);
    const parts = [];
    for (let p = 8; p < png.length;) {
      const length = png.readUInt32BE(p), type = png.toString('ascii', p + 4, p + 8);
      if (type === 'IDAT') parts.push(png.subarray(p + 8, p + 8 + length));
      p += length + 12;
    }
    assert.equal(zlib.inflateSync(Buffer.concat(parts)).length, (width * 4 + 1) * height);
  } finally { fs.rmSync(directory, { recursive: true, force: true }); }
});
