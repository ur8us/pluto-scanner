#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 4 ]; then
    echo "Usage: $0 <version> <target> <binary> <appimage-arch>" >&2
    exit 2
fi

VERSION="$1"
TARGET="$2"
BINARY="$3"
APPIMAGE_ARCH="$4"

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT_DIR="${OUT_DIR:-$ROOT_DIR/.release/out}"
APPDIR="$ROOT_DIR/.release/appimage-$TARGET/AppDir"
RUNTIME_DIR="$APPDIR/usr/share/pluto-scanner"

rm -rf "$APPDIR"
mkdir -p "$RUNTIME_DIR" "$APPDIR/usr/share/applications" "$APPDIR/usr/share/icons/hicolor/scalable/apps" "$OUT_DIR"

install -m 0755 "$BINARY" "$RUNTIME_DIR/pluto-scanner"
for asset in index.html bands.ini markers.ini README.md LICENSE PLUTO.MD SPEC.MD SPECTRUM_CALC.MD; do
    cp "$ROOT_DIR/$asset" "$RUNTIME_DIR/$asset"
done

cat > "$APPDIR/AppRun" <<EOF
#!/bin/sh
set -eu
src="\${APPDIR}/usr/share/pluto-scanner"
dst="\${XDG_DATA_HOME:-\$HOME/.local/share}/pluto-scanner/app-${VERSION}"
mkdir -p "\$dst"
for f in pluto-scanner index.html bands.ini README.md LICENSE PLUTO.MD SPEC.MD SPECTRUM_CALC.MD; do
    cp -f "\$src/\$f" "\$dst/\$f"
done
if [ ! -e "\$dst/markers.ini" ]; then
    cp "\$src/markers.ini" "\$dst/markers.ini"
fi
chmod +x "\$dst/pluto-scanner"
cd "\$dst"
exec ./pluto-scanner "\$@"
EOF
chmod +x "$APPDIR/AppRun"

cat > "$APPDIR/pluto-scanner.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Pluto SDR Scanner
Comment=Browser-based ADALM-Pluto SDR spectrum and waterfall scanner
Exec=pluto-scanner
Icon=pluto-scanner
Terminal=true
Categories=Network;
EOF
cp "$APPDIR/pluto-scanner.desktop" "$APPDIR/usr/share/applications/pluto-scanner.desktop"

cat > "$APPDIR/pluto-scanner.svg" <<'EOF'
<svg xmlns="http://www.w3.org/2000/svg" width="128" height="128" viewBox="0 0 128 128">
  <rect width="128" height="128" rx="18" fill="#101820"/>
  <path d="M16 82h96" stroke="#48d1cc" stroke-width="6" stroke-linecap="round"/>
  <path d="M18 70c12-20 23-20 34 0s22 20 34 0 22-20 24-2" fill="none" stroke="#f5d547" stroke-width="7" stroke-linecap="round"/>
  <circle cx="64" cy="38" r="14" fill="#ffffff"/>
  <circle cx="64" cy="38" r="6" fill="#101820"/>
</svg>
EOF
cp "$APPDIR/pluto-scanner.svg" "$APPDIR/usr/share/icons/hicolor/scalable/apps/pluto-scanner.svg"

tool="$ROOT_DIR/.release/appimagetool-${APPIMAGE_ARCH}.AppImage"
if [ ! -f "$tool" ]; then
    curl -fsSL \
        "https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-${APPIMAGE_ARCH}.AppImage" \
        -o "$tool"
    chmod +x "$tool"
fi

out="$OUT_DIR/pluto-scanner-${VERSION}-${TARGET}.AppImage"
ARCH="$APPIMAGE_ARCH" VERSION="$VERSION" APPIMAGE_EXTRACT_AND_RUN=1 "$tool" "$APPDIR" "$out"
chmod +x "$out"

echo "Packaged $out"
