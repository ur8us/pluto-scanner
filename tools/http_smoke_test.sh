#!/bin/sh
set -eu

BASE_URL="${BASE_URL:-http://localhost:8080}"

need_cmd() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "missing required command: $1" >&2
        exit 2
    }
}

expect_status() {
    expected="$1"
    method="$2"
    path="$3"
    body="${4:-}"
    if [ -n "$body" ]; then
        code="$(curl -sS -o /tmp/pluto-smoke-body.$$ -w '%{http_code}' \
            -X "$method" -H 'Content-Type: application/json' \
            --data "$body" "$BASE_URL$path")"
    else
        code="$(curl -sS -o /tmp/pluto-smoke-body.$$ -w '%{http_code}' \
            -X "$method" "$BASE_URL$path")"
    fi
    rm -f /tmp/pluto-smoke-body.$$
    if [ "$code" != "$expected" ]; then
        echo "$method $path returned $code, expected $expected" >&2
        exit 1
    fi
}

need_cmd curl
need_cmd python3

expect_status 200 GET /
expect_status 200 GET /api/status
expect_status 400 POST /api/fft '{"fft_size":"bad"}'
expect_status 400 POST /api/view '{"visible_start_hz":100,"visible_end_hz":50}'
expect_status 404 GET /does-not-exist

status_headers="$(mktemp)"
status_body="$(mktemp)"
trap 'rm -f "$status_headers" "$status_body"' EXIT
status_code="$(curl -sS -D "$status_headers" -o "$status_body" \
    -w '%{http_code}' "$BASE_URL/api/status")"
if [ "$status_code" != 200 ]; then
    echo "GET /api/status returned $status_code, expected 200" >&2
    exit 1
fi
python3 - "$status_headers" "$status_body" <<'PY'
import json
import sys

headers_path, body_path = sys.argv[1], sys.argv[2]
with open(body_path, "rb") as f:
    body = f.read()
with open(headers_path, "r", encoding="iso-8859-1") as f:
    headers = f.readlines()

content_lengths = [
    int(line.split(":", 1)[1].strip())
    for line in headers
    if line.lower().startswith("content-length:")
]
if not content_lengths:
    raise SystemExit("GET /api/status did not include Content-Length")
if content_lengths[-1] != len(body):
    raise SystemExit(
        f"GET /api/status Content-Length {content_lengths[-1]} "
        f"did not match body length {len(body)}"
    )

payload = json.loads(body.decode("utf-8"))
for key in ("device", "mode", "steps", "sample_rates"):
    if key not in payload:
        raise SystemExit(f"GET /api/status missing JSON key: {key}")
PY

echo "HTTP smoke checks passed for $BASE_URL"
