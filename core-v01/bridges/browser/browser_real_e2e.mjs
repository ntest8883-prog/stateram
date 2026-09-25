import http from "node:http";
import path from "node:path";
import process from "node:process";
import fs from "node:fs";
import puppeteer from "puppeteer";

const EXTENSION_ID = "joobkllejoacdkjggddlbgljgkokccge";

if (process.argv.length < 3) {
  console.error("usage: node browser_real_e2e.mjs <extension-dir> [output-json]");
  process.exit(2);
}

const extensionDir = path.resolve(process.argv[2]);
const outputPath = path.resolve(process.argv[3] || "browser_real_e2e.json");

if (!fs.existsSync(path.join(extensionDir, "manifest.json"))) {
  throw new Error(`Missing extension manifest: ${extensionDir}`);
}

function sleep(ms) {
  return new Promise(resolve => setTimeout(resolve, ms));
}

const requests = new Map();

const server = http.createServer((req, res) => {
  const u = new URL(req.url || "/", "http://127.0.0.1");
  const name = (u.pathname.split("/").filter(Boolean).pop() || "root")
    .replace(/[^a-zA-Z0-9_-]/g, "");

  requests.set(name, (requests.get(name) || 0) + 1);

  res.writeHead(200, {
    "Content-Type": "text/html; charset=utf-8",
    "Cache-Control": "no-store",
    "Connection": "close"
  });

  res.end(`<!doctype html>
<html>
<head><title>StateRAM E2E ${name}</title></head>
<body>
  <h1 id="marker">StateRAM E2E ${name}</h1>
  <p>This is a real HTTP tab used by the StateRAM browser bridge end-to-end check.</p>
</body>
</html>`);
});

await new Promise((resolve, reject) => {
  server.once("error", reject);
  server.listen(0, "127.0.0.1", resolve);
});

const address = server.address();
const baseUrl = `http://127.0.0.1:${address.port}`;

let browser;

try {
  browser = await puppeteer.launch({
    headless: true,
    pipe: true,
    dumpio: true,
    enableExtensions: [extensionDir],
    args: [
      "--no-first-run",
      "--no-default-browser-check",
      "--disable-background-networking"
    ]
  });

  const extensions = await browser.extensions();
  const extension = extensions.get(EXTENSION_ID);

  if (!extension) {
    const visible = [...extensions.entries()].map(([id, ext]) => ({
      id,
      name: ext.name,
      version: ext.version
    }));
    throw new Error(
      `StateRAM extension ID not loaded. Visible extensions: ${JSON.stringify(visible)}`
    );
  }

  if (extension.name !== "StateRAM Browser Bridge") {
    throw new Error(`Unexpected extension name: ${extension.name}`);
  }

  const workerTarget = await browser.waitForTarget(
    target =>
      target.type() === "service_worker" &&
      target.url() === `chrome-extension://${EXTENSION_ID}/background.js`,
    {timeout: 20000}
  );

  const worker = await workerTarget.worker();
  if (!worker) {
    throw new Error("StateRAM extension service worker unavailable");
  }

  const setup = await worker.evaluate(async baseUrlArg => {
    async function waitComplete(tabId) {
      for (let i = 0; i < 100; i++) {
        const tab = await chrome.tabs.get(tabId);
        if (tab.status === "complete") return tab;
        await new Promise(resolve => setTimeout(resolve, 100));
      }
      throw new Error(`tab ${tabId} did not finish loading`);
    }

    async function create(name, options = {}) {
      const tab = await chrome.tabs.create({
        url: `${baseUrlArg}/${name}`,
        active: Boolean(options.active),
        pinned: Boolean(options.pinned)
      });
      await waitComplete(tab.id);
      return tab.id;
    }

    const pinned = await create("pinned", {pinned: true, active: false});
    const cold = [];

    for (let i = 1; i <= 4; i++) {
      const id = await create(`cold${i}`, {active: true});
      cold.push(id);
      await new Promise(resolve => setTimeout(resolve, 250));
    }

    const active = await create("active", {active: true});
    await chrome.tabs.update(active, {active: true});

    await new Promise(resolve => setTimeout(resolve, 500));

    const snapshot = {};
    for (const id of [pinned, ...cold, active]) {
      const tab = await chrome.tabs.get(id);
      snapshot[id] = {
        active: tab.active,
        pinned: tab.pinned,
        discarded: tab.discarded,
        autoDiscardable: tab.autoDiscardable,
        url: tab.url,
        title: tab.title
      };
    }

    return {pinned, cold, active, snapshot};
  }, baseUrl);

  /*
   * Use real wall-clock idle time. Emergency browser policy requires one
   * minute of inactivity. No timestamp is forged for this end-to-end check.
   */
  await sleep(65000);

  const before = await worker.evaluate(async ids => {
    const out = {};
    for (const id of [ids.pinned, ...ids.cold, ids.active]) {
      const tab = await chrome.tabs.get(id);
      out[id] = {
        active: tab.active,
        pinned: tab.pinned,
        discarded: tab.discarded,
        autoDiscardable: tab.autoDiscardable,
        url: tab.url,
        title: tab.title
      };
    }
    return out;
  }, setup);

  for (const id of setup.cold) {
    if (before[id].discarded) {
      throw new Error(`Cold tab ${id} was discarded before StateRAM alarm`);
    }
  }

  if (before[setup.active].discarded) {
    throw new Error("Active tab was already discarded");
  }

  if (before[setup.pinned].discarded) {
    throw new Error("Pinned tab was already discarded");
  }

  /*
   * Trigger the same registered alarm path used during normal operation.
   * This only shortens the wait until the next scheduler tick; the extension
   * still executes its real alarm handler, native messaging, runtime policy,
   * eligibility rules, and chrome.tabs.discard calls.
   */
  await worker.evaluate(async () => {
    await chrome.alarms.create("stateram-memory-check", {
      when: Date.now() + 500
    });
  });

  const deadline = Date.now() + 20000;
  let after;

  while (Date.now() < deadline) {
    after = await worker.evaluate(async ids => {
      const out = {};
      for (const id of [ids.pinned, ...ids.cold, ids.active]) {
        const tab = await chrome.tabs.get(id);
        out[id] = {
          active: tab.active,
          pinned: tab.pinned,
          discarded: tab.discarded,
          autoDiscardable: tab.autoDiscardable,
          url: tab.url,
          title: tab.title
        };
      }

      const state = await chrome.storage.local.get({
        totalDiscarded: 0,
        lastPressure: 0,
        lastDecision: "unset"
      });

      return {tabs: out, state};
    }, setup);

    const discardedCold = setup.cold.filter(
      id => after.tabs[id].discarded
    );

    if (discardedCold.length === 4) break;
    await sleep(500);
  }

  if (!after) {
    throw new Error("No post-policy browser state was captured");
  }

  const discardedCold = setup.cold.filter(
    id => after.tabs[id].discarded
  );

  if (discardedCold.length !== 4) {
    throw new Error(
      `Expected all 4 eligible cold tabs to be discarded; got ${discardedCold.length}. State=${JSON.stringify(after)}`
    );
  }

  if (after.tabs[setup.active].discarded) {
    throw new Error("StateRAM discarded the active tab");
  }

  if (after.tabs[setup.pinned].discarded) {
    throw new Error("StateRAM discarded the pinned tab");
  }

  if (Number(after.state.lastPressure) !== 3) {
    throw new Error(
      `Expected emergency StateRAM pressure 3; got ${after.state.lastPressure}`
    );
  }

  if (Number(after.state.totalDiscarded) < 4) {
    throw new Error(
      `Extension did not account for four discarded tabs: ${JSON.stringify(after.state)}`
    );
  }

  const reloadId = setup.cold[0];
  const reloadName = "cold1";
  const requestsBeforeReload = requests.get(reloadName) || 0;

  await worker.evaluate(async tabId => {
    await chrome.tabs.update(tabId, {active: true});
  }, reloadId);

  let reloaded = null;
  const reloadDeadline = Date.now() + 20000;

  while (Date.now() < reloadDeadline) {
    reloaded = await worker.evaluate(async tabId => {
      const tab = await chrome.tabs.get(tabId);
      return {
        active: tab.active,
        discarded: tab.discarded,
        status: tab.status,
        title: tab.title,
        url: tab.url
      };
    }, reloadId);

    if (
      reloaded.active &&
      !reloaded.discarded &&
      reloaded.status === "complete" &&
      reloaded.title === "StateRAM E2E cold1"
    ) {
      break;
    }

    await sleep(300);
  }

  if (
    !reloaded ||
    !reloaded.active ||
    reloaded.discarded ||
    reloaded.status !== "complete" ||
    reloaded.title !== "StateRAM E2E cold1"
  ) {
    throw new Error(
      `Discarded tab did not reconstruct correctly: ${JSON.stringify(reloaded)}`
    );
  }

  const requestsAfterReload = requests.get(reloadName) || 0;
  if (requestsAfterReload <= requestsBeforeReload) {
    throw new Error(
      "Activating the discarded tab did not cause a real HTTP reload"
    );
  }

  const result = {
    pass: true,
    extensionId: EXTENSION_ID,
    extensionName: extension.name,
    realHttpTabs: 6,
    controlledRuntimePressure: 3,
    realIdleSeconds: 65,
    eligibleColdTabs: 4,
    coldTabsDiscarded: discardedCold.length,
    activeTabProtected: !after.tabs[setup.active].discarded,
    pinnedTabProtected: !after.tabs[setup.pinned].discarded,
    discardedTabReconstructed: true,
    realHttpReloadObserved:
      requestsAfterReload > requestsBeforeReload,
    totalDiscardedRecorded: Number(after.state.totalDiscarded),
    lastDecision: after.state.lastDecision,
    requestCounts: Object.fromEntries(requests.entries())
  };

  fs.writeFileSync(
    outputPath,
    JSON.stringify(result, null, 2) + "\n",
    "utf8"
  );

  console.log("STATERAM_REAL_CHROME_E2E=PASS");
  console.log(JSON.stringify(result, null, 2));

  await browser.uninstallExtension(EXTENSION_ID);

  const remaining = await browser.extensions();
  if (remaining.has(EXTENSION_ID)) {
    throw new Error("StateRAM extension remained installed after uninstall");
  }

  console.log("STATERAM_REAL_CHROME_EXTENSION_UNINSTALL=PASS");
} finally {
  if (browser) {
    await browser.close().catch(() => {});
  }

  await new Promise(resolve => server.close(resolve));
}
