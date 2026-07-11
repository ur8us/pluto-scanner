#!/bin/sh
set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR"
PLUTO_URI="${PLUTO_URI:-ip:pluto.local}"
exec "$DIR/pluto-scanner" --uri "$PLUTO_URI" "$@"
