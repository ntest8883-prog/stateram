# StateRAM Research History

The repository contains two layers.

## Current implementation

`core-v01/` is the maintained Core v0.1 cooperative 4→8 implementation.

It contains the integrated runtime, Workbench, guarded evaluator, and frozen validation path.

## Historical prototypes

Root-level files such as:

- `stateram9.c/.h`
- `stateram12_real_bench.c`
- `stateram13_final_bench.c`
- `stateram_phase3_appaware.cpp`
- `stateram_phase4_capsule.cpp`
- `stateram_phase5_incremental.cpp`
- `stateram_phase6_packed.cpp`
- `phase7_host.cpp` through `phase14_governor.cpp`
- Linux cgroup scripts and userfaultfd probes

are preserved research artifacts.

They document the progression from:

```text
page/segment virtualization
→ recipe reconstruction
→ recipe + delta
→ packed dormant capsules
→ cooperative SDK
→ lifecycle reclamation
→ background reconstruction
→ repeated workspace switching
→ rare-stall diagnosis
→ foreground-aware scheduling
→ integrated semantic Core v0.1
```

They are **not** the recommended entry point for current development.

## Historical scope

The Linux line established a scoped real 4→12 capacity/correctness result for its recipe+delta benchmark under a hard 4 GiB cgroup with zero ordinary swap.

The Windows line then focused on weak-PC practical interaction and converged on the integrated Core v0.1 cooperative 4→8 architecture.

Historical results retain their original scope; they should not be generalized into claims about arbitrary applications.
