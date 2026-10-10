import fs from 'node:fs';
for (const opt of [0, 3]) {
  const { instance: { exports: e } } = await WebAssembly.instantiate(fs.readFileSync(new URL(`../out/coretest_O${opt}.wasm`, import.meta.url)), { env: { host_log() {}, host_gpu_flush() {} } });
  const bad = e.run_tests(); console.log(`WASM O${opt}: ${e.test_count()} checks, ${bad} failures (first ${e.test_first_failure()})`);
  if (bad) process.exitCode = 1;
}
