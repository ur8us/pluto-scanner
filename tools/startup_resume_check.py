#!/usr/bin/env python3
"""Verify that an explicitly running receive resumes after process restart."""

import json
import os
import shutil
import signal
import socket
import subprocess
import tempfile
import time
import urllib.request


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_BINARY = os.environ.get(
    "PLUTO_CIC_TEST_BINARY",
    os.path.join(ROOT, ".build", "tests", "pluto-scanner-cic-test"),
)


def request_json(port, path, method="GET", payload=None):
    """Call one local scanner endpoint and decode its JSON response."""
    body = None if payload is None else json.dumps(payload).encode("utf-8")
    request = urllib.request.Request(
        f"http://127.0.0.1:{port}{path}",
        data=body,
        method=method,
        headers={"Content-Type": "application/json"},
    )
    with urllib.request.urlopen(request, timeout=2) as response:
        return json.loads(response.read().decode("utf-8"))


def wait_for_scanning(port, process, timeout=10):
    """Wait until the restarted synthetic backend reports an active receive."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError("scanner exited before saved receive resumed")
        try:
            status = request_json(port, "/api/status")
            if status.get("scanning") and status.get("resume_scan"):
                return status
        except Exception:
            pass
        time.sleep(0.05)
    raise RuntimeError("saved receive did not resume within the timeout")


def stop_process(process):
    """Stop a scanner process through its normal SIGINT shutdown path."""
    if process.poll() is not None:
        return
    process.send_signal(signal.SIGINT)
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


def main():
    """Run two process lifetimes against one persisted receive intent."""
    if not os.path.isfile(TEST_BINARY):
        raise RuntimeError(f"missing synthetic binary: {TEST_BINARY}")

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]

    first = None
    second = None
    with tempfile.TemporaryDirectory(prefix="pluto-startup-resume-") as temp_dir:
        for asset in ("index.html", "bands.ini", "markers.ini"):
            shutil.copy2(os.path.join(ROOT, asset), os.path.join(temp_dir, asset))
        temp_binary = os.path.join(temp_dir, os.path.basename(TEST_BINARY))
        shutil.copy2(TEST_BINARY, temp_binary)
        with open(os.path.join(temp_dir, "pluto-scanner.conf"), "w", encoding="utf-8") as config:
            config.write(
                "freq_start = 430000000\n"
                "freq_end = 470000000\n"
                "visible_start = 430000000\n"
                "visible_end = 470000000\n"
                "resume_scan = 1\n"
            )

        env = os.environ.copy()
        env["PLUTO_SYNTHETIC_REALTIME"] = "1"
        command = [temp_binary, "--port", str(port)]
        first = subprocess.Popen(
            command,
            cwd=temp_dir,
            env=env,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.STDOUT,
        )
        wait_for_scanning(port, first)
        stop_process(first)
        first = None

        second = subprocess.Popen(
            command,
            cwd=temp_dir,
            env=env,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.STDOUT,
        )
        resumed = wait_for_scanning(port, second)
        stopped = request_json(port, "/api/stop", "POST", {})
        if stopped.get("status") != "ok":
            raise RuntimeError(f"explicit Stop failed: {stopped}")
        stop_process(second)
        second = None

        with open(os.path.join(temp_dir, "pluto-scanner.conf"), encoding="utf-8") as config:
            saved_config = config.read()
        if "resume_scan = 0" not in saved_config:
            raise RuntimeError("explicit Stop did not clear persisted resume intent")

        print(
            json.dumps(
                {
                    "status": "ok",
                    "resumed": bool(resumed.get("scanning")),
                    "view_id": resumed.get("view_id"),
                    "resume_scan": resumed.get("resume_scan"),
                }
            )
        )
    if first is not None:
        stop_process(first)
    if second is not None:
        stop_process(second)


if __name__ == "__main__":
    main()
