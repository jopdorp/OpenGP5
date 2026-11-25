#!/usr/bin/env bash
set -euo pipefail

source_dir=$(cd "${1:?usage: tools/build_gp50_livewdf.sh LIVEWDF_CHECKOUT [OUTPUT_DIR]}" && pwd)
out=${2:-build/GP-50_livewdf}
mkdir -p "$out"
out=$(cd "$out" && pwd)
root=$(cd "$(dirname "$0")/.." && pwd)

cmake -S "$source_dir" -B "$out/livewdf-build" -DCMAKE_BUILD_TYPE=Release \
  -DLIVEWDF_BUILD_TESTS=OFF -DLIVEWDF_BUILD_EDITOR=OFF -DLIVEWDF_BUILD_PLUGIN=OFF
cmake --build "$out/livewdf-build" --target livewdf_opengp5_gp50

firmware="$out/livewdf-build/examples/opengp5/gp50/livewdf-gp50"
"$root/platform/build_firmware.sh" --gp50-emu "$firmware" "$out"
