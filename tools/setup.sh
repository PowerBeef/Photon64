#!/bin/sh
# Reproducible Linux x86_64 / macOS development setup; private ROMs are never downloaded.
set -eu
cd "$(dirname "$0")/.."
. tools/versions.env
node -e 'if (+process.versions.node.split(".")[0] < 24) throw Error("Node 24+ required")'
command -v "${NATIVE_CC:-cc}" >/dev/null
case "$(uname -s)-$(uname -m)" in
  Linux-x86_64)
    sdk=".tools/wasi-sdk-$WASI_VERSION-$WASI_ARCH"
    if [ ! -x "$sdk/bin/clang" ]; then
      mkdir -p .tools
      archive=".tools/wasi-sdk.tar.gz"
      curl -fL --retry 2 -o "$archive" "https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-34/wasi-sdk-$WASI_VERSION-$WASI_ARCH.tar.gz"
      echo "$WASI_SHA256  $archive" | sha256sum -c -
      tar --no-same-owner -xzf "$archive" -C .tools
      rm "$archive"
    fi
    ;;
  Darwin-*)
    sdk=""; command -v brew >/dev/null; brew list llvm lld >/dev/null || brew install llvm lld
    ;;
  *) echo 'Use a supported Clang + wasm-ld and set WASI_SDK_PATH or CC manually.' >&2; exit 1;;
esac
npm ci
"${PYTHON:-python3}" -m venv .venv
.venv/bin/python -m pip install -r tools/requirements.txt
ref="${REF:-../ref/angrylion-rdp-plus}"
if [ ! -d "$ref/.git" ]; then
  git clone https://github.com/ata4/angrylion-rdp-plus.git "$ref"
  git -C "$ref" checkout --detach "$ANGRYLION_COMMIT"
fi
[ "$(git -C "$ref" rev-parse HEAD)" = "$ANGRYLION_COMMIT" ] || { echo "Reference must be pinned to $ANGRYLION_COMMIT. Preserve your existing checkout and set REF to a pinned copy." >&2; exit 1; }
mkdir -p out
if [ -n "$sdk" ]; then printf 'export WASI_SDK_PATH="%s/%s"\n' "$(pwd)" "$sdk" > out/env.sh; else : > out/env.sh; fi
printf 'export REF="%s"\n' "$(cd "$ref" && pwd)" >> out/env.sh
printf 'export PATH="%s/.venv/bin:$PATH"\n' "$(pwd)" >> out/env.sh
echo 'Setup complete. Run: . out/env.sh && npm run validate'
if [ "${1:-}" = --browser ]; then npx playwright install chromium; fi
