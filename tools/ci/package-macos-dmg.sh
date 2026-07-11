#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 2 ]; then
    echo "Usage: $0 <version> <binary>" >&2
    exit 2
fi

VERSION="$1"
BINARY="$2"

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT_DIR="${OUT_DIR:-$ROOT_DIR/.release/out}"
DMG_ROOT="$ROOT_DIR/.release/dmg"
APP_NAME="Pluto SDR Scanner"
APP_DIR="$DMG_ROOT/${APP_NAME}.app"
MACOS_DIR="$APP_DIR/Contents/MacOS"
RES_DIR="$APP_DIR/Contents/Resources"
RUNTIME_DIR="$RES_DIR/runtime"

rm -rf "$DMG_ROOT"
mkdir -p "$MACOS_DIR" "$RUNTIME_DIR" "$OUT_DIR"

install -m 0755 "$BINARY" "$RUNTIME_DIR/pluto-scanner"
for asset in index.html bands.ini markers.ini README.md LICENSE PLUTO.MD SPEC.MD SPECTRUM_CALC.MD; do
    cp "$ROOT_DIR/$asset" "$RUNTIME_DIR/$asset"
done

cat > "$MACOS_DIR/pluto-scanner-launcher" <<EOF
#!/bin/sh
set -eu
bundle_dir="\$(cd "\$(dirname "\$0")/.." && pwd)"
src="\$bundle_dir/Resources/runtime"
dst="\$HOME/Library/Application Support/Pluto SDR Scanner/app-${VERSION}"
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
chmod +x "$MACOS_DIR/pluto-scanner-launcher"

cat > "$APP_DIR/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleExecutable</key>
  <string>pluto-scanner-launcher</string>
  <key>CFBundleIdentifier</key>
  <string>com.ur8us.pluto-scanner</string>
  <key>CFBundleName</key>
  <string>${APP_NAME}</string>
  <key>CFBundleDisplayName</key>
  <string>${APP_NAME}</string>
  <key>CFBundlePackageType</key>
  <string>APPL</string>
  <key>CFBundleShortVersionString</key>
  <string>${VERSION}</string>
  <key>CFBundleVersion</key>
  <string>${VERSION}</string>
  <key>LSMinimumSystemVersion</key>
  <string>11.0</string>
</dict>
</plist>
EOF

hdiutil create \
    -volname "$APP_NAME $VERSION" \
    -srcfolder "$APP_DIR" \
    -ov \
    -format UDZO \
    "$OUT_DIR/pluto-scanner-${VERSION}-macos-universal.dmg"

echo "Packaged macOS DMG"
