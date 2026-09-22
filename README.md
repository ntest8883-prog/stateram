# StateRAM-12 — Real 4→12 Candidate

This package marks a deliberate change in the project:

> Stop expanding the synthetic frontier. Make 4 GiB → 12 GiB real.

## What changed from the simulator

The RAM budget is no longer an integer in Python.

The real StateRAM run is intended to execute under Linux **cgroup v2**:

```text
memory.max = 4 GiB
memory.swap.max = 0
```

so the StateRAM process cannot quietly borrow ordinary swap to fake success.

The workload creates **12 GiB of real virtual logical state** using the
StateRAM recipe+delta userfaultfd engine.

State is split into **8 MiB execution segments**. The engine now has real
range operations:

```text
stateram9_materialize_range()
stateram9_checkpoint_range()
stateram9_dematerialize_safe_range()
```

These are the kernel-facing counterpart of StateRAM-11.6 segmented execution
shadowing.

## What the measured loop does

For every interaction:

```text
scheduled interaction
       ↓
measure any prefetch overrun / lateness
       ↓
materialize demanded 8 MiB segment if needed
       ↓
touch every page in that segment
       ↓
occasionally mutate sparse pages
       ↓
record foreground latency
       ↓
prefetch the next 2 execution segments
       ↓
prefetch one plausible next object's entry segment
       ↓
evict LRU segments if raw-state target is exceeded
       ↓
checkpoint sparse deltas before safe dematerialization
```

The CSV records:

- interaction foreground latency
- background-overrun lateness
- experienced latency = foreground + lateness
- actual process RSS
- StateRAM tracked raw residency
- userfaultfd missing faults
- userfaultfd write faults
- background and eviction work

## Why the raw target is ~3 GiB rather than 4 GiB

The process also needs:

- page representation metadata
- dirty tracking
- userfaultfd handler
- stacks / code / libraries
- sparse delta storage

The **whole process** is hard-capped at 4 GiB by the cgroup.

Keeping raw materialized state around 3 GiB leaves headroom for the rest of
StateRAM rather than pretending metadata is free.

## Three required experiments

### A — Large-RAM reference

On a Linux machine with comfortably more than 12 GiB physical RAM:

```bash
./run_12g_reference.sh
```

RAW mode pre-touches the complete 12 GiB state before timing interactions.

### B — Ordinary 4 GiB control

If the machine has swap configured:

```bash
./run_ordinary_4g_control.sh
```

This shows how the same 12 GiB raw workload behaves under a real 4 GiB cgroup
with ordinary Linux swapping.

### C — StateRAM 4→12

```bash
./run_stateram_4to12.sh
```

This is the experiment that matters.

It uses:

```text
12 GiB logical state
4 GiB cgroup hard memory limit
0 ordinary swap for the StateRAM process
8 MiB segmented execution shadowing
recipe + delta checkpointing
```

## Build

```bash
./build.sh
```

Before anything else:

```bash
./userfaultfd_probe
```

The probe needs a successful `UFFDIO_API`.

## Evidence standard

We do **not** call 4→12 achieved merely because the process completes.

The StateRAM run must be compared with the large-RAM reference on:

```text
experienced P50
experienced P95
experienced P99
% interactions >10 ms
% interactions >50 ms
peak RSS
fault counts
CPU/background work
```

A crash-free 12 GiB workload under 4 GiB is capacity evidence.

A latency distribution close to the large-RAM reference is the evidence needed
for the actual StateRAM claim.

## Current limitation

This package compiles in the ChatGPT environment, but the StateRAM run cannot
execute here because this environment does not expose userfaultfd.

So the package is a **real-kernel experimental candidate**, not a claimed
real-hardware result.

The next scientific result must come from a Linux host with:

- userfaultfd missing + write-protect support
- cgroup v2
- at least ~16 GiB host RAM for the reference run
- preferably an HDD if the ordinary-swap control is meant to match the
  original StateRAM target hardware
