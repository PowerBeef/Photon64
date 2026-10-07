// Minimal PNG writer (stored deflate blocks), RGBA8 input.
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
static uint32_t png_crc_t[256];
static uint32_t png_crc(uint32_t c, const uint8_t *d, size_t n) {
  if (!png_crc_t[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t k = i; for (int j = 0; j < 8; j++) k = (k >> 1) ^ (0xEDB88320u & (0u - (k & 1))); png_crc_t[i] = k; }
  c = ~c; for (size_t i = 0; i < n; i++) c = png_crc_t[(c ^ d[i]) & 255] ^ (c >> 8); return ~c;
}
static void png_chunk(FILE *f, const char *type, const uint8_t *d, uint32_t n) {
  uint8_t h[8] = { n >> 24, n >> 16, n >> 8, n, type[0], type[1], type[2], type[3] };
  fwrite(h, 1, 8, f); if (n) fwrite(d, 1, n, f);
  uint32_t c = png_crc(0, h + 4, 4); if (n) c = png_crc(c, d, n);
  uint8_t t[4] = { c >> 24, c >> 16, c >> 8, c }; fwrite(t, 1, 4, f);
}
static int png_write(const char *path, const uint8_t *rgba, int w, int h) {
  FILE *f = fopen(path, "wb"); if (!f) return 0;
  fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
  uint8_t ihdr[13] = { w >> 24, w >> 16, w >> 8, w, h >> 24, h >> 16, h >> 8, h, 8, 2, 0, 0, 0 };
  png_chunk(f, "IHDR", ihdr, 13);
  size_t raw = (size_t)h * (w * 3 + 1);
  uint8_t *z = malloc(raw + raw / 65535 * 5 + 16), *p = z;
  uint8_t *rawb = malloc(raw), *q = rawb;
  for (int y = 0; y < h; y++) { *q++ = 0; for (int x = 0; x < w; x++) { const uint8_t *s = rgba + (y * w + x) * 4; *q++ = s[0]; *q++ = s[1]; *q++ = s[2]; } }
  *p++ = 0x78; *p++ = 0x01;
  uint32_t a = 1, b = 0;
  for (size_t i = 0; i < raw; i++) { a = (a + rawb[i]) % 65521; b = (b + a) % 65521; }
  for (size_t off = 0; off < raw;) {
    size_t n = raw - off; if (n > 65535) n = 65535;
    *p++ = (off + n == raw); *p++ = n & 255; *p++ = n >> 8; *p++ = ~n & 255; *p++ = (~n >> 8) & 255;
    memcpy(p, rawb + off, n); p += n; off += n;
  }
  *p++ = b >> 8; *p++ = b; *p++ = a >> 8; *p++ = a;
  png_chunk(f, "IDAT", z, p - z); png_chunk(f, "IEND", 0, 0);
  free(z); free(rawb); fclose(f); return 1;
}
