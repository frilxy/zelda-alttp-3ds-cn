#!/usr/bin/env bash
#
# Cross-compile the three libraries the in-game updater needs -- curl 8.4.0,
# mbedTLS 2.28.8 and Jansson 2.14 -- for the 3DS with devkitARM, and install
# them into PREFIX so that platform/3ds/CMakeLists.txt finds them through
# -DUPDATE_DEPS_ROOT=PREFIX (libcurl.a, libjansson.a, libmbedtls.a,
# libmbedx509.a, libmbedcrypto.a + matching headers).
#
# This is the offline fallback for CI runs where the devkitPro pacman
# repository (which ships the prebuilt 3ds-curl / 3ds-mbedtls / 3ds-jansson
# packages) cannot be reached.  Versions, source URLs, SHA-256 digests and the
# devkitPro patches mirror sources.json / *.patch in this directory.
#
# usage: build-deps.sh [PREFIX]
#
# environment:
#   DEVKITPRO   devkitPro root            (default /opt/devkitpro)
#   DEPS_WORK   scratch build directory   (default ./.deps/work)
#   DEPS_JOBS   parallel make/cmake jobs  (default: nproc)
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX="${1:-${PWD}/.deps/prefix}"
WORK="${DEPS_WORK:-${PWD}/.deps/work}"
DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
TOOLCHAIN="${DEVKITPRO}/cmake/3DS.cmake"
LOCAL="${DEVKITPRO}/devkitARM"          # arm-none-eabi-* lives in bin/
DL="${WORK}/download"
SRC="${WORK}/src"
BLD="${WORK}/build"
JOBS="${DEPS_JOBS:-$(nproc 2>/dev/null || echo 4)}"

# 3DS ABI flags.  The CMake toolchain file supplies these for CMake projects;
# curl's autotools build needs them spelled out.  -D__3DS__ is what the
# devkitPro patches key off.
ARMFLAGS="-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -D__3DS__"
CTRU_INC="-I${DEVKITPRO}/libctru/include"
CTRU_LIB="-L${DEVKITPRO}/libctru/lib"

MBEDTLS_VER=2.28.8
MBEDTLS_URL="https://codeload.github.com/Mbed-TLS/mbedtls/tar.gz/refs/tags/v${MBEDTLS_VER}"
MBEDTLS_SHA=4fef7de0d8d542510d726d643350acb3cdb9dc76ad45611b59c9aa08372b4213
CURL_VER=8.4.0
CURL_URL="https://curl.se/download/curl-${CURL_VER}.tar.xz"
CURL_SHA=16c62a9c4af0f703d28bda6d7bbf37ba47055ad3414d70dec63e2e6336f2a82d
JANSSON_VER=2.14
JANSSON_URL="https://codeload.github.com/akheron/jansson/tar.gz/refs/tags/v${JANSSON_VER}"
JANSSON_SHA=c739578bf6b764aa0752db9a2fdadcfe921c78f1228c7ec0bb47fa804c55d17b

log()  { printf '\n=== %s\n' "$*"; }
fail() { printf 'error: %s\n' "$*" >&2; exit 1; }

fetch() { # fetch <file> <url> <sha256>
  local file="$1" url="$2" sha="$3" dest="${DL}/$1"
  if ! printf '%s  %s\n' "$sha" "$dest" | sha256sum -c --status - 2>/dev/null; then
    log "downloading ${file}"
    curl -fsSL --retry 3 --retry-delay 3 -o "${dest}.part" "$url"
    mv "${dest}.part" "$dest"
  fi
  printf '%s  %s\n' "$sha" "$dest" | sha256sum -c - >/dev/null ||
    fail "${file}: SHA-256 mismatch"
}

apply_patch() { # apply_patch <srcdir> <patchfile>
  log "applying $(basename "$2")"
  ( cd "$1" && patch -p1 --forward --batch < "$2" )
}

for tool in curl sha256sum patch tar cmake make; do
  command -v "$tool" >/dev/null || fail "required tool '$tool' not found"
done
[[ -f "$TOOLCHAIN" ]] || fail "3DS CMake toolchain not found at $TOOLCHAIN"
export PATH="${LOCAL}/bin:${PATH}"

rm -rf "$BLD"
mkdir -p "$DL" "$SRC" "$BLD" "$PREFIX"

fetch mbedtls.tar.gz "$MBEDTLS_URL" "$MBEDTLS_SHA"
fetch curl.tar.xz    "$CURL_URL"    "$CURL_SHA"
fetch jansson.tar.gz "$JANSSON_URL" "$JANSSON_SHA"

# ---------------------------------------------------------------------------
# mbedTLS  --  static libmbedtls/libmbedx509/libmbedcrypto
# ---------------------------------------------------------------------------
log "mbedTLS ${MBEDTLS_VER}"
tar xf "${DL}/mbedtls.tar.gz" -C "$SRC"
MBEDTLS_SRC="${SRC}/mbedtls-${MBEDTLS_VER}"
apply_patch "$MBEDTLS_SRC" "${HERE}/mbedtls.patch"

# Adjust the stock configuration from the outside (mbedTLS_USER_CONFIG_FILE is
# appended to include/mbedtls/config.h): take the RNG from libctru via the
# mbedtls_hardware_poll() added by mbedtls.patch, and drop the bits that do not
# fit a 3DS build.
USERCFG="${WORK}/3ds_mbedtls_user_config.h"
cat > "$USERCFG" <<'EOF'
/* 3DS (devkitARM) tuning for mbedTLS. Included at the end of mbedtls/config.h
   through MBEDTLS_USER_CONFIG_FILE. */
#ifndef ZELDA3_3DS_MBEDTLS_USER_CONFIG_H
#define ZELDA3_3DS_MBEDTLS_USER_CONFIG_H

/* Entropy: only libctru's sslcGenerateRandomData() (mbedtls.patch). */
#define MBEDTLS_ENTROPY_HARDWARE_ALT
#define MBEDTLS_NO_PLATFORM_ENTROPY

/* TLS 1.2 PSK/ECDHE suites want CMAC. */
#define MBEDTLS_CMAC_C

/* Not usable / not wanted on the 3DS. */
#undef MBEDTLS_SELF_TEST
#undef MBEDTLS_TIMING_C
#undef MBEDTLS_TIMING_ALT
#undef MBEDTLS_NET_C
#undef MBEDTLS_HAVE_TIME_DATE

#endif
EOF

cmake -S "$MBEDTLS_SRC" -B "${BLD}/mbedtls" \
  -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DCMAKE_C_FLAGS="-DMBEDTLS_USER_CONFIG_FILE=\"3ds_mbedtls_user_config.h\" -I${WORK}" \
  -DENABLE_TESTING=OFF \
  -DENABLE_PROGRAMS=OFF \
  -DENABLE_ZLIB_SUPPORT=OFF \
  -DLINK_WITH_PTHREAD=OFF \
  -DMBEDTLS_FATAL_WARNINGS=OFF \
  -DUSE_SHARED_MBEDTLS_LIBRARY=OFF \
  -DUSE_STATIC_MBEDTLS_LIBRARY=ON
cmake --build "${BLD}/mbedtls" --parallel "$JOBS"
cmake --install "${BLD}/mbedtls"

# ---------------------------------------------------------------------------
# Jansson  --  static libjansson
# ---------------------------------------------------------------------------
log "Jansson ${JANSSON_VER}"
tar xf "${DL}/jansson.tar.gz" -C "$SRC"
JANSSON_SRC="${SRC}/jansson-${JANSSON_VER}"

cmake -S "$JANSSON_SRC" -B "${BLD}/jansson" \
  -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DBUILD_SHARED_LIBS=OFF \
  -DJANSSON_BUILD_SHARED_LIBS=OFF \
  -DJANSSON_BUILD_DOCS=OFF \
  -DJANSSON_EXAMPLES=OFF \
  -DJANSSON_WITHOUT_TESTS=ON
cmake --build "${BLD}/jansson" --parallel "$JOBS"
cmake --install "${BLD}/jansson"

# ---------------------------------------------------------------------------
# curl  --  static libcurl with mbedTLS, HTTP(S) only, synchronous DNS
# ---------------------------------------------------------------------------
log "curl ${CURL_VER}"
tar xf "${DL}/curl.tar.xz" -C "$SRC"
CURL_SRC="${SRC}/curl-${CURL_VER}"
apply_patch "$CURL_SRC" "${HERE}/curl.patch"

export CC="${LOCAL}/bin/arm-none-eabi-gcc"
export AR="${LOCAL}/bin/arm-none-eabi-ar"
export RANLIB="${LOCAL}/bin/arm-none-eabi-ranlib"
export STRIP="${LOCAL}/bin/arm-none-eabi-strip"
export CFLAGS="${ARMFLAGS} ${CTRU_INC} -O2 -ffunction-sections -fdata-sections"
export CPPFLAGS="-DMBEDTLS_USER_CONFIG_FILE=\"3ds_mbedtls_user_config.h\" -I${WORK} -I${PREFIX}/include"
# The ARM libctru is needed for configure's link tests (sockets, getaddrinfo).
export LDFLAGS="${ARMFLAGS} ${CTRU_LIB} -L${PREFIX}/lib"
export LIBS="-lmbedtls -lmbedx509 -lmbedcrypto -lctru -lm"
export PKG_CONFIG_PATH="${PREFIX}/lib/pkgconfig"

( cd "${BLD}" && mkdir -p curl && cd curl &&
  "${CURL_SRC}/configure" \
    --host=arm-none-eabi \
    --prefix="$PREFIX" \
    --disable-shared --enable-static \
    --with-mbedtls="$PREFIX" \
    --without-zlib --without-brotli --without-zstd \
    --without-libidn2 --without-librtmp --without-libssh2 --without-libpsl \
    --without-nghttp2 \
    --disable-ldap --disable-ldaps --disable-rtsp --disable-dict \
    --disable-telnet --disable-tftp --disable-pop3 --disable-imap \
    --disable-smtp --disable-gopher --disable-manual --disable-mqtt \
    --disable-ipv6 --disable-unix-sockets \
    --disable-threaded-resolver --disable-ntlm-wb --disable-alt-svc \
    --disable-proxy --disable-versioned-symbols \
  > "${WORK}/curl-configure.log" 2>&1 ) ||
  { tail -n 60 "${WORK}/curl-configure.log"; fail "curl configure failed"; }

grep -E "^(  )?(SSL|Protocols|Features|Host):" "${WORK}/curl-configure.log" ||
  grep -E "mbedTLS|SSL support" "${WORK}/curl-configure.log" || true

make -C "${BLD}/curl" -j"$JOBS" > "${WORK}/curl-make.log" 2>&1 ||
  { tail -n 80 "${WORK}/curl-make.log"; fail "curl build failed"; }
make -C "${BLD}/curl" install >> "${WORK}/curl-make.log" 2>&1 ||
  { tail -n 40 "${WORK}/curl-make.log"; fail "curl install failed"; }

# ---------------------------------------------------------------------------
log "installed into ${PREFIX}"
missing=0
for lib in curl jansson mbedtls mbedx509 mbedcrypto; do
  if [[ -f "${PREFIX}/lib/lib${lib}.a" ]]; then
    printf '  ok    lib%s.a\n' "$lib"
  else
    printf '  MISSING lib%s.a\n' "$lib"; missing=1
  fi
done
for hdr in curl/curl.h jansson.h mbedtls/ssl.h; do
  [[ -f "${PREFIX}/include/${hdr}" ]] || { printf '  MISSING include/%s\n' "$hdr"; missing=1; }
done
[[ "$missing" == 0 ]] || fail "dependency build incomplete"
log "done"
