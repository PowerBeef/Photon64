// Physical bus, RCP peripherals (MI/VI/AI/PI/SI/RI), PIF + joybus, saves, scheduler.
#include "core.h"

SYS sys;
u8 rdram[RDRAM_MAX + 0x1000] __attribute__((aligned(4096)));
u8 spmem[0x2000] __attribute__((aligned(16)));
u8 eeprom[0x800];
u8 savemem[0x20000];
u8 mempak[4][0x8000];

// ---------------------------------------------------------------------------
// scheduler
// ---------------------------------------------------------------------------
void ev_set(int e, u64 t) { cpu.ev[e] = t; if (t < cpu.next_ev) cpu.next_ev = t; }
void ev_recalc(void) {
  u64 m = EV_NEVER;
  for (int i = 0; i < EV_MAX; i++) if (cpu.ev[i] < m) m = cpu.ev[i];
  cpu.next_ev = m;
}

// ---------------------------------------------------------------------------
// MI
// ---------------------------------------------------------------------------
static void mi_update(void) {
  if (sys.mi_intr & sys.mi_mask) cpu.cp0[C0_CAUSE] |= 0x400; else cpu.cp0[C0_CAUSE] &= ~0x400ull;
  cpu_check_irq();
}
void mi_raise(u32 bits) { sys.mi_intr |= bits; mi_update(); }
void mi_lower(u32 bits) { sys.mi_intr &= ~bits; mi_update(); }

// ---------------------------------------------------------------------------
// VI
// ---------------------------------------------------------------------------
static void vi_update(void) {
  while (cpu.cycles - sys.vi_field_start >= sys.vi_field_cycles) {
    sys.vi_field_start += sys.vi_field_cycles;
    sys.vi_field = (sys.vi[0] & 0x40) ? (sys.vi_field ^ 1) : 0;
  }
}
static u32 vi_halfline(void) {
  u32 vs = (sys.vi[6] & 0x3FF) + 1;
  u64 t = cpu.cycles - sys.vi_field_start;
  u32 hl = (u32)(t * vs / sys.vi_field_cycles);
  return hl >= vs ? vs - 1 : hl;
}
static void vi_schedule(void) {
  vi_update();
  u32 vs = (sys.vi[6] & 0x3FF) + 1;
  u32 vint = sys.vi[3] & 0x3FF;
  if (vint >= vs) vint = 0;
  u64 t = sys.vi_field_start + (u64)vint * sys.vi_field_cycles / vs;
  if (t <= cpu.cycles) t += sys.vi_field_cycles;
  ev_set(EV_VI, t);
}
static void vi_event(void) {
  vi_update();
  u32 vs = (sys.vi[6] & 0x3FF) + 1;
  if ((sys.vi[3] & 0x3FF) < vs) mi_raise(MI_VI);
  sys.frame_done = 1;
  sys.frames++;
  vi_schedule();
}

// ---------------------------------------------------------------------------
// AI
// ---------------------------------------------------------------------------
static u32 ai_freq(void) { u32 f = sys.vi_clock / (sys.ai_dacrate + 1); return f ? f : 1; }
static void ai_start(void) {
  u32 len = sys.ai_fifo[0].len;
  sys.ai_start = cpu.cycles;
  sys.ai_fifo[0].dur = (u64)(len / 4) * CPU_HZ / ai_freq();
  audio_push(sys.ai_fifo[0].addr, len, ai_freq());
  ev_set(EV_AI, cpu.cycles + sys.ai_fifo[0].dur);
}
static u32 ai_count(void) { return (sys.ai_status >> 30) == 3 ? 2 : (sys.ai_status >> 30) ? 1 : 0; }
static void ai_event(void) {
  if (sys.ai_status & 0x80000000u) {
    sys.ai_fifo[0] = sys.ai_fifo[1];
    sys.ai_status &= ~0x80000001u;
    mi_raise(MI_AI);
    ai_start();
  } else {
    sys.ai_status &= ~0x40000000u;
    sys.ai_fifo[0].len = 0;
  }
}
static u32 ai_remaining(void) {
  if (!(sys.ai_status & 0x40000000u) || !sys.ai_fifo[0].dur) return 0;
  u64 el = cpu.cycles - sys.ai_start;
  if (el >= sys.ai_fifo[0].dur) return 0;
  u32 len = sys.ai_fifo[0].len;
  return (u32)(len - (u64)len * el / sys.ai_fifo[0].dur) & ~7u;
}

// ---------------------------------------------------------------------------
// Saves: FlashRAM
// ---------------------------------------------------------------------------
enum { FL_IDLE, FL_ERASE, FL_WRITE, FL_READ, FL_STATUS };
static void flash_command(u32 cmd) {
  if (sys.save_type == SAVE_NONE) sys.save_type = SAVE_FLASH;
  if (sys.save_type != SAVE_FLASH) return;
  switch (cmd >> 24) {
    case 0x4B: sys.fl_erase_off = (cmd & 0xFFFF) * 128; sys.fl_mode = FL_ERASE; break;
    case 0x78: sys.fl_mode = FL_ERASE; sys.fl_status = 0x1111800800C2001Eull; break;
    case 0xA5: sys.fl_erase_off = (cmd & 0xFFFF) * 128; sys.fl_status = 0x1111800400C2001Eull; break;
    case 0xB4: sys.fl_mode = FL_WRITE; break;
    case 0xD2:
      if (sys.fl_mode == FL_ERASE) { memset(savemem + (sys.fl_erase_off & 0x1FF80), 0xFF, 128); sys.save_dirty = 1; }
      else if (sys.fl_mode == FL_WRITE) {
        for (u32 i = 0; i < 128; i++) savemem[(sys.fl_erase_off + i) & 0x1FFFF] = rdram[((sys.fl_write_off + i) & (RDRAM_MAX - 1)) ^ 3];
        sys.save_dirty = 1;
      }
      break;
    case 0xE1: sys.fl_mode = FL_STATUS; sys.fl_status = 0x1111800100C2001Eull; break;
    case 0xF0: sys.fl_mode = FL_READ; sys.fl_status = 0x11118004F0000000ull; break;
    default: break;
  }
}

// ---------------------------------------------------------------------------
// PI
// ---------------------------------------------------------------------------
static u32 pi_open_bus(u32 addr) { return ((addr & 0xFFFF) << 16) | (addr & 0xFFFF); }

static u8 cart_read8(u32 pa);
static u32 cart_read32(u32 pa) {
  if (pa >= 0x10000000 && pa < 0x1FC00000) {
    u32 off = pa - 0x10000000;
    if (off & 3) return ((u32)cart_read8(pa) << 24) | (cart_read8(pa + 1) << 16) | (cart_read8(pa + 2) << 8) | cart_read8(pa + 3);
    if (off + 4 <= sys.rom_size) return *(u32 *)(sys.rom + off);
    return pi_open_bus(pa);
  }
  if (pa >= 0x08000000 && pa < 0x10000000) {
    if (sys.save_type == SAVE_FLASH) return (u32)(sys.fl_status >> 32);
    if (sys.save_type == SAVE_SRAM) { u32 o = pa & 0x7FFC; return (savemem[o] << 24) | (savemem[o + 1] << 16) | (savemem[o + 2] << 8) | savemem[o + 3]; }
    return 0;
  }
  return pi_open_bus(pa);
}

static u8 cart_read8(u32 pa) {
  if (pa >= 0x10000000 && pa < 0x1FC00000) {
    u32 off = pa - 0x10000000;
    if (off < sys.rom_size) return sys.rom[off ^ 3];
    return (u8)(pi_open_bus(pa & ~1u) >> ((pa & 1) ? 0 : 8));
  }
  if (pa >= 0x08000000 && pa < 0x10000000) {
    if (sys.save_type == SAVE_FLASH) {
      if (sys.fl_mode == FL_STATUS) return (u8)(sys.fl_status >> (8 * (7 - (pa & 7))));
      return savemem[pa & 0x1FFFF];
    }
    if (sys.save_type == SAVE_NONE) sys.save_type = SAVE_SRAM;
    if (sys.save_type == SAVE_SRAM) return savemem[pa & 0x7FFF];
  }
  return 0;
}

static u32 pi_dma_cycles(u32 len, u32 cart) {
  u32 d2 = (cart >= 0x08000000 && cart < 0x10000000) || (cart >= 0x05000000 && cart < 0x06000000);
  u32 pwd = sys.pi[d2 ? 10 : 6] & 0xFF, rls = sys.pi[d2 ? 12 : 8] & 3, lat = sys.pi[d2 ? 9 : 5] & 0xFF, pgs = sys.pi[d2 ? 11 : 7] & 0xF;
  u32 pages = (len >> (pgs + 2)) + 1;
  u64 c = (u64)(14 + lat + 1 + 5) * pages + (u64)(pwd + 1 + rls + 1) * (len / 2);
  c = c * 3 / 2;
  return c > 0x10000000 ? 0x10000000 : (u32)c + 50;
}

// cart -> RDRAM, including the hardware's block/alignment quirks for unaligned transfers
static void pi_dma_write(void) {
  s32 length = (s32)(sys.pi[3] & 0xFFFFFF) + 1;
  u32 dram = sys.pi[0] & 0xFFFFFE, cart = sys.pi[1] & ~1u;
  u32 total = (u32)(length + 1) & ~1u;
  u32 wrlen = 0x7F;
  if (!(dram & 7)) {
    // a short single-block transfer writes exactly `length` bytes; anything else is rounded up to 16-bit units
    u32 n = (length < 127 && length <= (s32)(0x800 - (dram & 0x7FF))) ? (u32)length : total;
    if (dram + n > sys.rdram_size) n = dram < sys.rdram_size ? sys.rdram_size - dram : 0;
    if (cart >= 0x10000000 && cart < 0x1FC00000 && !((cart | n) & 3) && cart - 0x10000000 + n <= sys.rom_size) memcpy(rdram + dram, sys.rom + (cart - 0x10000000), n);
    else for (u32 i = 0; i < n; i++) rdram[(dram + i) ^ 3] = cart_read8(cart + i);
    gpu_mark_dirty(dram, n);
    dram = (dram + (u32)length + 7) & ~7u;
    cart += total;
  } else {
    u8 mem[128];
    s32 max_block = 128; int first = 1;
    while (length > 0) {
      s32 misalign = dram & 7;
      s32 dist_end = 0x800 - (dram & 0x7FF);
      s32 block_len = max_block - misalign < dist_end ? max_block - misalign : dist_end;
      s32 cur_len = length < block_len ? length : block_len;
      for (s32 i = 0; i < cur_len; i += 2) { mem[i] = cart_read8(cart); mem[i + 1] = cart_read8(cart + 1); cart += 2; length -= 2; }
      s32 n = cur_len - misalign;
      if (!(first && cur_len < 127 - misalign)) n = (n + 1) & ~1;
      if (n > 0) gpu_mark_dirty(dram, (u32)n);
      for (s32 i = 0; i < n; i++) { if (dram < sys.rdram_size) rdram[dram ^ 3] = mem[i]; dram++; }
      dram = (dram + 7) & ~7u;
      wrlen = cur_len <= 8 ? 127 - misalign : 127;
      first = 0;
      max_block = dist_end < 8 ? 128 - misalign : 128;
    }
  }
  sys.pi[0] = dram & 0xFFFFFF;
  sys.pi[1] = cart;
  sys.pi[3] = wrlen; sys.pi[2] = 0x7F;
  sys.pi[4] |= 1;
  ev_set(EV_PI, cpu.cycles + pi_dma_cycles(total, cart));
}

// RDRAM -> cart
static void pi_dma_read(void) {
  u32 len = (sys.pi[2] & 0xFFFFFF) + 1;
  u32 dram = sys.pi[0] & 0xFFFFFE, cart = sys.pi[1] & ~1u;
  if (len & 1) len++;
  if (cart >= 0x08000000 && cart < 0x10000000) {
    if (sys.save_type == SAVE_NONE) sys.save_type = SAVE_SRAM;
    if (sys.save_type == SAVE_FLASH) {
      if (sys.fl_mode == FL_WRITE) sys.fl_write_off = dram;
    } else if (sys.save_type == SAVE_SRAM) {
      for (u32 i = 0; i < len; i++) savemem[(cart + i) & 0x7FFF] = rdram[((dram + i) & (RDRAM_MAX - 1)) ^ 3];
      sys.save_dirty = 1;
    }
  }
  sys.pi[0] = (dram + len + 7) & ~7u;
  sys.pi[1] = (cart + len + 1) & ~1u;
  sys.pi[2] = 0x7F; sys.pi[3] = 0x7F;
  sys.pi[4] |= 1;
  ev_set(EV_PI, cpu.cycles + pi_dma_cycles(len, cart));
}

// ---------------------------------------------------------------------------
// PIF / joybus
// ---------------------------------------------------------------------------
static u8 pak_crc(const u8 *data) {
  u8 crc = 0;
  for (int i = 0; i <= 32; i++) {
    for (int mask = 0x80; mask; mask >>= 1) {
      u8 tap = (crc & 0x80) ? 0x85 : 0;
      crc <<= 1;
      if (i < 32 && (data[i] & mask)) crc |= 1;
      crc ^= tap;
    }
  }
  return crc;
}

static void joybus(int ch, u32 tx, u32 rx, u8 *cmd, u8 *res, u8 *rxp) {
  if (ch < 4) {
    if (!sys.pad_present[ch]) { *rxp |= 0x80; return; }
    switch (cmd[0]) {
      case 0x00: case 0xFF:
        if (rx > 0) res[0] = 0x05;
        if (rx > 1) res[1] = 0x00;
        if (rx > 2) res[2] = sys.pak[ch] ? 0x01 : 0x02;
        break;
      case 0x01: {
        u32 b = sys.buttons[ch];
        if (rx > 0) res[0] = b >> 8;
        if (rx > 1) res[1] = b & 0xFF;
        if (rx > 2) res[2] = (u8)(s8)sys.stick_x[ch];
        if (rx > 3) res[3] = (u8)(s8)sys.stick_y[ch];
        break;
      }
      case 0x02: {
        u32 addr = ((cmd[1] << 8) | cmd[2]) & 0xFFE0;
        if (sys.pak[ch] == 1 && addr < 0x8000) memcpy(res, mempak[ch] + addr, 32);
        else memset(res, (sys.pak[ch] == 2 && addr == 0x8000) ? 0x80 : 0, 32);
        res[32] = pak_crc(res);
        if (!sys.pak[ch]) res[32] ^= 0xFF;
        break;
      }
      case 0x03: {
        u32 addr = ((cmd[1] << 8) | cmd[2]) & 0xFFE0;
        if (sys.pak[ch] == 1 && addr < 0x8000) { memcpy(mempak[ch] + addr, cmd + 3, 32); sys.save_dirty = 1; }
        else if (sys.pak[ch] == 2 && addr == 0xC000) sys.rumble[ch] = cmd[3] & 1;
        res[0] = pak_crc(cmd + 3);
        if (!sys.pak[ch]) res[0] ^= 0xFF;
        break;
      }
      default: *rxp |= 0x80; break;
    }
    return;
  }
  if (ch == 4 && (sys.save_type == SAVE_EEP4K || sys.save_type == SAVE_EEP16K)) {
    u32 blocks = sys.save_type == SAVE_EEP16K ? 256 : 64;
    switch (cmd[0]) {
      case 0x00: case 0xFF:
        if (rx > 0) res[0] = 0x00;
        if (rx > 1) res[1] = sys.save_type == SAVE_EEP16K ? 0xC0 : 0x80;
        if (rx > 2) res[2] = 0x00;
        break;
      case 0x04: memcpy(res, eeprom + (cmd[1] % blocks) * 8, 8); break;
      case 0x05: memcpy(eeprom + (cmd[1] % blocks) * 8, cmd + 1 + 1, 8); res[0] = 0; sys.save_dirty = 1; break;
      default: *rxp |= 0x80; break;
    }
    return;
  }
  *rxp |= 0x80;
  (void)tx;
}

static void pif_process(void) {
  u8 *r = sys.pif;
  int ch = 0, i = 0;
  while (i < 63 && ch < 6) {
    u32 tx = r[i];
    if (tx == 0xFE) break;
    if (tx == 0xFF) { i++; continue; }
    if (tx == 0x00 || tx == 0xFD) { ch++; i++; continue; }
    tx &= 0x3F;
    u32 rx = r[i + 1] & 0x3F;
    if (i + 2 + tx + rx > 64) break;
    r[i + 1] &= 0x3F;
    joybus(ch, tx, rx, &r[i + 2], &r[i + 2 + tx], &r[i + 1]);
    i += 2 + tx + rx;
    ch++;
  }
}

static void pif_control(void) {
  u8 c = sys.pif[63];
  if (c & 0x02) {  // CIC challenge/response: not emulated, acknowledge
    sys.pif[63] &= ~0x02;
  }
  if (c & 0x08) sys.pif[63] &= ~0x08;
  if (c & 0x20) sys.pif[63] |= 0x80;
  if (c & 0x40) sys.pif[63] &= ~0x40;
}

static void si_done(u32 delay) { sys.si_status |= 1; ev_set(EV_SI, cpu.cycles + delay); }

// ---------------------------------------------------------------------------
// bus
// ---------------------------------------------------------------------------
#ifndef __wasm__
static void isv_flush(u32 len) { fwrite(sys.isv + 0x20, 1, len > 0xFFE0 ? 0xFFE0 : len, stdout); fflush(stdout); }
#else
static void isv_flush(u32 len) { host_log((const char *)sys.isv + 0x20, len, 1); }
#endif

u32 bus_read32(u32 pa) {
  if (pa < 0x03F00000) {
    if (pa >= sys.rdram_size) return 0;
    if (unlikely(gpu_stale_pg[pa >> 12]) && gpu_range_stale(pa & ~3u, 4)) { gpu_request_sync(); cpu_restart = 1; gpu_sync_cause[0] = 1; gpu_sync_cause[1] = pa; gpu_sync_cause[2] = cpu.ipc; }
    return RDRAM32(pa);
  }
  if (pa < 0x04000000) return sys.rdreg[(pa >> 2) % 10];
  switch (pa >> 20) {
    case 0x040:
      if (pa < 0x04040000) return *(u32 *)(spmem + (pa & 0x1FFC));
      if (pa < 0x04080000) return sp_reg_read((pa >> 2) & 7);
      if (pa < 0x040C0000) return sp_reg_read(8 + ((pa >> 2) & 1));
      return 0;
    case 0x041: return dp_reg_read((pa >> 2) & 7);
    case 0x042: return 0;
    case 0x043:
      switch ((pa >> 2) & 3) {
        case 0: return sys.mi_mode;
        case 1: return 0x02020102;
        case 2: return sys.mi_intr;
        default: return sys.mi_mask;
      }
    case 0x044: {
      u32 r = (pa >> 2) & 15;
      if (r == 4) { vi_update(); u32 hl = vi_halfline(); return (hl & ~1u) | sys.vi_field; }
      return r < 14 ? sys.vi[r] : 0;
    }
    case 0x045:
      if (((pa >> 2) & 7) == 3) return sys.ai_status | 0x01100000 | ((sys.ai_control & 1) << 25);
      return ai_remaining();
    case 0x046: {
      u32 r = (pa >> 2) & 15;
      return r < 13 ? sys.pi[r] : 0;
    }
    case 0x047: return sys.ri[(pa >> 2) & 7];
    case 0x048:
      switch ((pa >> 2) & 7) {
        case 0: return sys.si_dram;
        case 6: return sys.si_status;
        default: return 0;
      }
    default: break;
  }
  if (pa >= 0x13FF0000 && pa < 0x14000000) {
    if (pa == 0x13FF0000) return 0x49533634;   // "IS64"
    return *(u32 *)(sys.isv + (pa & 0xFFFC));
  }
  if (pa >= 0x05000000 && pa < 0x1FC00000) { sys.pi[1] = (pa & ~1u) + 4; return cart_read32(pa & ~1u); }
  if (pa >= 0x1FC007C0 && pa < 0x1FC00800) { u8 *p = sys.pif + (pa & 0x3C); return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
  return 0;
}

void bus_write32(u32 pa, u32 v, u32 mask) {
  if (pa < 0x03F00000) {
    if (pa < sys.rdram_size) {
      // a store that covers only part of a halfword needs the rest of it to be current
      if (unlikely(gpu_stale_pg[pa >> 12]) && (((mask >> 16) + 1) & 0xFFFE || ((mask & 0xFFFF) + 1) & 0xFFFE) && gpu_range_stale(pa & ~3u, 4)) {
        gpu_request_sync(); cpu_restart = 1; gpu_sync_cause[0] = 2; gpu_sync_cause[1] = pa; gpu_sync_cause[2] = cpu.ipc; return;
      }
      u32 *p = &RDRAM32(pa); *p = (*p & ~mask) | (v & mask);
      if (unlikely(gpu_watch[pa >> 12])) { u32 k = (pa & ~3u) >> 1; if (mask >> 16) rdp_hidden[k] |= 0x80; if (mask & 0xFFFF) rdp_hidden[k + 1] |= 0x80; }
    }
    return;
  }
  if (pa < 0x04000000) { sys.rdreg[(pa >> 2) % 10] = v; return; }
  switch (pa >> 20) {
    case 0x040:
      if (pa < 0x04040000) { rsp_sync(); if (unlikely(cpu_restart)) return; u32 *p = (u32 *)(spmem + (pa & 0x1FFC)); *p = (*p & ~mask) | (v & mask); }
      else if (pa < 0x04080000) sp_reg_write((pa >> 2) & 7, v);
      else if (pa < 0x040C0000) sp_reg_write(8 + ((pa >> 2) & 1), v);
      return;
    case 0x041: dp_reg_write((pa >> 2) & 7, v); return;
    case 0x043:
      switch ((pa >> 2) & 3) {
        case 0:
          sys.mi_mode = (sys.mi_mode & ~0x7Fu) | (v & 0x7F);
          if (v & 0x80) sys.mi_mode &= ~0x80u;
          if (v & 0x100) sys.mi_mode |= 0x80;
          if (v & 0x200) sys.mi_mode &= ~0x100u;
          if (v & 0x400) sys.mi_mode |= 0x100;
          if (v & 0x800) mi_lower(MI_DP);
          if (v & 0x1000) sys.mi_mode &= ~0x200u;
          if (v & 0x2000) sys.mi_mode |= 0x200;
          break;
        case 3:
          for (int i = 0; i < 6; i++) {
            if (v & (1u << (2 * i))) sys.mi_mask &= ~(1u << i);
            if (v & (2u << (2 * i))) sys.mi_mask |= (1u << i);
          }
          mi_update();
          break;
        default: break;
      }
      return;
    case 0x044: {
      u32 r = (pa >> 2) & 15;
      if (r == 4) { mi_lower(MI_VI); return; }
      if (r < 14) {
        u32 old = sys.vi[r];
        sys.vi[r] = v;
        if ((r == 3 || r == 6) && old != v) vi_schedule();
      }
      return;
    }
    case 0x045:
      switch ((pa >> 2) & 7) {
        case 0: if (ai_count() < 2) sys.ai_dram = v & 0xFFFFF8; break;
        case 1: {
          u32 len = v & 0x3FFF8;
          u32 n = ai_count();
          if (n < 2) {
            sys.ai_fifo[n].addr = sys.ai_dram; sys.ai_fifo[n].len = len;
            if (n == 0) { sys.ai_status |= 0x40000000u; mi_raise(MI_AI); ai_start(); }
            else sys.ai_status |= 0x80000001u;
          }
          break;
        }
        case 2: sys.ai_control = v & 1; break;
        case 3: mi_lower(MI_AI); break;
        case 4: sys.ai_dacrate = v & 0x3FFF; break;
        case 5: sys.ai_bitrate = v & 0xF; break;
        default: break;
      }
      return;
    case 0x046: {
      u32 r = (pa >> 2) & 15;
      switch (r) {
        case 0: sys.pi[0] = v & 0xFFFFFE; break;
        case 1: sys.pi[1] = v & ~1u; break;
        case 2: sys.pi[2] = v & 0xFFFFFF; pi_dma_read(); break;
        case 3: sys.pi[3] = v & 0xFFFFFF; pi_dma_write(); break;
        case 4:
          if (v & 1) { sys.pi[4] &= ~1u; cpu.ev[EV_PI] = EV_NEVER; }
          if (v & 2) { sys.pi[4] &= ~8u; mi_lower(MI_PI); }
          break;
        default: if (r < 13) sys.pi[r] = v & 0xFF; break;
      }
      return;
    }
    case 0x047: sys.ri[(pa >> 2) & 7] = v; return;
    case 0x048:
      switch ((pa >> 2) & 7) {
        case 0: sys.si_dram = v & 0xFFFFFF; break;
        case 1:  // PIF -> RDRAM
          pif_process();
          for (u32 i = 0; i < 64; i++) rdram[((sys.si_dram + i) & (RDRAM_MAX - 1)) ^ 3] = sys.pif[i];
          gpu_mark_dirty(sys.si_dram & (RDRAM_MAX - 1), 64);
          si_done(6000);
          break;
        case 4:  // RDRAM -> PIF
          for (u32 i = 0; i < 64; i++) sys.pif[i] = rdram[((sys.si_dram + i) & (RDRAM_MAX - 1)) ^ 3];
          pif_control();
          si_done(6000);
          break;
        case 6: sys.si_status &= ~0x1000u; mi_lower(MI_SI); break;
        default: break;
      }
      return;
    default: break;
  }
  if (pa >= 0x13FF0000 && pa < 0x14000000) {
    u32 *p = (u32 *)(sys.isv + (pa & 0xFFFC));
    if ((pa & 0xFFFC) == 0x14) { isv_flush(v); return; }
    // store big-endian so the text buffer is a plain byte string
    u32 nv = (__builtin_bswap32(*p) & ~mask) | (v & mask);
    *p = __builtin_bswap32(nv);
    return;
  }
  if (pa >= 0x08000000 && pa < 0x10000000) {
    if ((pa & 0x1FFFF) == 0x10000) flash_command(v);
    return;
  }
  if (pa >= 0x1FC007C0 && pa < 0x1FC00800) {
    u8 *p = sys.pif + (pa & 0x3C);
    u32 old = (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
    u32 nv = (old & ~mask) | (v & mask);
    p[0] = nv >> 24; p[1] = nv >> 16; p[2] = nv >> 8; p[3] = nv;
    pif_control();
    si_done(4000);
    return;
  }
}

// ---------------------------------------------------------------------------
// reset / boot
// ---------------------------------------------------------------------------
static u32 crc32_calc(const u8 *d, u32 n) {
  u32 c = 0xFFFFFFFF;
  for (u32 i = 0; i < n; i++) {
    c ^= d[i];
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
  }
  return ~c;
}

static u32 frame_paused;      // n64_run_frame() stopped part-way through a field
u32 rdram_next;                // amount of RDRAM the machine has from the next reset on (0: leave as it is)
void sys_reset(void) {
  u8 *rom = sys.rom; u32 rom_size = sys.rom_size, save_type = sys.save_type, rdsz = sys.rdram_size;
  u32 pres[4], pak[4];
  for (int i = 0; i < 4; i++) { pres[i] = sys.pad_present[i]; pak[i] = sys.pak[i]; }
  memset(&sys, 0, sizeof sys);
  sys.rom = rom; sys.rom_size = rom_size; sys.save_type = save_type;
  sys.rdram_size = rdram_next ? rdram_next : rdsz ? rdsz : RDRAM_MAX;
  for (int i = 0; i < 4; i++) { sys.pad_present[i] = pres[i]; sys.pak[i] = pak[i]; }
  memset(rdram, 0, RDRAM_MAX);
  memset(spmem, 0, sizeof spmem);
  memset(gpu_watch, 0, sizeof gpu_watch); gpu_watch_count = 0;
  memset(gpu_stale_pg, 0, sizeof gpu_stale_pg); gpu_stale_n = 0; gpu_sync_request = 0; cpu_restart = cpu_resume_one = 0; frame_paused = 0;
  cpu_reset();
  rsp_reset();
  rdp_reset();

  // region from header country code
  u8 country = rom_size > 0x40 ? rom[0x3E ^ 3] : 'E';
  sys.tv = 1;
  switch (country) { case 'D': case 'F': case 'I': case 'P': case 'S': case 'U': case 'X': case 'Y': case 'H': case 'L': sys.tv = 0; break; default: break; }
  sys.vi_clock = sys.tv == 0 ? 49656530 : 48681812;
  sys.vi_field_cycles = (u32)(CPU_HZ / (sys.tv == 0 ? 50 : 60));
  sys.vi[6] = sys.tv == 0 ? 0x271 : 0x20D;
  sys.vi[3] = 0x3FF;

  // CIC detection from IPL3 checksum (bytes are word-swapped in host memory)
  u32 seed = 0x3F; sys.cic = 6102;
  if (rom_size >= 0x1000) {
    static u8 tmp[0xFC0];
    for (u32 i = 0; i < 0xFC0; i++) tmp[i] = rom[(0x40 + i) ^ 3];
    switch (crc32_calc(tmp, 0xFC0)) {
      case 0x6170A4A1: sys.cic = 6101; seed = 0x3F; break;
      case 0x009E9EA3: sys.cic = 7102; seed = 0x3F; break;
      case 0x90BB6CB5: sys.cic = 6102; seed = 0x3F; break;
      case 0x0B050EE0: sys.cic = 6103; seed = 0x78; break;
      case 0x98BC2C86: sys.cic = 6105; seed = 0x91; break;
      case 0xACC8580A: sys.cic = 6106; seed = 0x85; break;
      default: break;
    }
  }
  // state left by IPL1/IPL2
  memcpy(spmem, rom, rom_size < 0x1000 ? rom_size : 0x1000);
  static const u32 imem_boot[8] = { 0x3C0DBFC0, 0x8DA807FC, 0x25AD07C0, 0x31080080, 0x5500FFFC, 0x3C0DBFC0, 0x8DA80024, 0x3C0BB000 };
  memcpy(spmem + 0x1000, imem_boot, sizeof imem_boot);
  cpu.r[11] = (s64)(s32)0xA4000040;
  cpu.r[19] = 0; cpu.r[20] = sys.tv; cpu.r[21] = 0; cpu.r[22] = seed; cpu.r[23] = 0;
  cpu.r[29] = (s64)(s32)0xA4001FF0;
  cpu.r[31] = (s64)(s32)0xA4001550;
  cpu.pc = 0xA4000040; cpu.npc = cpu.pc + 4;
  sys.pif[0x24] = 0x00; sys.pif[0x25] = sys.cic == 6105 ? 0x02 : 0x00; sys.pif[0x26] = seed; sys.pif[0x27] = 0x3F;
  sys.ri[0] = 0x0E; sys.ri[1] = 0x40; sys.ri[3] = 0x14; sys.ri[4] = 0x00063634;
  sys.mi_mode = 0;
  sys.sp_status = 1;
  sys.pi[5] = 0x40; sys.pi[6] = 0x12; sys.pi[7] = 0x07; sys.pi[8] = 0x03;
  // osMemSize
  RDRAM32(sys.cic == 6105 ? 0x3F0 : 0x318) = sys.rdram_size;
  cpu.cp0[C0_COMPARE] = 0;
  ev_set(EV_COMPARE, 0x200000000ull);
  vi_schedule();
}

// Runs to the end of the field. Returns 1 if it had to stop for the host to copy GPU results back first
// (call n64_sync_done() and then this function again to continue; nothing is lost or repeated).
void rdp_resume(void);
int n64_run_frame(void) {
  if (!frame_paused) sys.frame_done = 0;
  frame_paused = 0;
  rdp_resume();
  while (!sys.frame_done) {
    if (gpu_sync_request) { frame_paused = 1; return 1; }
    ev_recalc();
    cpu.irq_poll = 0;
    extern void cpu_poll_irq(void);
    // (an instruction that was stopped half-way runs to completion before interrupts are looked at again, exactly
    // as it would have without the stop)
    if (cpu_resume_one) { cpu_resume_one = 0; cpu.next_ev = cpu.cycles + 1; } else cpu_poll_irq();
    cpu_run();
    if (gpu_sync_request) { frame_paused = 1; return 1; }
    u64 now = cpu.cycles;
    for (int e = 0; e < EV_MAX; e++) {
      if (cpu.ev[e] > now) continue;
      cpu.ev[e] = EV_NEVER;
      switch (e) {
        case EV_VI: vi_event(); break;
        case EV_COMPARE: {
          cpu.cp0[C0_CAUSE] |= 0x8000;
          extern void cpu_compare_resched(void);
          cpu_compare_resched();
          break;
        }
        case EV_AI: ai_event(); break;
        case EV_PI: sys.pi[4] = (sys.pi[4] & ~1u) | 8; mi_raise(MI_PI); break;
        case EV_SI: sys.si_status = (sys.si_status & ~1u) | 0x1000; mi_raise(MI_SI); break;
        case EV_RSP: if (rsp_sync()) ev_set(EV_RSP, now); cpu_restart = 0; break;   // (owed instructions run first thing after the copy-back)
        case EV_DP: mi_raise(MI_DP); break;
        default: break;
      }
    }
  }
  return 0;
}
