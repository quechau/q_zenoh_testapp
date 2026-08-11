#!/usr/bin/env bash
# bootstrap.sh — fetch and build the two dependencies into third_party/prefix.
#
# Why this exists: zenoh-pico ships with TLS OFF (`Z_FEATURE_LINK_TLS 0`) and its CMake finds
# Mbed TLS through pkg-config rather than fetching it. Most distributions do not install the
# Mbed TLS headers by default, and this build must not require root. So both are built here,
# into a prefix inside the repo, and nothing outside it is touched.
#
#   ./scripts/bootstrap.sh          # ~3-5 minutes the first time, then it is a no-op
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
TP="$HERE/third_party"
PREFIX="$TP/prefix"
MBEDTLS_TAG="${MBEDTLS_TAG:-v3.6.2}"
ZENOH_PICO_TAG="${ZENOH_PICO_TAG:-1.9.0}"
# zenoh-pico only logs at compile time, so it is built VERBOSE (3) and the app filters at run
# time instead — see src/logfilter.c. Built any quieter, `--debug` could not turn anything on
# when something actually goes wrong.
ZENOH_DEBUG="${ZENOH_DEBUG:-3}"
JOBS="$(nproc 2>/dev/null || echo 4)"

mkdir -p "$TP"

if [ ! -f "$PREFIX/lib/pkgconfig/mbedtls.pc" ]; then
    echo "==> Mbed TLS $MBEDTLS_TAG"
    [ -d "$TP/mbedtls" ] || git clone --depth 1 --branch "$MBEDTLS_TAG" \
        https://github.com/Mbed-TLS/mbedtls.git "$TP/mbedtls"
    git -C "$TP/mbedtls" submodule update --init --depth 1
    cmake -S "$TP/mbedtls" -B "$TP/mbedtls/build" -DCMAKE_INSTALL_PREFIX="$PREFIX" \
          -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF \
          -DUSE_SHARED_MBEDTLS_LIBRARY=ON -DCMAKE_BUILD_TYPE=Release
    cmake --build "$TP/mbedtls/build" -j"$JOBS"
    cmake --install "$TP/mbedtls/build"
else
    echo "==> Mbed TLS already built"
fi

if [ ! -f "$PREFIX/lib/libzenohpico.so" ]; then
    echo "==> zenoh-pico $ZENOH_PICO_TAG (with TLS)"
    [ -d "$TP/zenoh-pico" ] || git clone --depth 1 --branch "$ZENOH_PICO_TAG" \
        https://github.com/eclipse-zenoh/zenoh-pico.git "$TP/zenoh-pico"
    PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" \
    cmake -S "$TP/zenoh-pico" -B "$TP/zenoh-pico/build" -DCMAKE_INSTALL_PREFIX="$PREFIX" \
          -DZ_FEATURE_LINK_TLS=1 -DZ_FEATURE_LINK_TCP=1 -DZ_FEATURE_UNICAST_PEER=1 \
          -DZENOH_DEBUG="${ZENOH_DEBUG:-1}" \
          -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release
    cmake --build "$TP/zenoh-pico/build" -j"$JOBS"
    cmake --install "$TP/zenoh-pico/build"
else
    echo "==> zenoh-pico already built"
fi

grep -q "define Z_FEATURE_LINK_TLS 1" "$PREFIX/include/zenoh-pico/config.h" \
    && echo "==> zenoh-pico has TLS enabled" \
    || { echo "!! zenoh-pico was built WITHOUT TLS — mTLS will not work"; exit 1; }
echo "==> done: $PREFIX"
