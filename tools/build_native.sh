#!/bin/sh
# Native headless builds.
#   out/native     - runs a ROM headlessly, dumps frames/audio (tools/native.c)
#   out/oracle_nn  - differential RDP test vs. Angrylion (needs ../ref/angrylion-rdp-plus)
set -e
cd "$(dirname "$0")/.." || exit 1
mkdir -p out
CC=${CC:-cc}
$CC -O3 -o out/native tools/native.c
ls -la out/native
$CC -O2 -o out/rsptest tools/rsptest.c
./out/rsptest
$CC -O2 -o out/bustest tools/bustest.c
./out/bustest
$CC -O2 -o out/cputest tools/cputest.c
./out/cputest
REF=../ref/angrylion-rdp-plus
if [ ! -f $REF/src/core/n64video.c ]; then
  echo "note: $REF not present, skipping out/oracle_nn (clone ata4/angrylion-rdp-plus next to the repo root)" >&2
  exit 0
fi
# n64video.c #includes its own rdp/vi modules, and parallel_* is stubbed in
# tools/oracle.c, so only n64video.c itself is compiled. -DNOISE_ZERO zeroes our
# noise sources; Angrylion's own sequence is deterministic (fixed seed).
$CC -O3 -DNOISE_ZERO -o out/oracle_nn tools/oracle.c $REF/src/core/n64video.c -I$REF/src/core
ls -la out/oracle_nn
