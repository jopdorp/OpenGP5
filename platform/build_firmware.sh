#!/bin/bash
# Build one firmware for the GP-5 open platform into ONE flashable image.
#   platform/build_firmware.sh <name> [out.bin]        name = a folder under firmwares/
#   platform/build_firmware.sh <path> [out.bin]        or any folder with dsp/firmware.h and nds32/*.c, e.g. a
#                                                      firmware in its own repository (default out: ./build/)
#   platform/build_firmware.sh --gp50-emu <firmware> [out-dir]    build both halves as emulator inputs
#   platform/build_firmware.sh --gp50-package <firmware> [out.bin]    build an experimental GP-50 HTFW package
# Needs: the two compilers (release/setup-toolchain.sh puts them in fw-build/), python3, and on macOS Docker
# (the control processor's compiler is a 32-bit Linux program; Linux and WSL run it directly).
# No Valeton file is needed: the image holds only our two sections, the DSP firmware (dsp) and the
# control-processor app (app). Valeton's other sections on the pedal (DSP data, models, presets) are not
# touched by a flash of this image.
# Flash a GP-5 image with:  python3 tools/gp5tool.py flash <out.bin> --sections all
set -euo pipefail
GP50=0
PACKAGE=0
case "${1:-}" in
  --gp50-emu) GP50=1; shift ;;
  --gp50-package) GP50=1; PACKAGE=1; shift ;;
esac
ARG=${1:?usage: platform/build_firmware.sh [--gp50-emu | --gp50-package] <firmware name | folder> [output]}
if [ -f "$ARG/dsp/firmware.h" ] && [[ "$ARG" == */* || "$ARG" == . || "$ARG" == .. ]]; then      # a folder, anywhere
  FWDIR=$(cd "$ARG" && pwd); NAME=$(basename "$FWDIR")
  EXT=1
else                                                                              # a name under firmwares/
  NAME=$ARG
  EXT=0
fi
CTL_OUT=platform/nds32/out
if [ "$GP50" = 1 ]; then
  CTL_OUT=$CTL_OUT/gp50
  if [ "$PACKAGE" = 1 ]; then OUT=${2:-build/GP-50_${NAME}-experimental.bin}
  else OUT=${2:-build/GP-50_$NAME}; fi
else OUT=${2:-build/GP-5_$NAME.bin}; fi
if [ "$EXT" = 1 ]; then
  mkdir -p "$(dirname "$OUT")"; OUT=$(cd "$(dirname "$OUT")" && pwd)/$(basename "$OUT")
fi
cd "$(dirname "$0")/.."
if [ "$EXT" = 0 ]; then FWDIR=$PWD/firmwares/$NAME; fi
[ -f "$FWDIR/dsp/firmware.h" ] || { echo "no $FWDIR/dsp/firmware.h"; exit 1; }
ls "$FWDIR/nds32/"*.c >/dev/null 2>&1 || { echo "no C files in $FWDIR/nds32/"; exit 1; }
TMP=$(mktemp -d)
CTL_TMP=
trap 'rm -rf "$TMP"; if [ -n "$CTL_TMP" ]; then rm -rf "$CTL_TMP"; fi' EXIT
if [ "$PACKAGE" = 1 ]; then
  mkdir -p "${CTL_OUT%/*}"
  CTL_TMP=$(mktemp -d "$CTL_OUT-package.XXXXXX")
  CTL_OUT=$CTL_TMP
fi
GEN=$CTL_OUT/gen; mkdir -p "$GEN"; rm -f "$GEN"/*  # firmware.json (1.0): checked, flags + metadata block
[ -f "$FWDIR/firmware.json" ] || { echo "no $FWDIR/firmware.json: OpenGP5 1.0 firmwares describe themselves there (docs/platform/api.md)"; exit 1; }
python3 platform/manifest.py "$FWDIR" --check
python3 platform/manifest.py "$FWDIR" --flags > "$GEN/manifest.flags"
python3 platform/manifest.py "$FWDIR" --meta "$GEN/gp5_meta.c"
mkdir -p "$(dirname "$OUT")"

echo "== DSP firmware"
if [ "$GP50" = 1 ]; then
  python3 platform/dsp/build.py "$TMP/dsp.bin" --gp50-raw --src responder.c --firmware "$FWDIR"
else
  python3 platform/dsp/build.py "$TMP/dsp.bin" --src responder.c --firmware "$FWDIR" \
    -D AUDIO_PASSTHROUGH -D AUDIO_LEVEL -D RESIDENT_TICK_PATCH
fi
echo "== control processor firmware"
# The Andes compiler is 32-bit Linux: run it directly where it can run (Linux / WSL with the 32-bit C library),
# else in Docker.
AS=fw-build/andes/v3s/nds32le-elf-mculib-v3s/nds32le-elf/bin/as
[ -e "$AS" ] || { echo "control-processor compiler missing: run release/setup-toolchain.sh"; exit 1; }
if [ "$(uname -s)" = Linux ] && "$AS" --version >/dev/null 2>&1; then
  GP50_BOARD="$GP50" GP5_NDS32_OUT="$CTL_TMP" GP5_FIRMWARE_DIR="$FWDIR" bash platform/nds32/build.sh
elif command -v docker >/dev/null 2>&1; then
  if [[ "$FWDIR" == "$PWD"/* ]]; then MNT=(); CFW=/work/${FWDIR#"$PWD"/}       # inside this tree
  else MNT=(-v "$FWDIR":/fw); CFW=/fw; fi                                      # elsewhere: mount it too
  docker run --rm --platform linux/386 -e GP50_BOARD="$GP50" -e GP5_NDS32_OUT="$CTL_TMP" \
    -e GP5_FIRMWARE_DIR="$CFW" -e GP5_DEBUG_PEEK="${GP5_DEBUG_PEEK:-}" -v "$PWD":/work ${MNT[@]+"${MNT[@]}"} debian:bookworm-slim \
      bash /work/platform/nds32/build.sh
else
  echo "The control-processor compiler is a 32-bit Linux program and cannot run here."
  echo "  Linux / WSL: install the 32-bit C library (Debian/Ubuntu: sudo apt install libc6-i386,"
  echo "               Arch: sudo pacman -S lib32-glibc, Fedora: sudo dnf install glibc.i686)"
  echo "  macOS:       install Docker Desktop"
  exit 1
fi
if [ "$PACKAGE" = 1 ]; then
  python3 platform/nds32/pack.py --gp50 "$CTL_OUT/app.bin" "$TMP/dsp.bin" "$OUT"
  exit 0
fi
if [ "$GP50" = 1 ]; then
  mkdir -p "$OUT"
  cp "$CTL_OUT/app.elf" "$OUT/control.elf"
  cp "$TMP/dsp.bin" "$OUT/audio.dsp"
  echo "GP-50 emulator inputs in $OUT"
  exit 0
fi
python3 platform/nds32/pack.py
echo "== one image"
python3 tools/gp5tool.py pack dsp="$TMP/dsp.bin" app=platform/nds32/out/app_0x62.bin -o "$OUT"
