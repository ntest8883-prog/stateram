import json
import os
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from selenium import webdriver
from selenium.webdriver.chrome.service import Service


EXTENSION_ID = "joobkllejoacdkjggddlbgljgkokccge"


class RequestState:
    def __init__(self):
        self.lock = threading.Lock()
        self.counts = {}

    def record(self, name):
        with self.lock:
            self.counts[name] = self.counts.get(name, 0) + 1

    def get(self, name):
        with self.lock:
            return self.counts.get(name, 0)

    def snapshot(self):
        with self.lock:
            return dict(self.counts)


STATE = RequestState()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        name = self.path.split("?", 1)[0].strip("/").split("/")[-1] or "root"
        name = "".join(ch for ch in name if ch.isalnum() or ch in "_-")
        STATE.record(name)

        body = f"""<!doctype html>
<html>
<head><title>StateRAM E2E {name}</title></head>
<body>
<h1 id="marker">StateRAM E2E {name}</h1>
<p>Real HTTP page used by StateRAM browser bridge validation.</p>
</body>
</html>""".encode("utf-8")

        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        return


def fail_if_js_error(result, label):
    if not isinstance(result, dict) or not result.get("ok"):
        raise RuntimeError(f"{label} failed: {result}")
    return result


def main():
    if len(sys.argv) < 3:
        raise SystemExit(
            "usage: browser_real_e2e_selenium.py <extension-dir> <output-json>"
        )

    extension_dir = Path(sys.argv[1]).resolve()
    output_path = Path(sys.argv[2]).resolve()

    if not (extension_dir / "manifest.json").exists():
        raise RuntimeError(f"missing extension manifest: {extension_dir}")

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server_thread = threading.Thread(target=server.serve_forever, daemon=True)
    server_thread.start()

    host, port = server.server_address
    base_url = f"http://{host}:{port}"

    options = webdriver.ChromeOptions()
    options.enable_bidi = True
    options.enable_webextensions = True
    options.add_argument("--headless=new")
    options.add_argument("--no-first-run")
    options.add_argument("--no-default-browser-check")
    options.add_argument("--disable-background-networking")
    options.add_experimental_option("enableExtensionTargets", True)

    service = None
    driver_dir = os.environ.get("CHROMEWEBDRIVER")
    if driver_dir:
        candidate = Path(driver_dir) / "chromedriver.exe"
        if candidate.exists():
            service = Service(str(candidate))

    driver = None
    extension_result = None

    try:
        if service:
            driver = webdriver.Chrome(service=service, options=options)
        else:
            driver = webdriver.Chrome(options=options)

        driver.set_script_timeout(30)

        extension_result = driver.webextension.install(path=str(extension_dir))
        extension_id = extension_result.get("extension")

        if extension_id != EXTENSION_ID:
            raise RuntimeError(
                f"unexpected extension id: {extension_id}; expected {EXTENSION_ID}"
            )

        diagnostics_url = (
            f"chrome-extension://{extension_id}/diagnostics.html"
        )
        driver.get(diagnostics_url)

        if driver.title != "StateRAM Browser Bridge Diagnostics":
            raise RuntimeError(
                f"diagnostics page did not load; title={driver.title!r}"
            )

        api_check = driver.execute_script(
            "return {tabs: typeof chrome.tabs, "
            "alarms: typeof chrome.alarms, "
            "storage: typeof chrome.storage};"
        )
        if api_check != {
            "tabs": "object",
            "alarms": "object",
            "storage": "object",
        }:
            raise RuntimeError(f"extension APIs unavailable: {api_check}")

        setup_script = r"""
const baseUrl = arguments[0];
const done = arguments[arguments.length - 1];

(async () => {
  async function waitComplete(tabId) {
    for (let i = 0; i < 120; i++) {
      const tab = await chrome.tabs.get(tabId);
      if (tab.status === "complete") return tab;
      await new Promise(resolve => setTimeout(resolve, 100));
    }
    throw new Error("tab did not finish loading: " + tabId);
  }

  async function create(name, options = {}) {
    const tab = await chrome.tabs.create({
      url: baseUrl + "/" + name,
      active: Boolean(options.active),
      pinned: Boolean(options.pinned)
    });
    await waitComplete(tab.id);
    return tab.id;
  }

  const pinned = await create("pinned", {
    pinned: true,
    active: false
  });

  const cold = [];
  for (let i = 1; i <= 4; i++) {
    const id = await create("cold" + i, {active: true});
    cold.push(id);
    await new Promise(resolve => setTimeout(resolve, 250));
  }

  const active = await create("active", {active: true});
  await chrome.tabs.update(active, {active: true});
  await new Promise(resolve => setTimeout(resolve, 500));

  const map = (await chrome.storage.local.get({
    lastActiveByTab: {}
  })).lastActiveByTab || {};

  done({
    ok: true,
    pinned,
    cold,
    active,
    tracked: {
      pinned: map[String(pinned)] ?? null,
      cold: cold.map(id => map[String(id)] ?? null),
      active: map[String(active)] ?? null
    }
  });
})().catch(error => {
  done({
    ok: false,
    error: String(error),
    stack: error && error.stack ? String(error.stack) : ""
  });
});
"""
        setup = fail_if_js_error(
            driver.execute_async_script(setup_script, base_url),
            "tab setup",
        )

        if any(v is None for v in setup["tracked"]["cold"]):
            raise RuntimeError(
                f"cold tab activation history was not recorded: {setup}"
            )

        # Use real wall-clock inactivity; no synthetic timestamp manipulation.
        time.sleep(65)

        snapshot_script = r"""
const ids = arguments[0];
const done = arguments[arguments.length - 1];

(async () => {
  const out = {};
  for (const id of [ids.pinned, ...ids.cold, ids.active]) {
    const tab = await chrome.tabs.get(id);
    out[String(id)] = {
      active: tab.active,
      pinned: tab.pinned,
      discarded: tab.discarded,
      autoDiscardable: tab.autoDiscardable,
      status: tab.status,
      title: tab.title,
      url: tab.url
    };
  }

  const state = await chrome.storage.local.get({
    totalDiscarded: 0,
    lastPressure: 0,
    lastDecision: "unset"
  });

  done({ok: true, tabs: out, state});
})().catch(error => {
  done({ok: false, error: String(error)});
});
"""
        before = fail_if_js_error(
            driver.execute_async_script(snapshot_script, setup),
            "pre-policy snapshot",
        )

        automatically_discarded = [
            tab_id
            for tab_id in setup["cold"]
            if before["tabs"][str(tab_id)]["discarded"]
        ]

        if before["tabs"][str(setup["active"])]["discarded"]:
            raise RuntimeError("active tab was discarded by automatic policy")

        if before["tabs"][str(setup["pinned"])]["discarded"]:
            raise RuntimeError("pinned tab was discarded by automatic policy")

        /*
         * The extension's natural one-minute alarm may already have fired by
         * this point. That is valid end-to-end behavior, not a failure.
         * If fewer than four eligible tabs were reclaimed naturally, schedule
         * the exact same registered alarm once more to finish the check.
         */
        trigger_script = r"""
const done = arguments[arguments.length - 1];
(async () => {
  await chrome.alarms.create("stateram-memory-check", {
    when: Date.now() + 500
  });
  done({ok: true});
})().catch(error => done({ok: false, error: String(error)}));
"""

        if len(automatically_discarded) < 4:
            fail_if_js_error(
                driver.execute_async_script(trigger_script),
                "alarm trigger",
            )

        after = None
        deadline = time.time() + 20

        while time.time() < deadline:
            after = fail_if_js_error(
                driver.execute_async_script(snapshot_script, setup),
                "post-policy snapshot",
            )

            discarded = [
                tab_id
                for tab_id in setup["cold"]
                if after["tabs"][str(tab_id)]["discarded"]
            ]

            if len(discarded) == 4:
                break

            time.sleep(0.5)

        if after is None:
            raise RuntimeError("no post-policy snapshot")

        discarded = [
            tab_id
            for tab_id in setup["cold"]
            if after["tabs"][str(tab_id)]["discarded"]
        ]

        if len(discarded) != 4:
            raise RuntimeError(
                "expected four eligible cold tabs to be discarded; "
                f"got {len(discarded)}; state={after}"
            )

        if after["tabs"][str(setup["active"])]["discarded"]:
            raise RuntimeError("StateRAM discarded active tab")

        if after["tabs"][str(setup["pinned"])]["discarded"]:
            raise RuntimeError("StateRAM discarded pinned tab")

        if int(after["state"]["lastPressure"]) != 3:
            raise RuntimeError(
                f"expected runtime pressure 3, got {after['state']['lastPressure']}"
            )

        if int(after["state"]["totalDiscarded"]) < 4:
            raise RuntimeError(
                f"discard accounting too small: {after['state']}"
            )

        reload_id = setup["cold"][0]
        requests_before = STATE.get("cold1")

        activate_script = r"""
const tabId = arguments[0];
const done = arguments[arguments.length - 1];
chrome.tabs.update(tabId, {active: true})
  .then(tab => done({ok: true, id: tab.id}))
  .catch(error => done({ok: false, error: String(error)}));
"""
        fail_if_js_error(
            driver.execute_async_script(activate_script, reload_id),
            "discarded tab activation",
        )

        reload_state = None
        reload_deadline = time.time() + 20

        single_tab_script = r"""
const tabId = arguments[0];
const done = arguments[arguments.length - 1];
chrome.tabs.get(tabId)
  .then(tab => done({
    ok: true,
    tab: {
      active: tab.active,
      discarded: tab.discarded,
      status: tab.status,
      title: tab.title,
      url: tab.url
    }
  }))
  .catch(error => done({ok: false, error: String(error)}));
"""

        while time.time() < reload_deadline:
            reload_state = fail_if_js_error(
                driver.execute_async_script(single_tab_script, reload_id),
                "reload snapshot",
            )["tab"]

            if (
                reload_state["active"]
                and not reload_state["discarded"]
                and reload_state["status"] == "complete"
                and reload_state["title"] == "StateRAM E2E cold1"
            ):
                break

            time.sleep(0.3)

        if (
            reload_state is None
            or not reload_state["active"]
            or reload_state["discarded"]
            or reload_state["status"] != "complete"
            or reload_state["title"] != "StateRAM E2E cold1"
        ):
            raise RuntimeError(
                f"discarded tab failed to reconstruct: {reload_state}"
            )

        requests_after = STATE.get("cold1")
        if requests_after <= requests_before:
            raise RuntimeError(
                "activating discarded tab did not produce a real HTTP reload"
            )

        result = {
            "pass": True,
            "browserName": driver.capabilities.get("browserName"),
            "browserVersion": driver.capabilities.get("browserVersion"),
            "extensionId": extension_id,
            "realHttpTabs": 6,
            "controlledRuntimePressure": 3,
            "realIdleSeconds": 65,
            "eligibleColdTabs": 4,
            "automaticAlarmDiscardedBeforeTrigger": len(
                automatically_discarded
            ),
            "coldTabsDiscarded": len(discarded),
            "activeTabProtected": not after["tabs"][str(setup["active"])][
                "discarded"
            ],
            "pinnedTabProtected": not after["tabs"][str(setup["pinned"])][
                "discarded"
            ],
            "discardedTabReconstructed": True,
            "realHttpReloadObserved": requests_after > requests_before,
            "totalDiscardedRecorded": int(after["state"]["totalDiscarded"]),
            "lastDecision": after["state"]["lastDecision"],
            "requestCounts": STATE.snapshot(),
        }

        output_path.write_text(
            json.dumps(result, indent=2) + "\n",
            encoding="utf-8",
        )

        print("STATERAM_REAL_CHROME_E2E=PASS")
        print(json.dumps(result, indent=2))

        driver.webextension.uninstall(extension_result)
        extension_result = None

        print("STATERAM_REAL_CHROME_EXTENSION_UNINSTALL=PASS")

    finally:
        if driver is not None:
            try:
                if extension_result is not None:
                    driver.webextension.uninstall(extension_result)
            except Exception as exc:
                print(
                    f"warning: extension cleanup failed: {exc}",
                    file=sys.stderr,
                )
            try:
                driver.quit()
            except Exception:
                pass

        server.shutdown()
        server.server_close()
        server_thread.join(timeout=5)


if __name__ == "__main__":
    main()
