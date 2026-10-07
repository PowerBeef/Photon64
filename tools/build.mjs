// Assemble the single-file HTML: template + WASM (base64) + shaders + scripts.
import fs from 'fs'; import path from 'path'; import { fileURLToPath } from 'url';
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const rd = f => fs.readFileSync(path.join(root, f), 'utf8');
const strip = src => src.split('\n').map(l => l.replace(/^\s+/, '').replace(/\s*\/\/ .*$/, '')).filter(l => l && (!l.startsWith('//') || l.startsWith('//#'))).join('\n');
const wasm = fs.readFileSync(path.join(root, 'out/n64.wasm')).toString('base64');
const shaders = {};
for (const n of ['rdp', 'vi', 'merge']) shaders[n] = strip(rd(`src/web/${n}.wgsl`));
fs.writeFileSync(path.join(root, 'out/shaders.json'), JSON.stringify(shaders));
const js = `(() => {
'use strict';
const WASM_B64 = "${wasm}";
const SHADERS = ${JSON.stringify(shaders)};
${rd('src/web/gpu.js')}
${rd('src/web/pad.js')}
${rd('src/web/art.js')}
${rd('src/web/app.js')}
})();`;
if (js.includes('</script')) throw new Error('script terminator inside bundle');
const html = rd('src/web/app.html').replace('<!--@SCRIPT-->', () => `<script>\n${js}\n</script>`);
fs.writeFileSync(path.join(root, 'out/photon64.html'), html);
console.log('out/photon64.html', (html.length / 1024).toFixed(1), 'KB (wasm', (wasm.length / 1024).toFixed(1), 'KB b64)');
