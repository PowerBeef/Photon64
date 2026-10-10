// Small physical-backend-independent exact checkpoint probe, including rendered frames.
// Run with an available Dawn adapter and local ROM; no noref/noexact bypass.
import { spawnSync } from 'node:child_process';
const args = process.argv.slice(2), rom = args[0] || 'roms/Mario Kart 64 (USA).z64';
for (const hd of [[], ['--hd', '0']]) {
  const r = spawnSync(process.execPath, ['tools/dawntest.mjs', rom, '300', '250,299', '', '--window', '2', ...hd], { stdio: 'inherit', timeout: 180000 });
  if (r.status !== 0) { console.error('GPU parity probe failed:', r.error?.message || r.status); process.exitCode = 1; }
}
