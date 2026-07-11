#!/usr/bin/env python3
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
OUT_DIR = os.environ.get("PHASE3_DIR", "PHASE3")


def api_stop(driver):
    script = """
const done = arguments[arguments.length - 1];
fetch('/api/stop', {method: 'POST', headers: {'Content-Type': 'application/json'}, body: '{}'})
  .then(r => r.json()).then(done).catch(e => done({status: 'error', message: String(e)}));
"""
    return driver.execute_async_script(script)


def wait_nonblank_waterfall(driver, timeout=90):
    WebDriverWait(driver, timeout).until(
        lambda d: d.execute_script(
            """
const c = document.getElementById('waterfall');
const ctx = c.getContext('2d', {willReadFrequently: true});
const a = ctx.getImageData(0, 0, c.width, c.height).data;
let n = 0;
for (let i = 0; i < a.length; i += 16) {
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


def metrics(driver, label):
    data = driver.execute_script(
        """
const wf = document.getElementById('waterfall');
const ctx = wf.getContext('2d', {willReadFrequently: true});
const pixels = ctx.getImageData(0, 0, wf.width, wf.height).data;
let nonblank = 0;
let checksum = 2166136261 >>> 0;
for (let i = 0; i < pixels.length; i += 64) {
  const v = (pixels[i] << 16) ^ (pixels[i + 1] << 8) ^ pixels[i + 2];
  if (v) nonblank++;
  checksum ^= v;
  checksum = Math.imul(checksum, 16777619) >>> 0;
}
return {
  status: document.getElementById('statusText').textContent,
  scan: document.getElementById('infoScan').textContent,
  zoom: document.getElementById('infoZoom').textContent,
  range: document.getElementById('infoRange').textContent,
  rate: document.getElementById('rateRangeVal').textContent,
  waterfall_nonblank: nonblank,
  waterfall_checksum: checksum
};
"""
    )
    data["label"] = label
    data["ts"] = time.time()
    data["zoom_value"] = zoom_value(data.get("zoom", ""))
    return data


def build_summary(records, errors):
    return {
        "status": "ok" if not errors else "error",
        "record_count": len(records),
        "errors": errors,
        "zoom_min": min((r["zoom_value"] for r in records), default=0),
        "zoom_max": max((r["zoom_value"] for r in records), default=0),
        "blank_records": sum(1 for r in records if r["waterfall_nonblank"] <= 0),
        "disconnected_records": sum(1 for r in records if r["status"] != "scanning"),
        "final": records[-1] if records else None,
    }


def write_artifact(records, errors):
    summary = build_summary(records, errors)
    artifact = {"summary": summary, "records": records}
    path = os.path.join(OUT_DIR, "browser-stress-results.json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump(artifact, f, indent=2)
    return path, summary


def dispatch_wheel(driver, delta):
    wf = driver.find_element(By.ID, "waterfall")
    driver.execute_script(
        """
const el = arguments[0];
const delta = arguments[1];
const r = el.getBoundingClientRect();
el.dispatchEvent(new WheelEvent('wheel', {
  deltaY: delta,
  shiftKey: true,
  bubbles: true,
  cancelable: true,
  clientX: r.left + r.width * 0.52,
  clientY: r.top + r.height * 0.50
}));
""",
        wf,
        delta,
    )


def apply_goto(driver, freq_mhz, target_zoom, animate=False, delay="0.2"):
    button = WebDriverWait(driver, 20).until(
        lambda d: d.find_element(By.ID, "gotoButton")
        if d.find_element(By.ID, "gotoButton").is_enabled()
        else False
    )
    button.click()
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
    WebDriverWait(driver, 20).until(
        lambda d: d.find_element(By.ID, "statusText").text == "scanning"
    )
    try:
        wait_nonblank_waterfall(driver, timeout=20)
    except TimeoutException:
        pass
    time.sleep(0.7)


def start_full_band(driver):
    driver.execute_script("localStorage.removeItem('plutoScanner.visibleView.v1');")
    if driver.find_element(By.ID, "statusText").text == "scanning":
        try:
            stop_button = WebDriverWait(driver, 8).until(
                lambda d: d.find_element(By.ID, "btnStop")
                if d.find_element(By.ID, "btnStop").is_enabled()
                else False
            )
            stop_button.click()
        except TimeoutException:
            api_stop(driver)
    else:
        api_stop(driver)
    WebDriverWait(driver, 30).until(
        lambda d: d.find_element(By.ID, "statusText").text == "idle"
    )
    WebDriverWait(driver, 10).until(
        lambda d: d.find_element(By.ID, "btnStart").is_enabled()
    )
    driver.execute_script(
        """
const values = {
  freqStart: '70',
  freqEnd: '6000',
  converterFreq: '0',
  samplerate: '61440000',
  rfBandwidth: '20000000',
  bwRatio: '0.85',
  gainMode: 'manual',
  vgaGain: '20'
};
for (const [id, value] of Object.entries(values)) {
  const el = document.getElementById(id);
  if (!el) continue;
  el.disabled = false;
  el.value = value;
  el.dispatchEvent(new Event('input', {bubbles: true}));
  el.dispatchEvent(new Event('change', {bubbles: true}));
}
document.getElementById('btnStart').disabled = false;
document.getElementById('btnStart').click();
"""
    )
    WebDriverWait(driver, 15).until(
        lambda d: d.find_element(By.ID, "statusText").text == "scanning"
    )
    driver.find_element(By.ID, "zoomReset").click()
    WebDriverWait(driver, 20).until(
        lambda d: "70 - 6000" in d.find_element(By.ID, "infoRange").text
    )
    WebDriverWait(driver, 20).until(
        lambda d: "steps" in d.find_element(By.ID, "infoScan").text.lower()
    )
    wait_nonblank_waterfall(driver, timeout=120)


def main():
    os.environ["NO_PROXY"] = "localhost,127.0.0.1"
    os.environ["no_proxy"] = "localhost,127.0.0.1"
    os.makedirs(OUT_DIR, exist_ok=True)

    opts = Options()
    opts.add_argument("-headless")
    if os.path.exists(FIREFOX_BIN):
        opts.binary_location = FIREFOX_BIN

    driver = webdriver.Firefox(options=opts)
    driver.set_window_size(1600, 1100)
    records = []
    errors = []
    artifact_path = None
    summary = None
    try:
        driver.get(BASE_URL)
        WebDriverWait(driver, 10).until(
            lambda d: "PLUTO SDR SCANNER" in d.find_element(By.TAG_NAME, "h1").text.upper()
        )
        WebDriverWait(driver, 10).until(
            lambda d: d.find_element(By.ID, "statusText").text in ("idle", "scanning")
        )

        start_full_band(driver)
        records.append(metrics(driver, "full-band-start"))

        wheel_sequences = [
            ("wheel-small", [-120] * 8 + [120] * 6 + [-240] * 6 + [240] * 8),
            ("wheel-medium", [-320] * 8 + [320] * 8),
            ("wheel-large", [-640] * 4 + [640] * 4 + [-160] * 6 + [160] * 6),
        ]
        for name, deltas in wheel_sequences:
            for idx, delta in enumerate(deltas):
                dispatch_wheel(driver, delta)
                time.sleep(0.18)
                records.append(metrics(driver, f"{name}-{idx:02d}"))

        driver.find_element(By.ID, "zoomReset").click()
        time.sleep(0.5)
        records.append(metrics(driver, "ladder-reset-a"))

        for pass_idx in range(2):
            for step in range(50):
                driver.find_element(By.ID, "zoomIn").click()
                time.sleep(0.08)
                rec = metrics(driver, f"ladder-{pass_idx}-in-{step:02d}")
                records.append(rec)
                if rec["zoom_value"] >= 999000:
                    break
            for step in range(50):
                driver.find_element(By.ID, "zoomOut").click()
                time.sleep(0.08)
                rec = metrics(driver, f"ladder-{pass_idx}-out-{step:02d}")
                records.append(rec)
                if rec["zoom_value"] <= 1.01:
                    break

        goto_cases = [
            (70, 1),
            (88, 10),
            (435, 100),
            (1000, 1000),
            (3000, 100000),
            (5990, 1000000),
            (435, 1),
        ]
        for freq, target_zoom in goto_cases:
            apply_goto(driver, freq, target_zoom, animate=False)
            records.append(metrics(driver, f"goto-noanim-{freq}-{target_zoom}"))

        animated_cases = [
            (1000, 100),
            (3000, 10000),
            (5990, 1000000),
            (435, 10),
        ]
        for freq, target_zoom in animated_cases:
            apply_goto(driver, freq, target_zoom, animate=True, delay="0.2")
            records.append(metrics(driver, f"goto-anim-{freq}-{target_zoom}"))

        for rec in records:
            if rec["status"] != "scanning":
                errors.append(f"{rec['label']}: status={rec['status']}")
            if rec["waterfall_nonblank"] <= 0:
                errors.append(f"{rec['label']}: blank waterfall")
    except BaseException as exc:
        errors.append(f"exception: {type(exc).__name__}: {exc}")
        try:
            records.append(metrics(driver, "exception-state"))
        except Exception as metric_exc:
            errors.append(
                f"exception metrics unavailable: {type(metric_exc).__name__}: {metric_exc}"
            )
    finally:
        artifact_path, summary = write_artifact(records, errors)
        driver.quit()
    print(json.dumps({"artifact": artifact_path, **summary}, indent=2))
    if errors:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
