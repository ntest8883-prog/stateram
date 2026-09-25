const HOST_NAME = "com.stateram.browser";
const ALARM_NAME = "stateram-memory-check";
const CHECK_MINUTES = 1;

async function loadState() {
  const data = await chrome.storage.local.get({
    lastActiveByTab: {},
    totalDiscarded: 0,
    lastPressure: 0,
    lastDecision: "idle"
  });
  return data;
}

async function saveState(patch) {
  await chrome.storage.local.set(patch);
}

function eligibleTab(tab) {
  if (!tab || tab.id === undefined) return false;
  if (tab.active || tab.pinned || tab.audible || tab.discarded) return false;
  if (tab.incognito) return false;
  if (tab.autoDiscardable === false) return false;

  const url = tab.url || tab.pendingUrl || "";
  if (!/^(https?:|file:)/i.test(url)) return false;

  return true;
}

async function ensureKnownTabs() {
  const tabs = await chrome.tabs.query({});
  const state = await loadState();
  const now = Date.now();
  const map = state.lastActiveByTab || {};

  for (const tab of tabs) {
    if (tab.id === undefined) continue;

    if (tab.active) {
      map[String(tab.id)] = now;
    } else if (map[String(tab.id)] === undefined) {
      /*
       * Fail conservative on restart: a tab with unknown history starts as
       * "recently active" instead of being immediately discardable.
       */
      map[String(tab.id)] = now;
    }
  }

  await saveState({lastActiveByTab: map});
}

async function setBridgeBadge(pressure, ok, discardedNow = 0) {
  if (!ok) {
    await chrome.action.setBadgeText({text: "!"});
    await chrome.action.setTitle({
      title: "StateRAM Browser Bridge: runtime unavailable"
    });
    return;
  }

  if (discardedNow > 0) {
    await chrome.action.setBadgeText({text: String(discardedNow)});
    await chrome.action.setTitle({
      title: `StateRAM Browser Bridge: reclaimed ${discardedNow} cold tab(s)`
    });
    return;
  }

  if (pressure > 0) {
    await chrome.action.setBadgeText({text: `P${pressure}`});
    await chrome.action.setTitle({
      title: `StateRAM Browser Bridge: memory pressure level ${pressure}`
    });
  } else {
    await chrome.action.setBadgeText({text: ""});
    await chrome.action.setTitle({
      title: "StateRAM Browser Bridge: memory pressure normal"
    });
  }
}

async function nativeDecision(status) {
  try {
    return await chrome.runtime.sendNativeMessage(HOST_NAME, status);
  } catch (error) {
    return {
      ok: false,
      reason: String(error),
      pressure: 0,
      discard: 0,
      minimumIdleSeconds: 4294967295
    };
  }
}

async function evaluateMemoryPressure() {
  const tabs = await chrome.tabs.query({});
  const state = await loadState();
  const now = Date.now();
  const map = state.lastActiveByTab || {};

  const candidates = [];

  for (const tab of tabs) {
    if (!eligibleTab(tab)) continue;

    const key = String(tab.id);
    const lastActive = Number(map[key] ?? now);
    const idleSeconds = Math.max(
      0,
      Math.floor((now - lastActive) / 1000)
    );

    candidates.push({
      id: tab.id,
      idleSeconds,
      lastActive
    });
  }

  candidates.sort((a, b) => a.lastActive - b.lastActive);

  const oldestIdleSeconds =
    candidates.length > 0
      ? candidates[0].idleSeconds
      : 0;

  const decision = await nativeDecision({
    type: "status",
    reclaimable: candidates.length,
    oldestIdleSeconds
  });

  if (!decision || decision.ok !== true) {
    await saveState({
      lastPressure: 0,
      lastDecision: decision?.reason || "runtime unavailable"
    });
    await setBridgeBadge(0, false);
    return;
  }

  const minimumIdleSeconds = Math.max(
    0,
    Number(decision.minimumIdleSeconds || 0)
  );

  const wanted = Math.max(
    0,
    Number(decision.discard || 0)
  );

  const eligible = candidates.filter(
    c => c.idleSeconds >= minimumIdleSeconds
  );

  let discardedNow = 0;

  for (const candidate of eligible.slice(0, wanted)) {
    try {
      const discarded = await chrome.tabs.discard(candidate.id);

      if (discarded && discarded.discarded) {
        discardedNow++;
      }
    } catch (_) {
      /*
       * Some browser/system pages can become protected between query and
       * discard. Ignore the individual tab rather than broadening privilege.
       */
    }
  }

  await saveState({
    totalDiscarded:
      Number(state.totalDiscarded || 0) + discardedNow,
    lastPressure: Number(decision.pressure || 0),
    lastDecision:
      discardedNow > 0
        ? `discarded ${discardedNow}`
        : "no reclaim needed"
  });

  await setBridgeBadge(
    Number(decision.pressure || 0),
    true,
    discardedNow
  );
}

chrome.runtime.onInstalled.addListener(async () => {
  await ensureKnownTabs();

  await chrome.alarms.create(ALARM_NAME, {
    delayInMinutes: 1,
    periodInMinutes: CHECK_MINUTES
  });

  await evaluateMemoryPressure();
});

chrome.runtime.onStartup.addListener(async () => {
  await ensureKnownTabs();

  await chrome.alarms.create(ALARM_NAME, {
    delayInMinutes: 1,
    periodInMinutes: CHECK_MINUTES
  });
});

chrome.alarms.onAlarm.addListener(async alarm => {
  if (alarm.name === ALARM_NAME) {
    await evaluateMemoryPressure();
  }
});

chrome.tabs.onActivated.addListener(async activeInfo => {
  const state = await loadState();
  const map = state.lastActiveByTab || {};

  map[String(activeInfo.tabId)] = Date.now();

  await saveState({
    lastActiveByTab: map
  });
});

chrome.tabs.onUpdated.addListener(async (tabId, changeInfo, tab) => {
  if (changeInfo.active === true || tab.active) {
    const state = await loadState();
    const map = state.lastActiveByTab || {};

    map[String(tabId)] = Date.now();

    await saveState({
      lastActiveByTab: map
    });
  }
});

chrome.tabs.onRemoved.addListener(async tabId => {
  const state = await loadState();
  const map = state.lastActiveByTab || {};

  delete map[String(tabId)];

  await saveState({
    lastActiveByTab: map
  });
});
