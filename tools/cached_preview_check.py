#!/usr/bin/env python3
"""Verify indexed-history hot handoff for the reported Principle 7 views."""

import json
import math
import os
import queue
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request


BASE = ""
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DISPLAY_BINS = int(os.environ.get("PLUTO_TEST_DISPLAY_BINS", "1800"))
FFT_SIZE = 2048
VIEW_FINE = (155_760_937.0, 155_761_390.0)
VIEW_COARSE = (155_760_838.0, 155_761_468.0)
FIRST_ROW_MAX_MS = float(os.environ.get("PLUTO_TEST_FIRST_ROW_MAX_MS", "2000"))
FOLLOWING_GAP_MAX_MS = float(
    os.environ.get("PLUTO_TEST_FOLLOWING_GAP_MAX_MS", "1000")
)
ROW_COLLECTION_TIMEOUT = float(
    os.environ.get("PLUTO_TEST_ROW_COLLECTION_TIMEOUT", "8")
)


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
RESULT_RE = re.compile(
    r"CIC tone result: checks (\d+), spectral errors (\d+), "
    r"sample-order errors (\d+)"
)


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


class SSEReader:
    """Continuously timestamp waterfall rows so view changes cannot miss one."""

    def __init__(self):
        self.rows = queue.Queue()
        self.error = None
        self.stop_requested = False
        self.thread = threading.Thread(target=self._run, daemon=True)

    def start(self):
        """Start the background EventSource reader."""
        self.thread.start()

    def _run(self):
        saw_event = False
        try:
            with urllib.request.urlopen(BASE + "/api/waterfall", timeout=60) as response:
                while not self.stop_requested:
                    line = response.readline().decode("utf-8", "replace").strip()
                    if line == "event: line":
                        saw_event = True
                    elif saw_event and line.startswith("data:"):
                        row = json.loads(line[5:].strip())
                        row["_arrival_monotonic"] = time.monotonic()
                        self.rows.put(row)
                        saw_event = False
        except Exception as exc:
            if not self.stop_requested:
                self.error = exc

    def collect_for_view(self, view_id, minimum_rows, timeout, require_live=True):
        """Collect timestamped rows for one backend view id."""
        deadline = time.monotonic() + timeout
        matched = []
        while time.monotonic() < deadline:
            try:
                row = self.rows.get(timeout=0.25)
            except queue.Empty:
                if self.error:
                    raise RuntimeError(f"SSE reader failed: {self.error}")
                continue
            if int(row.get("view", -1)) != int(view_id):
                continue
            matched.append(row)
            if len(matched) >= minimum_rows and (
                not require_live
                or any(int(item.get("preview", 0)) == 0 for item in matched)
            ):
                return matched
        raise RuntimeError(
            f"received {len(matched)} of {minimum_rows} rows for view {view_id}"
        )

    def stop(self):
        """Stop retaining rows; the daemon reader may remain in readline()."""
        self.stop_requested = True
        self.thread.join(timeout=1)


def allocate_loopback_port():
    """Reserve a currently free loopback port for one private test process."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]
    finally:
        sock.close()


def stop_backend_process(process):
    """Request backend process exit using a signal supported by this platform."""
    if os.name == "nt":
        process.terminate()
    else:
        process.send_signal(signal.SIGINT)


def int_field(payload, name):
    """Read a numeric JSON field as an integer."""
    return int(round(float(payload.get(name, 0))))


def expected_plan(bounds):
    """Return the exact minimum-FFT CIC and overlap factors for one view."""
    span_hz = bounds[1] - bounds[0]
    decim = math.ceil(DISPLAY_BINS * 4_000_000.0 / span_hz / FFT_SIZE)
    base_lps = 1_850_000.0 / (FFT_SIZE * decim)
    required_overlap = max(1, math.ceil(10.0 / base_lps))
    overlap = 1 << (required_overlap - 1).bit_length()
    return decim, min(overlap, FFT_SIZE)


def start_payload(bounds):
    """Build the high-zoom request at the configured logical display width."""
    return {
        "freq_start": 150,
        "freq_end": 160,
        "converter_freq": 0,
        "samplerate": 61_440_000,
        "rf_bandwidth": 20_000_000,
        "bw_ratio": 0.85,
        "gain_mode": "manual",
        "hardwaregain_db": 20,
        "fft_size": 1024,
        "display_bins": DISPLAY_BINS,
        "rate_limit_lps": 20,
        "min_rate_lps": 10,
        "visible_start_hz": bounds[0],
        "visible_end_hz": bounds[1],
    }


def assert_plan(label, payload, expected_decim, expected_overlap):
    """Validate the minimum-FFT planner and overlap identity."""
    if payload.get("mode") != "single":
        raise RuntimeError(f"{label}: expected single mode: {payload}")
    if int_field(payload, "effective_fft_size") != FFT_SIZE:
        raise RuntimeError(f"{label}: FFT was not minimized to {FFT_SIZE}: {payload}")
    if int_field(payload, "display_bins") != DISPLAY_BINS:
        raise RuntimeError(f"{label}: display width changed: {payload}")
    if int_field(payload, "decim_factor") != expected_decim:
        raise RuntimeError(f"{label}: unexpected CIC plan: {payload}")
    if int_field(payload, "overlap_factor") != expected_overlap:
        raise RuntimeError(f"{label}: unexpected overlap: {payload}")
    if int_field(payload, "decim_hop") * expected_overlap != FFT_SIZE:
        raise RuntimeError(f"{label}: FFT/hop/overlap identity failed: {payload}")
    if ("visible_bins_per_pixel" in payload
            and float(payload["visible_bins_per_pixel"]) < 1.0):
        raise RuntimeError(f"{label}: plan undersamples display pixels: {payload}")
    if float(payload.get("true_line_rate", 1)) > 0.5:
        raise RuntimeError(f"{label}: view is not a slow high-zoom plan: {payload}")


def validate_frame_ranges(label, rows, decim, hop, capture_epoch):
    """Prove that published FFT frames retain one indexed capture timeline."""
    expected_width = FFT_SIZE * decim
    expected_step = hop * decim
    previous_end = None
    for row in rows:
        if int_field(row, "capture_epoch") != capture_epoch:
            raise RuntimeError(f"{label}: capture epoch changed: {row}")
        start = int_field(row, "frame_sample_start")
        end = int_field(row, "frame_sample_end")
        if start < 0 or end <= start or end - start != expected_width:
            raise RuntimeError(f"{label}: invalid FFT sample range: {row}")
        if previous_end is not None:
            advance = end - previous_end
            if advance <= 0 or advance % expected_step != 0:
                raise RuntimeError(
                    f"{label}: sample timeline advanced by {advance}, "
                    f"expected a positive multiple of {expected_step}"
                )
        previous_end = end


def run_transition(reader, bounds, expected_decim, expected_overlap,
                   capture_epoch, physical_center_hz):
    """Perform and validate one no-restart indexed-history view transition."""
    requested_at = time.monotonic()
    plan = http_json(
        "POST",
        "/api/view",
        {
            "visible_start_hz": bounds[0],
            "visible_end_hz": bounds[1],
            "display_bins": DISPLAY_BINS,
        },
    )
    if plan.get("status") != "ok" or plan.get("transition") != "hot":
        raise RuntimeError(f"compatible transition restarted capture: {plan}")
    assert_plan("hot plan", plan, expected_decim, expected_overlap)

    view_id = int_field(plan, "view_id")
    rows = reader.collect_for_view(
        view_id, minimum_rows=6, timeout=ROW_COLLECTION_TIMEOUT
    )
    first_row_ms = (rows[0]["_arrival_monotonic"] - requested_at) * 1000.0
    if first_row_ms >= FIRST_ROW_MAX_MS:
        raise RuntimeError(f"first reused row took {first_row_ms:.1f} ms")
    if int_field(rows[0], "preview") != 1:
        raise RuntimeError(f"first row did not reuse indexed history: {rows[0]}")
    if not any(int_field(row, "preview") == 0 for row in rows):
        raise RuntimeError("hot handoff never reached live rows")

    expected_dsp_center = (bounds[0] + bounds[1]) * 0.5
    for row in rows:
        assert_plan("hot row", row, expected_decim, expected_overlap)
        if abs(float(row.get("physical_center_hz", 0)) - physical_center_hz) > 1e-6:
            raise RuntimeError(f"hot transition retuned the physical LO: {row}")
        if abs(float(row.get("dsp_center_hz", 0)) - expected_dsp_center) > 1e-6:
            raise RuntimeError(f"hot transition used the wrong DSP center: {row}")

    validate_frame_ranges(
        "hot rows", rows, expected_decim, int_field(plan, "decim_hop"),
        capture_epoch,
    )
    gaps_ms = [
        (rows[index]["_arrival_monotonic"]
         - rows[index - 1]["_arrival_monotonic"]) * 1000.0
        for index in range(1, len(rows))
    ]
    if gaps_ms and max(gaps_ms) >= FOLLOWING_GAP_MAX_MS:
        raise RuntimeError(f"hot handoff contained a visible row pause: {gaps_ms}")

    return {
        "view_id": view_id,
        "span_hz": bounds[1] - bounds[0],
        "fft": FFT_SIZE,
        "decim": expected_decim,
        "overlap": expected_overlap,
        "first_row_ms": first_row_ms,
        "max_following_gap_ms": max(gaps_ms) if gaps_ms else 0.0,
        "rows": len(rows),
    }


def run_check():
    """Run both directions of the reported Principle 7 transition."""
    global BASE
    if not os.path.isfile(TEST_BINARY):
        raise RuntimeError(
            f"missing {TEST_BINARY}; run make cic-synthetic-test or make check"
        )
    port = allocate_loopback_port()
    BASE = f"http://127.0.0.1:{port}"

    with tempfile.TemporaryDirectory(prefix="pluto-hot-history-") as temp_dir:
        binary = os.path.join(temp_dir, os.path.basename(TEST_BINARY))
        shutil.copy2(TEST_BINARY, binary)
        env = os.environ.copy()
        env["PLUTO_SYNTHETIC_REALTIME"] = "1"
        process = subprocess.Popen(
            [binary, "--port", str(port)],
            cwd=temp_dir,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        reader = None
        output = ""
        transitions = []
        try:
            wait_for_server(process)
            reader = SSEReader()
            reader.start()
            plan = http_json("POST", "/api/start", start_payload(VIEW_FINE))
            if plan.get("status") != "ok":
                raise RuntimeError(f"start failed: {plan}")
            fine_decim, fine_overlap = expected_plan(VIEW_FINE)
            coarse_decim, coarse_overlap = expected_plan(VIEW_COARSE)
            assert_plan("initial plan", plan, fine_decim, fine_overlap)
            initial_rows = reader.collect_for_view(
                int_field(plan, "view_id"), minimum_rows=3, timeout=20
            )
            live_rows = [row for row in initial_rows if int_field(row, "preview") == 0]
            if not live_rows:
                raise RuntimeError("initial view produced no live row")
            capture_epoch = int_field(live_rows[-1], "capture_epoch")
            physical_center_hz = float(live_rows[-1].get("physical_center_hz", 0))
            if capture_epoch <= 0 or physical_center_hz == 0:
                raise RuntimeError(f"missing capture identity metadata: {live_rows[-1]}")

            transitions.append(
                run_transition(
                    reader, VIEW_COARSE, coarse_decim, coarse_overlap,
                    capture_epoch, physical_center_hz,
                )
            )
            transitions.append(
                run_transition(
                    reader, VIEW_FINE, fine_decim, fine_overlap,
                    capture_epoch, physical_center_hz,
                )
            )

            http_json("POST", "/api/stop", {})
            stop_backend_process(process)
            output = process.communicate(timeout=10)[0]
        finally:
            if reader:
                reader.stop()
            if process.poll() is None:
                process.kill()
                output += process.communicate(timeout=5)[0]

    if len(re.findall(r"Hot replan \d+ complete", output)) != 2:
        raise RuntimeError(f"both hot replans did not complete\n{output[-4000:]}")
    if output.count("Scan thread stopped") != 1:
        raise RuntimeError("a compatible view change restarted the scan thread")
    forbidden = (
        "lost indexed history",
        "no complete indexed history",
        "failed to allocate DSP state",
        "CIC sample-order mismatch",
    )
    if any(message in output for message in forbidden):
        raise RuntimeError(f"hot handoff diagnostic failure\n{output[-4000:]}")
    results = RESULT_RE.findall(output)
    if not results:
        raise RuntimeError(f"missing synthetic integrity result\n{output[-4000:]}")
    checks, spectral_errors, order_errors = (int(value) for value in results[-1])
    if checks < 4 or spectral_errors != 0 or order_errors != 0:
        raise RuntimeError(
            f"integrity failure: checks={checks}, spectral={spectral_errors}, "
            f"order={order_errors}\n{output[-4000:]}"
        )

    return {
        "status": "ok",
        "capture_epoch": capture_epoch,
        "transitions": transitions,
        "spectral_errors": spectral_errors,
        "sample_order_errors": order_errors,
        "log_tail": output[-1200:],
    }


def main():
    try:
        print(json.dumps(run_check(), indent=2))
    except Exception as exc:
        print(f"cached preview check failed: {exc}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
