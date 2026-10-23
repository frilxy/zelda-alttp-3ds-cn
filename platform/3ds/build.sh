#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
DEVKITARM="${DEVKITARM:-${DEVKITPRO}/devkitARM}"
SDL_BUILD="${ROOT}/build-3ds/sdl"
SDL_PREFIX="${ROOT}/build-3ds/prefix"
# One build, and its artifact name keeps the -cn-bottom suffix on purpose: that is
# the exact name older installs' updaters look for, so they can upgrade in place
# instead of needing a manual reinstall.  The suffix is the only one that ever
# existed, and the new updater also matches by stem, so one pair of files serves
# both old and new installs.
SUFFIX="-cn-bottom"
BSCN_FLAG=ON
# The release version has a single source of truth: platform/3ds/CMakeLists.txt.
# It is compiled into the binary (the in-game updater compares against it) and
# used for the .3dsx name, so the CIA/3DSX asset names derived from it here can
# never drift from what the updater looks for.
VERSION="$(sed -n 's/^set(ZELDA3_3DS_VERSION "\([^"]*\)").*/\1/p' "${ROOT}/platform/3ds/CMakeLists.txt")"
if [[ -z "${VERSION}" ]]; then
  printf 'error: no ZELDA3_3DS_VERSION in platform/3ds/CMakeLists.txt\n' >&2
  exit 1
fi
# CI and the release tooling ask the script for the version instead of parsing
# CMakeLists.txt themselves, so there is exactly one parser to keep in sync.
if [[ "${1:-}" == "--print-version" ]]; then
  printf '%s\n' "${VERSION}"
  exit 0
fi
GAME_BUILD="${ROOT}/build-3ds${SUFFIX}/game"
OUT_BASE="zelda3-3ds-v${VERSION}${SUFFIX}"
TOOLS_ROOT="${ZELDA3_TOOLS_ROOT:-${ROOT}/../../Tools/bin}"

# Side-by-side test installs: a distinct title ID (and SMDH name) makes this
# CIA its own HOME menu entry instead of replacing the release. The SD data
# directory (sdmc:/3ds/Zelda 3DS) stays shared on purpose.
TITLE_ID="${ZELDA3_TITLE_ID:-0x5A141}"
PRODUCT_CODE="${ZELDA3_PRODUCT_CODE:-CTR-P-Z3D3}"
TITLE_NAME="${ZELDA3_TITLE_NAME:-Zelda 3D E3}"
SHORT_TITLE="${ZELDA3_SHORT_TITLE:-Zelda 3D E3}"

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
  -DSDL2_DIR="${SDL_PREFIX}/lib/cmake/SDL2" \
  -DUPDATE_DEPS_ROOT="${UPDATE_DEPS_ROOT:-${DEVKITPRO}/portlibs/3ds}" \
  -DBOTTOM_SCREEN_CN="${BSCN_FLAG}"
cmake --build "${GAME_BUILD}" --parallel

# CMake pegs the .3dsx name at zelda3-3ds-v<version>.3dsx; rename it so the
# bottom-screen-CN build doesn't collide with the standard build in the release.
if [[ "${SUFFIX}" != "" && -f "${GAME_BUILD}/zelda3-3ds-v${VERSION}.3dsx" ]]; then
  mv "${GAME_BUILD}/zelda3-3ds-v${VERSION}.3dsx" "${GAME_BUILD}/${OUT_BASE}.3dsx"
fi

MAKEROM="${MAKEROM:-${TOOLS_ROOT}/makerom}"
BANNERTOOL="${BANNERTOOL:-${TOOLS_ROOT}/bannertool}"
if [[ ! -x "${MAKEROM}" || ! -x "${BANNERTOOL}" ]]; then
  printf '3DSX listo. Para crear la CIA define MAKEROM y BANNERTOOL.\n'
  exit 0
fi

"${BANNERTOOL}" makesmdh \
  -s "${SHORT_TITLE}" \
  -l "${TITLE_NAME}" \
  -p "EstebanPdN" \
  -i "${ROOT}/platform/3ds/assets/icon.png" \
  -f visible,nosavebackups \
  -o "${GAME_BUILD}/zelda3-3ds.icn"

"${BANNERTOOL}" makebanner \
  -ci "${ROOT}/platform/3ds/assets/banner.cgfx" \
  -a "${ROOT}/platform/3ds/assets/banner.wav" \
  -o "${GAME_BUILD}/zelda3-3ds.bnr"

# The RSF carries the title identity; substitute it so test builds install
# alongside the release instead of replacing it.
TITLE_RSF="${GAME_BUILD}/zelda3-title.rsf"
sed -e "s|^  Title: .*|  Title: \"${TITLE_NAME}\"|" \
    -e "s|^  ProductCode: .*|  ProductCode: \"${PRODUCT_CODE}\"|" \
    -e "s|^  UniqueId: .*|  UniqueId: ${TITLE_ID}|" \
    "${ROOT}/platform/3ds/cia/zelda3.rsf" > "${TITLE_RSF}"

(
  cd "${ROOT}"
  "${MAKEROM}" \
    -f cia \
    -o "${GAME_BUILD}/${OUT_BASE}.cia" \
    -DAPP_ROMFS="build-3ds${SUFFIX}/game/romfs" \
    -rsf "${TITLE_RSF}" \
    -target t \
    -exefslogo \
    -elf "build-3ds${SUFFIX}/game/zelda3-3ds.elf" \
    -icon "build-3ds${SUFFIX}/game/zelda3-3ds.icn" \
    -banner "build-3ds${SUFFIX}/game/zelda3-3ds.bnr"
)

printf 'Listos:\n'
printf '  %s\n' "${GAME_BUILD}/${OUT_BASE}.3dsx"
printf '  %s\n' "${GAME_BUILD}/${OUT_BASE}.cia"
