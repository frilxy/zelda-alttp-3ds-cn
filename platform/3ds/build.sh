#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
if [[ ! -d "${DEVKITPRO}" && -d "${HOME}/devkitpro" ]]; then
  DEVKITPRO="${HOME}/devkitpro"
fi
DEVKITARM="${DEVKITARM:-${DEVKITPRO}/devkitARM}"
BUILD_ROOT="${ZELDA3_BUILD_ROOT:-${ROOT}/build-3ds}"
mkdir -p "${BUILD_ROOT}"
BUILD_ROOT="$(cd "${BUILD_ROOT}" && pwd)"
SDL_BUILD="${BUILD_ROOT}/sdl"
SDL_PREFIX="${BUILD_ROOT}/prefix"
GAME_BUILD="${BUILD_ROOT}/game"
TOOLS_ROOT="${ZELDA3_TOOLS_ROOT:-${ROOT}/../../Tools/bin}"

export DEVKITPRO DEVKITARM

# Reconfigure each build so moved checkouts cannot silently reuse stale paths.
  cmake \
    -S "${ROOT}/third_party/SDL2" \
    -B "${SDL_BUILD}" \
    -DCMAKE_TOOLCHAIN_FILE="${DEVKITPRO}/cmake/3DS.cmake" \
    -DCMAKE_INSTALL_PREFIX="${SDL_PREFIX}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DSDL_SHARED=OFF \
    -DSDL_STATIC=ON \
    -DSDL_TEST=OFF
cmake --build "${SDL_BUILD}" --parallel
cmake --install "${SDL_BUILD}"

# ZELDA3_BUNDLED_MODS: a folder of mods to build into the RomFS (none if unset).
BUNDLED_MODS="${ZELDA3_BUNDLED_MODS:-}"
if [[ -n "${BUNDLED_MODS}" ]]; then
  BUNDLED_MODS="$(cd "${BUNDLED_MODS}" && pwd)"
fi

# ZELDA3_TEST_SCENARIOS: the developer menu's test states (none if unset).
SCENARIOS="${ZELDA3_TEST_SCENARIOS:-}"
if [[ -n "${SCENARIOS}" ]]; then
  SCENARIOS="$(cd "${SCENARIOS}" && pwd)"
fi

cmake \
  -S "${ROOT}/platform/3ds" \
  -B "${GAME_BUILD}" \
  -DCMAKE_TOOLCHAIN_FILE="${DEVKITPRO}/cmake/3DS.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DSDL2_ROOT="${SDL_PREFIX}" \
  -DSDL2_DIR="${SDL_PREFIX}/lib/cmake/SDL2" \
  -DUPDATE_DEPS_ROOT="${UPDATE_DEPS_ROOT:-${DEVKITPRO}/portlibs/3ds}" \
  -DZELDA3_BUNDLED_MODS="${BUNDLED_MODS}" \
  -DZELDA3_TEST_SCENARIOS="${SCENARIOS}" \
  -DZELDA3_VERSION_LABEL="${ZELDA3_VERSION_LABEL:-}"
# The 3DSX is made again only when the ELF or the SMDH changes, not the
# RomFS: without it, the build makes it with this run's RomFS (its mods).
rm -f "${GAME_BUILD}"/*.3dsx
cmake --build "${GAME_BUILD}" --parallel
python3 "${ROOT}/scripts/check-3dsx.py" "${GAME_BUILD}/zelda3-3ds.3dsx"
VERSION="$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' "${GAME_BUILD}/CMakeCache.txt")"
if [[ -z "${VERSION}" ]]; then
  printf 'Cannot read the configured game version.\n' >&2
  exit 1
fi

MAKEROM="${MAKEROM:-${TOOLS_ROOT}/makerom}"
BANNERTOOL="${BANNERTOOL:-${TOOLS_ROOT}/bannertool}"
if [[ ! -x "${MAKEROM}" || ! -x "${BANNERTOOL}" ]]; then
  printf '3DSX ready. Set MAKEROM and BANNERTOOL to package the CIA.\n'
  exit 0
fi

"${BANNERTOOL}" makesmdh \
  -s "The Legend of Zelda" \
  -l "A Link to the Past 3DS port" \
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
    -o "${GAME_BUILD}/zelda3-3ds-v${VERSION}.cia" \
    "-DAPP_ROMFS=${GAME_BUILD}/romfs" \
    -rsf platform/3ds/cia/zelda3.rsf \
    -target t \
    -exefslogo \
    -elf "${GAME_BUILD}/zelda3-3ds.elf" \
    -icon "${GAME_BUILD}/zelda3-3ds.icn" \
    -banner "${GAME_BUILD}/zelda3-3ds.bnr"
)

printf 'Ready:\n'
printf '  %s\n' "${GAME_BUILD}/zelda3-3ds.3dsx"
printf '  %s\n' "${GAME_BUILD}/zelda3-3ds-v${VERSION}.cia"
