#!/bin/sh
# Native headless builds.
#   out/native     - runs a ROM headlessly, dumps frames/audio (tools/native.c)
#   out/oracle_nn  - differential RDP test vs. Angrylion (needs ../ref/angrylion-rdp-plus)
set -e
cd "$(dirname "$0")/.." || exit 1
mkdir -p out
CC=${CC:-cc}
CFLAGS=${CFLAGS:--O3 -ffp-contract=off}
LDFLAGS=${LDFLAGS:-}
$CC $CFLAGS -o out/native tools/native.c $LDFLAGS -lm
ls -la out/native
$CC $CFLAGS -o out/rsptest tools/rsptest.c $LDFLAGS -lm
./out/rsptest
$CC $CFLAGS -o out/bustest tools/bustest.c $LDFLAGS -lm
./out/bustest
$CC $CFLAGS -o out/cputest tools/cputest.c $LDFLAGS -lm
./out/cputest
REF=${REF:-../ref/angrylion-rdp-plus}
if [ ! -f "$REF/src/core/n64video.c" ]; then
  rm -f out/oracle_nn
  if [ "${REQUIRE_REFERENCE:-0}" = 1 ]; then echo "error: required reference missing" >&2; exit 1; fi
  echo "note: $REF not present, skipping out/oracle_nn (clone ata4/angrylion-rdp-plus next to the repo root)" >&2
  exit 0
fi
. tools/versions.env
[ "$(git -C "$REF" rev-parse HEAD)" = "$ANGRYLION_COMMIT" ] || { echo "error: reference revision differs from tools/versions.env" >&2; exit 1; }
# n64video.c #includes its own rdp/vi modules, and parallel_* is stubbed in
# tools/oracle.c, so only n64video.c itself is compiled. -DNOISE_ZERO zeroes our
# noise sources; Angrylion's own sequence is deterministic (fixed seed).
$CC $CFLAGS -DNOISE_ZERO -o out/oracle_nn tools/oracle.c "$REF/src/core/n64video.c" -I"$REF/src/core" $LDFLAGS -lm
ls -la out/oracle_nn
