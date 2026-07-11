#!/usr/bin/env python3
import argparse
import json
import os
import time

from selenium import webdriver
from selenium.common.exceptions import TimeoutException
from selenium.webdriver.common.by import By
from selenium.webdriver.firefox.options import Options
from selenium.webdriver.support.ui import WebDriverWait


BASE_URL = os.environ.get("BASE_URL", "http://localhost:8080")
FIREFOX_BIN = os.environ.get("FIREFOX_BIN", "firefox")


def api_stop(driver):
    script = """
const done = arguments[arguments.length - 1];
fetch('/api/stop', {method: 'POST', headers: {'Content-Type': 'application/json'}, body: '{}'})
  .then(r => r.json()).then(done).catch(e => done({status: 'error', message: String(e)}));
"""
    return driver.execute_async_script(script)


def api_status(driver):
    script = """
const done = arguments[arguments.length - 1];
fetch('/api/status')
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


def waterfall_pixels(driver):
    return driver.execute_script(
        """
const c = document.getElementById('waterfall');
const ctx = c.getContext('2d', {willReadFrequently: true});
const a = ctx.getImageData(0, 0, c.width, c.height).data;
let nonblank = 0;
let checksum = 2166136261 >>> 0;
for (let i = 0; i < a.length; i += 64) {
  const v = (a[i] << 16) ^ (a[i + 1] << 8) ^ a[i + 2];
  if (v) nonblank++;
  checksum ^= v;
  checksum = Math.imul(checksum, 16777619) >>> 0;
}
return {nonblank, checksum, width: c.width, height: c.height};
"""
    )


def set_and_start_fm(driver):
    if driver.find_element(By.ID, "statusText").text == "scanning":
        api_stop(driver)
    WebDriverWait(driver, 30).until(
        lambda d: d.find_element(By.ID, "statusText").text == "idle"
    )
    driver.execute_script(
        """
localStorage.removeItem('plutoScanner.visibleView.v1');
const values = {
  freqStart: '88',
  freqEnd: '108',
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
        lambda d: "88 - 108" in d.find_element(By.ID, "infoRange").text
    )
    wait_nonblank_waterfall(driver, timeout=120)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", default="images/fm-broadcast-waterfall.png")
    parser.add_argument("--wait-seconds", type=float, default=60.0)
    parser.add_argument("--width", type=int, default=1600)
    parser.add_argument("--height", type=int, default=1100)
    args = parser.parse_args()

    os.environ["NO_PROXY"] = "localhost,127.0.0.1"
    os.environ["no_proxy"] = "localhost,127.0.0.1"
    os.makedirs(os.path.dirname(args.output) or ".", exist_ok=True)

    opts = Options()
    opts.add_argument("-headless")
    if os.path.exists(FIREFOX_BIN):
        opts.binary_location = FIREFOX_BIN

    driver = webdriver.Firefox(options=opts)
    driver.set_window_size(args.width, args.height)
    try:
        driver.get(BASE_URL)
        WebDriverWait(driver, 10).until(
            lambda d: "PLUTO SDR SCANNER" in d.find_element(By.TAG_NAME, "h1").text.upper()
        )
        WebDriverWait(driver, 10).until(
            lambda d: d.find_element(By.ID, "statusText").text in ("idle", "scanning")
        )

        set_and_start_fm(driver)
        time.sleep(args.wait_seconds)
        wait_nonblank_waterfall(driver, timeout=10)
        driver.save_screenshot(args.output)

        status = api_status(driver)
        pixels = waterfall_pixels(driver)
        result = {
            "status": "ok",
            "screenshot": args.output,
            "info_range": driver.find_element(By.ID, "infoRange").text,
            "info_scan": driver.find_element(By.ID, "infoScan").text,
            "info_fft": driver.find_element(By.ID, "infoFFT").text,
            "waterfall": pixels,
            "api": {
                "mode": status.get("mode"),
                "steps": status.get("steps"),
                "effective_fft_size": status.get("effective_fft_size"),
                "decim_factor": status.get("decim_factor"),
                "raw_line_rate": status.get("raw_line_rate"),
            },
        }
        print(json.dumps(result, indent=2))
    except TimeoutException:
        driver.save_screenshot(args.output)
        raise
    finally:
        driver.quit()


if __name__ == "__main__":
    main()
