"""Capture real dashboard states on bank_test using Chrome DevTools.

Requires an installed Chromium browser and Python's optional `websockets`
package. No npm/build step is used. This script explicitly RESEEDS bank_test;
never run it alongside another integration-test process.
"""

import argparse
import base64
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import threading
import time
import urllib.request

from websockets.sync.client import connect


ROOT = Path(__file__).resolve().parents[1]


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def await_value(action, predicate, timeout=30):
    deadline = time.monotonic() + timeout
    last_error = None
    while time.monotonic() < deadline:
        try:
            value = action()
            if predicate(value):
                return value
        except (OSError, ValueError) as error:
            last_error = error
        threading.Event().wait(0.1)  # Test/browser polling, not engine synchronization.
    raise RuntimeError(f"browser/API condition timed out: {last_error}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", default=str(ROOT / "build-gcc" / "bank.exe"))
    parser.add_argument("--browser")
    args = parser.parse_args()
    candidates = [args.browser, shutil.which("chromium"), shutil.which("google-chrome"),
                  r"C:\Program Files\Google\Chrome\Application\chrome.exe",
                  r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"]
    browser = next((path for path in candidates if path and Path(path).is_file()), None)
    if browser is None:
        raise RuntimeError("Chromium browser missing; pass --browser")
    images = ROOT / "docs" / "img"
    if not images.is_dir():
        raise RuntimeError("docs/img must already exist")
    port, debug_port = free_port(), free_port()
    base = f"http://127.0.0.1:{port}"
    environment = dict(os.environ, BANK_DB_NAME="bank_test")

    def api(path, payload=None):
        data = None if payload is None else json.dumps(payload).encode()
        request = urllib.request.Request(base + path, data=data,
                                         headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(request, timeout=120) as response:
            return json.load(response)

    server = subprocess.Popen([args.binary, "--serve", "--port", str(port)],
                              cwd=ROOT, env=environment, stdin=subprocess.PIPE,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    chrome = None
    try:
        await_value(lambda: api("/api/stats"), lambda value: "running" in value)
        assert api("/api/reset", {"confirm": True})["reset"] is True
        assert api("/api/check", {})["passed"] is True
        with tempfile.TemporaryDirectory(prefix="bank-browser-", dir=os.environ.get("OMNIRUSH_TEMP")) as profile:
            chrome = subprocess.Popen([browser, "--headless", "--disable-gpu", "--hide-scrollbars",
                "--no-first-run", f"--user-data-dir={profile}",
                "--remote-debugging-address=127.0.0.1", f"--remote-debugging-port={debug_port}", "about:blank"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

            def tabs():
                with urllib.request.urlopen(f"http://127.0.0.1:{debug_port}/json", timeout=5) as response:
                    return json.load(response)

            pages = await_value(tabs, lambda value: any(tab["type"] == "page" for tab in value))
            page = next(tab for tab in pages if tab["type"] == "page")
            with connect(page["webSocketDebuggerUrl"], max_size=20_000_000) as websocket:
                sequence = 0

                def command(method, params=None):
                    nonlocal sequence
                    sequence += 1
                    websocket.send(json.dumps({"id": sequence, "method": method, "params": params or {}}))
                    while True:
                        result = json.loads(websocket.recv(timeout=120))
                        if result.get("id") == sequence:
                            if "error" in result:
                                raise RuntimeError(str(result["error"]))
                            return result.get("result", {})

                def evaluate(expression):
                    result = command("Runtime.evaluate", {"expression": expression,
                        "returnByValue": True, "awaitPromise": True})
                    if "exceptionDetails" in result:
                        raise RuntimeError(str(result["exceptionDetails"]))
                    return result["result"].get("value")

                def wait_dom(expression):
                    return await_value(lambda: evaluate(expression), bool, 120)

                def screenshot(name):
                    size = command("Page.getLayoutMetrics")["cssContentSize"]
                    encoded = command("Page.captureScreenshot", {"format": "png", "captureBeyondViewport": True,
                        "clip": {"x": 0, "y": 0, "width": size["width"], "height": size["height"], "scale": 1}})["data"]
                    (images / name).write_bytes(base64.b64decode(encoded))
                    print(f"captured docs/img/{name}")

                command("Page.enable")
                command("Emulation.setDeviceMetricsOverride", {"width": 1440, "height": 1200,
                    "deviceScaleFactor": 1, "mobile": False})
                command("Page.addScriptToEvaluateOnNewDocument", {"source":
                    "window.__dashboardErrors=[];window.addEventListener('error',e=>window.__dashboardErrors.push(e.message));"})
                command("Page.navigate", {"url": base})
                wait_dom("document.querySelectorAll('#accounts-body tr').length===50 && document.querySelector('#feed-status').textContent.includes('LIVE')")
                screenshot("dashboard-overview.png")

                evaluate("document.querySelector('[data-demo=RACE]').click()")
                wait_dom("document.querySelector('#demo-result').textContent.includes('Completed')")
                assert "lost updates observed" in evaluate("document.querySelector('#demo-result').textContent")
                screenshot("dashboard-race.png")

                evaluate("window.confirm=()=>true;document.querySelector('#reset-button').click()")
                wait_dom("document.querySelector('#demo-result').textContent.includes('Database reset and reseeded.')")
                evaluate("document.querySelector('[data-demo=SAFE]').click()")
                wait_dom("document.querySelector('#demo-result').textContent.includes('balances match')")
                evaluate("document.querySelector('[data-demo=DEADLOCK]').click()")
                wait_dom("document.querySelector('#demo-result').textContent.includes('workload finished')")
                evaluate("document.querySelector('#check-button').click()")
                wait_dom("document.querySelectorAll('#demo-result .status-committed').length===5")
                screenshot("dashboard-check.png")
                evaluate("document.querySelector('[data-demo=CRASH_RECOVER]').click()")
                wait_dom("document.querySelector('#demo-result').textContent.includes('Injected exception')")
                assert api("/api/check", {})["passed"] is True
                assert evaluate("window.__dashboardErrors") == []
                print("Real browser checks passed: race, safe, opposing transfers, I1-I5, abort/recover; no uncaught JS errors.")
            chrome.terminate()
            chrome.wait(timeout=20)
            chrome = None
    finally:
        if chrome is not None and chrome.poll() is None:
            chrome.terminate()
            chrome.wait(timeout=20)
        if server.poll() is None:
            server.stdin.write(b"\n")
            server.stdin.flush()
            server.wait(timeout=120)


if __name__ == "__main__":
    main()
