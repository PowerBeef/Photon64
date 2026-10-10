// Host API (shared by native harness and WASM exports)
#include "core.h"

#define AUDIO_RING 32768
s16 audio_buf[AUDIO_RING * 2];
u32 audio_wr, audio_freq;

void audio_push(u32 addr, u32 len, u32 freq) {
  audio_freq = freq;
  for (u32 i = 0; i < len; i += 4) {
    u32 a = (addr + i) & (RDRAM_MAX - 1);
    u32 w = RDRAM32(a);
    u32 k = (audio_wr++ & (AUDIO_RING - 1)) * 2;
    audio_buf[k] = (s16)(w >> 16);
    audio_buf[k + 1] = (s16)w;
  }
}

// ROM in any byte order -> host word order. Returns 0 if not an N64 ROM.
// Cartridge save hardware by game ID (3 letters = exact media + ID, 2 letters = any "N" cartridge).
// Everything else gets a 4K EEPROM.
static const char *const save_db[5] = { 0, 0,
  "B7 GT FU CW CZ D6 DO D2 3D MX GC IM NB MV M8 EV PP UB PD RZ R7 EP YS",
  "TE VB B5 FZ CFZ SI G6 GP YW HY IB PS CPS PA P4 J5 P6 PE JG ZL CZL KG MF RI UT UM OB PM RE AL T3 S4 A2 VP WL W2 WX CDZ",
  "CC DA AF JF KJ ZS M6 CK MQ PN PF PO CP2 P3 RH SQ T9 W4 DP" };
static u32 detect_save_type(const u8 *rom) {
  char id[3] = { (char)rom[0x3B ^ 3], (char)rom[0x3C ^ 3], (char)rom[0x3D ^ 3] };
  for (u32 t = 2; t <= 4; t++) {
    for (const char *p = save_db[t]; *p; ) {
      const char *e = p; while (*e && *e != ' ') e++;
      if (e - p == 3 ? (p[0] == id[0] && p[1] == id[1] && p[2] == id[2]) : (id[0] == 'N' && p[0] == id[1] && p[1] == id[2])) return t;
      p = *e ? e + 1 : e;
    }
  }
  return SAVE_EEP4K;
}

// Expansion Pak: the second 4 MB of RDRAM. Same ID notation as the save table.
//   1 uses it for extras (usually a high-resolution mode chosen in the game's menus)   2 needs it for part of its content
//   3 does not run (or runs only a fraction of the game) without it                    4 misbehaves when it is installed
// Sources: the published compatibility lists, IDs from the ares and No-Intro databases. A game that is not listed is not known
// to use the pak. Nothing depends on the list being complete: the pak is installed unless a game is in group 4.
static const char *const xpak_db[5] = { 0,
  "4W AY BE AS AR AC AM 32 ZO D4 CC DW DQ DZ MX F2 9F HV HY IS 3H CO KJ MD FL 2M JA Q9 QB QC CE P3 Q2 Y2 RV RE B5 RO RR SD SL DT NA EP RS O7 TF TQ 3T GB RC L2 T2 TK RW V8 VG XF IC PM",
  "SQ RU GX GD HT IJ",
  "DO ZS PD DP",
  "WB" };                         // Iggy's Reckin' Balls (the mupen64plus database runs it without the pak)
u32 xpak_mode;                    // host setting: 0 automatic, 1 installed, 2 removed
static u32 xpak_kind;             // group of the loaded game, 0 = not listed
static u32 detect_xpak(const u8 *rom) {
  char id[3] = { (char)rom[0x3B ^ 3], (char)rom[0x3C ^ 3], (char)rom[0x3D ^ 3] }, region = (char)rom[0x3E ^ 3];
  // Space Station Silicon Valley: the first US release (and the Japanese one built from it) crashes with the pak; later ones were fixed
  if (id[0] == 'N' && id[1] == 'S' && id[2] == 'V' && (region == 'J' || (region == 'E' && rom[0x3F ^ 3] == 0))) return 4;
  for (u32 t = 1; t <= 4; t++) {
    for (const char *p = xpak_db[t]; *p; ) {
      const char *e = p; while (*e && *e != ' ') e++;
      if ((id[0] == 'N' || id[0] == 'C') && p[0] == id[1] && p[1] == id[2]) return t;
      p = *e ? e + 1 : e;
    }
  }
  return 0;
}
static u32 xpak_size(void) { return xpak_mode == 1 ? RDRAM_MAX : xpak_mode == 2 ? RDRAM_MAX / 2 : xpak_kind == 4 ? RDRAM_MAX / 2 : RDRAM_MAX; }

int n64_load_rom(u8 *data, u32 size) {
  if (size < 0x1000) return 0;
  u32 magic = ((u32)data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
  size &= ~3u;
  if (magic == 0x80371240) { for (u32 i = 0; i < size; i += 4) { u8 a = data[i], b = data[i + 1]; data[i] = data[i + 3]; data[i + 1] = data[i + 2]; data[i + 2] = b; data[i + 3] = a; } }
  else if (magic == 0x37804012) { for (u32 i = 0; i < size; i += 4) { u8 a = data[i], b = data[i + 1]; data[i] = data[i + 2]; data[i + 1] = data[i + 3]; data[i + 2] = a; data[i + 3] = b; } }
  else if (magic == 0x40123780) { }
  else return 0;
  sys.rom = data; sys.rom_size = size;
  sys.save_type = detect_save_type(data);
  xpak_kind = detect_xpak(data);
  rdram_next = xpak_size();
  sys.pad_present[0] = 1;
  sys_reset();
  return 1;
}

// ---------------------------------------------------------------------------
// Exported host interface
// ---------------------------------------------------------------------------
extern u32 vi_out[]; extern u32 vi_out_w, vi_out_h, vi_blank;
extern u32 rdp_gpu_mode, rdp_hd_mode, gpu_exact, vi_status_clear, vi_status_set;
void vi_decode_only(void);
void rdp_flush(void);
void gpu_unwatch(int all);
extern u32 rdp_stat_prims, rdp_stat_flushes;

#ifdef __wasm__
void *memcpy(void *d, const void *s, size_t n) { return __builtin_memcpy(d, s, n); }
void *memmove(void *d, const void *s, size_t n) { return __builtin_memmove(d, s, n); }
void *memset(void *d, int c, size_t n) { return __builtin_memset(d, c, n); }
int memcmp(const void *a, const void *b, size_t n) {
  const u8 *x = a, *y = b;
  for (size_t i = 0; i < n; i++) if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
  return 0;
}
extern u8 __heap_base;
static u32 heap_top;
// The first 8 MB of the heap are a scratch area for the host (GPU copy-backs land there). It is reserved once and
// is not part of what n64_free_all() releases, so its address never changes while ROMs come and go.
#define HOST_SCRATCH_WORDS (1u << 21)
#define HEAP_START (((u32)(uintptr_t)&__heap_base + 0xFFFF) & ~0xFFFFu)
EXPORT(n64_alloc) void *n64_alloc(u32 size) {
  if (!size || size > 64u * 1024 * 1024) return 0;
  if (!heap_top) heap_top = HEAP_START + HOST_SCRATCH_WORDS * 4;
  u32 p = heap_top;
  if (heap_top > 0xFFFF0000u || size > 0xFFFF0000u - heap_top) return 0;
  u32 end = (heap_top + size + 0xFFFF) & ~0xFFFFu;
  u32 pages = __builtin_wasm_memory_size(0);
  if (end > pages * 65536u) { if (__builtin_wasm_memory_grow(0, (end - pages * 65536u + 65535) / 65536) == (size_t)-1) return 0; }
  heap_top = end;
  return (void *)(uintptr_t)p;
}
// The frontend owns one cartridge at a fixed address. Grow its capacity without
// moving it or overwriting the previous bytes if memory.grow fails.
EXPORT(n64_reserve_rom) void *n64_reserve_rom(u32 size) {
  if (!size || size > 64u * 1024 * 1024) return 0;
  u32 p = HEAP_START + HOST_SCRATCH_WORDS * 4;
  u32 end = (p + size + 0xFFFF) & ~0xFFFFu, pages = __builtin_wasm_memory_size(0);
  if (end > pages * 65536u && __builtin_wasm_memory_grow(0, (end - pages * 65536u) / 65536u) == (size_t)-1) return 0;
  if (heap_top < end) heap_top = end;
  return (void *)(uintptr_t)p;
}
EXPORT(n64_free_all) void n64_free_all(void) { heap_top = 0; }
EXPORT(n64_scratch) void *n64_scratch(void) {
  u32 end = HEAP_START + HOST_SCRATCH_WORDS * 4, pages = __builtin_wasm_memory_size(0);
  if (end > pages * 65536u && __builtin_wasm_memory_grow(0, (end - pages * 65536u + 65535) / 65536) == (size_t)-1) return 0;
  return (void *)(uintptr_t)HEAP_START;
}
// Save states are an image of all static memory ([0, __heap_base)); the ROM lives on the heap and is re-attached.
EXPORT(n64_state_size) u32 n64_state_size(void) { return (u32)(uintptr_t)&__heap_base; }
EXPORT(n64_set_rom) void n64_set_rom(u8 *data, u32 size) { sys.rom = data; sys.rom_size = size; }
#endif

// Table of addresses/sizes the host needs (all u32).
enum { HI_RDRAM, HI_VI_OUT, HI_VI_STATE, HI_AUDIO_BUF, HI_AUDIO_WR, HI_AUDIO_FREQ, HI_B_INFO, HI_B_PRIMS, HI_B_SPANS, HI_B_STATES, HI_B_TILES,
  HI_B_TMEM, HI_EEPROM, HI_SAVEMEM, HI_MEMPAK, HI_SYS_SAVE_TYPE, HI_SYS_SAVE_DIRTY, HI_BLENDER_LUT, HI_VI_REGS, HI_HIDDEN, HI_SHADOW16, HI_STATS, HI_TV, HI_RUMBLE, HI_COUNT };
static u32 host_info[HI_COUNT];
static u32 host_stats[16];
extern s32 b_prims[]; extern u32 b_spans[], b_states[], b_tiles[], b_tmem[]; extern u8 blender_lut[], rdp_hidden[]; extern u16 rdp_shadow16[];

EXPORT(n64_host_info) u32 *n64_host_info(void) {
  host_info[HI_RDRAM] = (u32)(uintptr_t)rdram;
  host_info[HI_VI_OUT] = (u32)(uintptr_t)vi_out;
  host_info[HI_VI_STATE] = (u32)(uintptr_t)&vi_state;
  host_info[HI_AUDIO_BUF] = (u32)(uintptr_t)audio_buf;
  host_info[HI_AUDIO_WR] = (u32)(uintptr_t)&audio_wr;
  host_info[HI_AUDIO_FREQ] = (u32)(uintptr_t)&audio_freq;
  host_info[HI_B_INFO] = (u32)(uintptr_t)&b_info;
  host_info[HI_B_PRIMS] = (u32)(uintptr_t)b_prims;
  host_info[HI_B_SPANS] = (u32)(uintptr_t)b_spans;
  host_info[HI_B_STATES] = (u32)(uintptr_t)b_states;
  host_info[HI_B_TILES] = (u32)(uintptr_t)b_tiles;
  host_info[HI_B_TMEM] = (u32)(uintptr_t)b_tmem;
  host_info[HI_EEPROM] = (u32)(uintptr_t)eeprom;
  host_info[HI_SAVEMEM] = (u32)(uintptr_t)savemem;
  host_info[HI_MEMPAK] = (u32)(uintptr_t)mempak;
  host_info[HI_SYS_SAVE_TYPE] = (u32)(uintptr_t)&sys.save_type;
  host_info[HI_SYS_SAVE_DIRTY] = (u32)(uintptr_t)&sys.save_dirty;
  host_info[HI_BLENDER_LUT] = (u32)(uintptr_t)blender_lut;
  host_info[HI_VI_REGS] = (u32)(uintptr_t)sys.vi;
  host_info[HI_HIDDEN] = (u32)(uintptr_t)rdp_hidden;
  host_info[HI_SHADOW16] = (u32)(uintptr_t)rdp_shadow16;
  host_info[HI_STATS] = (u32)(uintptr_t)host_stats;
  host_stats[8] = (u32)(uintptr_t)gpu_sync_cause;
  host_info[HI_TV] = (u32)(uintptr_t)&sys.tv;
  host_info[HI_RUMBLE] = (u32)(uintptr_t)sys.rumble;
  return host_info;
}

EXPORT(n64_load) int n64_load(u8 *data, u32 size) { return n64_load_rom(data, size); }
EXPORT(n64_reset) void n64_reset(void) { sys_reset(); }
// Returns 1 if the machine stopped part-way through the field because it needs GPU results copied back first:
// the host does that, calls n64_sync_done() and then n64_frame() again.
EXPORT(n64_frame) u32 n64_frame(void) {
  if (n64_run_frame()) return 1;
  rdp_flush();   // whatever the RDP has been sent by the end of the field becomes visible, as on hardware
  if (rdp_gpu_mode && !(sys.frames & 63)) gpu_unwatch(0);
  host_stats[0] = sys.frames; host_stats[1] = rdp_stat_prims; host_stats[2] = rdp_stat_flushes; host_stats[3] = (u32)rsp.icount;
  host_stats[4] = (u32)(cpu.idle_skipped >> 10); host_stats[5] = (u32)(cpu.cycles >> 10); host_stats[6] = cpu.pc;
  return 0;
}
// test tools: a few internals in one place
extern u32 rdp_debug_words[4];
EXPORT(n64_debug_state) u32 *n64_debug_state(void) {
  static u32 d[24];
  d[0] = sys.sp_status; d[1] = sys.dp_status; d[2] = sys.dp_start; d[3] = sys.dp_end; d[4] = sys.dp_current; d[5] = sys.mi_intr; d[6] = sys.mi_mask;
  d[7] = gpu_sync_request; d[8] = gpu_stale_n; d[9] = cpu_restart; d[10] = cpu_resume_one; d[11] = rdp_debug_words[0]; d[12] = rdp_debug_words[1];
  d[13] = (u32)cpu.cp0[12]; d[14] = (u32)cpu.cp0[13]; d[15] = cpu.pc; d[16] = (u32)cpu.cycles; d[17] = (u32)cpu.ev[EV_RSP]; d[18] = (u32)cpu.next_ev; d[19] = rdp_debug_words[2];
  return d;
}
EXPORT(n64_vi_render) void n64_vi_render(void) { vi_render(); }
EXPORT(n64_vi_decode) void n64_vi_decode(void) { vi_decode_only(); }
EXPORT(n64_input) void n64_input(u32 port, u32 buttons, s32 x, s32 y) { if (port < 4) { sys.buttons[port] = buttons; sys.stick_x[port] = x; sys.stick_y[port] = y; } }
EXPORT(n64_config) void n64_config(u32 key, u32 value) {
  switch (key) {
    case 0: rdp_flush(); rdp_gpu_mode = value; gpu_unwatch(1); break;   // the host follows up with n64_sync_full() passes when enabling
    case 1: cpu.cpi = value ? value : 4; break;
    case 2: if (value < 4) sys.pad_present[value] = 1; break;
    case 3: if (value < 4) sys.pad_present[value] = 0; break;
    case 4: if (value <= SAVE_FLASH) sys.save_type = value; break;
    case 5: sys.pak[0] = value; break;
    case 6: rdp_flush(); break;
    case 7: rdp_flush(); rdp_hd_mode = value; break;
    case 8: gpu_exact = value; break;
    case 9:                                               // picture: 0 = as the console shows it, 1 = raw pixels (no anti-alias blur, divot filter, interpolation or gamma noise)
      vi_status_clear = value ? 0x314 : 0; vi_status_set = value ? 0x300 : 0;
      break;                    // 0: never stop for GPU copy-backs (the CPU sees old framebuffer contents)     // conservative binning for the high-resolution GPU pass
    case 10: xpak_mode = value; if (sys.rom) rdram_next = xpak_size(); break;   // Expansion Pak; like the real thing it only changes with the power off (next reset)
    case 11: if ((value >> 8) < 4 && (value & 255) <= 2) sys.pak[value >> 8] = value & 255; break;
    default: break;
  }
}
// Expansion Pak status: bits 0-3 what the loaded game does with it (see xpak_db), bit 8 installed now, bit 9 installed after the next reset
EXPORT(n64_xpak) u32 n64_xpak(void) { return xpak_kind | (sys.rdram_size > RDRAM_MAX / 2 ? 0x100 : 0) | (rdram_next > RDRAM_MAX / 2 ? 0x200 : 0); }
EXPORT(n64_vi_dims) u32 n64_vi_dims(void) { return vi_out_w | (vi_out_h << 16) | (vi_blank << 31); }
