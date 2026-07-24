#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_ROOT="${BUILD_ROOT:-$ROOT_DIR/.release/build}"
DEPS_PREFIX="${DEPS_PREFIX:-$ROOT_DIR/.release/deps}"
LIBXML2_VERSION="${LIBXML2_VERSION:-2.12.10}"
LIBIIO_REF="${LIBIIO_REF:-v0.26}"
GENERATOR="${CMAKE_GENERATOR:-Ninja}"

mkdir -p "$BUILD_ROOT" "$DEPS_PREFIX"

fetch_tarball() {
    local url="$1"
    local archive="$2"
    local partial="${archive}.part"

    if [ -f "$archive" ]; then
        return
    fi

    # Upstream mirrors and GitHub can transiently return 5xx responses. Keep
    # failed downloads out of the cache so a later build never extracts a
    # truncated archive as if it were complete.
    rm -f "$partial"
    curl -fsSL \
        --connect-timeout 30 \
        --max-time 300 \
        --retry 6 \
        --retry-delay 5 \
        --retry-max-time 300 \
        "$url" -o "$partial"
    mv "$partial" "$archive"
}

extract_once() {
    local archive="$1"
    local dest="$2"
    if [ ! -d "$dest" ]; then
        mkdir -p "$dest"
        tar -xf "$archive" -C "$dest" --strip-components=1
    fi
}

cmake_arch_args=()
if [ -n "${CMAKE_OSX_ARCHITECTURES:-}" ]; then
    cmake_arch_args+=("-DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}")
fi
if [ -n "${CMAKE_OSX_DEPLOYMENT_TARGET:-}" ]; then
    cmake_arch_args+=("-DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}")
fi

libiio_c_flags="${CFLAGS:-}"
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        # libxml2's Windows headers use dllimport unless static consumers define this.
        libiio_c_flags="${libiio_c_flags} -DLIBXML_STATIC"
        ;;
esac
libiio_c_flags_args=()
if [ -n "$libiio_c_flags" ]; then
    libiio_c_flags_args+=("-DCMAKE_C_FLAGS=${libiio_c_flags}")
fi

libxml_minor="${LIBXML2_VERSION%.*}"
libxml_archive="$BUILD_ROOT/libxml2-${LIBXML2_VERSION}.tar.xz"
libxml_src="$BUILD_ROOT/libxml2-${LIBXML2_VERSION}"
fetch_tarball \
    "https://download.gnome.org/sources/libxml2/${libxml_minor}/libxml2-${LIBXML2_VERSION}.tar.xz" \
    "$libxml_archive"
extract_once "$libxml_archive" "$libxml_src"

cmake -S "$libxml_src" -B "$BUILD_ROOT/libxml2-build" -G "$GENERATOR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$DEPS_PREFIX" \
    -DBUILD_SHARED_LIBS=OFF \
    -DLIBXML2_WITH_ICONV=OFF \
    -DLIBXML2_WITH_ICU=OFF \
    -DLIBXML2_WITH_LZMA=OFF \
    -DLIBXML2_WITH_ZLIB=OFF \
    -DLIBXML2_WITH_HTTP=OFF \
    -DLIBXML2_WITH_PYTHON=OFF \
    -DLIBXML2_WITH_PROGRAMS=OFF \
    -DLIBXML2_WITH_TESTS=OFF \
    ${cmake_arch_args[@]+"${cmake_arch_args[@]}"}
cmake --build "$BUILD_ROOT/libxml2-build" --config Release --parallel
cmake --install "$BUILD_ROOT/libxml2-build" --config Release

libiio_archive="$BUILD_ROOT/libiio-${LIBIIO_REF}.tar.gz"
libiio_src="$BUILD_ROOT/libiio-${LIBIIO_REF}"
fetch_tarball \
    "https://github.com/analogdevicesinc/libiio/archive/refs/tags/${LIBIIO_REF}.tar.gz" \
    "$libiio_archive"
extract_once "$libiio_archive" "$libiio_src"

cmake -S "$libiio_src" -B "$BUILD_ROOT/libiio-build" -G "$GENERATOR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$DEPS_PREFIX" \
    -DCMAKE_PREFIX_PATH="$DEPS_PREFIX" \
    -DBUILD_SHARED_LIBS=OFF \
    -DOSX_FRAMEWORK=OFF \
    -DOSX_PACKAGE=OFF \
    -DLIBXML2_LIBRARY="$DEPS_PREFIX/lib/libxml2.a" \
    -DLIBXML2_INCLUDE_DIR="$DEPS_PREFIX/include/libxml2" \
    -DWITH_NETWORK_BACKEND=ON \
    -DWITH_XML_BACKEND=ON \
    -DWITH_USB_BACKEND=OFF \
    -DWITH_SERIAL_BACKEND=OFF \
    -DWITH_LOCAL_BACKEND=OFF \
    -DWITH_IIOD=OFF \
    -DWITH_TESTS=OFF \
    -DWITH_EXAMPLES=OFF \
    -DWITH_DOC=OFF \
    -DWITH_MAN=OFF \
    -DWITH_ZSTD=OFF \
    -DHAVE_DNS_SD=OFF \
    -DCPP_BINDINGS=OFF \
    -DCSHARP_BINDINGS=OFF \
    -DPYTHON_BINDINGS=OFF \
    ${libiio_c_flags_args[@]+"${libiio_c_flags_args[@]}"} \
    ${cmake_arch_args[@]+"${cmake_arch_args[@]}"}
cmake --build "$BUILD_ROOT/libiio-build" --config Release --parallel
cmake --install "$BUILD_ROOT/libiio-build" --config Release

echo "Static dependencies installed in $DEPS_PREFIX"
