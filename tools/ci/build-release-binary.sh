#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DEPS_PREFIX="${DEPS_PREFIX:-$ROOT_DIR/.release/deps}"
OUT="${OUT:-$ROOT_DIR/.release/bin/pluto-scanner${EXE_EXT:-}}"
CC="${CC:-cc}"
STATIC_MODE="${STATIC_MODE:-full}"

mkdir -p "$(dirname "$OUT")"

export PKG_CONFIG_PATH="$DEPS_PREFIX/lib/pkgconfig:$DEPS_PREFIX/lib64/pkgconfig:${PKG_CONFIG_PATH:-}"

pkg_cflags="$(pkg-config --cflags libiio libxml-2.0)"
pkg_libs="$(pkg-config --libs --static libiio libxml-2.0)"

ldflags=()
case "$STATIC_MODE" in
    full)
        ldflags+=("-static")
        ;;
    deps)
        ;;
    *)
        echo "Unknown STATIC_MODE: $STATIC_MODE" >&2
        exit 2
        ;;
esac

extra_libs=()
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        extra_libs+=("-lws2_32" "-liphlpapi")
        ;;
    Darwin)
        extra_libs+=("-lpthread")
        ;;
    *)
        extra_libs+=("-pthread" "-lm")
        ;;
esac

# shellcheck disable=SC2086
"$CC" -std=c99 -Wall -Wextra -Wformat-security -O2 \
    ${CFLAGS:-} $pkg_cflags \
    -o "$OUT" "$ROOT_DIR/main.c" \
    ${LDFLAGS:-} "${ldflags[@]}" $pkg_libs "${extra_libs[@]}"

file "$OUT" || true

if [ "${VERIFY_STATIC:-0}" = "1" ] && command -v ldd >/dev/null 2>&1; then
    if ldd "$OUT" 2>&1 | grep -Eq 'libiio|libxml2|not found'; then
        echo "Unexpected dynamic libiio/libxml2 dependency in $OUT" >&2
        ldd "$OUT" || true
        exit 1
    fi
fi

if [ "$(uname -s)" = "Darwin" ] && command -v otool >/dev/null 2>&1; then
    if otool -L "$OUT" | grep -Eq 'libiio|libxml2'; then
        echo "Unexpected dynamic libiio/libxml2 dependency in $OUT" >&2
        otool -L "$OUT"
        exit 1
    fi
fi

echo "Built $OUT"
