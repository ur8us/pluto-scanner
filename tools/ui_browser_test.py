#!/usr/bin/env python3
import json
import math
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


def api_view(driver, visible_start_hz, visible_end_hz, display_bins=None):
    """Apply one backend viewport using the visible CSS waterfall width."""
    if display_bins is None:
        display_bins = int(
            driver.execute_script(
                "return window.__plutoTestHooks.viewState().canvas_display_bins;"
            )
        )
    script = """
const done = arguments[arguments.length - 1];
fetch('/api/view', {
  method: 'POST',
  headers: {'Content-Type': 'application/json'},
  body: JSON.stringify({
    visible_start_hz: arguments[0],
    visible_end_hz: arguments[1],
    display_bins: arguments[2]
  })
}).then(r => r.json()).then(done)
  .catch(e => done({status: 'error', message: String(e)}));
"""
    result = driver.execute_async_script(
        script, visible_start_hz, visible_end_hz, display_bins
    )
    if result.get("status") != "ok":
        raise RuntimeError(result)
    return result


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
        assert driver.find_element(By.ID, "infoReceiverRange").is_displayed()
        if driver.find_element(By.ID, "btnStart").text.strip().lower() != "run":
            raise RuntimeError("main scan action button is not labeled Run")
        Select(driver.find_element(By.ID, "timeMarks")).select_by_value("15")
        assert (
            driver.execute_script("return localStorage.getItem('timeMarksSeconds');")
            == "15"
        )
        assert driver.find_element(By.ID, "gainMode").is_displayed()
        assert driver.find_element(By.ID, "vgaGain").is_displayed()
        rf_port = driver.find_element(By.ID, "rfPort")
        assert rf_port.is_displayed()
        if rf_port.is_enabled():
            raise RuntimeError("single-channel Input selector is not disabled")
        if [option.get_attribute("value") for option in Select(rf_port).options] != [
            "A_BALANCED"
        ]:
            raise RuntimeError("Input selector exposes unsupported receiver channels")
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
            "hover_wide_integer_ticks": "10489.500 MHz",
            "hover_sub_100khz": "10489.500 300 MHz",
            "hover_tenth_hz": "10489.500 0023 MHz",
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
        rate_format = driver.execute_script(
            "return window.__plutoTestHooks.trueRateFormatting();"
        )
        if rate_format != {
            "high": " (12 lines/s)",
            "normal": " (2.3 lines/s)",
            "sub_one": " (0.34 lines/s)",
            "sub_tenth": " (0.014 lines/s)",
        }:
            raise RuntimeError(f"unexpected FFT rate formatting: {rate_format}")
        zoom_format = driver.execute_script(
            "return window.__plutoTestHooks.zoomFormatting();"
        )
        if zoom_format != {
            "one": "1.00x",
            "ten": "10.0x",
            "hundred": "100.0x",
            "over_hundred": "101x",
            "max": "1000000x",
        }:
            raise RuntimeError(f"unexpected zoom formatting: {zoom_format}")
        receiver_clamp = driver.execute_script(
            "return window.__plutoTestHooks.receiverClampCheck();"
        )
        if receiver_clamp != {
            "direct": {"start": "46.875001", "end": "6000"},
            "converter": {"start": "88", "end": "108"},
        }:
            raise RuntimeError(f"unexpected receiver clamp: {receiver_clamp}")
        hash_conflicts = driver.execute_script(
            "return window.__plutoTestHooks.hashViewConflictCheck();"
        )
        if hash_conflicts != {
            "contained": {"left": 434800000, "right": 435200000},
            "excluded": None,
            "malformed": None,
        }:
            raise RuntimeError(f"unexpected URL/typed-band precedence: {hash_conflicts}")

        api_stop(driver)
        driver.refresh()
        WebDriverWait(driver, 10).until(
            lambda d: d.find_element(By.ID, "statusText").text == "idle"
            and d.find_element(By.ID, "freqStart").is_enabled()
        )

        driver.set_script_timeout(5)
        cadence_probe = driver.execute_async_script(
            "const done=arguments[0];"
            "window.__plutoTestHooks.presentationCadenceProbe()"
            ".then(done).catch((error)=>done({error:String(error)}));"
        )
        if cadence_probe.get("error"):
            raise RuntimeError(f"presentation cadence probe failed: {cadence_probe}")
        normal_cadence = cadence_probe.get("normal", {})
        if normal_cadence.get("previews") != 3 or normal_cadence.get("live") != 1:
            raise RuntimeError(f"presentation cadence lost rows: {cadence_probe}")
        normal_gaps = [float(value) for value in normal_cadence.get("gaps_ms", [])]
        if len(normal_gaps) != 3 or any(
            value < 25.0 or value > 100.0 for value in normal_gaps
        ):
            raise RuntimeError(f"preview/live pacing was uneven: {cadence_probe}")
        early_cadence = cadence_probe.get("early", {})
        early_gaps = [float(value) for value in early_cadence.get("gaps_ms", [])]
        if (
            early_cadence.get("previews") != 1
            or early_cadence.get("live") != 1
            or len(early_gaps) != 1
            or early_gaps[0] < 25.0
            or early_gaps[0] > 100.0
        ):
            raise RuntimeError(
                f"early live row did not supersede queued previews smoothly: {cadence_probe}"
            )

        visibility_probe = driver.execute_async_script(
            "const done=arguments[0];"
            "window.__plutoTestHooks.visibilityPresentationProbe()"
            ".then(done).catch((error)=>done({error:String(error)}));"
        )
        if visibility_probe.get("error"):
            raise RuntimeError(
                f"hidden-window presentation probe failed: {visibility_probe}"
            )
        if visibility_probe != {
            "while_hidden": 0,
            "rendered": [3],
            "ordered_timestamps": [1000, 1000, 1100],
        }:
            raise RuntimeError(
                f"hidden-window rows or time marks are not ordered: {visibility_probe}"
            )

        for element_id, value in [("freqStart", "430"), ("freqEnd", "470"), ("converterFreq", "0")]:
            el = driver.find_element(By.ID, element_id)
            el.clear()
            el.send_keys(value)
        Select(driver.find_element(By.ID, "gainMode")).select_by_value("manual")
        driver.execute_script(
            "const el=document.getElementById('vgaGain');"
            "el.value='20'; el.dispatchEvent(new Event('input',{bubbles:true}));"
        )
        driver.execute_script(
            "history.replaceState(null, '', '#view=434800000-435200000');"
        )
        driver.find_element(By.ID, "btnStart").click()
        WebDriverWait(driver, 12).until(
            lambda d: d.find_element(By.ID, "statusText").text == "scanning"
        )
        hash_status = api_status(driver)
        if (
            abs(float(hash_status.get("visible_start_hz", 0)) - 434_800_000) > 5
            or abs(float(hash_status.get("visible_end_hz", 0)) - 435_200_000)
            > 5
        ):
            raise RuntimeError(f"URL hash view was not used on Run: {hash_status}")
        driver.find_element(By.ID, "btnStop").click()
        WebDriverWait(driver, 15).until(
            lambda d: d.find_element(By.ID, "statusText").text == "idle"
        )
        driver.find_element(By.ID, "btnStart").click()
        WebDriverWait(driver, 12).until(
            lambda d: d.find_element(By.ID, "statusText").text == "scanning"
        )
        hash_restart_status = api_status(driver)
        if (
            abs(float(hash_restart_status.get("visible_start_hz", 0)) - 434_800_000)
            > 5
            or abs(float(hash_restart_status.get("visible_end_hz", 0)) - 435_200_000)
            > 5
        ):
            raise RuntimeError(
                f"URL hash view was not preserved after Stop/Run: {hash_restart_status}"
            )
        driver.find_element(By.ID, "btnStop").click()
        WebDriverWait(driver, 15).until(
            lambda d: d.find_element(By.ID, "statusText").text == "idle"
        )
        driver.execute_script(
            "history.replaceState(null, '', location.pathname + location.search);"
        )
        driver.find_element(By.ID, "btnStart").click()
        WebDriverWait(driver, 12).until(
            lambda d: d.find_element(By.ID, "statusText").text == "scanning"
        )
        first_rf_status = api_status(driver)
        if first_rf_status.get("rf_port") != "A_BALANCED":
            raise RuntimeError("selected Pluto RX input was not sent to the backend")
        rf_port = driver.find_element(By.ID, "rfPort")
        if rf_port.is_enabled() or rf_port.get_attribute("value") != "A_BALANCED":
            raise RuntimeError("single-channel Input state changed while scanning")
        WebDriverWait(driver, 15).until(
            lambda d: "steps" in d.find_element(By.ID, "infoScan").text.lower()
        )
        WebDriverWait(driver, 15).until(
            lambda d: "430 - 470 MHz, Span: 40 MHz"
            in d.find_element(By.ID, "infoRange").text
        )
        wait_nonblank_waterfall(driver)

        # At both zoom boundaries, repeated zoom requests must be true no-ops:
        # they must not alter the URL viewport or materialize a new waterfall
        # presentation when the span cannot become smaller or larger.
        driver.execute_script("document.getElementById('gotoButton').click();")
        driver.execute_script(
            "document.getElementById('gotoFreq').value='435';"
            "document.getElementById('gotoTargetZoom').value='1000000';"
            "document.getElementById('gotoOk').click();"
        )
        WebDriverWait(driver, 12).until(
            lambda d: abs(
                d.execute_script("return window.__plutoTestHooks.viewState();")[
                    "span_hz"
                ]
                - 40
            )
            < 0.01
        )
        max_zoom_state = driver.execute_script(
            "return window.__plutoTestHooks.viewState();"
        )
        max_zoom_hash = driver.execute_script("return location.hash;")
        for _ in range(3):
            driver.find_element(By.ID, "zoomIn").click()
        time.sleep(0.25)
        after_max_zoom_state = driver.execute_script(
            "return window.__plutoTestHooks.viewState();"
        )
        if (
            abs(after_max_zoom_state["start_hz"] - max_zoom_state["start_hz"]) > 0.01
            or abs(after_max_zoom_state["end_hz"] - max_zoom_state["end_hz"]) > 0.01
            or driver.execute_script("return location.hash;") != max_zoom_hash
        ):
            raise RuntimeError("zoom-in past maximum changed the viewport")

        driver.find_element(By.ID, "zoomReset").click()
        WebDriverWait(driver, 12).until(
            lambda d: abs(
                d.execute_script("return window.__plutoTestHooks.viewState();")[
                    "span_hz"
                ]
                - 40_000_000
            )
            < 0.01
        )
        min_zoom_state = driver.execute_script(
            "return window.__plutoTestHooks.viewState();"
        )
        min_zoom_hash = driver.execute_script("return location.hash;")
        for _ in range(3):
            driver.find_element(By.ID, "zoomOut").click()
        time.sleep(0.25)
        after_min_zoom_state = driver.execute_script(
            "return window.__plutoTestHooks.viewState();"
        )
        if (
            abs(after_min_zoom_state["start_hz"] - min_zoom_state["start_hz"]) > 0.01
            or abs(after_min_zoom_state["end_hz"] - min_zoom_state["end_hz"]) > 0.01
            or driver.execute_script("return location.hash;") != min_zoom_hash
        ):
            raise RuntimeError("zoom-out past minimum changed the viewport")

        receiver_window = driver.current_window_handle
        driver.switch_to.new_window("tab")
        time.sleep(3.0)
        driver.close()
        driver.switch_to.window(receiver_window)
        WebDriverWait(driver, 5).until(
            lambda d: not d.execute_script("return document.hidden;")
        )
        time.sleep(0.6)
        time_mark_diag = driver.execute_script(
            "return window.__plutoTestHooks.timeMarkDiagnostics();"
        )
        if (
            time_mark_diag.get("backward_pairs") != 0
            or time_mark_diag.get("suspended")
            or time_mark_diag.get("hidden_pending")
        ):
            raise RuntimeError(
                f"time marks did not recover cleanly after tab switch: {time_mark_diag}"
            )

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

        # Exercise Principle 7 with the exact 453 Hz/630 Hz reported views.
        # Backend planning follows CSS pixels, while the canvas backing store
        # may be wider on a high-DPI display.
        fine_view = (155_760_937.0, 155_761_390.0)
        coarse_view = (155_760_838.0, 155_761_468.0)
        logical_bins = int(
            driver.execute_script(
                "return window.__plutoTestHooks.viewState().canvas_display_bins;"
            )
        )
        deep_payload = {
            "freq_start": 150,
            "freq_end": 160,
            "visible_start_hz": fine_view[0],
            "visible_end_hz": fine_view[1],
            "converter_freq": 0,
            "samplerate": 61440000,
            "rf_bandwidth": 20000000,
            "bw_ratio": 0.85,
            "gain_mode": "manual",
            "hardwaregain_db": 20,
            "fft_size": 1024,
            "display_bins": logical_bins,
            "rate_limit_lps": 20,
            "min_rate_lps": 10,
        }
        deep_plan = api_start(driver, deep_payload)
        expected_fine_decim = math.ceil(
            logical_bins * 4_000_000.0 / (fine_view[1] - fine_view[0]) / 2048
        )
        if (
            int(deep_plan.get("effective_fft_size", 0)) != 2048
            or int(deep_plan.get("decim_factor", 0)) != expected_fine_decim
            or float(deep_plan.get("true_line_rate", 1)) > 0.5
        ):
            raise RuntimeError(f"deep cadence test did not minimize FFT work: {deep_plan}")
        driver.execute_script(
            "window.__plutoTestHooks.clearPresentationHistory();"
        )
        WebDriverWait(driver, 20).until(
            lambda d: len(
                [
                    row
                    for row in d.execute_script(
                        "return window.__plutoTestHooks.presentationHistory();"
                    )
                    if int(row.get("preview", 0)) == 0
                ]
            )
            >= 3
        )
        time.sleep(0.5)

        driver.execute_script(
            "window.__plutoTestHooks.clearPresentationHistory();"
        )
        request_at_ms = float(driver.execute_script("return performance.now();"))
        deep_view_plan = api_view(
            driver,
            coarse_view[0],
            coarse_view[1],
            logical_bins,
        )
        expected_coarse_decim = math.ceil(
            logical_bins * 4_000_000.0 / (coarse_view[1] - coarse_view[0]) / 2048
        )
        if (
            deep_view_plan.get("transition") != "hot"
            or int(deep_view_plan.get("effective_fft_size", 0)) != 2048
            or int(deep_view_plan.get("decim_factor", 0))
            != expected_coarse_decim
        ):
            raise RuntimeError(
                f"compatible Principle 7 view restarted or over-sized FFT: {deep_view_plan}"
            )
        deep_view_id = int(deep_view_plan.get("view_id", 0))

        def deep_transition_rows(active_driver):
            return [
                row
                for row in active_driver.execute_script(
                    "return window.__plutoTestHooks.presentationHistory();"
                )
                if int(row.get("view", 0)) == deep_view_id
            ]

        WebDriverWait(driver, 12).until(
            lambda d: (
                (rows := deep_transition_rows(d))
                and any(int(row.get("preview", 0)) != 0 for row in rows)
                and sum(int(row.get("preview", 0)) == 0 for row in rows) >= 5
            )
        )
        deep_rows = deep_transition_rows(driver)
        first_row_ms = float(deep_rows[0]["at_ms"]) - request_at_ms
        if int(deep_rows[0].get("preview", 0)) != 1 or first_row_ms >= 2000.0:
            raise RuntimeError(
                "deep transition did not present history promptly: "
                f"first={first_row_ms:.1f}ms rows={deep_rows}"
            )
        deep_gaps_ms = [
            float(deep_rows[index]["at_ms"])
            - float(deep_rows[index - 1]["at_ms"])
            for index in range(1, len(deep_rows))
        ]
        if not deep_gaps_ms:
            raise RuntimeError("deep transition produced no measurable row gaps")
        if min(deep_gaps_ms) < 8.0 or max(deep_gaps_ms) > 1000.0:
            raise RuntimeError(
                "deep preview/live transition contained a burst or pause: "
                f"gaps={deep_gaps_ms}, rows={deep_rows}"
            )

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
                    "deep_transition_rows": len(deep_rows),
                    "deep_gap_min_ms": min(deep_gaps_ms),
                    "deep_gap_max_ms": max(deep_gaps_ms),
                    "screenshot": screenshot,
                },
                indent=2,
            )
        )
    finally:
        driver.quit()


if __name__ == "__main__":
    main()
