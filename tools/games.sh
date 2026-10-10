#!/bin/bash
# Game regression suite. Each game is played from power-on with a recorded input script, three ways:
#   1. software renderer vs. the Angrylion reference, command list by command list (random bits zeroed on both sides);
#   2. WebGPU renderer (Dawn) vs. the software renderer: picture, framebuffer memory and machine state at checkpoints;
#   3. the same through the high-resolution code path at 1x, which must also be exact.
#   4. (mk64) the high-resolution pass at 4x: pictures built from texture tiles must show no joints (tools/hdseams.py).
# ROMs resolve from roms/ by short name; a <short>.z64 file next to this directory's parent overrides the local ROM copy.
# GPU steps use Dawn Node bindings when present, else headless Chromium (gputest.mjs).
#   tools/games.sh [sm64|ge|pd|mk64|smash|wdc ...]
#   tools/games.sh --check-roms [sm64|ge|pd|mk64|smash|wdc ...]   # report resolved ROM paths only
#   GPU_RUNNER=dawn|browser forces one; DAWN_WEBGPU points at the bindings.
cd "$(dirname "$0")/.." || exit 1
set -uo pipefail
mkdir -p out
DAWN_WEBGPU=${DAWN_WEBGPU:-$(pwd)/node_modules/webgpu/index.js}
export DAWN_WEBGPU
GPU_RUNNER=${GPU_RUNNER:-auto}
if [ "$GPU_RUNNER" = auto ]; then
  if [ -f "$DAWN_WEBGPU" ]; then GPU_RUNNER=dawn; else GPU_RUNNER=browser; fi
fi
runner=dawntest.mjs; [ "$GPU_RUNNER" = browser ] && runner=gputest.mjs
PY=${PY:-python3}
[ -x .venv/bin/python ] && PY=.venv/bin/python   # project venv first (Pillow/scipy live there)
frames_for() { case $1 in sm64) echo 4200;; ge) echo 4600;; pd) echo 6500;; mk64) echo 3300;; smash) echo 8000;; wdc) echo 14000;; esac; }
checks_for() { case $1 in
  sm64) echo "250,900,2000,2600,3780,4190";;
  ge) echo "700,1300,2500,2900,3450,4000,4599";;
  pd) echo "700,1400,2650,3500,4800,5140,5400,5700,6100,6499";;
  mk64) echo "500,700,1300,1700,2100,2500,2800,3299";;
  smash) echo "300,6000,6151,6755,7114,7999";;
  wdc) echo "300,6000,7418,7906,10441,13999";; esac; }
rom_for() {  # short name -> usable path; a root copy wins over roms/
  if [ -f "$1.z64" ]; then echo "$1.z64"; return 0; fi
  case $1 in
    sm64) r="Super Mario 64 (USA).z64";;
    ge) r="GoldenEye 007 (USA).z64";;
    pd) r="Perfect Dark (USA) (Rev 1).z64";;
    mk64) r="Mario Kart 64 (USA).z64";;
    smash) r="Super Smash Bros. (USA).z64";;
    wdc) r="World Driver Championship (USA).z64";;
    *) return 1;;
  esac
  [ -f "roms/$r" ] && echo "roms/$r"
}
if [ "${1:-}" = --check-roms ]; then
  shift; fail=0
  for g in ${@:-sm64 ge pd mk64 smash wdc}; do
    if rom=$(rom_for "$g"); then echo "$g: $rom"; else echo "$g: ROM not present"; fail=1; fi
  done
  exit $fail
fi
fail=0
for g in ${@:-sm64 ge pd mk64 smash wdc}; do
  rom=$(rom_for "$g") || { echo "$g: ROM not present, required lane failed"; fail=1; continue; }
  I=$(cat tools/inputs/$g.txt); n=$(frames_for $g)
  echo "== $g: software renderer vs. reference"
  if [ -x ./out/oracle_nn ]; then
    ./out/oracle_nn "$rom" $n -i "$I" | tail -2
    [ "${PIPESTATUS[0]}" -eq 0 ] || { echo "$g: oracle step failed"; fail=1; }
  else
    echo "$g: out/oracle_nn not built, reference step failed (run tools/build_native.sh with the Angrylion checkout present)"; fail=1
  fi
  for mode in "" "--hd 0"; do
    echo "== $g: WebGPU vs. software via $GPU_RUNNER${mode:+, high-resolution path at 1x}"
    tag=native; [ -n "$mode" ] && tag=hd1
    log="out/games_${g}_${tag}.log"
    if ! node tools/$runner "$rom" "$n" "$(checks_for "$g")" "$I" $mode > "$log" 2>&1; then
      echo "$g: GPU runner failed; see $log"; fail=1
    fi
    if ! $PY tools/gpures.py "$log" "$(checks_for "$g")"; then fail=1; fi
  done
  if [ $g = mk64 ]; then
    echo "== $g: joints between texture tiles at 4x"
    # (--hdwords: the first 4 MB of RDRAM, which is all this game draws to, fits lavapipe's 128 MB buffer limit at 4x)
    hdw=""; [ "$GPU_RUNNER" = dawn ] && hdw="--hdwords 2097152"   # lavapipe-only buffer workaround
    node tools/$runner "$rom" 1310 "500,700,1300" "$I" --hd 2 $hdw --window 6 --noref --images out/games_${g}_hd > out/games_${g}_hd.log 2>&1
    status=$?; [ "$status" -eq 0 ] || [ "$status" -eq 2 ] || { fail=1; continue; }
    $PY tools/hdseams.py out/games_${g}_hd_00500.png 4 0 140 320 232 "title backdrop (2-line strips)" 2.2 y || fail=1
    $PY tools/hdseams.py out/games_${g}_hd_01300.png 4 51 16 271 48 "heading (4-line strips)" 1.75 y || fail=1
    $PY tools/hdseams.py out/games_${g}_hd_01300.png 4 122 64 128 126 "portrait, vertical joint" 1.75 x || fail=1
    $PY tools/hdseams.py out/games_${g}_hd_01300.png 4 94 92 156 98 "portrait, horizontal joint" 1.75 y || fail=1
    $PY tools/hdseams.py out/games_${g}_hd_00700.png 4 107 74 137 80 "panel, joint under fill" 1.75 y || fail=1
  fi
done
exit $fail
