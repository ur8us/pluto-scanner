#!/usr/bin/env python3
"""Check modeled Pluto RFPLL correction in the synthetic HTTP backend."""

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


BASE = ""
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def find_test_binary():
    """Return the synthetic backend built by the normal Makefile target."""
    env_path = os.environ.get("PLUTO_CIC_TEST_BINARY")
    candidates = [env_path] if env_path else []
    candidates.extend(
        [
            os.path.join(ROOT, ".build", "tests", "pluto-scanner-cic-test"),
            os.path.join(ROOT, ".build", "tests", "pluto-scanner-cic-test.exe"),
        ]
    )
    for path in candidates:
        if path and os.path.isfile(path):
            return path
    return candidates[0]


TEST_BINARY = find_test_binary()


def http_json(method, path, payload=None, timeout=15):
    """Issue one local JSON request."""
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
    """Wait for the private synthetic backend to accept requests."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError("synthetic scanner exited during startup")
        try:
            return http_json("GET", "/api/status", timeout=1)
        except Exception:
            time.sleep(0.05)
    raise RuntimeError("synthetic scanner did not start")


def allocate_loopback_port():
    """Reserve a currently free loopback port for one private test process."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]
    finally:
        sock.close()


def start_payload(center_hz):
    """Build a narrow single-frequency request around the test coordinate."""
    # An odd span places this plan's source centre on a half-hertz boundary,
    # proving that the model includes the integer-Hz IIO write before RFPLL
    # fractional-word quantization.
    span_hz = 1001.0
    return {
        "freq_start": (center_hz - 1000.0) / 1e6,
        "freq_end": (center_hz + 1000.0) / 1e6,
        "converter_freq": 0,
        "gain_mode": "manual",
        "hardwaregain_db": 20,
        "fft_size": 1024,
        "display_bins": 1920,
        "rate_limit_lps": 20,
        "min_rate_lps": 10,
        "visible_start_hz": center_hz - span_hz * 0.5,
        "visible_end_hz": center_hz + span_hz * 0.5,
    }


def run_case(enabled):
    """Start one backend with the RFPLL model enabled or disabled."""
    global BASE
    center_hz = 840_000_123.5
    port = allocate_loopback_port()
    BASE = f"http://127.0.0.1:{port}"
    test_build_dir = os.path.dirname(os.path.abspath(TEST_BINARY))
    with tempfile.TemporaryDirectory(
        prefix="pluto-fq-coordinate-", dir=test_build_dir
    ) as temp_dir:
        binary = os.path.join(temp_dir, os.path.basename(TEST_BINARY))
        shutil.copy2(TEST_BINARY, binary)
        with open(os.path.join(temp_dir, "pluto-scanner.conf"), "w", encoding="ascii") as config:
            config.write(f"fq_err_correction = {1 if enabled else 0}\n")
        process = subprocess.Popen(
            # Keep the private executable below the test build directory.
            # MSYS2's global temporary mount can deny execution of freshly
            # copied .exe files on the GitHub Windows runner. The backend
            # loads its configuration beside the executable, so this also
            # keeps each test case isolated from local runtime state.
            [binary, "--port", str(port)],
            cwd=temp_dir,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        try:
            wait_for_server(process)
            response = http_json("POST", "/api/start", start_payload(center_hz))
            if response.get("status") != "ok":
                raise RuntimeError(f"start failed: {response}")
            status = http_json("GET", "/api/status")
            if int(status.get("fq_err_correction", -1)) != int(enabled):
                raise RuntimeError(f"wrong correction flag: {status}")
            model_hz = float(status.get("fq_err_model_hz", 0.0))
            effective_hz = float(status.get("fq_err_effective_hz", 0.0))
            visible_start = float(status["visible_start_hz"])
            visible_end = float(status["visible_end_hz"])
            display_bins = int(status["display_bins"])
            source_start = float(status["scan_start_hz"])
            source_span = float(status["source_span_hz"])
            requested_source_start = source_start - effective_hz
            actual_source_start = requested_source_start + model_hz

            # These are the same binary64 coordinate equations used by the
            # backend reducer: the spectrum line sees the actual LO, while its
            # source interval is either corrected or intentionally uncorrected.
            scale_x = (center_hz - visible_start) / (visible_end - visible_start) * display_bins
            plotted_hz = source_start + (center_hz - actual_source_start)
            spectrum_x = (plotted_hz - visible_start) / (visible_end - visible_start) * display_bins
            expected_x_error = (effective_hz - model_hz) / (visible_end - visible_start) * display_bins
            actual_x_error = spectrum_x - scale_x
            if abs(actual_x_error - expected_x_error) > 1e-6:
                raise RuntimeError(
                    "coordinate model mismatch: "
                    f"actual={actual_x_error} expected={expected_x_error}"
                )
            expected_effective = model_hz if enabled else 0.0
            if abs(effective_hz - expected_effective) > 1e-9:
                raise RuntimeError(
                    f"wrong effective correction: got {effective_hz}, "
                    f"expected {expected_effective}"
                )
            if abs(model_hz) < 0.5:
                raise RuntimeError(
                    "test view did not exercise the integer-Hz IIO request "
                    f"rounding contribution: {model_hz}"
                )
            http_json("POST", "/api/stop", {})
            return {
                "enabled": enabled,
                "model_hz": model_hz,
                "effective_hz": effective_hz,
                "scale_x": scale_x,
                "spectrum_x": spectrum_x,
                "x_error": actual_x_error,
                "source_span_hz": source_span,
            }
        finally:
            if process.poll() is None:
                process.send_signal(signal.SIGINT)
            try:
                process.communicate(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.communicate(timeout=5)


def main():
    """Run correction-on and correction-off coordinate checks."""
    if not os.path.isfile(TEST_BINARY):
        raise RuntimeError(f"missing {TEST_BINARY}; run make check")
    enabled = run_case(True)
    disabled = run_case(False)
    print(json.dumps({"status": "ok", "enabled": enabled, "disabled": disabled}, indent=2))


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"frequency coordinate check failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
