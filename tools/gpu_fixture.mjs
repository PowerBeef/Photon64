// Authored IPL3 test program: submit synthetic RDP streams, read/partially
// overwrite their wrapped targets, and repeat after a Count-register delay.
// Contains no Nintendo boot code or commercial bytes. Source is MIT.
import fs from 'node:fs';
export function gpuFixture() {
  const text = fs.readFileSync(new URL('./rdp_vectors.h', import.meta.url), 'utf8').split('// Small formats')[0];
  const commands = [...text.matchAll(/\{(0x[^}]+)\}/g)].flatMap(m => m[1].split(',').map(x => Number(x.trim())).filter(x => Number.isFinite(x)));
  const rom = new Uint8Array(4096), dv = new DataView(rom.buffer);
  dv.setUint32(0, 0x80371240); rom.set(new TextEncoder().encode('PHOTON GPU LIFECYCLE'), 0x20); rom[0x3e] = 69;
  commands.forEach((v,i) => dv.setUint32(0x400+i*4,v));
  const p = [], emit = v => p.push(v >>> 0);
  const lui = (r,v) => emit(0x3c000000 | r<<16 | v), ori = (r,v) => emit(0x34000000 | r<<16 | v);
  const store = (r,b,o,op=43) => emit(op<<26 | b<<21 | r<<16 | o);
  lui(8,0xa410); ori(9,2); store(9,8,12); // DPC XBUS: commands from DMEM
  const loop = p.length; ori(9,0x400); store(9,8,0); ori(9,0x400+commands.length*4); store(9,8,4);
  lui(15,0xa000); emit(0x90000000 | 15<<21 | 9<<16); // lbu of GPU-owned byte requests a barrier
  store(9,15,1,40); // partial CPU store must preserve the other byte's GPU value
  emit(0x400a4800); // mfc0 t2, Count
  lui(13,8);
  const wait = p.length; emit(0x400b4800); emit(11<<21 | 10<<16 | 12<<11 | 0x23); // subu
  emit(12<<21 | 13<<16 | 14<<11 | 0x2b); // sltu
  emit(5<<26 | 14<<21 | ((wait-p.length-1)&65535)); emit(0);
  emit(2<<26 | ((0xa4000040+loop*4)>>>2 & 0x3ffffff)); emit(0);
  p.forEach((v,i) => dv.setUint32(0x40+i*4,v));
  return rom;
}
