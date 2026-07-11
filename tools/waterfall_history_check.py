#!/usr/bin/env python3
"""Deterministically validate retained-waterfall frequency mapping."""

import json
import os

from selenium import webdriver
from selenium.webdriver.firefox.options import Options
from selenium.webdriver.support.ui import WebDriverWait


BASE_URL = os.environ.get("BASE_URL", "http://localhost:8080")
FIREFOX_BIN = os.environ.get("FIREFOX_BIN", "firefox")


def main():
    options = Options()
    options.add_argument("-headless")
    if os.path.exists(FIREFOX_BIN):
        options.binary_location = FIREFOX_BIN

    driver = webdriver.Firefox(options=options)
    driver.set_window_size(1200, 900)
    try:
        driver.get(BASE_URL)
        WebDriverWait(driver, 15).until(
            lambda d: d.execute_script(
                "return !!window.__plutoTestHooks;"
            )
        )
        result = driver.execute_async_script(
            """
const done = arguments[arguments.length - 1];
window.__plutoTestHooks.waterfallHistoryCheck().then(done).catch(error => {
  done({error: String(error), checks: {}});
});
"""
        )
    finally:
        driver.quit()

    expected = {
        "identity": [10, 20, 30, 40, 50, 60, 70, 80],
        "zoom_in": [30, 40, 50, 60],
        "zoom_out": [0, 0, 20, 40, 60, 80, 0, 0],
        "no_overlap": [0] * 8,
        "reversed": [0] * 8,
    }
    errors = []
    for name, values in expected.items():
        if result["checks"].get(name) != values:
            errors.append(
                f"{name}: got {result['checks'].get(name)}, expected {values}"
            )
    if not result.get("canonical_round_trip"):
        errors.append("canonical redraw did not restore the original row")
    if not result.get("invalid_row_black"):
        errors.append("history row without frequency metadata was stretched")

    print(json.dumps({"status": "ok" if not errors else "error", **result,
                      "errors": errors}, indent=2))
    if errors:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
