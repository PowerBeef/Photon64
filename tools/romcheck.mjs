// Deterministic execution evidence; a completed boot is not a compatibility verdict.
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { spawnSync } from 'node:child_process';
const args = process.argv.slice(2);
function flag(key, fallback) { const i = args.indexOf(key); if (i < 0) return fallback; const value = args[i + 1]; args.splice(i, 2); return value; }
const frames = +flag('--frames', 300), output = flag('--output', 'out/rom-results.json');
if (!Number.isInteger(frames) || frames <= 0) throw new Error('positive integer frames required');
const directory = args[0] || 'roms';
const files = dir => fs.readdirSync(dir, { withFileTypes: true }).flatMap(e => e.isDirectory() ? files(path.join(dir, e.name)) : /\.(z64|n64|v64|rom|bin)$/i.test(e.name) ? [path.join(dir, e.name)] : []).sort();
const roms = files(directory); if (!roms.length) throw new Error('no ROM fixtures found');
const sha = b => crypto.createHash('sha256').update(b).digest('hex');
const results = [];
for (const file of roms) {
  const data = fs.readFileSync(file), runs = [];
  for (let repeat = 0; repeat < 2; repeat++) {
    const run = spawnSync(process.execPath, ['tools/run_wasm.mjs', file, String(frames)], { encoding: 'utf8', timeout: 120000 });
    const summary = /prims=(\d+) flushes=(\d+) rsp=(\d+).*pc=([0-9a-f]+) mem=([\d.]+)MB/.exec(run.stdout || '');
    const hash = /hash rdram=([0-9a-f]+) audio=([0-9a-f]+)/.exec(run.stdout || '');
    const completed = new RegExp(`frames=${frames} total=`).test(run.stdout || '');
    runs.push({ exit: run.status, error: run.error?.message, completed, prims: summary && +summary[1], flushes: summary && +summary[2], rspInstructions: summary && +summary[3], pc: summary?.[4], memoryMiB: summary && +summary[5], rdramHash: hash?.[1], audioHash: hash?.[2], log: run.stdout, stderr: run.stderr });
  }
  const [a, b] = runs;
  const ok = runs.every(r => r.exit === 0 && r.completed && r.rdramHash && r.audioHash) && a.rdramHash === b.rdramHash && a.audioHash === b.audioHash && a.pc === b.pc && a.prims === b.prims;
  results.push({ file: path.basename(file), size: data.length, sha256: sha(data), outcome: ok ? 'PASS' : 'FAIL', evidence: 'deterministic execution only', runs });
  console.log(ok ? 'PASS' : 'FAIL', path.basename(file), `${frames} frames twice; prims=${a.prims}; rdram=${a.rdramHash}`);
}
const revision = spawnSync('git', ['rev-parse', 'HEAD'], { encoding: 'utf8' }).stdout.trim();
const dirty = !!spawnSync('git', ['status', '--porcelain'], { encoding: 'utf8' }).stdout.trim();
const report = { schemaVersion: 1, source: { revision, dirty }, node: process.version, wasmSha256: sha(fs.readFileSync('out/n64.wasm')), frames, results, limitation: 'Software-only headless execution, not GPU parity, hardware accuracy, audio-output quality or gameplay compatibility.' };
fs.mkdirSync(path.dirname(output), { recursive: true }); fs.writeFileSync(output, JSON.stringify(report, null, 2) + '\n');
process.exitCode = results.some(r => r.outcome !== 'PASS') ? 1 : 0;
