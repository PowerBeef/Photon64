import http from 'http';
const srv = http.createServer((q, r) => { r.writeHead(200, { 'content-type': 'text/html' }); r.end('<html></html>'); }); await new Promise(r => srv.listen(0, '127.0.0.1', r));
import { chromium } from 'playwright';
const sets = {
  a: ['--enable-unsafe-webgpu', '--enable-features=Vulkan', '--use-vulkan=native', '--use-angle=vulkan', '--ignore-gpu-blocklist', '--disable-gpu-sandbox', '--no-sandbox', '--use-webgpu-adapter=default'],
  b: ['--enable-unsafe-webgpu', '--enable-features=Vulkan,VulkanFromANGLE,DefaultANGLEVulkan', '--use-vulkan=native', '--use-gl=angle', '--use-angle=vulkan', '--ignore-gpu-blocklist', '--disable-gpu-sandbox', '--no-sandbox', '--disable-vulkan-surface'],
  c: ['--enable-unsafe-webgpu', '--enable-features=Vulkan', '--ignore-gpu-blocklist', '--disable-gpu-sandbox', '--no-sandbox', '--enable-gpu', '--use-webgpu-adapter=default', '--disable-vulkan-surface', '--use-angle=swiftshader'],
};
for (const [k, args] of Object.entries(sets)) {
  for (const headless of [true]) {
    let browser;
    try {
      browser = await chromium.launch({ headless, env: { ...process.env, VK_ICD_FILENAMES: '/usr/share/vulkan/icd.d/lvp_icd.json', VK_DRIVER_FILES: '/usr/share/vulkan/icd.d/lvp_icd.json' }, args });
      const page = await browser.newPage();
      await page.goto('http://127.0.0.1:' + srv.address().port + '/');
      const r = await page.evaluate(async () => { if (!navigator.gpu) return 'no gpu'; const a = await navigator.gpu.requestAdapter(); if (!a) return 'no adapter'; return JSON.stringify({ v: a.info.vendor, a: a.info.architecture, d: a.info.description, fb: a.info.isFallbackAdapter }); });
      console.log(k, r);
    } catch (e) { console.log(k, 'ERR', String(e.message).slice(0, 200)); }
    if (browser) await browser.close();
  }
}
srv.close();
