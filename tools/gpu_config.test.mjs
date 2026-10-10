import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
const html = fs.readFileSync(new URL('./gputest.html', import.meta.url), 'utf8');
const context = vm.createContext({ window: {}, performance, fetch() { throw new Error('fixture fetch'); } });
vm.runInContext(html.slice(html.lastIndexOf('<script>') + 8, html.lastIndexOf('</script>')), context);
test('exact HD-at-1x checks reject aliased storage before execution', async () => {
  for (const runner of ['runTest', 'runMulti']) {
    const result = await context.window[runner]({ hd: 0, hdWords: 0x200000 });
    assert.match(result.error, /full RDRAM storage.*aliases/);
  }
});
test('full-size HD checks and explicit diagnostics pass the configuration gate', async () => {
  for (const cfg of [{ hd: 0 }, { hd: 0, hdWords: 0x400000 }, { hd: 0, hdWords: 0x200000, noexact: true }])
    await assert.rejects(context.window.runTest(cfg), /fixture fetch/);
});
