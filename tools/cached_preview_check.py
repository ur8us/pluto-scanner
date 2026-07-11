#!/usr/bin/env python3
"""Verify that compatible deep-zoom view changes publish cached preview rows."""

import json
import os
import queue
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request


BASE = "http://127.0.0.1:8080"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def find_test_binary():
    """Return the hidden synthetic scanner binary path used by Makefile."""
    env_path = os.environ.get("PLUTO_CIC_TEST_BINARY")
    candidates = []
    if env_path:
        candidates.append(env_path)
    candidates.extend(
        [
            os.path.join(ROOT, ".build", "tests", "pluto-scanner-cic-test"),
            os.path.join(ROOT, ".build", "tests", "pluto-scanner-cic-test.exe"),
            os.path.join(ROOT, "pluto-scanner-cic-test"),
            os.path.join(ROOT, "pluto-scanner-cic-test.exe"),
        ]
    )
    for path in candidates:
        if os.path.isfile(path):
            return path
    return candidates[0]


TEST_BINARY = find_test_binary()


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


class SSEReader:
    """Background SSE reader used to catch rows published during /api/view."""

    def __init__(self):
        self.rows = queue.Queue()
        self.error = None
        self.stop_requested = False
        self.thread = threading.Thread(target=self._run, daemon=True)

    def start(self):
        self.thread.start()

    def _run(self):
        saw_event = False
        try:
            with urllib.request.urlopen(BASE + "/api/waterfall", timeout=30) as response:
                while not self.stop_requested:
                    line = response.readline().decode("utf-8", "replace").strip()
                    if line == "event: line":
                        saw_event = True
                    elif saw_event and line.startswith("data:"):
                        self.rows.put(json.loads(line[5:].strip()))
                        saw_event = False
        except Exception as exc:
            if not self.stop_requested:
                self.error = exc

    def collect_for_view(self, view_id, minimum_rows=5, timeout=10):
        """Collect rows matching one backend view id."""
        deadline = time.monotonic() + timeout
        matched = []
        while time.monotonic() < deadline and len(matched) < minimum_rows:
            try:
                row = self.rows.get(timeout=0.25)
            except queue.Empty:
                if self.error:
                    raise RuntimeError(f"SSE reader failed: {self.error}")
                continue
            if int(row.get("view", -1)) == int(view_id):
                matched.append(row)
        return matched

    def stop(self):
        self.stop_requested = True
        self.thread.join(timeout=1)


def port_is_free():
    """Return whether the scanner's fixed HTTP port is currently unused."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        return sock.connect_ex(("127.0.0.1", 8080)) != 0
    finally:
        sock.close()


def stop_backend_process(process):
    """Request backend process exit using a signal supported by this platform."""
    if os.name == "nt":
        process.terminate()
    else:
        process.send_signal(signal.SIGINT)


def view_bounds(center_hz, span_hz):
    """Return visible start/end for one centered view."""
    return center_hz - span_hz * 0.5, center_hz + span_hz * 0.5


def start_payload(center_hz, span_hz):
    """Build a deep x64 overlapped single-frequency request."""
    visible_start, visible_end = view_bounds(center_hz, span_hz)
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
        "min_rate_lps": 20,
        "visible_start_hz": visible_start,
        "visible_end_hz": visible_end,
    }


def int_field(payload, name):
    """Read a numeric JSON field as an integer."""
    return int(round(float(payload.get(name, 0))))


def run_check():
    """Run the cache preview validation against a private synthetic backend."""
    if not os.path.isfile(TEST_BINARY):
        raise RuntimeError(
            f"missing {TEST_BINARY}; run make cic-synthetic-test or make check"
        )
    if not port_is_free():
        raise RuntimeError("TCP port 8080 is already in use")

    center = 435_000_000.0
    span_hz = 4096.0e6 / 3_000_000.0
    shifted_start, shifted_end = view_bounds(center + 25.0, span_hz)

    with tempfile.TemporaryDirectory(prefix="pluto-cache-preview-") as temp_dir:
        binary = os.path.join(temp_dir, os.path.basename(TEST_BINARY))
        shutil.copy2(TEST_BINARY, binary)
        process = subprocess.Popen(
            [binary],
            cwd=temp_dir,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        reader = None
        output = ""
        rate_preview_rows = []
        rate_live_rows = []
        try:
            wait_for_server(process)
            plan = http_json("POST", "/api/start", start_payload(center, span_hz))
            if plan.get("status") != "ok":
                raise RuntimeError(f"start failed: {plan}")
            if int_field(plan, "decim_factor") != 64:
                raise RuntimeError(f"expected x64 CIC plan, got {plan}")
            if int_field(plan, "overlap_factor") != 64:
                raise RuntimeError(f"expected x64 overlap, got {plan}")

            warmup_rows = read_sse_rows(8, timeout=20)
            if any(int(row.get("preview", 0)) != 0 for row in warmup_rows):
                raise RuntimeError("initial cache warmup unexpectedly used previews")

            reader = SSEReader()
            reader.start()
            time.sleep(0.2)
            view_plan = http_json(
                "POST",
                "/api/view",
                {
                    "visible_start_hz": shifted_start,
                    "visible_end_hz": shifted_end,
                    "display_bins": 1024,
                },
            )
            if view_plan.get("status") != "ok":
                raise RuntimeError(f"view change failed: {view_plan}")
            view_id = int_field(view_plan, "view_id")
            rows = reader.collect_for_view(view_id, minimum_rows=6, timeout=15)
            if len(rows) < 3:
                raise RuntimeError(f"only {len(rows)} rows captured for view {view_id}")
            if int(rows[0].get("preview", 0)) != 1:
                raise RuntimeError(f"first row for new view was not cached preview: {rows[0]}")

            preview_rows = [row for row in rows if int(row.get("preview", 0)) == 1]
            live_rows = [row for row in rows if int(row.get("preview", 0)) == 0]
            if not preview_rows:
                raise RuntimeError("no cached preview rows captured")
            if not live_rows:
                raise RuntimeError("no live row captured after cached previews")
            expected_count = int(preview_rows[0].get("preview_count", 0))
            if expected_count < 1:
                raise RuntimeError(f"invalid preview_count in {preview_rows[0]}")
            sequences = [int(row.get("preview_sequence", 0)) for row in preview_rows]
            if sequences != list(range(1, len(sequences) + 1)):
                raise RuntimeError(f"preview sequences were not contiguous: {sequences}")
            if len(preview_rows) > expected_count:
                raise RuntimeError(
                    f"captured {len(preview_rows)} previews but preview_count was {expected_count}"
                )
            for row in rows:
                if int_field(row, "decim_factor") != 64 or int_field(row, "overlap_factor") != 64:
                    raise RuntimeError(f"row changed CIC/overlap metadata: {row}")

            time.sleep(0.2)
            rate_plan = http_json(
                "POST",
                "/api/rate",
                {"min_rate_lps": 10, "rate_limit_lps": 50},
            )
            if rate_plan.get("status") != "ok":
                raise RuntimeError(f"rate change failed: {rate_plan}")
            rate_view_id = int_field(rate_plan, "view_id")
            rate_rows = reader.collect_for_view(
                rate_view_id, minimum_rows=8, timeout=15
            )
            if len(rate_rows) < 2:
                raise RuntimeError(
                    f"only {len(rate_rows)} rows captured for rate view {rate_view_id}"
                )
            if int(rate_rows[0].get("preview", 0)) != 1:
                raise RuntimeError(
                    "first row after waterfall-rate change was not cached "
                    f"preview: {rate_rows[0]}"
                )
            rate_preview_rows = [
                row for row in rate_rows if int(row.get("preview", 0)) == 1
            ]
            rate_live_rows = [
                row for row in rate_rows if int(row.get("preview", 0)) == 0
            ]
            if not rate_preview_rows:
                raise RuntimeError("no cached preview rows captured after rate change")
            if not rate_live_rows:
                raise RuntimeError("no live row captured after rate-change previews")

            http_json("POST", "/api/stop", {})
            stop_backend_process(process)
            output = process.communicate(timeout=10)[0]
        finally:
            if reader:
                reader.stop()
            if process.poll() is None:
                process.kill()
                output += process.communicate(timeout=5)[0]

    return {
        "status": "ok",
        "preview_rows": len(preview_rows),
        "preview_count": expected_count,
        "live_rows": len(live_rows),
        "rate_preview_rows": len(rate_preview_rows),
        "rate_live_rows": len(rate_live_rows),
        "first_line_ms": float(view_plan.get("first_line_ms", 0.0)),
        "log_tail": output[-1000:],
    }


def main():
    try:
        print(json.dumps(run_check(), indent=2))
    except Exception as exc:
        print(f"cached preview check failed: {exc}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
