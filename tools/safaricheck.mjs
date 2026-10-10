// Branded macOS Safari through Apple's bundled WebDriver; no Playwright substitute.
// Enable Remote Automation yourself first, or use the disposable hosted Safari job.
import fs from 'node:fs';
import path from 'node:path';
import http from 'node:http';
import { spawn } from 'node:child_process';
import assert from 'node:assert/strict';
import { auditUi } from './ui-audit.mjs';
const evidence = process.env.UI_EVIDENCE || 'out/safari-evidence'; fs.mkdirSync(evidence, { recursive: true });
const report = { outcome: 'FAIL', browser: 'native Safari', cases: [], driverLog: '', limitations: 'macOS Safari only. Window dimensions may be clamped by Safari or the hosted display; actual CSS viewport is recorded. No connected iOS device or mobile GPU claim.' };
const html = fs.readFileSync('out/photon64.html'), fixture = fs.readFileSync(process.argv[2] || 'testroms/RSPCP2VRCP.N64');
const uploadPath = path.resolve('out/Safari-homebrew-with-a-deliberately-long-cartridge-filename.N64');
fs.copyFileSync(process.argv[2] || 'testroms/RSPCP2VRCP.N64', uploadPath);
const server = http.createServer((req, res) => { const binary = req.url === '/fixture.n64'; res.setHeader('Content-Type', binary ? 'application/octet-stream' : 'text/html'); res.end(binary ? fixture : html); });
await new Promise(r => server.listen(0, '127.0.0.1', r));
const port = +(process.env.SAFARI_DRIVER_PORT || 5001), driver = spawn('/usr/bin/safaridriver', ['--port', String(port)]);
driver.stdout.on('data', b => report.driverLog += b); driver.stderr.on('data', b => report.driverLog += b);
let driverError; driver.on('error', e => { driverError = e; });
const delay = ms => new Promise(r => setTimeout(r, ms));
let session, phase = 'driver-start', diagnostic = false;
const deadline = Date.now() + +(process.env.SAFARI_BUDGET_MS || 360000);
report.journal = [];
function checkpoint() { fs.writeFileSync(path.join(evidence, 'result.json'), JSON.stringify(report, null, 2) + '\n'); }
function mark(label) { phase = label; report.phase = label; checkpoint(); }

async function command(method, endpoint, body) {
  const item = { phase, method, endpoint, script: typeof body?.script === 'string' ? body.script.slice(0, 160) : undefined, started: new Date().toISOString() };
  report.journal.push(item); checkpoint();
  try {
  const res = await fetch(`http://127.0.0.1:${port}${endpoint}`, { method, headers: { 'Content-Type': 'application/json' }, body: body === undefined ? undefined : JSON.stringify(body), signal: AbortSignal.timeout(diagnostic ? 2500 : Math.max(1, Math.min(20000, deadline - Date.now()))) });
  const data = await res.json();
  if (!res.ok || data.value?.error) throw new Error(endpoint + ': ' + JSON.stringify(data));
  item.finished = new Date().toISOString(); return data.value;
  } catch (e) { item.error = String(e); throw e; } finally { checkpoint(); }
}
const cmd = (method, endpoint, body) => command(method, `/session/${session}${endpoint}`, body);
const evaluate = (fn, arg = null) => cmd('POST', '/execute/sync', { script: `return (${fn.toString()})(arguments[0]);`, args: [arg] });
const recordErrors = () => evaluate(() => {
  window.__uiErrors = [];
  addEventListener('error', e => window.__uiErrors.push(e.message));
  addEventListener('unhandledrejection', e => window.__uiErrors.push(String(e.reason)));
  window.__uploadEvents = [];
  document.getElementById('file').addEventListener('change', e => window.__uploadEvents.push([...e.target.files].map(f => ({ name: f.name, size: f.size, type: f.type }))), { capture: true });
});
async function evaluateAsync(fn, arg = null) {
  const result = await cmd('POST', '/execute/async', { script: `const done = arguments[arguments.length - 1]; Promise.resolve((${fn.toString()})(arguments[0])).then(value => done({value}), e => done({error: String(e.stack || e)}));`, args: [arg] });
  if (result.error) throw Error(result.error); return result.value;
}
async function wait(fn) {
  const until = Math.min(deadline, Date.now() + 30000);
  while (Date.now() < until) { if (await evaluate(fn)) return; await delay(250); }
  throw Error('Timed out: ' + fn.toString());
}
async function click(selector) {
  await evaluate(s => document.querySelector(s).scrollIntoView({ block: 'nearest' }), selector);
  const el = await cmd('POST', '/element', { using: 'css selector', value: selector });
  await cmd('POST', `/element/${el['element-6066-11e4-a52e-4f735466cecf']}/click`, {});
}
async function key(value, shift = false) {
  const actions = [];
  if (shift) actions.push({ type: 'keyDown', value: '\uE008' });
  actions.push({ type: 'keyDown', value }, { type: 'keyUp', value });
  if (shift) actions.push({ type: 'keyUp', value: '\uE008' });
  await cmd('POST', '/actions', { actions: [{ type: 'key', id: 'keyboard', actions }] });
}
async function shot(name) { fs.writeFileSync(path.join(evidence, name + '.png'), Buffer.from(await cmd('GET', '/screenshot'), 'base64')); }
try {
  for (let n = 0; ; n++) {
    if (driverError) throw driverError;
    try { await command('GET', '/status'); break; } catch (e) { if (n >= 40) throw e; await delay(250); }
  }
  const created = await command('POST', '/session', { capabilities: { alwaysMatch: { browserName: 'safari' } } });
  session = created.sessionId; assert.ok(session); report.capabilities = created.capabilities;
  await cmd('POST', '/timeouts', { script: 30000, pageLoad: 60000, implicit: 0 });
  await cmd('POST', '/url', { url: `http://127.0.0.1:${server.address().port}` });
  await wait(() => !!window.__photon?.ex);
  await recordErrors();
  // Probe an independent file input with no app handler and no early clearing.
  // Do not classify an app import failure as an automation capability failure.
  await evaluate(() => { const input = document.createElement('input'); input.type = 'file'; input.id = 'driver-file-probe'; input.hidden = true; document.body.appendChild(input); });
  const probeElement = await cmd('POST', '/element', { using: 'css selector', value: '#driver-file-probe' });
  await cmd('POST', `/element/${probeElement['element-6066-11e4-a52e-4f735466cecf']}/value`, { text: uploadPath });
  const probe = await evaluateAsync(async () => {
    const f = document.getElementById('driver-file-probe').files[0];
    if (!f) throw Error('Safari driver did not select a probe file');
    try { const bytes = new Uint8Array(await f.arrayBuffer()); return { size: f.size, length: bytes.length, prefix: [...bytes.slice(0, 4)] }; }
    catch (e) { return { size: f.size, error: e.name, message: e.message }; }
  });
  if (probe.error) {
    assert.ok(['NotReadableError', 'SecurityError'].includes(probe.error), 'Unexpected file-probe failure');
    report.filesystemUpload = { outcome: 'SKIP', ...probe, reason: 'The independent OS-backed File read fails before the app participates. Native filesystem chooser validation is blocked on this runner.' };
  } else {
    assert.equal(probe.length, fixture.length); assert.deepEqual(probe.prefix, [...fixture.subarray(0, 4)]);
    report.filesystemUpload = { outcome: 'PASS', ...probe };
  }
  console.log('Safari independent file probe:', JSON.stringify(report.filesystemUpload));
  await evaluate(() => document.getElementById('driver-file-probe').remove());
  for (const [name, width, height] of [['narrow', 390, 844], ['landscape', 1000, 500], ['desktop', 1000, 740]].filter(([n]) => !process.env.SAFARI_CASES || process.env.SAFARI_CASES.split(',').includes(n))) {
    mark(name + '/start');
    await cmd('POST', '/window/rect', { x: 0, y: 0, width, height }); await delay(500);
    const row = { name, requestedWindow: [width, height], phases: [] }; report.cases.push(row);
    const check = async label => { mark(name + "/" + label); const result = await evaluateAsync(auditUi); row.phases.push({ phase: label, ...result }); assert.deepEqual(result.errors, [], name + '/' + phase + ': ' + result.errors.join('; ')); };
    await check('library'); await click('#h-set'); await check('home-settings');
    await evaluate(() => document.querySelector('#pg-settings [data-nav=back]').focus());
    await key('\uE004', true); assert.ok(await evaluate(() => document.getElementById('sheet').contains(document.activeElement)));
    await key('\uE004'); assert.ok(await evaluate(() => document.activeElement.matches('#pg-settings [data-nav=back]')));
    await evaluate(() => document.getElementById('tab-video').focus()); await key('\uE014');
    assert.equal(await evaluate(() => document.getElementById('tab-console').getAttribute('aria-selected')), 'true');
    const oldPak = await evaluate(() => +document.getElementById('s-pak').value);
    await evaluate(() => document.querySelector('#s-pak + .seg [aria-checked=true]').focus()); await key('\uE014');
    assert.equal(await evaluate(() => document.getElementById('s-pak').value), ({1:'2',2:'0',0:'1'})[oldPak]);
    await key('\uE00C'); assert.ok(await evaluate(() => document.activeElement.id === 'h-set'));
    await evaluate(() => window.__photon.settings.renderer = 'sw');
    await evaluate(() => window.__uploadEvents = []);
    if (report.filesystemUpload.outcome === 'PASS') {
      const input = await cmd('POST', '/element', { using: 'css selector', value: '#file' });
      await cmd('POST', `/element/${input['element-6066-11e4-a52e-4f735466cecf']}/value`, { text: uploadPath });
      row.fileInput = 'OS-backed file selected through native WebDriver';
    } else {
      // Still exercise production openFile/File.arrayBuffer with real fixture
      // bytes in Safari. This is a browser-backed File and synthetic change,
      // explicitly separate from the blocked OS-backed chooser probe above.
      await evaluateAsync(async filename => {
        const response = await fetch('/fixture.n64'); if (!response.ok) throw Error('Fixture fetch failed');
        const transfer = new DataTransfer(); transfer.items.add(new File([await response.arrayBuffer()], filename));
        const input = document.getElementById('file'); input.files = transfer.files; input.dispatchEvent(new Event('change', { bubbles: true }));
      }, path.basename(uploadPath));
      row.fileInput = 'Browser-backed File + synthetic change; native filesystem upload is SKIP';
    }
    row.upload = await evaluate(() => ({ events: window.__uploadEvents, selected: [...document.getElementById('file').files].map(f => ({ name: f.name, size: f.size })), rom: window.__photon.rom?.name, toast: document.getElementById('toast').textContent }));
    console.log('Safari staged upload:', JSON.stringify(row.upload));
    await wait(() => !!window.__photon.rom && new Uint32Array(window.__photon.ex.memory.buffer, window.__photon.hi[21], 8)[0] > 2);
    await click('#b-menu'); await check('game-menu'); await shot(name + '-menu');
    const fields = await evaluate(() => new Uint32Array(window.__photon.ex.memory.buffer, window.__photon.hi[21], 8)[0]); await delay(150);
    assert.equal(await evaluate(() => new Uint32Array(window.__photon.ex.memory.buffer, window.__photon.hi[21], 8)[0]), fields);
    await click('#m-save'); await wait(() => document.querySelectorAll('#slots .slot').length === 4); await check('save-states'); await key('\uE00C');
    await click('#m-load'); await wait(() => document.querySelectorAll('#slots .slot').length === 4); await check('load-states'); await key('\uE00C');
    await click('#m-set');
    for (const tab of ['video', 'console', 'pad', 'data']) { await click('#tab-' + tab); await check('settings-' + tab); await shot(name + '-' + tab); }
    await key('\uE00C'); assert.ok(await evaluate(() => document.activeElement.id === 'm-set'));
    await click('#m-resume'); await wait(() => document.getElementById('sheet').hidden);
    assert.ok(await evaluateAsync(async n => {
      for (let i = 0; i < 120; i++) { if (new Uint32Array(window.__photon.ex.memory.buffer, window.__photon.hi[21], 8)[0] > n) return true; await new Promise(r => setTimeout(r, 50)); }
      return false;
    }, fields), 'Resume must advance emulated fields');
    await check('stage');
    await click('#b-menu'); await click('#m-save'); await wait(() => document.querySelectorAll('#slots .slot').length === 4);
    mark(name + '/state-slot-action');
    await click('#slots .slot:first-child'); await wait(() => document.getElementById('sheet').hidden);
    await click('#b-menu'); await click('#m-load'); await wait(() => document.querySelector('#slots .slot')?.disabled === false);
    mark(name + '/state-slot-action');
    await click('#slots .slot:first-child'); await wait(() => document.getElementById('sheet').hidden);
    row.stateSlotClicks = 'PASS: saved and restored slot 1 through native WebDriver clicks';
    assert.equal(await evaluateAsync(async () => {
      const p = window.__photon; p.setPaused(true);
      new Uint8Array(p.ex.memory.buffer)[p.hi[12]] = 0x5A;
      const generation = new Uint32Array(p.ex.memory.buffer, p.hi[16], 1); generation[0] = (generation[0] + 1) >>> 0 || 1; return p.flushSaves();
    }), true);
    await click('#b-menu'); await click('#m-home'); await wait(() => !!document.querySelector('#lib .cart:not(.add)'));
    assert.deepEqual(await evaluate(() => window.__uiErrors), []);
    await cmd('POST', '/refresh', {}); await wait(() => !!window.__photon?.ex && !!document.querySelector('#lib .cart:not(.add)')); await recordErrors();
    await evaluate(() => window.__photon.settings.renderer = 'sw'); await click('#lib .cart:not(.add)');
    await wait(() => !!window.__photon.rom);
    assert.equal(await evaluate(() => new Uint8Array(window.__photon.ex.memory.buffer)[window.__photon.hi[12]]), 0x5A);
    await click('#b-menu'); await click('#m-home'); await wait(() => !document.getElementById('home').hidden);
    await check('cached-library');
    console.log('PASS native Safari', name, JSON.stringify(row.phases[0].viewport));
  }
  assert.deepEqual(await evaluate(() => window.__uiErrors), []);
  report.outcome = 'PASS'; console.log('PASS native Safari', report.capabilities.browserVersion);
} catch (e) {
  report.error = e.stack; report.firstFailure = { phase, command: report.journal.at(-1), driverExit: driver.exitCode, at: new Date().toISOString() }; diagnostic = true; checkpoint();
  if (session) {
    report.failureState = await evaluate(() => ({ rom: window.__photon?.rom?.name, running: window.__photon?.running, toast: document.getElementById('toast')?.textContent, fileValue: document.getElementById('file')?.value, files: [...(document.getElementById('file')?.files || [])].map(f => ({ name: f.name, size: f.size })), events: window.__uploadEvents, errors: window.__uiErrors })).catch(() => null);
    console.log('Safari failure state:', JSON.stringify(report.failureState));
    await shot('failure').catch(() => {});
  }
  throw e;
}
finally { diagnostic = true; if (session) await cmd('DELETE', '').catch(() => {}); driver.kill(); server.close(); fs.writeFileSync(path.join(evidence, 'result.json'), JSON.stringify(report, null, 2) + '\n'); }
