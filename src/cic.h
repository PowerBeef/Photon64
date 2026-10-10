// CIC challenge transition adapted from ares/n64/cic/commands.cpp at
// 9408cb43d4948fc3ea6e152a307a34348df3fe04 (ISC).
// Copyright (c) 2004-2025 ares team, Near et al.
// The full permission and warranty notice is in third_party/ares/LICENSE.txt
// and embedded in every standalone HTML build. This adaptation processes packed
// PIF bytes; it does not emulate the physical serial interface or boot lockout.
static void cic_challenge(u8 *bytes, int real) {
  static const u8 next_key[2][16] = {
    {4,7,10,7,14,5,14,1,12,15,8,15,6,3,6,9},
    {4,1,10,7,14,5,14,1,12,9,8,5,6,3,12,9}
  };
  u32 key = 11, table = 0;
  for (u32 i = 0; i < 30; i++) {
    u32 shift = (i & 1) ? 0 : 4, challenge = (bytes[i >> 1] >> shift) & 15;
    u32 response = real ? (key + 5 * challenge) & 15 : challenge ^ 15;
    if (real) {
      key = next_key[table][response];
      u32 sign = response >> 3, magnitude = (sign ? ~response : response) & 7;
      u32 next = magnitude % 3 == 1 ? sign : sign ^ 1;
      if (table && (response == 1 || response == 9)) next = 1;
      if (table && (response == 11 || response == 14)) next = 0;
      table = next;
    }
    bytes[i >> 1] = (bytes[i >> 1] & ~(15u << shift)) | (response << shift);
  }
}
