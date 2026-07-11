#!/usr/bin/env python3
"""Exercise the production CIC/Hann/FFT path with a deterministic complex tone."""

import argparse
import json
import os
import re
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
RESULT_RE = re.compile(
    r"CIC tone result: checks (\d+), spectral errors (\d+), sample-order errors (\d+)"
)
METRIC_RE = re.compile(
    r"CIC tone spectrum (PASS|ERROR): frame (\d+), decim x(\d+), .*"
    r"-6 dB width (\d+) bins, distant (-?[0-9.]+) dBc"
)


def http_json(method, path, payload=None, timeout=15):
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
    """Wait until the synthetic scanner accepts HTTP requests."""
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


def read_sse_rows(count, timeout=20):
    """Read a fixed number of waterfall rows from the scanner SSE stream."""
    rows = []
    deadline = time.monotonic() + timeout
    saw_event = False
    with urllib.request.urlopen(BASE + "/api/waterfall", timeout=timeout) as response:
        while len(rows) < count and time.monotonic() < deadline:
            line = response.readline().decode("utf-8", "replace").strip()
            if line == "event: line":
                saw_event = True
            elif saw_event and line.startswith("data:"):
                rows.append(json.loads(line[5:].strip()))
                saw_event = False
    if len(rows) != count:
        raise RuntimeError(f"received {len(rows)} of {count} expected SSE rows")
    return rows


def port_is_free():
    """Return whether the scanner's fixed HTTP port is currently unused."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        return sock.connect_ex(("127.0.0.1", 8080)) != 0
    finally:
        sock.close()


def start_payload(span_hz, min_rate_lps=0):
    """Build a single-frequency request centered at 435 MHz."""
    center = 435_000_000.0
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


def run_case(
    name,
    span_hz,
    expected_decim,
    expected_overlap=1,
    min_rate_lps=0,
    fault=None,
    period=0,
    rows=3,
):
    """Run one clean or deliberately corrupted synthetic CIC case."""
    if not port_is_free():
        raise RuntimeError("TCP port 8080 is already in use")

    with tempfile.TemporaryDirectory(prefix="pluto-cic-tone-") as temp_dir:
        binary = os.path.join(temp_dir, "pluto-scanner-cic-test")
        shutil.copy2(TEST_BINARY, binary)
        env = os.environ.copy()
        if fault:
            env["PLUTO_SYNTHETIC_FAULT"] = fault
            env["PLUTO_SYNTHETIC_FAULT_PERIOD"] = str(period)
        process = subprocess.Popen(
            [binary],
            cwd=temp_dir,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        output = ""
        try:
            wait_for_server(process)
            plan = http_json(
                "POST", "/api/start", start_payload(span_hz, min_rate_lps)
            )
            if plan.get("status") != "ok":
                raise RuntimeError(f"start failed: {plan}")
            if int(plan.get("decim_factor", 0)) != expected_decim:
                raise RuntimeError(
                    f"planned x{plan.get('decim_factor')} instead of x{expected_decim}"
                )
            if int(round(float(plan.get("overlap_factor", 0)))) != expected_overlap:
                raise RuntimeError(
                    f"planned overlap {plan.get('overlap_factor')} instead of "
                    f"{expected_overlap}"
                )
            if int(plan.get("decim_hop", 0)) * expected_overlap != int(
                plan.get("effective_fft_size", 0)
            ):
                raise RuntimeError(f"invalid FFT/hop/overlap identity: {plan}")
            received = read_sse_rows(rows)
            http_json("POST", "/api/stop", {})
            process.send_signal(signal.SIGINT)
            output = process.communicate(timeout=10)[0]
        finally:
            if process.poll() is None:
                process.kill()
                output += process.communicate(timeout=5)[0]

    results = RESULT_RE.findall(output)
    metrics = METRIC_RE.findall(output)
    if not results:
        raise RuntimeError(f"{name}: missing final tone result\n{output[-4000:]}")
    checks, spectral_errors, order_errors = (int(value) for value in results[-1])
    if checks < rows or len(metrics) < rows:
        raise RuntimeError(f"{name}: only {checks} checks and {len(metrics)} metrics")
    if any(int(row.get("decim_factor", 0)) != expected_decim for row in received):
        raise RuntimeError(f"{name}: SSE decimation changed")
    if any(
        int(round(float(row.get("overlap_factor", 0.0)))) != expected_overlap
        for row in received
    ):
        raise RuntimeError(f"{name}: SSE overlap did not match plan")

    if fault is None:
        if spectral_errors != 0 or order_errors != 0:
            raise RuntimeError(
                f"{name}: clean stream had {spectral_errors} spectral and "
                f"{order_errors} order errors"
            )
        if any(status != "PASS" or int(width) > 3 or float(dbc) > -70.0
               for status, _, _, width, dbc in metrics):
            raise RuntimeError(f"{name}: clean tone exceeded Hann leakage limits")
    else:
        if order_errors == 0:
            raise RuntimeError(f"{name}: injected {fault} was not detected by range checks")
        if spectral_errors == 0:
            raise RuntimeError(f"{name}: periodic {fault} did not fail spectral checks")

    distant_values = [float(metric[4]) for metric in metrics]
    return {
        "name": name,
        "decim": expected_decim,
        "min_rate_lps": min_rate_lps,
        "overlap": expected_overlap,
        "fault": fault or "none",
        "checks": checks,
        "spectral_errors": spectral_errors,
        "sample_order_errors": order_errors,
        "best_distant_dbc": min(distant_values),
        "worst_distant_dbc": max(distant_values),
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()

    if not os.path.isfile(TEST_BINARY):
        raise RuntimeError(f"missing {TEST_BINARY}; run make pluto-scanner-cic-test")

    cases = [
        run_case("clean_x2", 40_960.0, 2),
        run_case("clean_x2_min20", 40_960.0, 2, 2, 20),
        run_case("clean_x64", 4096.0e6 / 3_000_000.0, 64),
        run_case("clean_x64_min1", 4096.0e6 / 3_000_000.0, 64, 4, 1),
        run_case("clean_x64_min2", 4096.0e6 / 3_000_000.0, 64, 8, 2),
        run_case("clean_x64_min5", 4096.0e6 / 3_000_000.0, 64, 16, 5),
        run_case("clean_x64_min10", 4096.0e6 / 3_000_000.0, 64, 32, 10),
        run_case("clean_x64_min20", 4096.0e6 / 3_000_000.0, 64, 64, 20),
        run_case("clean_x256", 409.6, 256, rows=2),
        run_case("clean_x256_min20", 409.6, 256, 256, 20, rows=2),
        run_case("periodic_skip_x64", 4096.0e6 / 3_000_000.0, 64,
                 fault="skip", period=1000, rows=2),
        run_case("periodic_duplicate_x64", 4096.0e6 / 3_000_000.0, 64,
                 fault="duplicate", period=1000, rows=2),
    ]
    result = {"status": "ok", "cases": cases}
    if not args.quiet:
        print(json.dumps(result, indent=2))


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"CIC synthetic signal check failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
