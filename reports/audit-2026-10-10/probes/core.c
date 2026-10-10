// Diagnostic observations against audited source; no commercial fixtures.
#include "../../../src/n64.c"
#include <stdio.h>
#include <string.h>
void host_log(const char *s,u32 a,u32 b) {(void)s;(void)a;(void)b;}
void host_gpu_flush(void) {}
static void setup(u32 op) {
 memset(&sys,0,sizeof sys);sys.rdram_size=RDRAM_MAX;cpu_reset();
 gpu_unwatch(1);cpu_restart=cpu_resume_one=0;
 RDRAM32(0x1000)=op;cpu.pc=0x80001000;cpu.npc=cpu.pc+4;cpu.next_ev=cpu.cycles+cpu.cpi;
}
int main(int argc,char **argv) {
 if(argc>1 && !strcmp(argv[1],"rom-ub")) {
  static u8 rom[4096]={0x80,0x37,0x12,0x40};
  printf("ROM load=%d\n",n64_load_rom(rom,sizeof rom));return 0;
 }
 if(argc>1 && !strcmp(argv[1],"bus-ub")) {
  sys.save_type=SAVE_SRAM;savemem[0]=0x80;
  printf("SRAM read=%08x\n",cart_read32(0x08000000));return 0;
 }
 for(u32 op=26;op<=38;op++) {
  if(op!=26 && op!=27 && op!=34 && op!=38)continue;
  setup((op<<26)|(8u<<21)|(9u<<16)|3);cpu.r[8]=0x4000;cpu_run();
  printf("load opcode=%u cause=%u BadVAddr=%08x expected=00004003\n",op,(u32)(cpu.cp0[C0_CAUSE]>>2)&31,(u32)cpu.cp0[C0_BADVADDR]);
 }
 setup((35u<<26)|(8u<<21)|(9u<<16));
 cpu.cp0[C0_STATUS]=0x10; // user mode, EXL/ERL clear
 // Run from a mapped user page, read a kernel-only direct-mapped address.
 map_r[1]=(uintptr_t)(rdram+0x1000);cpu.pc=0x1000;cpu.npc=0x1004;
 cpu.r[8]=(s64)(s32)0x80002000;RDRAM32(0x2000)=0x12345678;cpu_run();
 printf("user kernel read: cause=%u value=%08x (expected AdEL)\n",(u32)(cpu.cp0[C0_CAUSE]>>2)&31,(u32)cpu.r[9]);
 setup(0);gpu_exact=1;gpu_mark_stale((RDRAM_MAX>>1)-2,4);
 printf("wrapped render range: endStale=%d startStale=%d (expected both 1)\n",gpu_range_stale(RDRAM_MAX-4,4),gpu_range_stale(0,4));
 static u8 rom[4096]={0x40,0x12,0x37,0x80};sys.rom=rom;sys.rom_size=sizeof rom;
 sys.save_dirty=7;eeprom[0]=0xA5;sys_reset();
 printf("reset: dirty=%u battery=%02x (dirty must survive or commit first)\n",sys.save_dirty,eeprom[0]);
 return 0;
}
