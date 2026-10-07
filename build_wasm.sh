#!/bin/sh
# Build the core to WebAssembly (freestanding, SIMD128, bulk memory).
# Needs a clang that can link wasm32 (Apple's cannot): Homebrew LLVM, wasi-sdk, or $CC.
set -e
if [ -z "$CC" ]; then
  for c in /opt/homebrew/opt/llvm/bin/clang /usr/local/opt/llvm/bin/clang; do
    if [ -x "$c" ]; then CC=$c; break; fi
  done
fi
if [ -z "$CC" ] && [ -n "$WASI_SDK_PATH" ]; then CC=$WASI_SDK_PATH/bin/clang; fi
: "${CC:=clang}"
mkdir -p out
$CC --target=wasm32 -O3 -msimd128 -mbulk-memory -mnontrapping-fptoint -msign-ext -mmutable-globals \
  -ffp-contract=off -fno-math-errno -fvisibility=hidden -nostdlib -ffreestanding \
  -Wall -Wno-unused-function -Wno-unused-variable \
  -Wl,--no-entry -Wl,--export=__heap_base -Wl,-z,stack-size=1048576 -Wl,--strip-all -Wl,--lto-O3 -flto \
  -o out/n64.wasm src/n64.c
ls -la out/n64.wasm
