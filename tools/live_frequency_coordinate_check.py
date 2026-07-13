#!/usr/bin/env python3
"""Measure a stable live carrier from scanner SSE raw-peak metadata."""

import argparse
import base64
import json
import statistics
import sys
import time
import urllib.request


def get_json(base_url, path):
    """Fetch one JSON API response from the running local scanner."""
    with urllib.request.urlopen(base_url + path, timeout=10) as response:
        return json.loads(response.read().decode("utf-8"))


def read_live_rows(base_url, duration_s, expected_hz):
    """Read uninterrupted non-preview single-mode rows for a wall-clock duration."""
    rows = []
    view_id = None
    started = time.monotonic()
    saw_event = False
    with urllib.request.urlopen(base_url + "/api/waterfall", timeout=duration_s + 30) as response:
        while True:
            line = response.readline().decode("utf-8", "replace").strip()
            if line == "event: line":
                saw_event = True
                continue
            if not saw_event or not line.startswith("data:"):
                continue
            saw_event = False
            row = json.loads(line[5:].strip())
            if row.get("mode") != "single" or int(row.get("preview", 0)) != 0:
                continue
            row_view = int(row.get("view", -1))
            if view_id is None:
                view_id = row_view
            elif row_view != view_id:
                raise RuntimeError("view changed during live coordinate capture")
            raw_peak_hz = float(row.get("raw_peak_hz", 0.0))
            if raw_peak_hz <= 0.0:
                raise RuntimeError("backend did not provide raw_peak_hz metadata")
            packed = base64.b64decode(row["db"])
            max_level = max(packed)
            max_indices = [index for index, value in enumerate(packed) if value == max_level]
            display_peak_hz = float(row["f0"]) + (
                statistics.median(max_indices) + 0.5
            ) * (float(row["f1"]) - float(row["f0"])) / len(packed)
            rows.append(
                {
                    "elapsed_s": time.monotonic() - started,
                    "raw_peak_hz": raw_peak_hz,
                    "raw_offset_hz": raw_peak_hz - expected_hz,
                    "display_peak_hz": display_peak_hz,
                    "display_offset_hz": display_peak_hz - expected_hz,
                    "max_level": max_level,
                    "max_level_bins": len(max_indices),
                    "live_first_input_ms": int(row.get("live_first_input_ms", 0)),
                    "live_first_fft_ms": int(row.get("live_first_fft_ms", 0)),
                    "live_first_publish_ms": int(row.get("live_first_publish_ms", 0)),
                }
            )
            if rows[-1]["elapsed_s"] >= duration_s:
                break
    if not rows:
        raise RuntimeError("received no valid live SSE rows")
    return rows


def summary(rows, status, expected_hz, verbose):
    """Build a compact JSON report including raw and display-bin coordinates."""
    raw_offsets = [row["raw_offset_hz"] for row in rows]
    display_offsets = [row["display_offset_hz"] for row in rows]
    result = {
        "status": "ok",
        "expected_hz": expected_hz,
        "duration_s": rows[-1]["elapsed_s"],
        "rows": len(rows),
        "raw_peak_median_hz": statistics.median(row["raw_peak_hz"] for row in rows),
        "raw_offset_median_hz": statistics.median(raw_offsets),
        "raw_offset_min_hz": min(raw_offsets),
        "raw_offset_max_hz": max(raw_offsets),
        "raw_offset_stdev_hz": statistics.pstdev(raw_offsets),
        "display_offset_median_hz": statistics.median(display_offsets),
        "max_transport_level": max(row["max_level"] for row in rows),
        "fq_err_model_hz": status.get("fq_err_model_hz"),
        "fq_err_effective_hz": status.get("fq_err_effective_hz"),
        "mode": status.get("mode"),
        "effective_fft_size": status.get("effective_fft_size"),
        "decim_factor": status.get("decim_factor"),
        "visible_start_hz": status.get("visible_start_hz"),
        "visible_end_hz": status.get("visible_end_hz"),
    }
    if verbose:
        result["rows_detail"] = rows
    return result


def main():
    """Run the non-CI live carrier measurement against an existing scanner."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default="http://127.0.0.1:8080")
    parser.add_argument("--duration-s", type=float, default=300.0)
    parser.add_argument("--expected-hz", type=float, default=840000000.0)
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()
    if args.duration_s < 300.0:
        raise SystemExit("--duration-s must be at least 300 seconds for this live validation")

    status = get_json(args.url, "/api/status")
    if not status.get("scanning"):
        raise SystemExit("scanner is not running; start the saved configuration first")
    rows = read_live_rows(args.url, args.duration_s, args.expected_hz)
    print(json.dumps(summary(rows, status, args.expected_hz, args.verbose), indent=2))


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"live frequency coordinate check failed: {error}", file=sys.stderr)
        raise SystemExit(1)
