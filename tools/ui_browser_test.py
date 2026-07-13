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


def api_status(driver):
    """Return the current scanner status through the browser origin."""
    script = """
const done = arguments[arguments.length - 1];
fetch('/api/status').then(r => r.json()).then(done)
  .catch(e => done({status: 'error', message: String(e)}));
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
        assert len(driver.find_elements(By.ID, "samplerate")) == 0
        assert len(driver.find_elements(By.ID, "rfBandwidth")) == 0
        assert len(driver.find_elements(By.ID, "bwRatio")) == 0
        assert driver.find_element(By.ID, "infoSampleRate").is_displayed()
        assert driver.find_element(By.ID, "infoRfBandwidth").is_displayed()
        assert driver.find_element(By.ID, "infoBwUsage").is_displayed()
        Select(driver.find_element(By.ID, "timeMarks")).select_by_value("15")
        assert (
            driver.execute_script("return localStorage.getItem('timeMarksSeconds');")
            == "15"
        )
        assert driver.find_element(By.ID, "gainMode").is_displayed()
        assert driver.find_element(By.ID, "vgaGain").is_displayed()
        assert driver.find_element(By.ID, "rfPort").is_displayed()
        scale_format = driver.execute_script(
            "return window.__plutoTestHooks.scaleFormatting();"
        )
        if scale_format != {
            "range": "10489.5003 - 10489.5006 MHz, Span: 300 Hz",
            "span_khz": "12.5 kHz",
            "span_hz": "300 Hz",
            "whole_mhz": "10489 MHz",
            "khz": "10489.500 MHz",
            "hz": "10489.500 300 MHz",
            "hover_like_scale": "10489.500 300 MHz",
            "delta": "10Hz",
            "second_delta_left": 10489500100,
            "second_delta_right": 10489500200,
            "delta_candidates": ["<- 10Hz ->", "<-10Hz->", "<10Hz>", "10Hz"],
            "ruler_hz": "999.5 Hz",
            "auto_levels": {"min": 45, "max": 181},
            "coordinate_fraction": 0.75,
            "coordinate_hz": 10489500300,
        }:
            raise RuntimeError(f"unexpected scale formatting: {scale_format}")

        api_stop(driver)
        time.sleep(1.0)

        for element_id, value in [("freqStart", "430"), ("freqEnd", "470"), ("converterFreq", "0")]:
            el = driver.find_element(By.ID, element_id)
            el.clear()
            el.send_keys(value)
        Select(driver.find_element(By.ID, "gainMode")).select_by_value("manual")
        Select(driver.find_element(By.ID, "rfPort")).select_by_value("B_BALANCED")
        driver.execute_script(
            "const el=document.getElementById('vgaGain');"
            "el.value='20'; el.dispatchEvent(new Event('input',{bubbles:true}));"
        )
        driver.find_element(By.ID, "btnStart").click()
        WebDriverWait(driver, 12).until(
            lambda d: d.find_element(By.ID, "statusText").text == "scanning"
        )
        if api_status(driver).get("rf_port") != "B_BALANCED":
            raise RuntimeError("selected Pluto RX input was not sent to the backend")
        WebDriverWait(driver, 15).until(
            lambda d: "steps" in d.find_element(By.ID, "infoScan").text.lower()
        )
        WebDriverWait(driver, 15).until(
            lambda d: "430 - 470 MHz, Span: 40 MHz"
            in d.find_element(By.ID, "infoRange").text
        )
        wait_nonblank_waterfall(driver)

        # CSS layout zoom exercises the same resize path as browser Ctrl+/-
        # without relying on host-specific browser zoom automation. It must not
        # send a display-bin-only backend view request or break scanner zoom.
        before_layout_status = api_status(driver)
        before_layout_view = int(before_layout_status.get("view_id", 0))
        before_layout_state = driver.execute_script(
            "return window.__plutoTestHooks.viewState();"
        )
        before_layout_history = driver.execute_script(
            "return window.__plutoTestHooks.waterfallDiagnostics();"
        )
        if int(before_layout_history.get("history_rows", 0)) <= 0:
            raise RuntimeError("no retained rows before layout-only zoom")
        if int(before_layout_history.get("nonblack_pixels", 0)) <= 0:
            raise RuntimeError("waterfall was blank before layout-only zoom")
        driver.execute_script(
            "document.documentElement.style.zoom='125%';"
            "window.dispatchEvent(new Event('resize'));"
        )
        time.sleep(0.8)
        WebDriverWait(driver, 10).until(
            lambda d: int(
                d.execute_script(
                    "return window.__plutoTestHooks.waterfallDiagnostics();"
                ).get("nonblack_pixels", 0)
            )
            > 0
        )
        layout_in_history = driver.execute_script(
            "return window.__plutoTestHooks.waterfallDiagnostics();"
        )
        after_layout_status = api_status(driver)
        after_layout_state = driver.execute_script(
            "return window.__plutoTestHooks.viewState();"
        )
        driver.execute_script(
            "document.documentElement.style.zoom='';"
            "window.dispatchEvent(new Event('resize'));"
        )
        time.sleep(0.8)
        WebDriverWait(driver, 10).until(
            lambda d: int(
                d.execute_script(
                    "return window.__plutoTestHooks.waterfallDiagnostics();"
                ).get("nonblack_pixels", 0)
            )
            > 0
        )
        layout_out_history = driver.execute_script(
            "return window.__plutoTestHooks.waterfallDiagnostics();"
        )
        if int(after_layout_status.get("view_id", 0)) != before_layout_view:
            raise RuntimeError("layout-only zoom restarted the scanner backend")
        if not after_layout_status.get("scanning"):
            raise RuntimeError("layout-only zoom stopped scanning")
        if int(after_layout_state.get("stream_display_bins", 0)) != int(
            before_layout_state.get("stream_display_bins", 0)
        ):
            raise RuntimeError("layout-only zoom changed the active stream width")
        for label, history in [
            ("layout zoom-in", layout_in_history),
            ("layout zoom-out", layout_out_history),
        ]:
            if int(history.get("history_rows", 0)) < int(
                before_layout_history.get("history_rows", 0)
            ):
                raise RuntimeError(f"{label} discarded retained waterfall rows")
            if int(history.get("nonblack_pixels", 0)) <= 0:
                raise RuntimeError(f"{label} left the retained waterfall blank")
        zoom_before_keyboard = driver.find_element(By.ID, "infoZoom").text
        driver.execute_script(
            "window.dispatchEvent(new KeyboardEvent('keydown',"
            "{key:'+',bubbles:true,cancelable:true}));"
        )
        WebDriverWait(driver, 10).until(
            lambda d: d.find_element(By.ID, "infoZoom").text
            != zoom_before_keyboard
        )

        driver.find_element(By.ID, "btnStop").click()
        WebDriverWait(driver, 15).until(
            lambda d: d.find_element(By.ID, "statusText").text == "idle"
        )
        time.sleep(3.0)
        stopped_status = driver.execute_async_script(
            """
const done = arguments[arguments.length - 1];
fetch('/api/status')
  .then(r => r.json()).then(done)
  .catch(e => done({status: 'error', message: String(e)}));
"""
        )
        if stopped_status.get("scanning"):
            raise RuntimeError("explicit Stop was followed by automatic restart")
        driver.find_element(By.ID, "btnStart").click()
        WebDriverWait(driver, 12).until(
            lambda d: d.find_element(By.ID, "statusText").text == "scanning"
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
        WebDriverWait(driver, 15).until(
            lambda d: abs(
                d.execute_script("return window.__plutoTestHooks.viewState();")[
                    "center_hz"
                ]
                - 435_000_000
            )
            < 1
        )
        goto_state = driver.execute_script(
            "return window.__plutoTestHooks.viewState();"
        )
        if abs(goto_state["span_hz"] - 400_000) > 5:
            raise RuntimeError(f"static Go To did not apply target zoom: {goto_state}")
        WebDriverWait(driver, 15).until(
            lambda d: (
                (status := api_status(d)).get("scanning")
                and abs(float(status.get("visible_start_hz", 0)) - 434_800_000)
                < 5
                and abs(float(status.get("visible_end_hz", 0)) - 435_200_000)
                < 5
                and abs(
                    d.execute_script("return window.__plutoTestHooks.viewState();")[
                        "span_hz"
                    ]
                    - 400_000
                )
                < 5
            )
        )

        driver.execute_script("document.getElementById('gotoButton').click();")
        driver.execute_script(
            "document.getElementById('gotoFreq').value='435';"
            "document.getElementById('gotoTargetZoom').value='200';"
            "const animate=document.getElementById('gotoAnimate');"
            "animate.checked=true; animate.dispatchEvent(new Event('change',{bubbles:true}));"
            "document.getElementById('gotoDelay').value='0.2';"
            "document.getElementById('gotoOk').click();"
        )
        WebDriverWait(driver, 10).until(
            lambda d: d.execute_script(
                "return window.__plutoTestHooks.viewState().goto_active;"
            )
        )
        WebDriverWait(driver, 30).until(
            lambda d: not d.execute_script(
                "return window.__plutoTestHooks.viewState().goto_active;"
            )
        )
        goto_state = driver.execute_script(
            "return window.__plutoTestHooks.viewState();"
        )
        if (
            abs(goto_state["center_hz"] - 435_000_000) > 1
            or abs(goto_state["span_hz"] - 200_000) > 5
        ):
            raise RuntimeError(f"animated Go To did not finish at target: {goto_state}")

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
