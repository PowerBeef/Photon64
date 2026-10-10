// Sequential, fresh-process WASM replays. Fields are not game-rendered frames.
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import crypto from 'node:crypto';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { parseArgs } from 'node:util';

const self = fileURLToPath(import.meta.url);
const sha = data => crypto.createHash('sha256').update(data).digest('hex');
const buttons = { A: 0x8000, B: 0x4000, Z: 0x2000, START: 0x1000, DU: 0x800, DD: 0x400, DL: 0x200, DR: 0x100, L: 0x20, R: 0x10, CU: 8, CD: 4, CL: 2, CR: 1 };
export function parseInputs(text) {
  if (!text.trim()) return [];
  return text.trim().split(',').map(token => {
    const parts = token.trim().split(':');
    const frame = Number(parts[0]), duration = parts.length === 3 ? Number(parts[2]) : 1;
    if (parts.length < 2 || parts.length > 3 || !parts[0] || !Number.isSafeInteger(frame) || frame < 0 || !Number.isSafeInteger(duration) || duration < 1) throw new Error(`Invalid input event: ${token}`);
    const event = { frame, duration, buttons: 0 };
    for (const key of parts[1].split('+')) {
      if (Object.hasOwn(buttons, key)) event.buttons |= buttons[key];
      else if (/^[XY]=-?\d+$/.test(key) && Number(key.slice(2)) >= -128 && Number(key.slice(2)) <= 127) event[key[0].toLowerCase()] = Number(key.slice(2));
      else throw new Error(`Unknown button or invalid axis: ${key}`);
    }
    return event;
  });
}
export function summarize(values) {
  if (!values.length || values.some(x => !Number.isFinite(x) || x < 0)) throw new Error('Timing samples must be nonempty, finite and nonnegative');
  const sorted = [...values].sort((a, b) => a - b);
  const quantile = p => sorted[Math.max(0, Math.ceil(p * sorted.length) - 1)];
  return { count: values.length, meanMs: values.reduce((a, b) => a + b, 0) / values.length, p50Ms: quantile(.5), p95Ms: quantile(.95), p99Ms: quantile(.99), maxMs: sorted.at(-1) };
}
function worker(cfg) {
  const wasm = fs.readFileSync(cfg.wasm), rom = fs.readFileSync(cfg.rom);
  let flushes = 0;
  const ex = new WebAssembly.Instance(new WebAssembly.Module(wasm), { env: { host_log() {}, host_gpu_flush() { flushes++; } } }).exports;
  const ptr = ex.n64_alloc(rom.length);
  if (!ptr) throw new Error('ROM allocation failed');
  new Uint8Array(ex.memory.buffer, ptr, rom.length).set(rom);
  if (!ex.n64_load(ptr, rom.length)) throw new Error('Invalid ROM');
  const hi = Array.from(new Uint32Array(ex.memory.buffer, ex.n64_host_info(), 24));
  const samples = [], coreSamples = [], viSamples = [];
  let visibleFields = 0;
  const start = performance.now();
  for (let frame = 0; frame < cfg.frames; frame++) {
    let b = 0, x = 0, y = 0;
    for (const e of cfg.events) if (frame >= e.frame && frame < e.frame + e.duration) {
      b |= e.buttons; x = e.x ?? x; y = e.y ?? y;
    }
    ex.n64_input(0, b, x, y);
    const t = performance.now();
    if (ex.n64_frame()) throw new Error(`Unexpected GPU synchronization in software mode at field ${frame}`);
    const afterCore = performance.now();
    ex.n64_vi_render();
    const afterVi = performance.now();
    if (frame >= cfg.warmup) {
      samples.push(afterVi - t); coreSamples.push(afterCore - t); viSamples.push(afterVi - afterCore);
      if (!(ex.n64_vi_dims() >>> 31)) visibleFields++;
    }
  }
  const elapsedMs = performance.now() - start;
  const stats = Array.from(new Uint32Array(ex.memory.buffer, hi[21], 16));
  const dims = ex.n64_vi_dims(), width = dims & 65535, height = (dims >>> 16) & 32767;
  if (stats[0] !== cfg.frames || !visibleFields || flushes) throw new Error('Incomplete replay, no valid VI fields, or unexpected GPU flush');
  const view = (address, length) => new Uint8Array(ex.memory.buffer, address, length);
  const image = view(hi[1], width * height * 4);
  const hashes = { rdram: sha(view(hi[0], 8 * 1024 * 1024)), audioRing: sha(view(hi[3], 65536 * 2)), vi: sha(image) };
  const rgb = Buffer.alloc(width * height * 3);
  for (let p = 0; p < width * height; p++) rgb.set(image.subarray(p * 4, p * 4 + 3), p * 3);
  fs.writeFileSync(cfg.image, Buffer.concat([Buffer.from(`P6\n${width} ${height}\n255\n`), rgb]));
  const result = { elapsedMs, measured: summarize(samples), core: summarize(coreSamples), vi: summarize(viSamples), samplesMs: samples, visibleFields, memoryMiB: ex.memory.buffer.byteLength / 1048576, maxRssKiB: process.resourceUsage().maxRSS,
    final: { fields: stats[0], primitives: stats[1], flushes: stats[2], rspInstructions: stats[3], pc: stats[6].toString(16), width, height, blank: !!(dims >>> 31), hashes },
    debugWords: Array.from(new Uint32Array(ex.memory.buffer, ex.n64_debug_state(), 20)) };
  fs.writeFileSync(cfg.result, JSON.stringify(result, null, 2) + '\n');
}
function main() {
  if (process.argv[2] === '--worker') return worker(JSON.parse(process.argv[3]));
  const { values, positionals } = parseArgs({ allowPositionals: true, options: {
    frames: { type: 'string', default: '600' }, warmup: { type: 'string', default: '120' }, repeats: { type: 'string', default: '3' },
    inputs: { type: 'string' }, output: { type: 'string', default: 'out/benchmark' }, wasm: { type: 'string', default: 'out/n64.wasm' },
    timeout: { type: 'string', default: '300000' }, profile: { type: 'boolean', default: false }
  } });
  if (positionals.length !== 1) throw new Error('Usage: node tools/benchmark.mjs ROM [--frames 4200 --warmup 3800 --inputs tools/inputs/sm64.txt --repeats 3 --output out/bench-sm64 --profile]');
  const frames = Number(values.frames), warmup = Number(values.warmup), repeats = Number(values.repeats), timeout = Number(values.timeout);
  if (![frames, warmup, repeats, timeout].every(Number.isSafeInteger) || frames < 1 || frames > 1000000 || warmup < 0 || warmup >= frames || repeats < 2 || repeats > 20 || timeout < 1) throw new Error('Invalid frame, warmup, repeat or timeout limits');
  const rom = path.resolve(positionals[0]), wasm = path.resolve(values.wasm), output = path.resolve(values.output);
  const inputText = values.inputs ? fs.readFileSync(values.inputs, 'utf8') : '';
  const events = parseInputs(inputText);
  fs.mkdirSync(output, { recursive: true });
  const git = args => spawnSync('git', args, { encoding: 'utf8' }).stdout.trim();
  const report = { schemaVersion: 1, createdAt: new Date().toISOString(), source: { revision: git(['rev-parse', 'HEAD']), dirty: !!git(['status', '--porcelain']) },
    host: { node: process.version, v8: process.versions.v8, platform: os.platform(), arch: os.arch(), cpu: os.cpus()[0]?.model, logicalCpus: os.cpus().length, availableParallelism: os.availableParallelism(), loadAverage: os.loadavg() },
    rom: { name: path.basename(rom), sha256: sha(fs.readFileSync(rom)) }, wasmSha256: sha(fs.readFileSync(wasm)), inputsSha256: sha(inputText),
    frames, warmup, repeats, events, runs: [], outcome: 'FAIL',
    interpretation: 'Software CPU/RSP/RDP plus VI time per emulated field, excluding boot/warmup. No browser presentation, audio playback or physical GPU. Shared-host relative measurement; repeat agreement is not hardware accuracy.' };
  const cfg = { rom, wasm, frames, warmup, events };
  function run(label, profile = false) {
    const result = path.join(output, `${label}.json`), image = path.join(output, `${label}.ppm`);
    fs.rmSync(result, { force: true });
    const flags = profile ? ['--cpu-prof', `--cpu-prof-dir=${output}`, '--cpu-prof-name=replay.cpuprofile'] : [];
    const child = spawnSync(process.execPath, [...flags, self, '--worker', JSON.stringify({ ...cfg, result, image })], { encoding: 'utf8', timeout, maxBuffer: 1024 * 1024 });
    fs.writeFileSync(path.join(output, `${label}.log`), (child.stdout || '') + (child.stderr || ''));
    if (child.status !== 0) throw new Error(`${label} failed: ${child.error?.message || child.signal || child.status}; see its log`);
    const data = JSON.parse(fs.readFileSync(result));
    console.log(`${label}: mean=${data.measured.meanMs.toFixed(3)}ms/field p95=${data.measured.p95Ms.toFixed(3)} p99=${data.measured.p99Ms.toFixed(3)} visible=${data.visibleFields}`);
    return data;
  }
  try {
    for (let repeat = 0; repeat < repeats; repeat++) report.runs.push(run(`run-${repeat + 1}`));
    const reference = JSON.stringify(report.runs[0].final);
    if (report.runs.some(r => JSON.stringify(r.final) !== reference)) throw new Error('Repeated replays disagree in final state or image/audio hashes');
    report.repeatMeansMs = report.runs.map(r => r.measured.meanMs);
    report.medianRunMeanMs = summarize(report.repeatMeansMs).p50Ms;
    if (values.profile) {
      report.profileRun = run('profile', true);
      if (JSON.stringify(report.profileRun.final) !== reference) throw new Error('Profiled replay differs from unprofiled runs');
      report.profileScope = 'Whole process including boot/warmup; excluded from benchmark statistics.';
    }
    report.outcome = 'PASS';
  } catch (error) { report.error = error.message; process.exitCode = 1; }
  fs.writeFileSync(path.join(output, 'summary.json'), JSON.stringify(report, null, 2) + '\n');
  console.log(`${report.outcome}: replay completion and determinism${report.error ? ': ' + report.error : ''}`);
}
if (process.argv[1] && path.resolve(process.argv[1]) === self) main();
