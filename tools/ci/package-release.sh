#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 4 ]; then
    echo "Usage: $0 <version> <target> <binary> <tar.gz|zip>" >&2
    exit 2
fi

VERSION="$1"
TARGET="$2"
BINARY="$3"
FORMAT="$4"

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT_DIR="${OUT_DIR:-$ROOT_DIR/.release/out}"
PKG_NAME="pluto-scanner-${VERSION}-${TARGET}"
STAGE_ROOT="$ROOT_DIR/.release/package-$TARGET"
PKG_DIR="$STAGE_ROOT/$PKG_NAME"

rm -rf "$STAGE_ROOT"
mkdir -p "$PKG_DIR" "$OUT_DIR"

case "$TARGET" in
    windows-*) install -m 0755 "$BINARY" "$PKG_DIR/pluto-scanner.exe" ;;
    *) install -m 0755 "$BINARY" "$PKG_DIR/pluto-scanner" ;;
esac

for asset in index.html bands.ini markers.ini README.md LICENSE PLUTO.MD SPEC.MD SPECTRUM_CALC.MD; do
    cp "$ROOT_DIR/$asset" "$PKG_DIR/$asset"
done

cat > "$PKG_DIR/RUNNING.txt" <<'EOF'
Run from this directory so the scanner can find index.html, bands.ini, and
markers.ini next to the executable.

Default Pluto URI:
  ip:192.168.2.1

Examples:
  ./pluto-scanner
  ./pluto-scanner --uri 192.168.2.1
  ./pluto-scanner --uri ip:192.168.2.1
  PLUTO_URI=pluto.local ./pluto-scanner

Open the UI at:
  http://localhost:8080/
EOF

case "$FORMAT" in
    tar.gz)
        tar -C "$STAGE_ROOT" -czf "$OUT_DIR/$PKG_NAME.tar.gz" "$PKG_NAME"
        ;;
    zip)
        py=python3
        if ! command -v "$py" >/dev/null 2>&1; then
            py=python
        fi
        (cd "$STAGE_ROOT" && "$py" -m zipfile -c "$OUT_DIR/$PKG_NAME.zip" "$PKG_NAME")
        ;;
    *)
        echo "Unsupported package format: $FORMAT" >&2
        exit 2
        ;;
esac

echo "Packaged $PKG_NAME.$FORMAT"
