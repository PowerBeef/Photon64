#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
if [ -z "${CC:-}" ] && [ -n "${WASI_SDK_PATH:-}" ]; then CC="$WASI_SDK_PATH/bin/clang"; fi
: "${CC:=clang}"
for opt in 0 3; do
  "$CC" --target=wasm32 -O"$opt" -ffp-contract=off -nostdlib -ffreestanding -msimd128 -Wl,--no-entry -Wl,-z,stack-size=1048576 -o "out/coretest_O$opt.wasm" tools/coretest.c
done
