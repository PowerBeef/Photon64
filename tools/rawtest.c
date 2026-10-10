#define NATIVE_NO_MAIN
#include "native.c"
int main(void) {
  const u32 widths[] = {0, 1, 320, 640, 641, 2048, 4095};
  for (u32 i = 0; i < sizeof widths / sizeof *widths; i++) for (u32 type = 0; type < 4; type++) {
    sys.vi[0] = type; sys.vi[1] = RDRAM_MAX - 1; sys.vi[2] = widths[i];
    int ok = dump_raw_fb("out/rawtest.png");
    if (ok != (widths[i] != 0 && type >= 2)) return 1;
  }
  if (dump_raw_fb("/nonexistent/photon/raw.png")) return 1;
  remove("out/rawtest.png"); puts("29 raw-output boundary checks passed"); return 0;
}
