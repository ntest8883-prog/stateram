# StateRAM Core v0.1 — Guarded Local 4→8 Result

## Result

**Defined Core v0.1 cooperative 4→8 target: PASS**

Target workload:

- machine class: 4 GiB Windows PC
- logical application state: **3840 MiB**
- integration model: cooperative semantic StateRAM application

Final evaluator:

- A correctness: **PASS**
- B capacity: **PASS**
- C experience: **PASS**
- overall: **PASS**

## A — correctness

- foreground patches verified: **480**
- promotion failures: **0**

The evaluator repeatedly wrote to document/history state, streamed state out of raw residency, reacquired it, and verified the recorded foreground modifications.

## B — capacity

| Metric | Result |
|---|---:|
| Logical application state | 3840 MiB |
| Coverage samples | 120 |
| Raw before forced trim | 93.750 MiB |
| Raw after forced trim | 32.000 MiB |
| Overlay at capacity checkpoint | 1920 KiB |
| Promotions | 728 |
| Streaming evictions | 600 |

This is a semantic/reconstructible capacity result. It is **not** a claim that arbitrary 3.75 GiB of simultaneously-hot incompressible bytes were stored in 32 MiB.

## C — experience

| Interaction | P95 | P99 | Max |
|---|---:|---:|---:|
| Workspace-visible-state wake | 0.008 ms | 0.011 ms | 0.012 ms |
| Foreground edit | 3.609 ms | — | 11.054 ms |
| Deep search/index request | 22.454 ms | 32.366 ms | 69.388 ms |
| Preview request | 23.056 ms | — | 44.215 ms |
| File-backed asset request | 16.691 ms | — | 34.438 ms |

## Final process snapshot

- raw resident: **256 MiB**
- sparse overlay: **2640 KiB**
- promotions: **2013**
- streaming evictions: **989**
- promotion failures: **0**
- process working set: **276.426 MiB**
- process private memory: **278.965 MiB**
- process transfer reads: **0 MiB**
- process transfer writes: **0.012 MiB**

## Guard/safety result

- status: **COMPLETED_PASS**
- start available RAM: **1374.2 MiB**
- minimum available RAM: **1025.1 MiB**
- peak evaluator process memory: **314 MiB**
- peak sampled child CPU: **50.1% of total machine**
- peak sampled system busy: **100%**
- exit code: **0**

The brief 100% system-busy sample is retained as a future optimization concern; it did not cause the evaluator's interaction-latency or safety gates to fail.

## Scope of the claim

A successful Core v0.1 result supports:

> A cooperative semantic application can expose roughly 3.75 GiB of application logical state on the tested 4 GiB Windows PC, preserve checked foreground modifications across dormancy/reconstruction, and keep measured foreground interactions responsive while the runtime retains only a much smaller raw working set.

It does **not** support a claim that:

- 4 GiB DRAM literally becomes 8 GiB DRAM;
- all 8 GiB workloads behave equivalently;
- arbitrary unmodified applications are transparently virtualized;
- arbitrary incompressible fully-hot state is representable this way.

## Deliberate control limitation

The evaluator did not materialize a full 3840 MiB raw control on the 4 GiB target machine.

That was intentional: the project's safety rule is that the user's only PC must never become the experiment. A full raw control designed to exhaust memory would violate that rule.

The result should therefore be described as a **cooperative workload 4→8 result**, not as universal hardware equivalence.
