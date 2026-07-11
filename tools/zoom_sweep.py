#!/usr/bin/env python3
import argparse
import json
import math
import os
import signal
import socket
import statistics
import subprocess
import sys
import time
import urllib.error
import urllib.request


BASE_URL = "http://127.0.0.1:8080"
RATE_LIMIT_GUARD = 0.85
CIC_STAGES = 3


def fft_window_scale(fft_size):
    if fft_size <= 1:
        return 1.0
    return 2.0 / float(fft_size - 1)


def cic_dc_scale(decim):
    if decim <= 1:
        return 1.0
    return 1.0 / float(decim ** CIC_STAGES)


def cic_gain(decim, freq_hz, raw_samplerate):
    if decim <= 1 or raw_samplerate <= 0 or abs(freq_hz) < 1.0e-12:
        return 1.0
    x = math.pi * freq_hz / raw_samplerate
    den = decim * math.sin(x)
    if abs(den) < 1.0e-18:
        return 1.0
    return abs(math.sin(decim * x) / den) ** CIC_STAGES


def cic_weight(decim, freq_hz, raw_samplerate):
    gain = max(cic_gain(decim, freq_hz, raw_samplerate), 0.05)
    return min(1.0 / gain, 8.0)


def load_config(path):
    cfg = {}
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, value = line.split("=", 1)
            key = key.strip()
            value = value.strip()
            try:
                cfg[key] = float(value)
            except ValueError:
                pass
    return cfg


def request_json(method, path, body=None, timeout=5):
    data = None
    headers = {}
    if body is not None:
        data = json.dumps(body).encode("utf-8")
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(BASE_URL + path, data=data, method=method, headers=headers)
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


def wait_status(timeout=8):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            return request_json("GET", "/api/status", timeout=1)
        except (urllib.error.URLError, TimeoutError, ConnectionError):
            time.sleep(0.2)
    raise RuntimeError("scanner HTTP server did not become ready")


def open_events():
    req = urllib.request.Request(BASE_URL + "/api/waterfall", method="GET")
    return urllib.request.urlopen(req, timeout=10)


def start_server(use_existing=False):
    if use_existing:
        wait_status()
        return None
    proc = subprocess.Popen(
        ["./run-scanner.sh"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        start_new_session=True,
    )
    try:
        wait_status()
    except Exception:
        stop_server(proc)
        raise
    return proc


def stop_server(proc):
    if not proc or proc.poll() is not None:
        return
    try:
        os.killpg(proc.pid, signal.SIGINT)
    except ProcessLookupError:
        return
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGTERM)
        proc.wait(timeout=5)


def config_request(cfg, visible_start, visible_end):
    return {
        "freq_start": cfg.get("freq_start", 50_000_000.0) / 1.0e6,
        "freq_end": cfg.get("freq_end", 2_000_000_000.0) / 1.0e6,
        "converter_freq": cfg.get("converter_freq", 0.0) / 1.0e6,
        "samplerate": cfg.get("samplerate", 61_440_000.0),
        "rf_bandwidth": cfg.get("rf_bandwidth", 20_000_000.0),
        "bw_ratio": cfg.get("bw_ratio", 0.85),
        "gain_mode": cfg.get("gain_mode", "manual"),
        "hardwaregain_db": float(cfg.get("hardwaregain_db", 20.0)),
        "fft_size": int(cfg.get("fft_size", 1024)),
        "min_rate_lps": int(cfg.get("min_rate_lps", 0)),
        "rate_limit_lps": int(cfg.get("rate_limit_lps", 20)),
        "display_bins": int(cfg.get("display_bins", 1024)),
        "visible_start_hz": visible_start,
        "visible_end_hz": visible_end,
    }


def parse_sse_event(resp, deadline):
    event = None
    data_parts = []
    while time.monotonic() < deadline:
        try:
            raw = resp.readline()
        except socket.timeout:
            return None, None
        if not raw:
            return None, None
        line = raw.decode("utf-8", errors="replace").strip()
        if not line:
            if event or data_parts:
                data = "\n".join(data_parts)
                return event, data
            continue
        if line.startswith("event:"):
            event = line[6:].strip()
        elif line.startswith("data:"):
            data_parts.append(line[5:].strip())
    return None, None


def measure_lines(view_id, duration, warmup):
    stamps = []
    sizes = []
    payloads = []
    deadline = time.monotonic() + warmup + duration
    measure_start = time.monotonic() + warmup
    with open_events() as resp:
        while time.monotonic() < deadline:
            event, data = parse_sse_event(resp, deadline)
            if event != "line" or not data:
                continue
            try:
                payload = json.loads(data)
            except json.JSONDecodeError:
                continue
            if int(payload.get("view", -1)) != int(view_id):
                continue
            now = time.monotonic()
            if now >= measure_start:
                stamps.append(now)
                payloads.append(payload)
                sizes.append(len(("event: line\ndata: " + data + "\n\n").encode("utf-8")))
    if len(stamps) < 2:
        return {
            "lines": len(stamps),
            "actual_lps": 0.0,
            "avg_ms": 0.0,
            "jitter_ms": 0.0,
            "max_gap_ms": 0.0,
            "avg_bytes": statistics.mean(sizes) if sizes else 0.0,
            "last_payload": payloads[-1] if payloads else {},
        }
    intervals = [b - a for a, b in zip(stamps, stamps[1:])]
    elapsed = stamps[-1] - stamps[0]
    actual_lps = (len(stamps) - 1) / elapsed if elapsed > 0 else 0.0
    avg_ms = 1000.0 * statistics.mean(intervals)
    jitter_ms = 1000.0 * statistics.pstdev(intervals) if len(intervals) > 1 else 0.0
    max_gap_ms = 1000.0 * max(intervals)
    return {
        "lines": len(stamps),
        "actual_lps": actual_lps,
        "avg_ms": avg_ms,
        "jitter_ms": jitter_ms,
        "max_gap_ms": max_gap_ms,
        "avg_bytes": statistics.mean(sizes) if sizes else 0.0,
        "last_payload": payloads[-1] if payloads else {},
    }


def zoom_values(max_zoom):
    values = []
    decade = 1.0
    while decade <= max_zoom * 1.0001:
        for mult in (1.0, 2.0, 5.0):
            z = decade * mult
            if z <= max_zoom * 1.0001:
                values.append(z)
        decade *= 10.0
    if values[-1] < max_zoom:
        values.append(max_zoom)
    return values


def expected_lps(status):
    raw = float(status.get("raw_line_rate", 0.0))
    limit = float(status.get("rate_limit_lps", 0.0))
    if limit <= 0:
        return raw
    if raw <= limit:
        return raw
    return limit * RATE_LIMIT_GUARD


def estimated_first_line_seconds(status):
    mode = status.get("mode", "")
    fft = float(status.get("effective_fft_size", 0.0))
    decim = float(status.get("decim_factor", 1.0))
    samplerate = float(status.get("samplerate", 0.0))
    samples = float(status.get("effective_input_samples", 0.0))
    if mode == "single" and fft > 0:
        if decim > 1:
            stream = min(samplerate if samplerate > 0 else 1_850_000.0, 1_850_000.0)
            return (fft * decim) / stream if stream > 0 else 0.0
        stream = min(samplerate if samplerate > 0 else 2_100_000.0, 2_100_000.0)
        return samples / stream if samples > 0 and stream > 0 else 0.0
    raw = float(status.get("raw_line_rate", 0.0))
    return 1.0 / raw if raw > 0 else 0.0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", default="pluto-scanner.conf")
    parser.add_argument("--duration", type=float, default=2.0)
    parser.add_argument("--max-duration", type=float, default=5.0)
    parser.add_argument("--warmup", type=float, default=0.8)
    parser.add_argument("--max-zoom", type=float, default=1_000_000.0)
    parser.add_argument("--out", default="REPORT.MD")
    parser.add_argument("--freq-start-mhz", type=float, default=70.0)
    parser.add_argument("--freq-end-mhz", type=float, default=6000.0)
    parser.add_argument("--center-mhz", type=float, default=None)
    parser.add_argument("--min-rate-lps", type=int, default=10)
    parser.add_argument("--rate-limit-lps", type=int, default=20)
    parser.add_argument("--display-bins", type=int, default=1024)
    parser.add_argument("--use-existing", action="store_true")
    args = parser.parse_args()

    cfg = load_config(args.config)
    cfg["freq_start"] = args.freq_start_mhz * 1.0e6
    cfg["freq_end"] = args.freq_end_mhz * 1.0e6
    cfg["converter_freq"] = 0.0
    cfg["min_rate_lps"] = args.min_rate_lps
    cfg["rate_limit_lps"] = args.rate_limit_lps
    cfg["display_bins"] = args.display_bins
    cfg["samplerate"] = 61_440_000.0
    cfg["rf_bandwidth"] = 20_000_000.0
    cfg["bw_ratio"] = 0.85
    full_start = cfg["freq_start"]
    full_end = cfg["freq_end"]
    full_span = max(1.0, full_end - full_start)
    center = args.center_mhz * 1.0e6 if args.center_mhz else (full_start + full_end) * 0.5

    proc = start_server(args.use_existing)
    rows = []
    try:
        for index, zoom in enumerate(zoom_values(args.max_zoom), 1):
            span = max(1.0, full_span / zoom)
            left = center - span * 0.5
            right = center + span * 0.5
            if left < full_start:
                right += full_start - left
                left = full_start
            if right > full_end:
                left -= right - full_end
                right = full_end
            left = max(full_start, left)
            right = min(full_end, right)

            if index == 1:
                plan = request_json("POST", "/api/start", config_request(cfg, left, right), timeout=8)
            else:
                plan = request_json("POST", "/api/view", {
                    "visible_start_hz": left,
                    "visible_end_hz": right,
                    "display_bins": int(cfg.get("display_bins", 1024)),
                    "view_id": int(rows[-1]["view_id"]) if rows else 1,
                }, timeout=8)
            status = request_json("GET", "/api/status", timeout=3)
            view_id = int(status.get("view_id", plan.get("view_id", 0)))
            expected = expected_lps(status)
            measure_duration = args.duration
            first_line = estimated_first_line_seconds(status)
            if expected > 0.0:
                measure_duration = max(
                    measure_duration,
                    min(args.max_duration, first_line + 8.0 / expected),
                )
            measured = measure_lines(view_id, measure_duration, args.warmup)
            scan_start = float(status.get("scan_start_hz", left))
            scan_end = float(status.get("scan_end_hz", right))
            visible_span = max(1.0, right - left)
            source_span = max(1.0, scan_end - scan_start)
            line_bins = int(status.get("line_bins", measured["last_payload"].get("source_bins", 0)))
            raw_line_bins = int(status.get("raw_line_bins", measured["last_payload"].get("raw_source_bins", line_bins)))
            visible_bins = line_bins
            second_if_hz = float(status.get("second_if_hz", 0.0))
            zero_if_guard_hz = float(status.get("zero_if_guard_hz", 0.0))
            samplerate = float(status.get("samplerate", 0.0))
            decim = int(status.get("decim_factor", 1))
            fft = int(status.get("effective_fft_size", 0))
            edge_freq_hz = min(source_span * 0.5, second_if_hz + visible_span * 0.5)
            row = {
                "zoom": zoom,
                "span_hz": right - left,
                "mode": status.get("mode", "?"),
                "steps": int(status.get("steps", 0)),
                "samplerate": samplerate,
                "rf_bandwidth": float(status.get("rf_bandwidth", 0.0)),
                "bw_ratio": float(status.get("bw_ratio", 0.0)),
                "second_if_hz": second_if_hz,
                "zero_if_guard_hz": zero_if_guard_hz,
                "raw_lps": float(status.get("raw_line_rate", 0.0)),
                "expected_lps": expected,
                "actual_lps": measured["actual_lps"],
                "lines": measured["lines"],
                "avg_ms": measured["avg_ms"],
                "jitter_ms": measured["jitter_ms"],
                "max_gap_ms": measured["max_gap_ms"],
                "avg_bytes": measured["avg_bytes"],
                "rate_drop_factor": int(status.get("rate_drop_factor", 0)),
                "fft": fft,
                "decim": decim,
                "decim_hop": int(status.get("decim_hop", 0)),
                "samples": int(status.get("effective_input_samples", 0)),
                "line_bins": line_bins,
                "raw_line_bins": raw_line_bins,
                "visible_bins": visible_bins,
                "bins_per_px": visible_bins / float(args.display_bins),
                "raw_bins_per_px": raw_line_bins / float(args.display_bins),
                "fft_weight": fft_window_scale(fft),
                "cic_dc_scale": cic_dc_scale(decim),
                "cic_edge_weight": cic_weight(decim, edge_freq_hz, samplerate),
                "view_id": view_id,
            }
            rows.append(row)
            print(
                f"{zoom:10.1f}x {row['mode']:6s} steps={row['steps']:3d} "
                f"raw={row['raw_lps']:7.1f} exp={row['expected_lps']:5.1f} "
                f"actual={row['actual_lps']:5.1f} jitter={row['jitter_ms']:6.1f}ms"
            )
    finally:
        try:
            request_json("POST", "/api/stop", {}, timeout=2)
        except Exception:
            pass
        stop_server(proc)

    with open(args.out, "w", encoding="utf-8") as f:
        f.write("# Zoom smoothness report\n\n")
        f.write(f"Band: `{args.freq_start_mhz:g}..{args.freq_end_mhz:g} MHz`\n\n")
        f.write(f"Waterfall rate request: min `{args.min_rate_lps}` lines/s, max `{args.rate_limit_lps}` lines/s\n\n")
        f.write(
            "| Zoom | Mode | Steps | Raw lps | Expected lps | Actual lps | "
            "Avg ms | Jitter ms | Max gap ms | Avg bytes | SR MSPS | RF BW MHz | "
            "PB | 2nd IF kHz | Zero guard kHz | FFT | Decim | Hop | Samples | "
            "Line bins | Raw bins | Visible bins | bins/px | raw bins/px | "
            "FFT weight | CIC DC | CIC edge |\n"
        )
        f.write("|---:|:---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n")
        for row in rows:
            f.write(
                f"| {row['zoom']:.1f} | {row['mode']} | {row['steps']} | "
                f"{row['raw_lps']:.1f} | {row['expected_lps']:.1f} | "
                f"{row['actual_lps']:.1f} | {row['avg_ms']:.1f} | "
                f"{row['jitter_ms']:.1f} | {row['max_gap_ms']:.1f} | "
                f"{row['avg_bytes']:.0f} | {row['samplerate'] / 1e6:.2f} | "
                f"{row['rf_bandwidth'] / 1e6:.2f} | {row['bw_ratio']:.2f} | "
                f"{row['second_if_hz'] / 1e3:.1f} | "
                f"{row['zero_if_guard_hz'] / 1e3:.1f} | "
                f"{row['fft']} | {row['decim']} | {row['decim_hop']} | "
                f"{row['samples']} | {row['line_bins']} | {row['raw_line_bins']} | "
                f"{row['visible_bins']:.0f} | "
                f"{row['bins_per_px']:.2f} | {row['raw_bins_per_px']:.2f} | "
                f"{row['fft_weight']:.8g} | {row['cic_dc_scale']:.8g} | "
                f"{row['cic_edge_weight']:.3f} |\n"
            )


if __name__ == "__main__":
    main()
