#!/usr/bin/env python3
import json
import os
import time

from selenium import webdriver
from selenium.webdriver.common.by import By
from selenium.webdriver.firefox.options import Options
from selenium.webdriver.support.ui import Select, WebDriverWait


BASE_URL = os.environ.get("BASE_URL", "http://localhost:8080")
FIREFOX_BIN = os.environ.get("FIREFOX_BIN", "firefox")


def wait_nonblank_waterfall(driver):
    script = """
const c = document.getElementById('waterfall');
const ctx = c.getContext('2d');
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
    WebDriverWait(driver, 20).until(lambda d: d.execute_script(script))


def api_start(driver, payload):
    script = """
const done = arguments[arguments.length - 1];
fetch('/api/start', {
  method: 'POST',
  headers: {'Content-Type': 'application/json'},
  body: JSON.stringify(arguments[0])
}).then(r => r.json()).then(done).catch(e => done({status: 'error', message: String(e)}));
"""
    result = driver.execute_async_script(script, payload)
    if result.get("status") != "ok":
        raise RuntimeError(result)
    return result


def api_stop(driver):
    script = """
const done = arguments[arguments.length - 1];
fetch('/api/stop', {method: 'POST', headers: {'Content-Type': 'application/json'}, body: '{}'})
  .then(r => r.json()).then(done).catch(e => done({status: 'error', message: String(e)}));
"""
    return driver.execute_async_script(script)


def main():
    os.environ["NO_PROXY"] = "localhost,127.0.0.1"
    os.environ["no_proxy"] = "localhost,127.0.0.1"

    opts = Options()
    opts.add_argument("-headless")
    if os.path.exists(FIREFOX_BIN):
        opts.binary_location = FIREFOX_BIN

    driver = webdriver.Firefox(options=opts)
    driver.set_window_size(1400, 1000)
    try:
        driver.get(BASE_URL)
        WebDriverWait(driver, 10).until(
            lambda d: "PLUTO SDR SCANNER" in d.find_element(By.TAG_NAME, "h1").text.upper()
        )
        WebDriverWait(driver, 10).until(
            lambda d: d.find_element(By.ID, "statusText").text in ("idle", "scanning")
        )

        assert not driver.find_element(By.ID, "directSampling").is_displayed()
        assert len(driver.find_elements(By.ID, "freqComp")) == 0
        assert len(driver.find_elements(By.ID, "infoRangeMode")) == 0
        assert len(driver.find_elements(By.ID, "infoFw")) == 0
        assert len(driver.find_elements(By.ID, "rssiVal")) == 0
        assert len(driver.find_elements(By.ID, "peakVal")) == 0
        assert driver.find_element(By.ID, "infoRange").is_displayed()
        assert driver.find_element(By.ID, "rfBandwidth").is_displayed()
        assert not driver.find_element(By.ID, "samplerate").is_enabled()
        assert not driver.find_element(By.ID, "rfBandwidth").is_enabled()
        assert not driver.find_element(By.ID, "bwRatio").is_enabled()
        assert driver.find_element(By.ID, "gainMode").is_displayed()
        assert driver.find_element(By.ID, "vgaGain").is_displayed()

        api_stop(driver)
        time.sleep(1.0)

        for element_id, value in [("freqStart", "430"), ("freqEnd", "470"), ("converterFreq", "0")]:
            el = driver.find_element(By.ID, element_id)
            el.clear()
            el.send_keys(value)
        Select(driver.find_element(By.ID, "gainMode")).select_by_value("manual")
        driver.execute_script(
            "const el=document.getElementById('vgaGain');"
            "el.value='20'; el.dispatchEvent(new Event('input',{bubbles:true}));"
        )
        driver.find_element(By.ID, "btnStart").click()
        WebDriverWait(driver, 12).until(
            lambda d: d.find_element(By.ID, "statusText").text == "scanning"
        )
        WebDriverWait(driver, 15).until(
            lambda d: "steps" in d.find_element(By.ID, "infoScan").text.lower()
        )
        WebDriverWait(driver, 15).until(
            lambda d: "430 - 470" in d.find_element(By.ID, "infoRange").text
        )
        wait_nonblank_waterfall(driver)

        before_zoom = driver.find_element(By.ID, "infoZoom").text
        wf = driver.find_element(By.ID, "waterfall")
        driver.execute_script(
            "const el=arguments[0]; const r=el.getBoundingClientRect();"
            "el.dispatchEvent(new WheelEvent('wheel',{deltaY:-240,shiftKey:true,"
            "bubbles:true,cancelable:true,clientX:r.left+400,clientY:r.top+200}));",
            wf,
        )
        time.sleep(0.8)
        after_zoom = driver.find_element(By.ID, "infoZoom").text
        if before_zoom == after_zoom:
            raise RuntimeError("zoom display did not change")

        driver.execute_script("document.getElementById('gotoButton').click();")
        time.sleep(0.3)
        driver.execute_script(
            "document.getElementById('gotoFreq').value='435';"
            "document.getElementById('gotoTargetZoom').value='100';"
            "document.getElementById('gotoOk').click();"
        )
        time.sleep(0.8)

        driver.execute_script(
            "const el=arguments[0]; const r=el.getBoundingClientRect();"
            "el.dispatchEvent(new MouseEvent('mousedown',{clientX:r.left+200,clientY:r.top+200,ctrlKey:true,bubbles:true}));"
            "el.dispatchEvent(new MouseEvent('mousemove',{clientX:r.left+500,clientY:r.top+260,ctrlKey:true,bubbles:true}));"
            "el.dispatchEvent(new MouseEvent('mouseup',{clientX:r.left+500,clientY:r.top+260,ctrlKey:true,bubbles:true}));",
            wf,
        )

        single_payload = {
            "freq_start": 435,
            "freq_end": 436,
            "converter_freq": 0,
            "samplerate": 61440000,
            "rf_bandwidth": 20000000,
            "bw_ratio": 0.85,
            "gain_mode": "manual",
            "hardwaregain_db": 20,
            "fft_size": 1024,
            "display_bins": 1024,
            "rate_limit_lps": 10,
            "min_rate_lps": 0,
        }
        api_start(driver, single_payload)
        WebDriverWait(driver, 12).until(
            lambda d: "single" in d.find_element(By.ID, "infoScan").text.lower()
            or "fixed" in d.find_element(By.ID, "infoScan").text.lower()
        )

        out_dir = "ui-test-screenshots"
        os.makedirs(out_dir, exist_ok=True)
        screenshot = os.path.join(out_dir, "pluto-ui.png")
        driver.save_screenshot(screenshot)
        print(
            json.dumps(
                {
                    "status": "ok",
                    "zoom_before": before_zoom,
                    "zoom_after": after_zoom,
                    "info_scan": driver.find_element(By.ID, "infoScan").text,
                    "screenshot": screenshot,
                },
                indent=2,
            )
        )
    finally:
        driver.quit()


if __name__ == "__main__":
    main()
