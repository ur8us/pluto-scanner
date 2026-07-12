#!/usr/bin/env python3
import argparse
import json
import math
import os
import random
import re
import time

from selenium import webdriver
from selenium.common.exceptions import TimeoutException
from selenium.webdriver.common.by import By
from selenium.webdriver.firefox.options import Options
from selenium.webdriver.support.ui import WebDriverWait


BASE_URL = os.environ.get("BASE_URL", "http://localhost:8080")
FIREFOX_BIN = os.environ.get("FIREFOX_BIN", "firefox")
DEFAULT_OUT = "PHASE3/frontend-random-validation.json"
SAMPLE_RATE = 61_440_000.0
SCAN_BUF_LEN = 1024.0
PLUTO_EST_HOP_MS = 3.4
PLUTO_EST_SINGLE_STREAM_SPS = 2_100_000.0
PLUTO_EST_CIC_STREAM_SPS = 1_850_000.0


def api(driver, path, payload=None):
    script = """
const done = arguments[arguments.length - 1];
const path = arguments[0];
const payload = arguments[1];
const options = payload === null ? {} : {
  method: 'POST',
  headers: {'Content-Type': 'application/json'},
  body: JSON.stringify(payload)
};
fetch(path, options)
  .then(r => r.json()).then(done).catch(e => done({status: 'error', message: String(e)}));
"""
    return driver.execute_async_script(script, path, payload)


def api_status(driver):
    return api(driver, "/api/status", None)


def wait_status(driver, expected, timeout=30):
    WebDriverWait(driver, timeout).until(
        lambda d: d.find_element(By.ID, "statusText").text == expected
    )


def wait_nonblank_waterfall(driver, timeout=90):
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


def zoom_value(text):
    match = re.search(r"([0-9.]+)", text or "")
    return float(match.group(1)) if match else 0.0


def traffic_kbytes_per_s(text):
    match = re.search(r"([0-9.]+)", text or "")
    return float(match.group(1)) if match else 0.0


def metrics(driver, label):
    data = driver.execute_script(
        """
const wf = document.getElementById('waterfall');
const ctx = wf.getContext('2d', {willReadFrequently: true});
const pixels = ctx.getImageData(0, 0, wf.width, wf.height).data;
let nonblank = 0;
let checksum = 2166136261 >>> 0;
for (let i = 0; i < pixels.length; i += 4) {
  const v = (pixels[i] << 16) ^ (pixels[i + 1] << 8) ^ pixels[i + 2];
  if (v) nonblank++;
  checksum ^= v;
  checksum = Math.imul(checksum, 16777619) >>> 0;
}
return {
  status: document.getElementById('statusText').textContent,
  scan: document.getElementById('infoScan').textContent,
  fft: document.getElementById('infoFFT').textContent,
  zoom: document.getElementById('infoZoom').textContent,
  range: document.getElementById('infoRange').textContent,
  rate: document.getElementById('rateRangeVal').textContent,
  traffic: document.getElementById('infoTraffic').textContent,
  waterfall_nonblank: nonblank,
  waterfall_checksum: checksum
};
"""
    )
    data["label"] = label
    data["zoom_value"] = zoom_value(data.get("zoom", ""))
    data["ts"] = time.time()
    return data


def set_values_and_start(driver, values):
    driver.execute_script(
        """
localStorage.removeItem('plutoScanner.visibleView.v1');
const values = arguments[0];
for (const [id, value] of Object.entries(values)) {
  const el = document.getElementById(id);
  if (!el) continue;
  el.disabled = false;
  el.value = String(value);
  el.dispatchEvent(new Event('input', {bubbles: true}));
  el.dispatchEvent(new Event('change', {bubbles: true}));
}
document.getElementById('btnStart').disabled = false;
document.getElementById('btnStart').click();
""",
        values,
    )
    wait_status(driver, "scanning", timeout=20)


def start_fm_scan(driver):
    if driver.find_element(By.ID, "statusText").text == "scanning":
        api(driver, "/api/stop", {})
        wait_status(driver, "idle", timeout=30)
    set_values_and_start(
        driver,
        {
            "freqStart": "88",
            "freqEnd": "108",
            "converterFreq": "0",
            "samplerate": "61440000",
            "rfBandwidth": "20000000",
            "bwRatio": "0.85",
            "gainMode": "manual",
            "vgaGain": "20",
        },
    )
    driver.find_element(By.ID, "zoomReset").click()
    WebDriverWait(driver, 20).until(
        lambda d: "88 - 108"
        in d.execute_script(
            "return document.getElementById('infoRange').textContent;"
        )
    )
    wait_nonblank_waterfall(driver, timeout=120)
    wait_status(driver, "scanning", timeout=20)


def dispatch_wheel(driver, delta, anchor=0.5):
    wf = driver.find_element(By.ID, "waterfall")
    driver.execute_script(
        """
const el = arguments[0];
const delta = arguments[1];
const anchor = arguments[2];
const r = el.getBoundingClientRect();
el.dispatchEvent(new WheelEvent('wheel', {
  deltaY: delta,
  shiftKey: true,
  bubbles: true,
  cancelable: true,
  clientX: r.left + r.width * anchor,
  clientY: r.top + r.height * 0.45
}));
""",
        wf,
        delta,
        anchor,
    )


def click_goto(driver, freq_mhz, target_zoom, animate=False, delay="0.1"):
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
        str(freq_mhz),
        str(target_zoom),
        bool(animate),
        str(delay),
    )
    if animate:
        WebDriverWait(driver, 90).until(
            lambda d: "active"
            not in d.find_element(By.ID, "gotoButton").get_attribute("class")
        )
    wait_status(driver, "scanning", timeout=30)
    try:
        wait_nonblank_waterfall(driver, timeout=20)
    except TimeoutException:
        pass


def drag_ruler(driver):
    wf = driver.find_element(By.ID, "waterfall")
    driver.execute_script(
        """
const el = arguments[0];
const r = el.getBoundingClientRect();
el.dispatchEvent(new PointerEvent('pointerdown', {
  pointerId: 3, clientX: r.left + r.width * 0.2, clientY: r.top + r.height * 0.35,
  ctrlKey: true, bubbles: true
}));
el.dispatchEvent(new PointerEvent('pointermove', {
  pointerId: 3, clientX: r.left + r.width * 0.72, clientY: r.top + r.height * 0.52,
  ctrlKey: true, bubbles: true
}));
el.dispatchEvent(new PointerEvent('pointerup', {
  pointerId: 3, clientX: r.left + r.width * 0.72, clientY: r.top + r.height * 0.52,
  ctrlKey: true, bubbles: true
}));
""",
        wf,
    )


def open_and_cancel_marker(driver):
    wf = driver.find_element(By.ID, "waterfall")
    driver.execute_script(
        """
const el = arguments[0];
const r = el.getBoundingClientRect();
el.dispatchEvent(new MouseEvent('click', {
  clientX: r.left + r.width * 0.35,
  clientY: r.top + r.height * 0.35,
  shiftKey: true,
  bubbles: true
}));
""",
        wf,
    )
    time.sleep(0.2)
    driver.execute_script(
        """
const dialog = document.getElementById('markerDialog');
if (dialog && dialog.classList.contains('open'))
  document.getElementById('markerCancel').click();
"""
    )


def exercise_sliders(driver, rng):
    gain = rng.choice([5, 15, 25, 35])
    min_idx = rng.choice([0, 2, 4])
    max_idx = rng.choice([4, 5, 6, 7])
    wf_min = rng.choice([0, 5, 10])
    wf_max = rng.choice([35, 50, 70])
    driver.execute_script(
        """
document.getElementById('vgaGain').value = arguments[0];
document.getElementById('vgaGain').dispatchEvent(new Event('input', {bubbles: true}));
document.getElementById('rateMinSlider').value = arguments[1];
document.getElementById('rateMinSlider').dispatchEvent(new Event('input', {bubbles: true}));
document.getElementById('rateMaxSlider').value = arguments[2];
document.getElementById('rateMaxSlider').dispatchEvent(new Event('input', {bubbles: true}));
const auto = document.getElementById('wfAuto');
auto.checked = false;
auto.dispatchEvent(new Event('change', {bubbles: true}));
document.getElementById('wfMin').value = arguments[3];
document.getElementById('wfMin').dispatchEvent(new Event('input', {bubbles: true}));
document.getElementById('wfMax').value = arguments[4];
document.getElementById('wfMax').dispatchEvent(new Event('input', {bubbles: true}));
""",
        str(gain),
        str(min_idx),
        str(max_idx),
        str(wf_min),
        str(wf_max),
    )
    time.sleep(1.0)


def expected_scan_hop_ms(samples):
    if samples >= 8192:
        return 8.9
    if samples >= 4096:
        return 6.0
    if samples >= 2048:
        return 4.8
    return PLUTO_EST_HOP_MS


def expected_scan_line_rate(steps, samples):
    return 1000.0 / (expected_scan_hop_ms(samples) * float(steps))


def expected_single_stream_sps(line_samples):
    _ = line_samples
    return PLUTO_EST_SINGLE_STREAM_SPS


def assert_close(errors, label, actual, expected, rel=0.05, abs_tol=0.05):
    if not math.isfinite(float(actual)):
        errors.append(f"{label}: non-finite actual {actual}")
        return
    if abs(float(actual) - expected) > max(abs_tol, abs(expected) * rel):
        errors.append(f"{label}: expected {expected:.3f}, got {float(actual):.3f}")


def validate_scan_plan(status, errors):
    if status.get("mode") != "scan":
        errors.append(f"scan mode expected, got {status.get('mode')}")
    if int(status.get("steps", 0)) < 2:
        errors.append(f"scan steps expected >=2, got {status.get('steps')}")
    if int(status.get("decim_factor", -1)) != 1:
        errors.append(f"scan decim expected 1, got {status.get('decim_factor')}")
    line_bins = int(status.get("line_bins", 0))
    display_bins = int(status.get("display_bins", 0))
    if display_bins > 0 and line_bins != display_bins:
        errors.append(
            f"scan published bins expected to equal display, got {line_bins}/{display_bins}"
        )
    raw_line_bins = int(status.get("raw_line_bins", 0))
    if display_bins > 0 and raw_line_bins > 0 and raw_line_bins < display_bins:
        errors.append(
            f"scan raw bins expected to cover display before reduction, got {raw_line_bins}/{display_bins}"
        )
    samplerate = float(status.get("samplerate", SAMPLE_RATE))
    rf_bandwidth = float(status.get("rf_bandwidth", 0))
    if rf_bandwidth >= samplerate:
        errors.append(
            f"scan RF bandwidth expected < sample rate, got {rf_bandwidth}/{samplerate}"
        )
    steps = int(status.get("steps", 0))
    samples = float(status.get("effective_input_samples", SCAN_BUF_LEN))
    if steps > 0:
        assert_close(
            errors,
            "scan raw_line_rate",
            float(status.get("raw_line_rate", 0)),
            expected_scan_line_rate(steps, samples),
        )


def validate_single_plan(status, errors):
    if status.get("mode") != "single":
        errors.append(f"single mode expected, got {status.get('mode')}")
    if int(status.get("steps", 0)) != 1:
        errors.append(f"single steps expected 1, got {status.get('steps')}")
    if int(status.get("effective_fft_size", 0)) < 2048:
        errors.append(
            f"single effective FFT expected >=2048, got {status.get('effective_fft_size')}"
        )
    line_bins = int(status.get("line_bins", 0))
    display_bins = int(status.get("display_bins", 0))
    if display_bins > 0 and line_bins != display_bins:
        errors.append(
            f"single published bins expected to equal display, got {line_bins}/{display_bins}"
        )
    samplerate = float(status.get("samplerate", SAMPLE_RATE))
    rf_bandwidth = float(status.get("rf_bandwidth", 0))
    if rf_bandwidth >= samplerate:
        errors.append(
            f"single RF bandwidth expected < sample rate, got {rf_bandwidth}/{samplerate}"
        )
    if samplerate > 0 and rf_bandwidth > samplerate * 0.5 + 1.0:
        errors.append(
            f"single displayed RF bandwidth expected <= half sample rate, got {rf_bandwidth}/{samplerate}"
        )
    zero_guard = float(status.get("zero_if_guard_hz", 0))
    second_if = float(status.get("second_if_hz", 0))
    if zero_guard <= 0:
        errors.append(f"single zero_if_guard_hz expected >0, got {zero_guard}")
    if second_if <= 0:
        errors.append(f"single second_if_hz expected >0, got {second_if}")
    decim = int(status.get("decim_factor", 0))
    if decim < 1 or decim & (decim - 1):
        errors.append(f"single decim expected power-of-two >=1, got {decim}")
    line_samples = float(status.get("effective_input_samples", 0))
    decim_hop = int(status.get("decim_hop", 0))
    fft = int(status.get("effective_fft_size", 0))
    if decim > 1 and decim_hop <= 0:
        errors.append(f"single CIC decim_hop expected >0, got {decim_hop}")
    if line_samples <= 0:
        errors.append("single effective_input_samples missing")
    else:
        stream_sps = (
            PLUTO_EST_CIC_STREAM_SPS
            if decim > 1
            else expected_single_stream_sps(line_samples)
        )
        if samplerate > 0:
            stream_sps = min(stream_sps, samplerate)
        expected_samples = (decim_hop * decim) if decim > 1 else fft
        if expected_samples > 0 and int(round(line_samples)) != expected_samples:
            errors.append(
                f"single effective_input_samples expected {expected_samples}, got {line_samples}"
            )
        assert_close(
            errors,
            "single raw_line_rate",
            float(status.get("raw_line_rate", 0)),
            stream_sps / line_samples,
            rel=0.02,
        )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", default=DEFAULT_OUT)
    parser.add_argument("--seed", type=int, default=20260703)
    parser.add_argument("--iterations", type=int, default=28)
    args = parser.parse_args()

    rng = random.Random(args.seed)
    os.environ["NO_PROXY"] = "localhost,127.0.0.1"
    os.environ["no_proxy"] = "localhost,127.0.0.1"
    os.makedirs(os.path.dirname(args.output) or ".", exist_ok=True)

    opts = Options()
    opts.add_argument("-headless")
    if os.path.exists(FIREFOX_BIN):
        opts.binary_location = FIREFOX_BIN

    driver = webdriver.Firefox(options=opts)
    driver.set_window_size(1600, 1100)
    records = []
    statuses = []
    errors = []
    try:
        driver.get(BASE_URL)
        WebDriverWait(driver, 10).until(
            lambda d: "PLUTO SDR SCANNER" in d.find_element(By.TAG_NAME, "h1").text.upper()
        )
        WebDriverWait(driver, 10).until(
            lambda d: d.find_element(By.ID, "statusText").text in ("idle", "scanning")
        )
        if driver.find_elements(By.ID, "infoFw"):
            errors.append("FW indicator should not be present")

        start_fm_scan(driver)
        scan_status = api_status(driver)
        statuses.append({"label": "fm-scan", "status": scan_status})
        validate_scan_plan(scan_status, errors)
        records.append(metrics(driver, "fm-scan-start"))

        actions = [
            "zoom_in",
            "zoom_out",
            "pan_left",
            "pan_right",
            "wheel_in",
            "wheel_out",
            "goto_scan",
            "goto_single",
            "goto_animated",
            "ruler",
            "marker_cancel",
            "sliders",
            "reset",
        ]

        for idx in range(args.iterations):
            action = rng.choice(actions)
            if action == "zoom_in":
                driver.find_element(By.ID, "zoomIn").click()
            elif action == "zoom_out":
                driver.find_element(By.ID, "zoomOut").click()
            elif action == "pan_left":
                driver.find_element(By.ID, "panLeft").click()
            elif action == "pan_right":
                driver.find_element(By.ID, "panRight").click()
            elif action == "wheel_in":
                dispatch_wheel(driver, -rng.choice([120, 240, 480]), rng.uniform(0.25, 0.75))
            elif action == "wheel_out":
                dispatch_wheel(driver, rng.choice([120, 240, 480]), rng.uniform(0.25, 0.75))
            elif action == "goto_scan":
                click_goto(driver, rng.choice([88, 96, 100, 106]), rng.choice([1, 2, 5, 10]))
            elif action == "goto_single":
                click_goto(driver, rng.choice([96, 100, 104]), rng.choice([1000, 10000, 100000]))
            elif action == "goto_animated":
                click_goto(driver, rng.choice([96, 100, 104]), rng.choice([100, 1000]), True)
            elif action == "ruler":
                drag_ruler(driver)
            elif action == "marker_cancel":
                open_and_cancel_marker(driver)
            elif action == "sliders":
                exercise_sliders(driver, rng)
            elif action == "reset":
                driver.find_element(By.ID, "zoomReset").click()

            wait_status(driver, "scanning", timeout=20)
            planned_status = api_status(driver)
            planned_first_line_s = float(
                planned_status.get("first_line_ms") or 0
            ) / 1000.0
            settle_seconds = rng.uniform(1.5, 3.5)
            if 0.0 < planned_first_line_s <= 5.0:
                settle_seconds = max(settle_seconds, planned_first_line_s + 1.0)
            time.sleep(settle_seconds)
            rec = metrics(driver, f"random-{idx:02d}-{action}")
            rec["planned_first_line_ms"] = float(
                planned_status.get("first_line_ms") or 0
            )
            rec["settle_seconds"] = settle_seconds
            records.append(rec)
            if rec["waterfall_nonblank"] <= 0:
                if rec["planned_first_line_ms"] <= settle_seconds * 1000:
                    if traffic_kbytes_per_s(rec.get("traffic", "")) > 0.1:
                        rec["blank_but_streaming"] = True
                    else:
                        errors.append(f"{rec['label']}: blank waterfall")
                else:
                    rec["blank_before_planned_first_line"] = True
            if rec["status"] != "scanning":
                errors.append(f"{rec['label']}: status {rec['status']}")

        click_goto(driver, 100, 100000, False)
        time.sleep(3.0)
        single_status = api_status(driver)
        statuses.append({"label": "single-100mhz", "status": single_status})
        validate_single_plan(single_status, errors)
        records.append(metrics(driver, "single-plan-check"))

        click_goto(driver, 100, 1, False)
        time.sleep(3.0)
        scan_status = api_status(driver)
        statuses.append({"label": "scan-restored", "status": scan_status})
        validate_scan_plan(scan_status, errors)
        records.append(metrics(driver, "scan-restored-check"))

        driver.find_element(By.ID, "btnStop").click()
        wait_status(driver, "idle", timeout=30)
        records.append(metrics(driver, "stop-check"))
        driver.find_element(By.ID, "btnStart").click()
        wait_status(driver, "scanning", timeout=30)
        wait_nonblank_waterfall(driver, timeout=60)
        records.append(metrics(driver, "restart-check"))
    except BaseException as exc:
        errors.append(f"exception: {type(exc).__name__}: {exc}")
        try:
            records.append(metrics(driver, "exception-state"))
            statuses.append({"label": "exception", "status": api_status(driver)})
        except Exception:
            pass
    finally:
        driver.quit()

    summary = {
        "status": "ok" if not errors else "error",
        "seed": args.seed,
        "record_count": len(records),
        "errors": errors,
        "blank_records": sum(1 for r in records if r.get("waterfall_nonblank", 0) <= 0),
        "disconnected_records": sum(1 for r in records if r.get("status") == "disconnected"),
        "zoom_min": min((r.get("zoom_value", 0) for r in records), default=0),
        "zoom_max": max((r.get("zoom_value", 0) for r in records), default=0),
        "final": records[-1] if records else None,
    }
    artifact = {"summary": summary, "records": records, "statuses": statuses}
    with open(args.output, "w", encoding="utf-8") as f:
        json.dump(artifact, f, indent=2)
    print(json.dumps({"artifact": args.output, **summary}, indent=2))
    if errors:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
