#!/usr/bin/env python3
import argparse
import json
import os
import re
import time

from selenium import webdriver
from selenium.common.exceptions import TimeoutException
from selenium.webdriver.common.by import By
from selenium.webdriver.firefox.options import Options
from selenium.webdriver.support.ui import WebDriverWait


BASE_URL = os.environ.get("BASE_URL", "http://localhost:8080")
FIREFOX_BIN = os.environ.get("FIREFOX_BIN", "firefox")


def zoom_values(max_zoom):
    values = []
    decade = 1.0
    while decade <= max_zoom * 1.0001:
        for mult in (1.0, 2.0, 5.0):
            value = decade * mult
            if value <= max_zoom * 1.0001:
                values.append(value)
        decade *= 10.0
    if values[-1] < max_zoom:
        values.append(max_zoom)
    return values


def api(driver, path, body=None):
    script = """
const done = arguments[arguments.length - 1];
const path = arguments[0];
const body = arguments[1];
const options = body === null ? {} : {
  method: 'POST',
  headers: {'Content-Type': 'application/json'},
  body: JSON.stringify(body)
};
fetch(path, options).then(r => r.json()).then(done)
  .catch(e => done({status: 'error', message: String(e)}));
"""
    return driver.execute_async_script(script, path, body)


def wait_status_text(driver, expected, timeout=30):
    WebDriverWait(driver, timeout).until(
        lambda d: d.find_element(By.ID, "statusText").text == expected
    )


def wait_nonblank_waterfall(driver, timeout=40):
    WebDriverWait(driver, timeout).until(
        lambda d: d.execute_script(
            """
const c = document.getElementById('waterfall');
const ctx = c.getContext('2d', {willReadFrequently: true});
const a = ctx.getImageData(0, 0, c.width, c.height).data;
let n = 0;
for (let i = 0; i < a.length; i += 4) {
  if (a[i] || a[i + 1] || a[i + 2]) {
    n++;
    if (n > 50) return true;
  }
}
return false;
"""
        )
    )


def parse_zoom(text):
    match = re.search(r"([0-9.]+)", text or "")
    return float(match.group(1)) if match else 0.0


def ui_metrics(driver, label, requested_zoom):
    data = driver.execute_script(
        """
const wf = document.getElementById('waterfall');
const ctx = wf.getContext('2d', {willReadFrequently: true});
const pixels = ctx.getImageData(0, 0, wf.width, wf.height).data;
const transition = window.__plutoTestHooks
  ? window.__plutoTestHooks.transitionState()
  : {};
let nonblank = 0;
let checksum = 2166136261 >>> 0;
for (let i = 0; i < pixels.length; i += 4) {
  const v = (pixels[i] << 16) ^ (pixels[i + 1] << 8) ^ pixels[i + 2];
  if (v) nonblank++;
  checksum ^= v;
  checksum = Math.imul(checksum, 16777619) >>> 0;
}
return {
  status_text: document.getElementById('statusText').textContent,
  range: document.getElementById('infoRange').textContent,
  zoom: document.getElementById('infoZoom').textContent,
  scan: document.getElementById('infoScan').textContent,
  fft: document.getElementById('infoFFT').textContent,
  rate: document.getElementById('rateRangeVal').textContent,
  traffic: document.getElementById('infoTraffic').textContent,
  nonblank: nonblank,
  checksum: checksum,
  last_line: transition.last_line || null,
  goto_active: !!transition.goto_active,
  pending_view_change: !!transition.pending_view_change
};
"""
    )
    status = api(driver, "/api/status", None)
    data.update(
        {
            "label": label,
            "requested_zoom": requested_zoom,
            "zoom_value": parse_zoom(data.get("zoom", "")),
            "api": {
                key: status.get(key)
                for key in (
                    "mode",
                    "active_mode",
                    "steps",
                    "raw_line_rate",
                    "first_line_ms",
                    "rate_limit_lps",
                    "min_rate_lps",
                    "minimum_rate_limited",
                    "minimum_rate_achieved",
                    "visible_raw_bins",
                    "visible_bins_per_pixel",
                    "effective_fft_size",
                    "decim_factor",
                    "decim_hop",
                    "effective_input_samples",
                    "visible_start_hz",
                    "visible_end_hz",
                    "samplerate",
                    "rf_bandwidth",
                    "bw_ratio",
                    "second_if_hz",
                    "zero_if_guard_hz",
                    "line_bins",
                    "raw_line_bins",
                    "display_bins",
                )
            },
            "ts": time.time(),
        }
    )
    return data


def open_goto(driver, center_mhz, zoom, animate, delay):
    WebDriverWait(driver, 20).until(
        lambda d: d.find_element(By.ID, "gotoButton").is_enabled()
    )
    driver.find_element(By.ID, "gotoButton").click()
    WebDriverWait(driver, 5).until(
        lambda d: "open" in d.find_element(By.ID, "gotoDialog").get_attribute("class")
    )
    driver.execute_script(
        """
document.getElementById('gotoFreq').value = arguments[0];
document.getElementById('gotoTargetZoom').value = arguments[1];
const animate = document.getElementById('gotoAnimate');
animate.checked = arguments[2];
animate.dispatchEvent(new Event('change', {bubbles: true}));
document.getElementById('gotoDelay').value = arguments[3];
document.getElementById('gotoOk').click();
""",
        str(center_mhz),
        str(zoom),
        bool(animate),
        str(delay),
    )
    if animate:
        WebDriverWait(driver, 120).until(
            lambda d: (
                "active"
                not in d.find_element(By.ID, "gotoButton").get_attribute("class")
                and abs(
                    parse_zoom(d.find_element(By.ID, "infoZoom").text) - zoom
                )
                <= max(0.01, zoom * 0.005)
                and not d.execute_script(
                    "return window.__plutoTestHooks.transitionState().pending_view_change;"
                )
            )
        )
    else:
        WebDriverWait(driver, 60).until(
            lambda d: abs(
                parse_zoom(d.find_element(By.ID, "infoZoom").text) - zoom
            )
            <= max(0.01, zoom * 0.005)
            and not d.execute_script(
                "return window.__plutoTestHooks.transitionState().pending_view_change;"
            )
        )
    wait_status_text(driver, "scanning", timeout=30)


def start_full_band(driver, start_mhz, end_mhz, min_rate, max_rate):
    api(driver, "/api/stop", {})
    driver.execute_script("localStorage.removeItem('plutoScanner.visibleView.v1');")
    WebDriverWait(driver, 30).until(
        lambda d: d.find_element(By.ID, "statusText").text in ("idle", "disconnected")
    )
    driver.execute_script(
        """
document.getElementById('freqStart').value = arguments[0];
document.getElementById('freqEnd').value = arguments[1];
document.getElementById('converterFreq').value = '0';
document.getElementById('minRate').value = String(arguments[2]);
document.getElementById('rateLimit').value = String(arguments[3]);
updateRateRangeDisplay();
document.getElementById('btnStart').click();
""",
        str(start_mhz),
        str(end_mhz),
        int(min_rate),
        int(max_rate),
    )
    wait_status_text(driver, "scanning", timeout=40)
    try:
        wait_nonblank_waterfall(driver, timeout=60)
    except TimeoutException:
        pass


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", default="PHASE3/browser-zoom-matrix.json")
    parser.add_argument("--freq-start-mhz", type=float, default=70.0)
    parser.add_argument("--freq-end-mhz", type=float, default=6000.0)
    parser.add_argument("--max-zoom", type=float, default=1_000_000.0)
    parser.add_argument("--min-rate-lps", type=int, default=10)
    parser.add_argument("--rate-limit-lps", type=int, default=20)
    parser.add_argument("--animated-delay", default="0.2")
    parser.add_argument("--settle-seconds", type=float, default=2.0)
    args = parser.parse_args()

    os.environ["NO_PROXY"] = "localhost,127.0.0.1"
    os.environ["no_proxy"] = "localhost,127.0.0.1"
    os.makedirs(os.path.dirname(args.output) or ".", exist_ok=True)

    options = Options()
    options.add_argument("-headless")
    if os.path.exists(FIREFOX_BIN):
        options.binary_location = FIREFOX_BIN

    center_mhz = (args.freq_start_mhz + args.freq_end_mhz) * 0.5
    records = []
    errors = []
    transition_stress = None
    driver = webdriver.Firefox(options=options)
    driver.set_window_size(1600, 1100)
    try:
        driver.get(BASE_URL)
        WebDriverWait(driver, 15).until(
            lambda d: "MILLION TIMES ZOOM" in d.find_element(By.TAG_NAME, "h1").text.upper()
        )
        start_full_band(
            driver,
            args.freq_start_mhz,
            args.freq_end_mhz,
            args.min_rate_lps,
            args.rate_limit_lps,
        )
        for mode, animate in (("manual", False), ("goto-animated", True)):
            for zoom in zoom_values(args.max_zoom):
                open_goto(driver, center_mhz, zoom, animate, args.animated_delay)
                time.sleep(args.settle_seconds)
                rec = ui_metrics(driver, f"{mode}-{zoom:g}", zoom)
                records.append(rec)
                if rec["status_text"] != "scanning":
                    errors.append(f"{rec['label']}: status={rec['status_text']}")
                if rec["nonblank"] <= 0:
                    errors.append(f"{rec['label']}: blank waterfall")
                zoom_error = abs(rec["zoom_value"] - zoom)
                if zoom_error > max(0.01, zoom * 0.005):
                    errors.append(
                        f"{rec['label']}: reached zoom {rec['zoom_value']}, expected {zoom}"
                    )
                expected_span = (
                    args.freq_end_mhz - args.freq_start_mhz
                ) * 1e6 / zoom
                actual_span = (
                    float(rec["api"].get("visible_end_hz") or 0)
                    - float(rec["api"].get("visible_start_hz") or 0)
                )
                if abs(actual_span - expected_span) > max(2.0, expected_span * 1e-5):
                    errors.append(
                        f"{rec['label']}: API span {actual_span}, expected {expected_span}"
                    )
                api_plan = rec.get("api") or {}
                if api_plan.get("mode") == "single" and args.min_rate_lps > 0:
                    if not api_plan.get("minimum_rate_achieved"):
                        errors.append(
                            f"{rec['label']}: minimum rate not achieved: {api_plan}"
                        )
                    if float(api_plan.get("raw_line_rate") or 0) + 1e-6 < args.min_rate_lps:
                        errors.append(
                            f"{rec['label']}: raw rate {api_plan.get('raw_line_rate')} "
                            f"below minimum {args.min_rate_lps}"
                        )
                if animate:
                    line = rec.get("last_line") or {}
                    if not line or int(line.get("preview") or 0) != 0:
                        errors.append(f"{rec['label']}: no matching live final row")
                    for line_key, api_key in (
                        ("steps", "steps"),
                        ("effective_fft_size", "effective_fft_size"),
                        ("decim_factor", "decim_factor"),
                    ):
                        if int(line.get(line_key) or 0) != int(api_plan.get(api_key) or 0):
                            errors.append(
                                f"{rec['label']}: row {line_key}={line.get(line_key)} "
                                f"does not match API {api_plan.get(api_key)}"
                            )
        full_start_hz = args.freq_start_mhz * 1e6
        full_end_hz = args.freq_end_mhz * 1e6
        center_hz = (full_start_hz + full_end_hz) * 0.5
        spans = [
            full_end_hz - full_start_hz,
            100_000,
            20_000_000,
            10_000,
            200_000_000,
            1_000,
            1_000_000,
        ]
        windows = [
            [center_hz - span * 0.5, center_hz + span * 0.5]
            for span in spans
        ]
        transition_stress = driver.execute_async_script(
            """
const done = arguments[arguments.length - 1];
window.__plutoTestHooks.stressViewRequests(arguments[0])
  .then(done)
  .catch(error => done({error: String(error)}));
""",
            windows,
        )
        expected_start, expected_end = windows[-1]
        view_delta = int(transition_stress.get("after_view") or 0) - int(
            transition_stress.get("before_view") or 0
        )
        if view_delta > 2:
            errors.append(
                f"transition stress committed {view_delta} backend views, expected at most 2"
            )
        if int(transition_stress.get("applied_responses") or 0) != 1:
            errors.append(
                "transition stress did not apply exactly the newest response: "
                f"{transition_stress}"
            )
        if (
            abs(float(transition_stress.get("final_start") or 0) - expected_start) > 2
            or abs(float(transition_stress.get("final_end") or 0) - expected_end) > 2
        ):
            errors.append(f"transition stress ended at stale view: {transition_stress}")
        if not transition_stress.get("scanning"):
            errors.append(f"transition stress stopped scanning: {transition_stress}")
    except BaseException as exc:
        errors.append(f"exception: {type(exc).__name__}: {exc}")
        try:
            records.append(ui_metrics(driver, "exception-state", 0))
        except Exception:
            pass
    finally:
        driver.quit()

    summary = {
        "status": "ok" if not errors else "error",
        "record_count": len(records),
        "errors": errors,
        "zoom_min": min((r["requested_zoom"] for r in records), default=0),
        "zoom_max": max((r["requested_zoom"] for r in records), default=0),
        "blank_records": sum(1 for r in records if r.get("nonblank", 0) <= 0),
        "disconnected_records": sum(1 for r in records if r.get("status_text") != "scanning"),
    }
    with open(args.output, "w", encoding="utf-8") as f:
        json.dump(
            {
                "summary": summary,
                "transition_stress": transition_stress,
                "records": records,
            },
            f,
            indent=2,
        )
    print(json.dumps({"artifact": args.output, **summary}, indent=2))
    if errors:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
