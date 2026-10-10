// Shared types and global machine state.
#ifndef CORE_H
#define CORE_H
#include <stdint.h>
#include <stddef.h>

typedef uint8_t u8;   typedef int8_t s8;
typedef uint16_t u16; typedef int16_t s16;
typedef uint32_t u32; typedef int32_t s32;
typedef uint64_t u64; typedef int64_t s64;

// The differential renderer owns a second framebuffer image. Observe guest
// writes there, including writes that repeat the reference's current value.
// Production builds retain their direct store mappings and have no callbacks.
#ifdef RDP_ORACLE
void rdp_oracle_cpu_write(u32 pa, u32 value, u32 mask);
void rdp_oracle_dma_write(u32 pa, u32 len);
#define CPU_WRITE_MAP(h) ((void)(h), (uintptr_t)0)
#define RDP_CPU_WRITE(pa, v, mask) rdp_oracle_cpu_write(pa, v, mask)
#define RDP_DMA_WRITE(pa, len) rdp_oracle_dma_write(pa, len)
#else
#define CPU_WRITE_MAP(h) (h)
#define RDP_CPU_WRITE(pa, v, mask) ((void)0)
#define RDP_DMA_WRITE(pa, len) ((void)(pa), (void)(len))
#endif

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define NOINLINE __attribute__((noinline))
#define INLINE static inline __attribute__((always_inline))

#ifdef __wasm__
#define EXPORT(name) __attribute__((export_name(#name)))
#define IMPORT(name) __attribute__((import_module("env"), import_name(#name)))
void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
void *memmove(void *d, const void *s, size_t n);
int memcmp(const void *a, const void *b, size_t n);
#else
#include <string.h>
#include <stdio.h>
#define EXPORT(name)
#define IMPORT(name)
#endif

// host callbacks
IMPORT(host_log) void host_log(const char *msg, u32 a, u32 b);
IMPORT(host_gpu_flush) void host_gpu_flush(void);

#define CPU_HZ 93750000ull
#define RDRAM_MAX 0x800000u

// ---- events -------------------------------------------------------------
enum { EV_VI, EV_COMPARE, EV_AI, EV_PI, EV_SI, EV_RSP, EV_DP, EV_MAX };
#define EV_NEVER (~0ull)

// ---- CPU ------------------------------------------------------------------
typedef struct { u32 mask; u64 hi; u32 lo0, lo1; u8 g; } TLBEntry;

typedef union { u64 u; s64 s; double d; struct { u32 lo, hi; }; struct { float f, fh; }; } FReg;

typedef struct {
  s64 r[32];
  s64 hi, lo;
  u32 pc, npc, ipc;
  u32 branch, delay;
  u32 cpi;                 // master cycles per instruction
  u64 cycles, next_ev;
  u64 ev[EV_MAX];
  u64 cp0[32];
  u64 cp0_latch;
  u64 count_base;          // Count = (cycles - count_base) >> 1
  u32 llbit;
  u32 fcr31, fcr0;
  FReg f[32];
  u32 fmask;               // 31 when Status.FR=1, else 30
  TLBEntry tlb[32];
  u32 irq_poll;
  u64 idle_skipped;
} CPU;

enum { C0_INDEX, C0_RANDOM, C0_ENTRYLO0, C0_ENTRYLO1, C0_CONTEXT, C0_PAGEMASK, C0_WIRED, C0_7,
  C0_BADVADDR, C0_COUNT, C0_ENTRYHI, C0_COMPARE, C0_STATUS, C0_CAUSE, C0_EPC, C0_PRID,
  C0_CONFIG, C0_LLADDR, C0_WATCHLO, C0_WATCHHI, C0_XCONTEXT, C0_21, C0_22, C0_23,
  C0_24, C0_25, C0_PERR, C0_CACHEERR, C0_TAGLO, C0_TAGHI, C0_ERROREPC, C0_31 };

enum { EXC_INT = 0, EXC_MOD = 1, EXC_TLBL = 2, EXC_TLBS = 3, EXC_ADEL = 4, EXC_ADES = 5, EXC_IBE = 6, EXC_DBE = 7,
  EXC_SYS = 8, EXC_BP = 9, EXC_RI = 10, EXC_CPU = 11, EXC_OV = 12, EXC_TR = 13, EXC_FPE = 15, EXC_WATCH = 23 };

// ---- system / peripherals -------------------------------------------------
enum { MI_SP = 1, MI_SI = 2, MI_AI = 4, MI_VI = 8, MI_PI = 16, MI_DP = 32 };

typedef struct {
  // MI
  u32 mi_mode, mi_intr, mi_mask;
  // VI
  u32 vi[16];
  u64 vi_field_start; u32 vi_field_cycles; u32 vi_field;
  // AI
  u32 ai_dram, ai_len, ai_control, ai_status, ai_dacrate, ai_bitrate;
  struct { u32 addr, len; u64 dur; } ai_fifo[2];
  u64 ai_start;
  // PI
  u32 pi[13]; u32 pi_latch;
  // RI
  u32 ri[8];
  // SI
  u32 si_dram, si_status;
  u8 pif[64];
  // RDRAM regs
  u32 rdreg[10];
  // SP
  u32 sp_mem_addr, sp_dram_addr, sp_rd_len, sp_wr_len, sp_status, sp_sema;
  // DP
  u32 dp_start, dp_end, dp_current, dp_status, dp_clock, dp_bufbusy, dp_pipebusy, dp_tmem;
  // cart
  u8 *rom; u32 rom_size, rom_mask;
  u32 cic, tv;             // tv: 0 PAL, 1 NTSC, 2 MPAL
  u32 rdram_size;
  u32 save_type;           // 0 none/auto, 1 eep4k, 2 eep16k, 3 sram, 4 flash
  u32 save_dirty;          // nonzero write generation; host clears only a committed matching snapshot
  // flash
  u32 fl_mode; u64 fl_status; u32 fl_erase_off; u32 fl_write_off;
  // input
  u32 buttons[4]; s32 stick_x[4], stick_y[4]; u32 pad_present[4]; u32 pak[4];   // pak: 0 none, 1 controller pak, 2 rumble pak
  u32 rumble[4];
  // run control
  u32 frame_done, frames;
  u32 vi_clock;
  // ISViewer
  u8 isv[0x10000];
} SYS;

enum { SAVE_NONE, SAVE_EEP4K, SAVE_EEP16K, SAVE_SRAM, SAVE_FLASH };

extern CPU cpu;
extern SYS sys;
extern u8 rdram[];          // host-endian 32-bit words
extern u8 spmem[0x2000];    // DMEM then IMEM, host-endian words
extern uintptr_t map_r[1 << 20], map_w[1 << 20];
extern u8 eeprom[0x800];
extern u8 savemem[0x20000];
extern u8 mempak[4][0x8000];

#define RDRAM32(a) (*(u32 *)(rdram + ((a) & ~3u)))

// GPU coherency: pages the GPU renderer draws into are "watched" (CPU stores take the slow path) so that every
// CPU-side write can be flagged per halfword (bit 7 of rdp_hidden[]) and merged exactly with GPU-side results.
extern u8 gpu_watch[RDRAM_MAX >> 12];
extern u32 gpu_watch_count;
// Exact GPU -> CPU feedback: memory the GPU has drawn into and the CPU copy has not received yet is "stale".
// Stale pages lose their fast load mapping; touching stale memory (a CPU load, a partial store, an RSP DMA, an RDP
// texture load) stops the machine in front of that access and asks the host to copy the GPU's results back
// (n64_frame() returns 1), after which the access is repeated. The emulated machine never sees a difference.
extern u8 gpu_stale_pg[RDRAM_MAX >> 12];
extern u32 gpu_stale_n, gpu_sync_request, cpu_restart, cpu_resume_one;
int gpu_range_stale(u32 pa, u32 len);
void gpu_request_sync(void);
extern u32 gpu_sync_cause[4];   // what asked for the last copy-back: kind (1 CPU load, 2 CPU store, 3 RSP DMA, 4 RDP texture load), address, pc
void cpu_stale_page(u32 pg, int on);
extern u8 rdp_hidden[];
extern u16 rdp_shadow16[];
void gpu_mark_dirty(u32 pa, u32 len);
// hidden (9th) bits of halfword k whose current value is v
#define HIDDEN_AT(k, v) ((rdp_shadow16[k] == (v) && !(rdp_hidden[k] & 0x80)) ? ((u32)rdp_hidden[k] & 3u) : ((v) & 1) * 3u)

// cpu.c
void cpu_reset(void);
void cpu_run(void);
void cpu_check_irq(void);
void cpu_exception(u32 code, u32 ce);
void tlb_remap_all(void);
// bus.c
u32 bus_read32(u32 pa);
void bus_write32(u32 pa, u32 val, u32 mask);
void mi_raise(u32 bits);
void mi_lower(u32 bits);
void ev_set(int e, u64 t);
void ev_recalc(void);
void sys_reset(void);
extern u32 rdram_next, xpak_mode;
int n64_run_frame(void);
// rsp.c
void rsp_run(s32 n);
u32 rsp_sync(void);
void rsp_reset(void);
u32 sp_reg_read(u32 r);
void sp_reg_write(u32 r, u32 v);
u32 dp_reg_read(u32 r);
void dp_reg_write(u32 r, u32 v);
// rdp.c
void rdp_reset(void);
void rdp_process(void);
// vi.c
void vi_render(void);
// audio
void audio_push(u32 addr, u32 len, u32 freq);

#endif
