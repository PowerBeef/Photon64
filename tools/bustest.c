// JoyBus PIF-packet bounds tests: a short tx/rx must raise the channel error
// bit, never read past the command or write past the response slot.
//   cc -O2 -o out/bustest tools/bustest.c && ./out/bustest
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../src/n64.c"

void host_log(const char *msg, u32 a, u32 b) { (void)msg; (void)a; (void)b; }
void host_gpu_flush(void) {}

static int t_bad = 0, t_n = 0;
static void t_check(const char *name, int ok) { t_n++; printf("%s %s\n", ok ? "ok  " : "FAIL", name); if (!ok) t_bad++; }

// Fill the PIF area with sentinels, then lay the packet down. Trailing 0xAA
// bytes safely terminate the channel scan (0xAA & 0x3F overruns 64).
static void pif_setup(const u8 *pkt, int n) {
  memset(&sys, 0, sizeof sys);
  memset(sys.pif, 0xAA, sizeof sys.pif);
  memcpy(sys.pif, pkt, n);
}
static int sentinels_ok(int from, int to) {
  for (int i = from; i < to; i++) if (sys.pif[i] != 0xAA) return 0;
  return 1;
}
static int err_bit(int rx_off) { return (sys.pif[rx_off] & 0x80) != 0; }

int main(void) {
  // --- 0x02 mempak read, channel 0: tx=3 rx=33 cmd ---
  for (int i = 0; i < 32; i++) mempak[0][i] = (u8)(i * 3 + 1);
  u8 rd[] = { 3, 33, 0x02, 0x00, 0x00 };
  pif_setup(rd, sizeof rd); sys.pad_present[0] = 1; sys.pak[0] = 1;
  pif_process();
  t_check("0x02 normal: 32B payload + crc, no error", !err_bit(1) &&
    memcmp(&sys.pif[5], mempak[0], 32) == 0 && sys.pif[37] == pak_crc(mempak[0]));

  // --- 0x02 with short rx: must error, must not touch bytes past the slot ---
  u8 rd_short[] = { 3, 1, 0x02, 0x00, 0x00, 0xFE };
  pif_setup(rd_short, sizeof rd_short); sys.pad_present[0] = 1; sys.pak[0] = 1;
  pif_process();
  t_check("0x02 short rx: error bit set", err_bit(1));
  t_check("0x02 short rx: bytes past slot intact", sentinels_ok(6, 64));

  // --- 0x02 with short tx: address bytes missing ---
  u8 rd_tx[] = { 1, 33, 0x02, 0xFE };
  pif_setup(rd_tx, sizeof rd_tx); sys.pad_present[0] = 1; sys.pak[0] = 1;
  pif_process();
  t_check("0x02 short tx: error bit set", err_bit(1));

  // --- 0x03 mempak write, channel 0: tx=35 rx=1 cmd ---
  u8 wr[2 + 35] = { 35, 1, 0x03, 0x00, 0x20 };
  for (int i = 0; i < 32; i++) wr[5 + i] = (u8)(0x80 + i);
  memset(mempak[0], 0, 0x8000);
  pif_setup(wr, sizeof wr); sys.pad_present[0] = 1; sys.pak[0] = 1;
  pif_process();
  t_check("0x03 normal: payload stored + crc ack", !err_bit(1) &&
    memcmp(mempak[0] + 0x20, wr + 5, 32) == 0 && sys.pif[37] == pak_crc(wr + 5));

  // --- 0x03 with short tx: data missing, mempak must be untouched ---
  u8 wr_tx[] = { 4, 1, 0x03, 0x00, 0x20, 0x99, 0xFE };
  memset(mempak[0], 0, 0x8000);
  pif_setup(wr_tx, sizeof wr_tx); sys.pad_present[0] = 1; sys.pak[0] = 1;
  pif_process();
  t_check("0x03 short tx: error bit set", err_bit(1));
  t_check("0x03 short tx: mempak untouched", mempak[0][0x20] == 0 && sys.save_dirty == 0);

  // --- 0x03 with rx=0: nowhere to put the ack ---
  u8 wr_rx0[2 + 35] = { 35, 0, 0x03, 0x00, 0x40 };
  for (int i = 0; i < 32; i++) wr_rx0[5 + i] = (u8)i;
  pif_setup(wr_rx0, sizeof wr_rx0); sys.pad_present[0] = 1; sys.pak[0] = 1;
  pif_process();
  t_check("0x03 rx=0: error bit set", err_bit(1));
  t_check("0x03 rx=0: slot byte intact", sys.pif[37] == 0xAA);

  // --- 0x04/0x05 EEPROM, channel 4 (four skips + command) ---
  u8 e4[] = { 0xFD, 0xFD, 0xFD, 0xFD, 2, 8, 0x04, 0x07, 0xFE };
  for (int i = 0; i < 8; i++) eeprom[7 * 8 + i] = (u8)(0xE0 + i);
  pif_setup(e4, sizeof e4); sys.save_type = SAVE_EEP4K;
  pif_process();
  t_check("0x04 normal: 8B block, no error", !err_bit(5) && memcmp(&sys.pif[8], eeprom + 7 * 8, 8) == 0);

  u8 e4_short[] = { 0xFD, 0xFD, 0xFD, 0xFD, 2, 3, 0x04, 0x07, 0xFE };
  pif_setup(e4_short, sizeof e4_short); sys.save_type = SAVE_EEP4K;
  pif_process();
  t_check("0x04 short rx: error bit set", err_bit(5));
  t_check("0x04 short rx: bytes past slot intact", sentinels_ok(11, 64));

  u8 e5[4 + 2 + 10] = { 0xFD, 0xFD, 0xFD, 0xFD, 10, 1, 0x05, 0x03 };
  for (int i = 0; i < 8; i++) e5[8 + i] = (u8)(0x50 + i);
  memset(eeprom, 0, sizeof eeprom);
  pif_setup(e5, sizeof e5); sys.save_type = SAVE_EEP4K;
  pif_process();
  t_check("0x05 normal: block stored + zero ack", !err_bit(5) &&
    memcmp(eeprom + 3 * 8, e5 + 8, 8) == 0 && sys.pif[16] == 0);

  u8 e5_tx[] = { 0xFD, 0xFD, 0xFD, 0xFD, 3, 1, 0x05, 0x03, 0x51, 0xFE };
  memset(eeprom, 0, sizeof eeprom);
  pif_setup(e5_tx, sizeof e5_tx); sys.save_type = SAVE_EEP4K;
  pif_process();
  t_check("0x05 short tx: error bit set", err_bit(5));
  t_check("0x05 short tx: eeprom untouched", eeprom[3 * 8] == 0 && sys.save_dirty == 0);

  // --- 0x00/0x01 keep their clamping behavior on short rx (no error bit) ---
  u8 c0[] = { 1, 1, 0x00, 0xFE };
  pif_setup(c0, sizeof c0); sys.pad_present[0] = 1; sys.pak[0] = 1;
  pif_process();
  t_check("0x00 short rx: clamps, no error", !err_bit(1) && sys.pif[3] == 0x05 && sentinels_ok(4, 64));

  printf("%d checks, %d failures\n", t_n, t_bad);
  return t_bad != 0;
}
