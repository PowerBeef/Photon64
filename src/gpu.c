// Support code for the WebGPU renderer: tile binning and CPU<->GPU framebuffer coherency.
//
// GPU "framebuffer memory" is a u32 per RDRAM halfword: low 16 bits = data, bits 16-17 = hidden (9th) bits.
// CPU RDRAM and the GPU copy are kept eventually-consistent with a three-way merge against gpu_shadow16
// (the CPU-side value as of the last sync): CPU writes are pushed before the next GPU access, GPU results
// arrive back asynchronously and are applied only where the CPU has not written in the meantime.
#include "core.h"

#define BIN_SHIFT 3
#define BIN_MAX_TX 128
#define BIN_MAX_ROWS 200000
#define BIN_MAX_ENTRIES (1u << 20)
typedef struct { u16 prim, row, x0, x1; } BinRow;
static BinRow bin_rows[BIN_MAX_ROWS];
static u32 bin_nrows, bin_entries;
u32 b_bins[2 * BIN_MAX_TX * BIN_MAX_TX + BIN_MAX_ENTRIES] __attribute__((aligned(16)));
u32 b_bins_words;

u16 gpu_shadow16[RDRAM_MAX / 2];
u8 gpu_watch[RDRAM_MAX >> 12];
u32 gpu_watch_count;
static u32 gpu_watch_seen[RDRAM_MAX >> 12];   // frame number of the last GPU batch that drew into the page
void cpu_watch_page(u32 pg, int on);
void tlb_remap_all(void);

// ---- stale tracking (see core.h) ----
u8 gpu_stale_pg[RDRAM_MAX >> 12];
u32 gpu_stale_n, gpu_sync_request, gpu_exact = 1, gpu_hint;
static struct { u32 a, b; } stale_rng[16];    // byte ranges [a, b) drawn by the GPU since the last copy-back

int gpu_range_stale(u32 pa, u32 len) {
  u32 end = pa + len;
  for (u32 i = 0; i < gpu_stale_n; i++) if (pa < stale_rng[i].b && end > stale_rng[i].a) return 1;
  return 0;
}
u32 gpu_sync_cause[4];
void gpu_request_sync(void) { gpu_sync_request = 1; cpu.next_ev = 0; }

// The GPU has just been sent primitives that draw into halfwords [idx16, idx16 + count).
static void gpu_mark_stale(u32 idx16, u32 count) {
  if (!gpu_exact || !count) return;
  u32 a = idx16 << 1, b = a + (count << 1);
  if (a >= RDRAM_MAX) return;
  if (b > RDRAM_MAX) b = RDRAM_MAX;
  u32 i;
  for (i = 0; i < gpu_stale_n; i++) if (a >= stale_rng[i].a && b <= stale_rng[i].b) return;      // already covered
  for (i = 0; i < gpu_stale_n; i++) if (a <= stale_rng[i].b && b >= stale_rng[i].a) break;       // grow a neighbour
  if (i == gpu_stale_n) {
    if (gpu_stale_n == 16) { i = 0; }                                                             // table full: widen the first entry
    else { gpu_stale_n++; stale_rng[i].a = a; stale_rng[i].b = b; }
  }
  if (a < stale_rng[i].a) stale_rng[i].a = a;
  if (b > stale_rng[i].b) stale_rng[i].b = b;
  int changed = 0;
  for (u32 pg = stale_rng[i].a >> 12; pg <= (stale_rng[i].b - 1) >> 12; pg++) if (!gpu_stale_pg[pg]) { gpu_stale_pg[pg] = 1; cpu_stale_page(pg, 1); changed = 1; }
  if (changed) tlb_remap_all();
}

// The host has copied everything back (or GPU and CPU memory were made identical): nothing is stale any more.
static void gpu_stale_clear(void) {
  int changed = 0;
  for (u32 pg = 0; pg < (RDRAM_MAX >> 12); pg++) if (gpu_stale_pg[pg]) { gpu_stale_pg[pg] = 0; cpu_stale_page(pg, 0); changed = 1; }
  gpu_stale_n = 0;
  if (changed) tlb_remap_all();
}
EXPORT(n64_sync_done) void n64_sync_done(void) { gpu_stale_clear(); gpu_sync_request = 0; }

void gpu_mark_dirty(u32 pa, u32 len) {
  if (!gpu_watch_count || !len) return;
  u32 end = pa + len; if (end > RDRAM_MAX) end = RDRAM_MAX;
  for (u32 k = pa >> 1; k < (end + 1) >> 1; k++) if (gpu_watch[k >> 11]) rdp_hidden[k] |= 0x80;
}

// The GPU is about to render into halfwords [idx16, idx16 + count): from now on CPU writes there must be tracked.
static void gpu_watch_region(u32 idx16, u32 count) {
  u32 a = idx16 >> 11, b = (idx16 + count + 2047) >> 11, added = 0;
  if (b > (RDRAM_MAX >> 12)) b = RDRAM_MAX >> 12;
  for (u32 pg = a; pg < b; pg++) {
    gpu_watch_seen[pg] = sys.frames;
    if (!gpu_watch[pg]) { gpu_watch[pg] = 1; gpu_watch_count++; cpu_watch_page(pg, 1); added = 1; }
  }
  if (added) tlb_remap_all();
}

// Pages nobody has rendered to for a while go back to fast stores (all = leave GPU mode).
void gpu_unwatch(int all) {
  u32 removed = 0;
  if (!gpu_watch_count) return;
  for (u32 pg = 0; pg < (RDRAM_MAX >> 12); pg++) {
    // (a stale page stays watched: the copy-back still has to tell CPU-written halfwords from GPU-written ones)
    if (gpu_watch[pg] && (all || (sys.frames - gpu_watch_seen[pg] > 240 && !gpu_stale_pg[pg]))) { gpu_watch[pg] = 0; gpu_watch_count--; cpu_watch_page(pg, 0); removed = 1; }
  }
  if (all) gpu_stale_clear();
  if (removed) tlb_remap_all();
}
#define SYNC_MAX (1024 * 1024)
u32 sync_stage[SYNC_MAX] __attribute__((aligned(16)));   // upload staging (value | hidden << 16 | valid << 31)
u32 sync_runs[2 * 64];                                    // (start, count) pairs of changed halfword runs
u32 sync_nruns, sync_changed;

static void bin_reset(void) { bin_nrows = 0; bin_entries = 0; }

static int bin_would_overflow(u32 lines, u32 fb_width) {
  u32 rows = (lines >> BIN_SHIFT) + 2, tx = (fb_width >> BIN_SHIFT) + 1;
  return bin_nrows + rows > BIN_MAX_ROWS || bin_entries + rows * tx > BIN_MAX_ENTRIES;
}

// spans: the primitive's span records for lines [ylo, yhi]
static void bin_primitive(u32 prim, s32 ylo, s32 yhi, const u32 *spans, u32 fb_width) {
  if (!fb_width) return;
  s32 tr0 = ylo >> BIN_SHIFT, tr1 = yhi >> BIN_SHIFT;
  for (s32 tr = tr0; tr <= tr1 && tr < BIN_MAX_TX; tr++) {
    s32 y0 = tr << BIN_SHIFT, y1 = y0 + (1 << BIN_SHIFT) - 1;
    if (y0 < ylo) y0 = ylo;
    if (y1 > yhi) y1 = yhi;
    s32 x0 = 0x7FFFFFFF, x1 = -1;
    for (s32 y = y0; y <= y1; y++) {
      const u32 *sp = spans + (y - ylo) * 8;
      s32 sx, ex;
      if (rdp_hd_mode) {   // conservative range (see span_setup)
        if (!(sp[7] & 2)) continue;
        sx = (sp[7] >> 2) & 0x1FFF; ex = (sp[7] >> 15) & 0x1FFF;
      } else {
        if (!(sp[7] & 1)) continue;
        sx = sp[4] & 0xFFFF; ex = sp[4] >> 16;
      }
      if (sx < x0) x0 = sx;
      if (ex > x1) x1 = ex;
    }
    if (x1 >= (s32)fb_width) x1 = fb_width - 1;
    if (x1 < x0) continue;
    BinRow *r = &bin_rows[bin_nrows++];
    r->prim = prim; r->row = tr; r->x0 = x0 >> BIN_SHIFT; r->x1 = x1 >> BIN_SHIFT;
    bin_entries += r->x1 - r->x0 + 1;
  }
}

// Build the per-tile primitive lists (CSR) for the batch. Returns number of u32 words to upload.
static u32 bin_build(u32 fb_width, u32 fb_height, u32 *tiles_x, u32 *ntiles_out) {
  u32 tx = (fb_width + 7) >> BIN_SHIFT, ty = (fb_height + 7) >> BIN_SHIFT;
  if (tx > BIN_MAX_TX) tx = BIN_MAX_TX;
  if (ty > BIN_MAX_TX) ty = BIN_MAX_TX;
  u32 nt = tx * ty;
  u32 *off = b_bins, *cnt = b_bins + nt, *list = b_bins + 2 * nt;
  memset(cnt, 0, nt * 4);
  for (u32 i = 0; i < bin_nrows; i++) {
    BinRow *r = &bin_rows[i];
    if (r->row >= ty) continue;
    u32 x1 = r->x1 < tx ? r->x1 : tx - 1;
    for (u32 x = r->x0; x <= x1; x++) cnt[r->row * tx + x]++;
  }
  u32 total = 0;
  for (u32 i = 0; i < nt; i++) { off[i] = total; total += cnt[i]; cnt[i] = 0; }
  for (u32 i = 0; i < bin_nrows; i++) {
    BinRow *r = &bin_rows[i];
    if (r->row >= ty) continue;
    u32 x1 = r->x1 < tx ? r->x1 : tx - 1;
    for (u32 x = r->x0; x <= x1; x++) { u32 t = r->row * tx + x; list[off[t] + cnt[t]++] = r->prim; }
  }
  *tiles_x = tx; *ntiles_out = nt;
  return 2 * nt + total;
}

// ---- coherency ---------------------------------------------------------------
// Scan [idx16, idx16 + count) for CPU-side changes. Fills sync_stage (count entries) and sync_runs.
// Returns number of changed halfwords. If more than 64 runs are found sync_nruns is set to 0xFFFFFFFF and the
// host uploads the staging area with the masked-merge pass instead of per-run writes.
EXPORT(n64_sync_scan) u32 n64_sync_scan(u32 idx16, u32 count) {
  if (count > SYNC_MAX) count = SYNC_MAX;
  const u32 m16 = (RDRAM_MAX >> 1) - 1;
  u32 changed = 0, nruns = 0, in_run = 0;
  for (u32 i = 0; i < count; i++) {
    u32 k = (idx16 + i) & m16;
    u32 v = *(u16 *)(rdram + ((k ^ 1) << 1));
    if (v != gpu_shadow16[k] || (rdp_hidden[k] & 0xC0)) {
      // hidden bits: what the software renderer left there if it wrote this word last, else the CPU-write rule
      u32 h = HIDDEN_AT(k, v);
      gpu_shadow16[k] = (u16)v; rdp_hidden[k] = (u8)h; rdp_shadow16[k] = (u16)v;
      sync_stage[i] = v | (h << 16) | 0x80000000u;
      changed++;
      if (!in_run) { if (nruns < 64) { sync_runs[nruns * 2] = i; sync_runs[nruns * 2 + 1] = 0; } nruns++; in_run = 1; }
      if (nruns <= 64) sync_runs[(nruns - 1) * 2 + 1]++;
    } else { sync_stage[i] = 0; in_run = 0; }
  }
  sync_nruns = nruns > 64 ? 0xFFFFFFFFu : nruns;
  sync_changed = changed;
  return changed;
}

// Unconditional variant used when the GPU copy is (re)initialised: stage everything in the range.
EXPORT(n64_sync_full) void n64_sync_full(u32 idx16, u32 count) {
  if (count > SYNC_MAX) count = SYNC_MAX;
  const u32 m16 = (RDRAM_MAX >> 1) - 1;
  for (u32 i = 0; i < count; i++) {
    u32 k = (idx16 + i) & m16;
    u32 v = *(u16 *)(rdram + ((k ^ 1) << 1));
    u32 h = HIDDEN_AT(k, v);
    gpu_shadow16[k] = (u16)v; rdp_hidden[k] = (u8)h; rdp_shadow16[k] = (u16)v;
    sync_stage[i] = v | (h << 16) | 0x80000000u;
  }
}

// Apply GPU results (u32 per halfword at src) for [idx16, idx16 + count) where the CPU has not written since.
EXPORT(n64_readback_apply) void n64_readback_apply(u32 idx16, u32 count, const u32 *src) {
  const u32 m16 = (RDRAM_MAX >> 1) - 1;
  for (u32 i = 0; i < count; i++) {
    u32 k = (idx16 + i) & m16;
    u16 *p = (u16 *)(rdram + ((k ^ 1) << 1));
    if (*p == gpu_shadow16[k] && !(rdp_hidden[k] & 0xC0)) {
      u32 g = src[i];
      *p = (u16)g; gpu_shadow16[k] = (u16)g;
      rdp_hidden[k] = (g >> 16) & 3; rdp_shadow16[k] = (u16)g;
    }
  }
}
EXPORT(n64_sync_ptrs) u32 *n64_sync_ptrs(void) {
  static u32 t[8];
  t[0] = (u32)(uintptr_t)sync_stage; t[1] = (u32)(uintptr_t)sync_runs; t[2] = (u32)(uintptr_t)&sync_nruns; t[3] = (u32)(uintptr_t)b_bins;
  t[7] = (u32)(uintptr_t)&gpu_feedback_dirty;
  t[4] = (u32)(uintptr_t)&b_bins_words; t[5] = SYNC_MAX; t[6] = (u32)(uintptr_t)&gpu_hint;
  return t;
}

// Constant tables for the shader: perspective-divide table (64 x {point, slope}) then the two 4x4 dither matrices.
void rdp_init_tables(void);
EXPORT(n64_gpu_tables) u32 *n64_gpu_tables(void) {
  static u32 t[128 + 32];
  rdp_init_tables();
  for (int i = 0; i < 64; i++) { t[i * 2] = (u32)(s32)persp_table[i][0]; t[i * 2 + 1] = (u32)(s32)persp_table[i][1]; }
  for (int i = 0; i < 32; i++) t[128 + i] = dither_matrices[i >> 4][i & 15];
  return t;
}
