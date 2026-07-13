#!/usr/bin/env python3
import json
import os
import sys
import time
import urllib.request

BASE = os.environ.get("BASE_URL", "http://localhost:8080")


def post_json(path, payload):
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        BASE + path,
        data=data,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=10) as resp:
        return json.loads(resp.read().decode("utf-8"))


def get_json(path):
    with urllib.request.urlopen(BASE + path, timeout=10) as resp:
        return json.loads(resp.read().decode("utf-8"))


def read_one_sse(timeout_s=10):
    start = time.monotonic()
    saw_event = False
    with urllib.request.urlopen(BASE + "/api/waterfall", timeout=timeout_s) as resp:
        while time.monotonic() - start < timeout_s:
            line = resp.readline().decode("utf-8", "replace").strip()
            if line == "event: line":
                saw_event = True
            elif line.startswith("data:") and saw_event:
                return json.loads(line[5:].strip())
    raise TimeoutError("no SSE line event received")


def run_case(name, payload, expect_mode):
    print(f"starting {name}", file=sys.stderr, flush=True)
    result = post_json("/api/start", payload)
    if result.get("status") != "ok":
        raise RuntimeError(f"{name}: start failed: {result}")
    row = read_one_sse()
    print(f"received {name} SSE row", file=sys.stderr, flush=True)
    status = get_json("/api/status")
    if row.get("mode") != expect_mode:
        raise RuntimeError(f"{name}: row mode {row.get('mode')} != {expect_mode}")
    if status.get("active_mode") != expect_mode:
        raise RuntimeError(f"{name}: active mode {status.get('active_mode')} != {expect_mode}")
    if row.get("source_bins") != row.get("display_bins"):
        raise RuntimeError(
            f"{name}: published source bins {row.get('source_bins')} != display bins {row.get('display_bins')}"
        )
    if float(status.get("rf_bandwidth", 0)) >= float(status.get("samplerate", 1)):
        raise RuntimeError(
            f"{name}: RF bandwidth {status.get('rf_bandwidth')} must be less than sample rate {status.get('samplerate')}"
        )
    if expect_mode == "single":
        samplerate = float(status.get("samplerate", 0))
        rf_bandwidth = float(status.get("rf_bandwidth", 0))
        if samplerate > 0 and rf_bandwidth > samplerate * 0.5 + 1.0:
            raise RuntimeError(
                f"{name}: single-mode displayed RF bandwidth {rf_bandwidth} exceeds half sample rate {samplerate}"
            )
        if float(status.get("zero_if_guard_hz", 0)) <= 0:
            raise RuntimeError(f"{name}: missing single-mode zero IF guard")
        if float(status.get("second_if_hz", 0)) <= 0:
            raise RuntimeError(f"{name}: missing single-mode second IF")
    return {
        "name": name,
        "mode": row.get("mode"),
        "steps": status.get("steps"),
        "display_bins": row.get("display_bins"),
        "source_bins": row.get("source_bins"),
        "raw_source_bins": row.get("raw_source_bins"),
        "samplerate": status.get("samplerate"),
        "rf_bandwidth": status.get("rf_bandwidth"),
        "second_if_hz": status.get("second_if_hz"),
        "zero_if_guard_hz": status.get("zero_if_guard_hz"),
        "rssi_db": status.get("rssi_db"),
        "input_peak": status.get("input_peak"),
        "clipped_samples": status.get("clipped_samples"),
    }


def main():
    common = {
        "samplerate": 61440000,
        "rf_bandwidth": 20000000,
        "bw_ratio": 0.85,
        "gain_mode": "manual",
        "hardwaregain_db": 20,
        "fft_size": 1024,
        "display_bins": 1024,
        "rate_limit_lps": 10,
        "min_rate_lps": 0,
    }
    cases = [
        (
            "scan_430_470",
            dict(common, freq_start=430, freq_end=470, converter_freq=0),
            "scan",
        ),
        (
            "single_435_436",
            dict(common, freq_start=435, freq_end=436, converter_freq=0),
            "single",
        ),
        (
            "fm_band_with_minus_530mhz_converter",
            dict(common, freq_start=88, freq_end=108, converter_freq=-530),
            "scan",
        ),
    ]

    results = []
    for name, payload, mode in cases:
        results.append(run_case(name, payload, mode))
    post_json("/api/stop", {})
    print(json.dumps({"status": "ok", "cases": results}, indent=2))


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"headless tester failed: {exc}", file=sys.stderr)
        sys.exit(1)
