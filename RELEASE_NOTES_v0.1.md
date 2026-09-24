# StateRAM Core v0.1 Release Notes

## StateRAM Core v0.1 — Cooperative 4→8

This release freezes the first integrated StateRAM architecture that met its predefined cooperative 4→8 target on the guarded 4 GiB Windows machine.

## Included architecture

- semantic object registry;
- 256 KiB physical movement segments;
- partial residency as a normal steady state;
- on-demand foreground promotion;
- generation-safe foreground write overlays;
- recipe + delta representations;
- file-backed references;
- discardable/rebuildable state;
- streaming dormancy/eviction;
- adaptive raw-residency budgeting;
- foreground-aware CPU scheduling;
- lightweight transition-based prewarm hints;
- guarded Windows evaluation launcher.

## Final result

Guarded local evaluation:

- **3840 MiB logical application state**
- correctness: PASS
- capacity: PASS
- experience: PASS
- overall: PASS

Representative local latencies:

- edit P95: 3.609 ms
- search P95: 22.454 ms
- preview P95: 23.056 ms
- file-backed asset P95: 16.691 ms

The final process working set snapshot was ~276.4 MiB, with no promotion failures.

## Correct interpretation

The v0.1 result is intentionally scoped:

> StateRAM can make a cooperative, semantically-described 8-GiB-class workload practical on the tested 4 GiB PC by keeping only immediately valuable information raw-resident and representing other state more cheaply.

This release does not claim universal 4→8 equivalence for arbitrary unmodified applications.

## Safety

No driver, pagefile change, registry modification, boot modification, process injection, or Administrator execution is required by the v0.1 evaluator.

## Research lineage

Earlier prototype files remain in the repository as historical evidence. The maintained implementation is now `core-v01/`.

## Next development target

The next milestone is not "Phase 16."

It is **integration engineering**:

- stabilize SDK ergonomics;
- create useful adapters/plugins;
- integrate a genuinely useful cooperative application;
- measure sustained real-use behavior;
- investigate transparent integration only if cooperative deployment proves valuable.
