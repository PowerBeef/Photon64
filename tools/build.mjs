// Assemble the single-file HTML: template + WASM (base64) + shaders + scripts.
import crypto from 'node:crypto'; import { execFileSync } from 'node:child_process'; import fs from 'fs'; import path from 'path'; import { fileURLToPath } from 'url';
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const rd = f => fs.readFileSync(path.join(root, f), 'utf8');
const strip = src => src.split('\n').map(l => {  // cut // comments, but never inside '...' or "..."
  let s = '', str = null;
  for (let i = 0; i < l.length; i++) {
    const c = l[i];
    if (str) { s += c; if (c === '\\') s += l[++i] ?? ''; else if (c === str) str = null; }
    else if (c === '"' || c === "'") { str = c; s += c; }
    else if (c === '/' && l[i + 1] === '/' && l[i + 2] !== '#') break;  // (//#.. directives survive for pp())
    else s += c;
  }
  return s.replace(/^\s+/, '').replace(/\s+$/, '');
}).filter(l => l && (!l.startsWith('//') || l.startsWith('//#'))).join('\n');
const wasmPath = path.join(root, 'out/n64.wasm');
if (!fs.existsSync(wasmPath)) throw new Error('out/n64.wasm is missing — run ./build_wasm.sh first');
const wasm = fs.readFileSync(wasmPath).toString('base64');
const sourceMeta = { revision: execFileSync('git', ['rev-parse','HEAD'], { cwd: root, encoding: 'utf8' }).trim(), dirty: !!execFileSync('git', ['status','--porcelain'], { cwd: root, encoding: 'utf8' }).trim(), wasm_sha256: crypto.createHash('sha256').update(fs.readFileSync(wasmPath)).digest('hex') };
const shaders = {};
for (const n of ['rdp', 'vi', 'merge']) shaders[n] = strip(rd(`src/web/${n}.wgsl`));
fs.writeFileSync(path.join(root, 'out/shaders.json'), JSON.stringify(shaders));
const js = `(() => {
'use strict';
const WASM_B64 = "${wasm}";
const SOURCE_META = ${JSON.stringify(sourceMeta)};
const SHADERS = ${JSON.stringify(shaders)};
const BRAND_SVG = ${JSON.stringify(rd('assets/logo-on-dark.svg').trim().replace('role="img" aria-label="Photon64"', 'aria-hidden="true" focusable="false"'))};
${rd('src/web/gpu.js')}
${rd('src/web/pad.js')}
${rd('src/web/art.js')}
${rd('src/web/app.js')}
})();`;
if (js.includes('</script')) throw new Error('script terminator inside bundle');
const notices = '<!-- ares ISC license\n' + rd('third_party/ares/LICENSE.txt').replaceAll('--', '—') + '\n-->\n' + '<!-- SoftFloat 3e license\n' + rd('third_party/softfloat/COPYING.txt').replaceAll('--', '—') + '\n-->\n';
const html = notices + rd('src/web/app.html').replace('<!--@SCRIPT-->', () => `<script>\n${js}\n</script>`);
fs.writeFileSync(path.join(root, 'out/photon64.html'), html);
console.log('out/photon64.html', (html.length / 1024).toFixed(1), 'KB (wasm', (wasm.length / 1024).toFixed(1), 'KB b64)');

const sha = b => crypto.createHash('sha256').update(b).digest('hex');
const buildInfo = { source_sha: execFileSync('git', ['rev-parse','HEAD'], { cwd: root, encoding: 'utf8' }).trim(), dirty: !!execFileSync('git', ['status','--porcelain'], { cwd: root, encoding: 'utf8' }).trim(), node: process.version, wasm_sha256: sha(fs.readFileSync(wasmPath)), html_sha256: sha(Buffer.from(html)), toolPins: rd('tools/versions.env').trim(), components: ['Photon64 MIT','SoftFloat 3e BSD-3-Clause','ares CIC adaptation ISC'] };
fs.writeFileSync(path.join(root,'out/build-info.json'), JSON.stringify(buildInfo,null,2) + '\n');
