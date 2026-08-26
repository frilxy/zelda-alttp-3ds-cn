#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
DEVKITARM="${DEVKITARM:-${DEVKITPRO}/devkitARM}"
SDL_BUILD="${ROOT}/build-3ds/sdl"
SDL_PREFIX="${ROOT}/build-3ds/prefix"
# Build a separate "bottom-screen Chinese" version when BOTTOM_SCREEN_CN=1.
SUFFIX=""
BSCN_FLAG=OFF
if [[ "${BOTTOM_SCREEN_CN:-}" == "1" ]]; then
  SUFFIX="-cn-bottom"
  BSCN_FLAG=ON
fi
GAME_BUILD="${ROOT}/build-3ds${SUFFIX}/game"
OUT_BASE="zelda3-3ds-v3.0-E3${SUFFIX}"
TOOLS_ROOT="${ZELDA3_TOOLS_ROOT:-${ROOT}/../../Tools/bin}"

export DEVKITPRO DEVKITARM

if [[ ! -f "${SDL_PREFIX}/lib/cmake/SDL2/SDL2Config.cmake" ]]; then
  cmake \
    -S "${ROOT}/app/jni/SDL2" \
    -B "${SDL_BUILD}" \
    -DCMAKE_TOOLCHAIN_FILE="${DEVKITPRO}/cmake/3DS.cmake" \
    -DCMAKE_INSTALL_PREFIX="${SDL_PREFIX}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DSDL_SHARED=OFF \
    -DSDL_STATIC=ON \
    -DSDL_TEST=OFF
fi
cmake --build "${SDL_BUILD}" --parallel
cmake --install "${SDL_BUILD}"

cmake \
  -S "${ROOT}/platform/3ds" \
  -B "${GAME_BUILD}" \
  -DCMAKE_TOOLCHAIN_FILE="${DEVKITPRO}/cmake/3DS.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DSDL2_ROOT="${SDL_PREFIX}" \
  -DBOTTOM_SCREEN_CN="${BSCN_FLAG}"
cmake --build "${GAME_BUILD}" --parallel

# CMake pegs the .3dsx name at zelda3-3ds-v3.0-E3.3dsx; rename it so the
# bottom-screen-CN build doesn't collide with the standard build in the release.
if [[ "${SUFFIX}" != "" && -f "${GAME_BUILD}/zelda3-3ds-v3.0-E3.3dsx" ]]; then
  mv "${GAME_BUILD}/zelda3-3ds-v3.0-E3.3dsx" "${GAME_BUILD}/${OUT_BASE}.3dsx"
fi

MAKEROM="${MAKEROM:-${TOOLS_ROOT}/makerom}"
BANNERTOOL="${BANNERTOOL:-${TOOLS_ROOT}/bannertool}"
if [[ ! -x "${MAKEROM}" || ! -x "${BANNERTOOL}" ]]; then
  printf '3DSX listo. Para crear la CIA define MAKEROM y BANNERTOOL.\n'
  exit 0
fi

"${BANNERTOOL}" makesmdh \
  -s "Zelda 3DS EXP 3" \
  -l "A Link to the Past 3DS experimental 3" \
  -p "EstebanPdN" \
  -i "${ROOT}/platform/3ds/assets/icon.png" \
  -f visible,nosavebackups \
  -o "${GAME_BUILD}/zelda3-3ds.icn"

"${BANNERTOOL}" makebanner \
  -ci "${ROOT}/platform/3ds/assets/banner.cgfx" \
  -a "${ROOT}/platform/3ds/assets/banner.wav" \
  -o "${GAME_BUILD}/zelda3-3ds.bnr"

(
  cd "${ROOT}"
  "${MAKEROM}" \
    -f cia \
    -o "${GAME_BUILD}/${OUT_BASE}.cia" \
    -DAPP_ROMFS="build-3ds${SUFFIX}/game/romfs" \
    -rsf platform/3ds/cia/zelda3.rsf \
    -target t \
    -exefslogo \
    -elf "build-3ds${SUFFIX}/game/zelda3-3ds.elf" \
    -icon "build-3ds${SUFFIX}/game/zelda3-3ds.icn" \
    -banner "build-3ds${SUFFIX}/game/zelda3-3ds.bnr"
)

printf 'Listos:\n'
printf '  %s\n' "${GAME_BUILD}/${OUT_BASE}.3dsx"
printf '  %s\n' "${GAME_BUILD}/${OUT_BASE}.cia"
