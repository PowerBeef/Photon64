// RSP vector-op unit tests for ops with no test ROM: VMACQ, VRSQ/VRSQL/VRSQH
// (+ VRCP/VMOV locks for the errata-derived behaviors). Drives vec_slow directly
// with hand-encoded instructions; no ROM needed.
//   cc -O2 -o out/rsptest tools/rsptest.c && ./out/rsptest
#include <stdio.h>
#include <stdint.h>
#include "../src/n64.c"

void host_log(const char *msg, u32 a, u32 b) { (void)msg; (void)a; (void)b; }
void host_gpu_flush(void) {}

// e=(op>>21)&15, vt=(op>>16)&31, vs/de=(op>>11)&31 (de = low 3 bits), vd=(op>>6)&31, fn=op&0x3F
#define VOP(e, vt, vsde, vd, fn) ((u32)(((e) << 21) | ((vt) << 16) | ((vsde) << 11) | ((vd) << 6) | (fn)))

static int t_bad = 0, t_n = 0;
static void t_check(const char *name, int ok) { t_n++; printf("%s %s\n", ok ? "ok  " : "FAIL", name); if (!ok) t_bad++; }

static void acc_lanes(int v0, int v1) {  // distinct acc_l/vd sentinels to catch stray writes
  for (int i = 0; i < 8; i++) { rsp.acc_l.u[i] = (u16)v0; rsp.v[1].e[i] = v1; }
}

// integer floor(sqrt(n)) by binary search; exact, no libm
static u32 isqrt_floor(u64 n) {
  u64 lo = 0, hi = (u64)1 << 23;
  while (lo < hi) { u64 mid = (lo + hi + 1) >> 1; if (mid * mid <= n) lo = mid; else hi = mid - 1; }
  return (u32)lo;
}

static void t_vmacq(void) {
  // zero/magnitude test covers acc[47:21] (acc_hm>>5); low 21 bits are ignored
  rsp_reset(); acc_lanes(0, 0x5A5A); rsp.acc_hm[0] = 31; rsp.acc_l.u[0] = 0xFFFF;
  vec_slow(VOP(0, 2, 0, 1, 0x0B));
  t_check("vmacq: [47:21]==0 ignores low bits (31,accl=ffff)", rsp.acc_hm[0] == 31 && rsp.v[1].e[0] == 0 && rsp.acc_l.u[0] == 0xFFFF);
  rsp_reset(); rsp.acc_hm[0] = 32; acc_lanes(0, 0x5A5A);
  vec_slow(VOP(0, 2, 0, 1, 0x0B));
  t_check("vmacq: bit21 set keeps 32", rsp.acc_hm[0] == 32 && rsp.v[1].e[0] == 16);
  rsp_reset(); rsp.acc_hm[0] = 64; acc_lanes(0, 0x5A5A);
  vec_slow(VOP(0, 2, 0, 1, 0x0B));
  t_check("vmacq: 64-32", rsp.acc_hm[0] == 32 && rsp.v[1].e[0] == 16);
  rsp_reset(); rsp.acc_hm[0] = -1; acc_lanes(0, 0x5A5A);
  vec_slow(VOP(0, 2, 0, 1, 0x0B));
  t_check("vmacq: -1 keeps (bit21 set)", rsp.acc_hm[0] == -1 && rsp.v[1].e[0] == -16);
  rsp_reset(); rsp.acc_hm[0] = -64; acc_lanes(0, 0x5A5A);
  vec_slow(VOP(0, 2, 0, 1, 0x0B));
  t_check("vmacq: -64+32", rsp.acc_hm[0] == -32 && rsp.v[1].e[0] == -16);
  rsp_reset(); rsp.acc_hm[0] = (s32)0x7FFFFFFFu; acc_lanes(0, 0x5A5A);
  vec_slow(VOP(0, 2, 0, 1, 0x0B));
  t_check("vmacq: saturate high -> 32752", rsp.acc_hm[0] == (s32)0x7FFFFFFFu && rsp.v[1].e[0] == 32752);
  rsp_reset(); rsp.acc_hm[0] = (s32)0x80000000u; acc_lanes(0, 0x5A5A);
  vec_slow(VOP(0, 2, 0, 1, 0x0B));
  t_check("vmacq: saturate low -> -32768", rsp.acc_hm[0] == (s32)0x80000020u && rsp.v[1].e[0] == -32768);
  rsp_reset(); rsp.acc_hm[0] = 64; rsp.acc_hm[1] = 31; rsp.acc_hm[2] = -64; acc_lanes(0, 0x5A5A);
  vec_slow(VOP(0, 2, 0, 1, 0x0B));
  t_check("vmacq: lanes independent", rsp.acc_hm[0] == 32 && rsp.acc_hm[1] == 31 && rsp.acc_hm[2] == -32 &&
    rsp.v[1].e[0] == 16 && rsp.v[1].e[1] == 0 && rsp.v[1].e[2] == -16);
}

static int acc_is(u16 v) { for (int i = 0; i < 8; i++) if (rsp.acc_l.u[i] != v) return 0; return 1; }
static int vd_intact(s16 want, int lane) {
  for (int i = 0; i < 8; i++) if (i != lane && rsp.v[1].e[i] != want) return 0;
  return 1;
}

static void t_vrsq(void) {
  // VRSQ input 0 -> max out; single-lane write; ACC_L = broadcast VT[e]
  rsp_reset();
  for (int i = 0; i < 8; i++) rsp.v[2].e[i] = (s16)(i + 1);
  rsp.v[2].e[3] = 0; acc_lanes(0xA5A5, 0x5A5A); rsp.divdp = 1; rsp.divout = 0x1111;
  vec_slow(VOP(11, 2, 5, 1, 0x34));
  t_check("vrsq: input 0 -> vd=low(-1) divout=high(0x7FFF)", rsp.v[1].e[5] == -1 && vd_intact(0x5A5A, 5) &&
    rsp.divdp == 0 && rsp.divout == 0x7FFF && acc_is(0));
  // VRSQ input -32768 edge -> -1
  rsp_reset();
  for (int i = 0; i < 8; i++) rsp.v[2].e[i] = (s16)(i + 1);
  rsp.v[2].e[0] = (s16)0x8000; acc_lanes(0xA5A5, 0x5A5A);
  vec_slow(VOP(8, 2, 7, 1, 0x34));
  t_check("vrsq: input -32768 -> vd=0 divout=-1", rsp.v[1].e[7] == 0 && vd_intact(0x5A5A, 7) && rsp.divout == -1 && acc_is(0x8000));
  // ACC_L broadcast of a nonzero element
  rsp_reset();
  for (int i = 0; i < 8; i++) rsp.v[2].e[i] = (s16)(0x100 * (i + 1));
  acc_lanes(0xA5A5, 0x5A5A);
  vec_slow(VOP(10, 2, 0, 1, 0x34));
  t_check("vrsq: acc_l = broadcast vt[2]", acc_is(0x0300) && vd_intact(0x5A5A, 0));
  // rsqrt table: independent integer recomputation of every entry
  rsp_reset();
  int table_ok = 1;
  for (u32 i = 0; i < 512; i++) {
    u64 a = (i + 512) >> (i & 1);
    u32 b = isqrt_floor((((u64)1 << 44) - 1) / a);   // largest b with a*b*b < 2^44
    if (b < (1u << 17)) b = 1u << 17;
    if (rsp.rsq[i] != (u16)(b >> 1)) { table_ok = 0; break; }
  }
  t_check("vrsq: rsq table exact (512 entries)", table_ok);
  t_check("vrcp: rcp[0] anchor", rsp.rcp[0] == 0xFFFF);
  // VRSQH stages a 32-bit input ...
  rsp_reset();
  for (int i = 0; i < 8; i++) rsp.v[2].e[i] = (s16)(i + 1);
  rsp.v[2].e[4] = 0x5678; acc_lanes(0xA5A5, 0x5A5A); rsp.divout = 0x1234;
  vec_slow(VOP(12, 2, 3, 1, 0x36));
  t_check("vrsqh: stages divin, returns old divout", rsp.divdp == 1 && rsp.divin == 0x5678 && rsp.v[1].e[3] == 0x1234 &&
    vd_intact(0x5A5A, 3) && acc_is(0x5678));
  // ... and VRSQL consumes it, clearing divdp
  rsp.divin = 0x1234; rsp.v[2].e[4] = 0x5678; acc_lanes(0xA5A5, 0x5A5A);
  vec_slow(VOP(12, 2, 6, 1, 0x35));
  s16 got = rsp.v[1].e[6];
  int intact = vd_intact(0x5A5A, 6) && acc_is(0x5678) && rsp.divdp == 0;
  rsp.divdp = 1;   // re-stage the same 32-bit input for the determinism rerun
  vec_slow(VOP(12, 2, 6, 1, 0x35));
  t_check("vrsql: consumes divdp, deterministic", intact && rsp.v[1].e[6] == got && got != 0x5A5A);
  // VRCP control: input 0 edge (ROM-tested path, locked here too)
  rsp_reset();
  for (int i = 0; i < 8; i++) rsp.v[2].e[i] = (s16)(i + 1);
  rsp.v[2].e[1] = 0; acc_lanes(0xA5A5, 0x5A5A); rsp.divdp = 1;
  vec_slow(VOP(9, 2, 2, 1, 0x30));
  t_check("vrcp: input 0 -> vd=-1 divout=0x7FFF", rsp.v[1].e[2] == -1 && vd_intact(0x5A5A, 2) && rsp.divdp == 0 && acc_is(0));
}

static void t_vmov(void) {
  // errata: source uses vector-op indexing, not a plain copy
  rsp_reset();
  for (int i = 0; i < 8; i++) rsp.v[2].u[i] = (u16)(10 + i);
  acc_lanes(0xA5A5, 0x5A5A);
  vec_slow(VOP(8, 2, 1, 1, 0x33));   // e=8: src = vt[0] regardless of de
  t_check("vmov: e=8 scalar src", rsp.v[1].u[1] == 10 && vd_intact(0x5A5A, 1) && acc_is(10));
  rsp_reset();
  for (int i = 0; i < 8; i++) rsp.v[2].u[i] = (u16)(10 + i);
  acc_lanes(0xA5A5, 0x5A5A);
  vec_slow(VOP(2, 2, 3, 1, 0x33));   // e=2: src lane = (0)|(3&6) = 2
  t_check("vmov: e=2 lane-mix src", rsp.v[1].u[3] == 12 && vd_intact(0x5A5A, 3));
}

int main(void) {
  t_vmacq();
  t_vrsq();
  t_vmov();
  printf("rsptest: %d checks, %d failed\n", t_n, t_bad);
  return t_bad != 0;
}
