#!/usr/bin/env python3
"""Exercise indexed-history Principle 7 handoffs on a live Pluto stream."""

import argparse
import json
import math
import queue
import sys
import threading
import time
import urllib.request


VIEW_FINE = (155_760_937.0, 155_761_390.0)
VIEW_COARSE = (155_760_838.0, 155_761_468.0)
FFT_SIZE = 2048
HARDWARE_SAMPLE_RATE = 4_000_000.0
ESTIMATED_CIC_STREAM_RATE = 1_850_000.0
MIN_RATE_LPS = 10.0


def http_json(base_url, method, path, payload=None, timeout=20):
    """Send one scanner API request and decode its JSON response."""
    data = None if payload is None else json.dumps(payload).encode("utf-8")
    request = urllib.request.Request(
        base_url + path,
        data=data,
        headers={"Content-Type": "application/json"},
        method=method,
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return json.loads(response.read().decode("utf-8"))


def int_field(payload, name):
    """Read a numeric JSON field as an integer."""
    return int(round(float(payload.get(name, 0))))


def expected_plan(bounds, display_bins):
    """Return minimum FFT planner CIC and overlap factors for one view."""
    span_hz = bounds[1] - bounds[0]
    decim = math.ceil(
        display_bins * HARDWARE_SAMPLE_RATE / span_hz / FFT_SIZE
    )
    base_lps = ESTIMATED_CIC_STREAM_RATE / (FFT_SIZE * decim)
    required_overlap = max(1, math.ceil(MIN_RATE_LPS / base_lps))
    overlap = 1 << (required_overlap - 1).bit_length()
    return decim, min(overlap, FFT_SIZE)


class SSEReader:
    """Read and timestamp waterfall rows without missing transition events."""

    def __init__(self, base_url):
        self.base_url = base_url
        self.rows = queue.Queue()
        self.error = None
        self.stop_requested = False
        self.thread = threading.Thread(target=self._run, daemon=True)

    def start(self):
        """Start the EventSource reader thread."""
        self.thread.start()

    def _run(self):
        saw_line = False
        try:
            with urllib.request.urlopen(
                self.base_url + "/api/waterfall", timeout=60
            ) as response:
                while not self.stop_requested:
                    line = response.readline().decode("utf-8", "replace").strip()
                    if line == "event: line":
                        saw_line = True
                    elif saw_line and line.startswith("data:"):
                        row = json.loads(line[5:].strip())
                        row["_arrival_monotonic"] = time.monotonic()
                        self.rows.put(row)
                        saw_line = False
        except Exception as exc:
            if not self.stop_requested:
                self.error = exc

    def collect(self, view_id, minimum_rows, timeout, require_live=True):
        """Collect rows for one view while discarding stale earlier views."""
        deadline = time.monotonic() + timeout
        matched = []
        while time.monotonic() < deadline:
            try:
                row = self.rows.get(timeout=0.25)
            except queue.Empty:
                if self.error:
                    raise RuntimeError(f"SSE reader failed: {self.error}")
                continue
            if int_field(row, "view") != view_id:
                continue
            matched.append(row)
            if len(matched) >= minimum_rows and (
                not require_live
                or any(int_field(item, "preview") == 0 for item in matched)
            ):
                return matched
        raise RuntimeError(
            f"received {len(matched)} of {minimum_rows} rows for view {view_id}"
        )

    def stop(self):
        """Stop retaining rows and briefly join the reader thread."""
        self.stop_requested = True
        self.thread.join(timeout=1)


def start_payload(bounds, display_bins):
    """Build the exact live high-zoom start request."""
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
        "display_bins": display_bins,
        "rate_limit_lps": 20,
        "min_rate_lps": 10,
        "visible_start_hz": bounds[0],
        "visible_end_hz": bounds[1],
    }


def assert_plan(payload, bounds, display_bins):
    """Validate one minimum-FFT single-mode response."""
    expected_decim, expected_overlap = expected_plan(bounds, display_bins)
    if payload.get("status") != "ok" or payload.get("mode") != "single":
        raise RuntimeError(f"view did not enter single mode: {payload}")
    if int_field(payload, "effective_fft_size") != FFT_SIZE:
        raise RuntimeError(f"FFT was not minimized to {FFT_SIZE}: {payload}")
    if int_field(payload, "decim_factor") != expected_decim:
        raise RuntimeError(f"unexpected CIC factor: {payload}")
    if int_field(payload, "overlap_factor") != expected_overlap:
        raise RuntimeError(f"unexpected overlap factor: {payload}")
    if int_field(payload, "decim_hop") * expected_overlap != FFT_SIZE:
        raise RuntimeError(f"FFT/hop/overlap identity failed: {payload}")
    return expected_decim


def validate_rows(rows, bounds, decim, capture_epoch, physical_center_hz):
    """Validate capture identity, frame ranges, and cadence for one handoff."""
    expected_width = FFT_SIZE * decim
    expected_center = (bounds[0] + bounds[1]) * 0.5
    previous_end = None
    for row in rows:
        if int_field(row, "capture_epoch") != capture_epoch:
            raise RuntimeError(f"capture epoch changed during hot handoff: {row}")
        if abs(float(row.get("physical_center_hz", 0)) - physical_center_hz) > 1e-6:
            raise RuntimeError(f"physical Pluto LO changed during handoff: {row}")
        if abs(float(row.get("dsp_center_hz", 0)) - expected_center) > 1e-6:
            raise RuntimeError(f"wrong DSP center after handoff: {row}")
        if int_field(row, "hot_replan_failures") != 0:
            raise RuntimeError(f"backend reported a hot replan failure: {row}")
        start = int_field(row, "frame_sample_start")
        end = int_field(row, "frame_sample_end")
        if end <= start or end - start != expected_width:
            raise RuntimeError(f"invalid FFT source sample range: {row}")
        if previous_end is not None:
            advance = end - previous_end
            expected_step = int_field(row, "decim_hop") * decim
            if advance <= 0 or advance % expected_step != 0:
                raise RuntimeError(
                    f"sample timeline advanced by {advance}, expected a positive "
                    f"multiple of {expected_step}"
                )
        previous_end = end

    gaps_ms = [
        (rows[index]["_arrival_monotonic"]
         - rows[index - 1]["_arrival_monotonic"]) * 1000.0
        for index in range(1, len(rows))
    ]
    if gaps_ms and max(gaps_ms) >= 1000.0:
        raise RuntimeError(f"hot handoff contained a row pause: {gaps_ms}")
    return max(gaps_ms) if gaps_ms else 0.0


def heartbeat_wait(base_url, seconds):
    """Wait while keeping the backend frontend-activity watchdog satisfied."""
    deadline = time.monotonic() + max(0.0, seconds)
    while time.monotonic() < deadline:
        status = http_json(base_url, "GET", "/api/status", timeout=5)
        if not int_field(status, "scanning"):
            raise RuntimeError(f"scanner stopped during live dwell: {status}")
        time.sleep(min(5.0, max(0.0, deadline - time.monotonic())))


def run(args):
    """Run alternating live handoffs for at least the requested duration."""
    reader = SSEReader(args.base_url)
    reader.start()
    transitions = []
    started = time.monotonic()
    try:
        try:
            http_json(args.base_url, "POST", "/api/stop", {})
        except Exception:
            pass
        plan = http_json(
            args.base_url,
            "POST",
            "/api/start",
            start_payload(VIEW_FINE, args.display_bins),
            timeout=30,
        )
        assert_plan(plan, VIEW_FINE, args.display_bins)
        initial_rows = reader.collect(int_field(plan, "view_id"), 3, timeout=40)
        live_rows = [row for row in initial_rows if int_field(row, "preview") == 0]
        if not live_rows:
            raise RuntimeError("initial live view produced no non-preview row")
        capture_epoch = int_field(live_rows[-1], "capture_epoch")
        physical_center_hz = float(live_rows[-1].get("physical_center_hz", 0))
        if capture_epoch <= 0 or physical_center_hz == 0:
            raise RuntimeError("initial rows omitted capture identity metadata")

        test_started = time.monotonic()
        index = 0
        while time.monotonic() - test_started < args.duration_seconds:
            bounds = VIEW_COARSE if index % 2 == 0 else VIEW_FINE
            requested_at = time.monotonic()
            response = http_json(
                args.base_url,
                "POST",
                "/api/view",
                {
                    "visible_start_hz": bounds[0],
                    "visible_end_hz": bounds[1],
                    "display_bins": args.display_bins,
                },
            )
            if response.get("transition") != "hot":
                raise RuntimeError(f"compatible view restarted capture: {response}")
            decim = assert_plan(response, bounds, args.display_bins)
            rows = reader.collect(int_field(response, "view_id"), 8, timeout=12)
            first_row_ms = (
                rows[0]["_arrival_monotonic"] - requested_at
            ) * 1000.0
            if int_field(rows[0], "preview") != 1 or first_row_ms >= 2000.0:
                raise RuntimeError(
                    f"history row was not prompt: preview={rows[0].get('preview')} "
                    f"first={first_row_ms:.1f} ms"
                )
            max_gap_ms = validate_rows(
                rows, bounds, decim, capture_epoch, physical_center_hz
            )
            transitions.append(
                {
                    "view_id": int_field(response, "view_id"),
                    "span_hz": bounds[1] - bounds[0],
                    "decim": decim,
                    "first_row_ms": first_row_ms,
                    "max_gap_ms": max_gap_ms,
                }
            )
            print(
                f"transition {index + 1}: span {bounds[1] - bounds[0]:.0f} Hz, "
                f"CIC {decim}, first {first_row_ms:.1f} ms, "
                f"max gap {max_gap_ms:.1f} ms",
                flush=True,
            )
            index += 1
            elapsed = time.monotonic() - requested_at
            heartbeat_wait(args.base_url, args.transition_interval - elapsed)

        return {
            "status": "ok",
            "requested_duration_seconds": args.duration_seconds,
            "elapsed_seconds": time.monotonic() - test_started,
            "total_elapsed_seconds": time.monotonic() - started,
            "display_bins": args.display_bins,
            "capture_epoch": capture_epoch,
            "physical_center_hz": physical_center_hz,
            "transition_count": len(transitions),
            "first_row_max_ms": max(
                item["first_row_ms"] for item in transitions
            ),
            "following_gap_max_ms": max(
                item["max_gap_ms"] for item in transitions
            ),
            "transitions": transitions,
        }
    finally:
        reader.stop()
        try:
            http_json(args.base_url, "POST", "/api/stop", {})
        except Exception:
            pass


def main():
    """Parse arguments, execute the live run, and emit JSON results."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-url", default="http://127.0.0.1:8080")
    parser.add_argument("--duration-seconds", type=float, default=300.0)
    parser.add_argument("--transition-interval", type=float, default=15.0)
    parser.add_argument("--display-bins", type=int, default=1800)
    parser.add_argument("--output")
    args = parser.parse_args()
    try:
        result = run(args)
    except Exception as exc:
        print(f"live Principle 7 check failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
    rendered = json.dumps(result, indent=2)
    if args.output:
        with open(args.output, "w", encoding="utf-8") as output:
            output.write(rendered + "\n")
    print(rendered)


if __name__ == "__main__":
    main()
