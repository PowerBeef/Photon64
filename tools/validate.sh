#!/bin/sh
# Required baseline: no proprietary ROM, browser, or physical GPU prerequisite.
set -eu
cd "$(dirname "$0")/.."
if [ -f out/env.sh ]; then . ./out/env.sh; fi
./build_wasm.sh
node tools/build.mjs
npm test
REQUIRE_REFERENCE=1 CC="${NATIVE_CC:-cc}" tools/build_native.sh
ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0}; export ASAN_OPTIONS
for name in rsptest bustest cputest coretest rawtest; do
  "${NATIVE_CC:-cc}" -O1 -g -ffp-contract=off -fsanitize=address,undefined -fno-sanitize-recover=all -o "out/${name}_san" "tools/$name.c" -lm
  "./out/${name}_san"
done
./tools/build_test_wasm.sh
node tools/coretest.mjs
node tools/xpaktest.mjs
node tools/romcheck.mjs testroms --frames 300 --output out/homebrew.json
PY=${PY:-python3}; [ ! -x .venv/bin/python ] || PY=.venv/bin/python
"$PY" tools/compare_test.py

"$PY" tools/cmp_raw_ref.py testroms 120
for rom in testroms/*.N64; do ./out/vitest "$rom" 120; done
