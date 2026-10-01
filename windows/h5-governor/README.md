# StateRAM H5 Adaptive Governor

H5 is the first StateRAM runtime aimed at **real, unmodified Windows applications** rather than a synthetic allocation harness.

It does not create RAM and does not claim that 4 GiB has become 8 GiB. Its purpose is to reduce the severe foreground stalls observed when the 4 GiB target laptop enters heavy memory pressure.

## Architecture

H5 uses documented Windows APIs only:

- `QueryWorkingSet` learns the pages that are actually resident while an app is active.
- `SetProcessInformation(ProcessMemoryPriority)` makes idle background app pages easier for Windows to trim while leaving foreground and CPU-busy work protected.
- `PrefetchVirtualMemory` prewarms the remembered hot set when an app returns to the foreground. Windows can issue large/concurrent I/O instead of waiting for many demand faults.
- `EmptyWorkingSet` is used only at critical pressure, for one large truly-idle background app at a time, after its hot set has been remembered.

H5 never injects code into another process, edits application memory, disables Windows paging, changes CPU priority, or installs a driver.

## Pressure policy

- Green: observe and learn; restore changed memory priorities.
- Yellow: idle background apps -> memory priority 4.
- Orange: idle background apps -> memory priority 2.
- Red: idle background apps -> memory priority 1.
- Critical red: one large background app may be trimmed if it has been idle >=20 s, CPU usage <0.5%, working set >=160 MiB, and the per-process trim cooldown has elapsed.

A process using >=2.5% total CPU, or used in the foreground in the last 5 seconds, is treated as active and is not sacrificed.

## Hot-set learning and return

The current foreground app group (window-owning process plus descendants) is sampled every 5 seconds. H5 remembers up to 128 MiB per process and up to 256 MiB per app group, preferring private pages over shared pages.

On a foreground switch, H5 restores normal memory priority and asynchronously prefetches remembered ranges. The prefetch cap automatically shrinks when available RAM is low.

## Commands

Self-test:

```powershell
StateRAMH5Governor.exe --selftest
```

Observe policy without changing process state:

```powershell
StateRAMH5Governor.exe --dry-run
```

Run active governor:

```powershell
StateRAMH5Governor.exe
```

Stop with `Ctrl+C`. Memory priorities changed by H5 are restored before exit.

## Acceptance boundary

H5 is not declared successful by counters alone. It must be compared against ordinary Windows on the same laptop using the user's actual applications (FL Studio/plugins, Go-Splitter, Office/multitasking, Asphalt 8 where practical). The relevant outcome is reduced severe lag / shorter return-to-app stalls without data corruption or destabilizing active work.
