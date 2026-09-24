# StateRAM Core v0.1 — Cooperative 4→8 Memory Virtualization

StateRAM asks a different question from ordinary paging:

> What information must physically exist right now, and what is the cheapest exact representation of everything else?

Core v0.1 is a cooperative Windows user-mode runtime built around semantic objects, 256 KiB movement segments, partial residency, recipe/file-backed reconstruction, foreground write overlays, streaming dormancy, and foreground-aware scheduling.

## v0.1 result

The defined v0.1 target was a **4 GiB Windows machine carrying an ~8-GiB-class cooperative workload** without obvious swap-like interaction stalls.

On the guarded 4 GiB target PC, the evaluator carried **3840 MiB (3.75 GiB) of logical application state** and all three final gates passed:

- A — correctness: PASS
- B — capacity: PASS
- C — experience: PASS

Key local measurements:

- 480 foreground write patches verified
- 93.75 MiB raw before the forced streaming trim
- 32 MiB raw after trim
- 728 promotions and 600 streaming evictions during the capacity stage
- edit P95: 3.609 ms
- search P95: 22.454 ms
- preview P95: 23.056 ms
- file-backed asset P95: 16.691 ms
- no promotion failures
- final process working set: ~276.4 MiB
- guarded run completed without a safety abort

See [core-v01/RESULTS_4TO8.md](core-v01/RESULTS_4TO8.md) for the scoped interpretation.

## What this proves

Within the cooperative workload used for Core v0.1:

> A 4 GiB Windows machine can carry several gigabytes more logical application state than it could comfortably keep raw-resident, while StateRAM keeps only currently valuable information in RAM and preserves foreground writes through exact compact/reconstructible representations.

## What this does not prove

This release does not claim:

- that Windows now has 8 GiB of physical RAM;
- that every arbitrary 8 GiB workload works on 4 GiB;
- transparent acceleration of unmodified Chrome, VS Code, games, etc.;
- that 3.75 GiB of simultaneously-hot incompressible bytes can live in a few hundred MiB at DRAM latency;
- replacement of Windows virtual memory.

Core v0.1 is deliberately cooperative. Applications expose semantic state and access intent through the StateRAM API.

## Architecture

The integrated implementation lives under:

```text
core-v01/
  include/
  src/
  workbench/
  evaluation/
```

Core concepts:

```text
semantic object
      ↓
256 KiB segments
      ↓
RAW / COMPRESSED / RECIPE / RECIPE+DELTA / FILE_REF / DISCARDABLE
      ↓
foreground acquire + generation-safe writes
      ↓
streaming eviction + on-demand promotion
      ↓
background prewarm only when worthwhile
```

Foreground interaction always outranks background compaction/reconstruction.

## Safety model

The final evaluator:

- refuses Administrator elevation;
- requires memory headroom before starting;
- runs one isolated child process;
- uses a 1280 MiB hard process-memory Job Object limit;
- aborts on low-memory conditions;
- aborts sustained abnormal CPU pressure;
- applies a hard timeout;
- changes no driver, registry, pagefile, boot, or security settings.

## Historical research

The repository root still contains earlier experimental Linux and Windows prototypes. They are preserved as research history rather than presented as the current implementation.

See [docs/RESEARCH_HISTORY.md](docs/RESEARCH_HISTORY.md).

## Current direction

The research question for Core v0.1 is complete within its defined scope.

The next work is integration/product engineering, not another numbered micro-phase:

1. stabilize the cooperative SDK surface;
2. build useful application adapters/plugins;
3. test larger real cooperative software;
4. only then investigate transparent integration mechanisms if justified.

## Release scope

Version: **v0.1 — Cooperative 4→8**

See [RELEASE_NOTES_v0.1.md](RELEASE_NOTES_v0.1.md).
