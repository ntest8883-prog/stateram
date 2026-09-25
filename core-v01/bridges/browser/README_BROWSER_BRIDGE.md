# StateRAM Browser Bridge v0.2

This is the first adapter aimed at making an **ordinary, unmodified application class** participate in StateRAM without process injection or a kernel driver.

The target is Chromium-based browsers such as Chrome and Edge.

## What it does

The browser extension tracks inactive, discardable tabs.

Once per minute it asks the StateRAM Runtime Host for the current machine memory-pressure policy through a native messaging bridge.

Under pressure, the adapter asks the browser to discard only eligible cold tabs.

A discarded tab:

- stays visible in the tab strip;
- is no longer kept as a live rendered tab;
- reloads when the user activates it again.

The browser itself remains responsible for reconstructing the tab. StateRAM supplies the **machine-level pressure and foreground policy** that decides when semantic reclamation is worthwhile.

## Why this is StateRAM

StateRAM's core idea is not "compress every byte ourselves."

It is:

> Keep the minimum information physically resident that is needed for the user's current experience, and represent colder state in the cheapest exact/reconstructible form available.

For a browser tab, the browser already knows how to turn a live rendered tab into a much cheaper dormant representation and reconstruct it later.

So the bridge uses that semantic capability instead of forcing browser pages through an unsafe external memory copier.

```text
browser tab
   │
   ├─ active / important ───────────────► keep live
   │
   └─ cold + reclaimable
            │
            ▼
      StateRAM Runtime pressure
            │
            ▼
      browser tab discard
            │
            ▼
      cheap dormant browser state
            │
      user activates tab
            ▼
      browser reconstructs tab
```

## Reclamation policy

The initial balanced policy is conservative:

| Runtime pressure | Earliest tab age | Max tabs reclaimed per check |
|---|---:|---:|
| 0 — normal | never | 0 |
| 1 — mild | 15 minutes | 1 |
| 2 — high | 5 minutes | 2 |
| 3 — emergency | 1 minute | 4 |

The extension never intentionally discards:

- the active tab;
- pinned tabs;
- audible tabs;
- incognito tabs;
- tabs the browser marks non-discardable;
- browser-internal pages.

The current check interval is one minute.

## Safety and reversibility

This bridge deliberately avoids:

- process injection;
- DLL injection;
- browser memory patching;
- kernel drivers;
- pagefile changes;
- Administrator privileges.

The extension uses the browser's supported tab-discard API.

The native messaging registration is current-user only and has a matching uninstall script.

The native host accepts messages only from the fixed StateRAM extension ID:

`joobkllejoacdkjggddlbgljgkokccge`

## Components

```text
StateRAM Runtime Host
        │
        ▼
StateRAM_Browser_NativeHost.exe
        │  Chromium native messaging
        ▼
StateRAM Browser Bridge extension
        │
        ▼
Chrome / Edge tab lifecycle
```

## Important limitations

This is a browser adapter, not whole-PC transparent StateRAM.

It can reduce browser pressure only by using tab lifecycle controls the browser already exposes.

It does not manage arbitrary Chrome heap objects, JavaScript state, GPU memory, or other applications.

Discarding/reloading a tab can lose purely in-page transient state that the website/browser does not persist. The conservative age thresholds, pinned/audible exclusions, and pressure-only policy reduce that risk but do not make it impossible.

## Product direction

The important result is architectural:

> StateRAM can coordinate an ordinary application's own semantic reclamation mechanisms from the machine-level runtime.

The same bridge model can later be used for other applications that expose supported plugin or lifecycle APIs.

This is preferable to unsafe injection whenever an official semantic integration point exists.
