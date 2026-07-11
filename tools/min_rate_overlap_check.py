#!/usr/bin/env python3
"""Verify that MIN WATERFALL RATE uses CIC overlap without changing resolution."""

import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request


BASE = "http://127.0.0.1:8080"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_BINARY = os.path.join(ROOT, "pluto-scanner-cic-test")


def http_json(method, path, payload=None, timeout=20):
    """Send one scanner JSON request and decode its response."""
    data = None if payload is None else json.dumps(payload).encode("utf-8")
    request = urllib.request.Request(
        BASE + path,
        data=data,
        headers={"Content-Type": "application/json"},
        method=method,
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return json.loads(response.read().decode("utf-8"))


def wait_for_server(process, timeout=10):
    """Wait for the copied synthetic backend to accept HTTP requests."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError("synthetic scanner exited during startup")
        try:
            http_json("GET", "/api/status", timeout=1)
            return
        except Exception:
            time.sleep(0.05)
    raise RuntimeError("synthetic scanner did not start")


def read_one_sse_row(timeout=20):
    """Read one waterfall SSE row from the backend."""
    deadline = time.monotonic() + timeout
    saw_event = False
    with urllib.request.urlopen(BASE + "/api/waterfall", timeout=timeout) as response:
        while time.monotonic() < deadline:
            line = response.readline().decode("utf-8", "replace").strip()
            if line == "event: line":
                saw_event = True
            elif saw_event and line.startswith("data:"):
                return json.loads(line[5:].strip())
    raise RuntimeError("timed out waiting for one SSE row")


def port_is_free():
    """Return whether the scanner's fixed HTTP port is currently unused."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        return sock.connect_ex(("127.0.0.1", 8080)) != 0
    finally:
        sock.close()


def start_payload(min_rate_lps=0):
    """Build a deep x64 single-frequency request centered at 435 MHz."""
    center = 435_000_000.0
    span_hz = 4096.0e6 / 3_000_000.0
    return {
        "freq_start": 430,
        "freq_end": 440,
        "converter_freq": 0,
        "samplerate": 61_440_000,
        "rf_bandwidth": 20_000_000,
        "bw_ratio": 0.85,
        "gain_mode": "manual",
        "hardwaregain_db": 20,
        "fft_size": 1024,
        "display_bins": 1024,
        "rate_limit_lps": 100,
        "min_rate_lps": min_rate_lps,
        "visible_start_hz": center - span_hz * 0.5,
        "visible_end_hz": center + span_hz * 0.5,
    }


def int_field(payload, name):
    """Read a numeric JSON field as an integer."""
    return int(round(float(payload.get(name, 0))))


def assert_plan(label, payload, baseline, expected_min, expected_overlap):
    """Assert that one plan preserves resolution and only changes overlap."""
    if payload.get("mode") != "single":
        raise RuntimeError(f"{label}: expected single mode, got {payload.get('mode')}")
    if int_field(payload, "min_rate_lps") != expected_min:
        raise RuntimeError(f"{label}: min rate was not applied: {payload}")
    if int_field(payload, "decim_factor") != int_field(baseline, "decim_factor"):
        raise RuntimeError(f"{label}: CIC decimation changed: {payload}")
    if int_field(payload, "effective_fft_size") != int_field(baseline, "effective_fft_size"):
        raise RuntimeError(f"{label}: effective FFT changed: {payload}")
    if "active_samplerate" in payload and int_field(payload, "active_samplerate") != int_field(baseline, "active_samplerate"):
        raise RuntimeError(f"{label}: active sample rate changed: {payload}")
    if "active_rf_bandwidth" in payload and int_field(payload, "active_rf_bandwidth") != int_field(baseline, "active_rf_bandwidth"):
        raise RuntimeError(f"{label}: active RF bandwidth changed: {payload}")
    overlap = int(round(float(payload.get("overlap_factor", 0.0))))
    if overlap != expected_overlap:
        raise RuntimeError(f"{label}: overlap {overlap} != {expected_overlap}: {payload}")
    if int_field(payload, "decim_hop") * expected_overlap != int_field(payload, "effective_fft_size"):
        raise RuntimeError(f"{label}: FFT/hop/overlap identity failed: {payload}")
    expected_flag = 1 if expected_overlap > 1 else 0
    if int_field(payload, "minimum_rate_overlap") != expected_flag:
        raise RuntimeError(f"{label}: minimum_rate_overlap flag mismatch: {payload}")
    if int_field(payload, "minimum_rate_achieved") != 1:
        raise RuntimeError(f"{label}: minimum rate not achieved: {payload}")


def run_check():
    """Run the overlap API/SSE validation against a private synthetic backend."""
    if not os.path.isfile(TEST_BINARY):
        raise RuntimeError(f"missing {TEST_BINARY}; run make pluto-scanner-cic-test")
    if not port_is_free():
        raise RuntimeError("TCP port 8080 is already in use")

    with tempfile.TemporaryDirectory(prefix="pluto-min-rate-") as temp_dir:
        binary = os.path.join(temp_dir, "pluto-scanner-cic-test")
        shutil.copy2(TEST_BINARY, binary)
        process = subprocess.Popen(
            [binary],
            cwd=temp_dir,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        output = ""
        try:
            wait_for_server(process)
            baseline = http_json("POST", "/api/start", start_payload(0))
            if baseline.get("status") != "ok":
                raise RuntimeError(f"start failed: {baseline}")
            if int_field(baseline, "decim_factor") != 64:
                raise RuntimeError(f"expected x64 CIC plan, got {baseline}")
            assert_plan("start_min0", baseline, baseline, 0, 1)

            cases = [
                (0, 1),
                (1, 4),
                (2, 8),
                (5, 16),
                (10, 32),
                (20, 64),
            ]
            results = []
            for min_rate, expected_overlap in cases:
                api_plan = http_json(
                    "POST", "/api/min-rate", {"min_rate_lps": min_rate}
                )
                assert_plan(
                    f"api_min{min_rate}",
                    api_plan,
                    baseline,
                    min_rate,
                    expected_overlap,
                )
                row = read_one_sse_row()
                row["min_rate_lps"] = min_rate
                assert_plan(
                    f"sse_min{min_rate}",
                    row,
                    baseline,
                    min_rate,
                    expected_overlap,
                )
                results.append(
                    {
                        "min_rate_lps": min_rate,
                        "fft": int_field(api_plan, "effective_fft_size"),
                        "decim": int_field(api_plan, "decim_factor"),
                        "hop": int_field(api_plan, "decim_hop"),
                        "overlap": expected_overlap,
                        "raw_line_rate": float(api_plan.get("raw_line_rate", 0.0)),
                    }
                )
            http_json("POST", "/api/stop", {})
            process.send_signal(signal.SIGINT)
            output = process.communicate(timeout=10)[0]
        finally:
            if process.poll() is None:
                process.kill()
                output += process.communicate(timeout=5)[0]

    return {"status": "ok", "cases": results, "log_tail": output[-1000:]}


def main():
    try:
        print(json.dumps(run_check(), indent=2))
    except Exception as exc:
        print(f"min-rate overlap check failed: {exc}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
