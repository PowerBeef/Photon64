// Assemble the single-file HTML: template + WASM (base64) + shaders + scripts.
import fs from 'fs'; import path from 'path'; import { fileURLToPath } from 'url';
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
const shaders = {};
for (const n of ['rdp', 'vi', 'merge']) shaders[n] = strip(rd(`src/web/${n}.wgsl`));
fs.writeFileSync(path.join(root, 'out/shaders.json'), JSON.stringify(shaders));
const js = `(() => {
'use strict';
const WASM_B64 = "${wasm}";
const SHADERS = ${JSON.stringify(shaders)};
const BRAND_SVG = ${JSON.stringify(rd('assets/logo-on-dark.svg').trim().replace('role="img" aria-label="Photon64"', 'aria-hidden="true" focusable="false"'))};
${rd('src/web/gpu.js')}
${rd('src/web/pad.js')}
${rd('src/web/art.js')}
${rd('src/web/app.js')}
})();`;
if (js.includes('</script')) throw new Error('script terminator inside bundle');
const notices = '<!-- SoftFloat 3e license\n' + rd('third_party/softfloat/COPYING.txt').replaceAll('--', '—') + '\n-->\n';
const html = notices + rd('src/web/app.html').replace('<!--@SCRIPT-->', () => `<script>\n${js}\n</script>`);
fs.writeFileSync(path.join(root, 'out/photon64.html'), html);
console.log('out/photon64.html', (html.length / 1024).toFixed(1), 'KB (wasm', (wasm.length / 1024).toFixed(1), 'KB b64)');
