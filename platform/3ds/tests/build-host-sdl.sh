#!/usr/bin/env bash
# Build the same vendored SDL2 used by the game for portable host regressions.
set -euo pipefail
if [[ $# != 1 ]]; then
  printf 'Usage: %s BUILD_DIRECTORY\n' "$0" >&2
  exit 2
fi
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
mkdir -p "$1"
OUTPUT="$(cd "$1" && pwd)"
cmake -S "${ROOT}/third_party/SDL2" -B "${OUTPUT}/build" \
  -DCMAKE_INSTALL_PREFIX="${OUTPUT}/prefix" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DSDL_SHARED=ON -DSDL_STATIC=ON -DSDL_TEST=OFF
cmake --build "${OUTPUT}/build" --parallel
cmake --install "${OUTPUT}/build"
printf 'Use SDL2_CONFIG="%s" when running host tests.\n' "${OUTPUT}/prefix/bin/sdl2-config"
