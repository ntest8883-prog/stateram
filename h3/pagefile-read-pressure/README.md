# StateRAM H3-B5 controlled pagefile-read pressure harness

This is a user-mode validation helper for the H3-B5 verifier. It does **not**
modify pagefile contents, paging data, the driver service, or boot settings.

The harness:

- speaks the H3-B5 Version 3 / 288-byte telemetry ABI;
- requires an already-running verifier with zero mismatches;
- requires at least one canonical pagefile identity and no pagefile-table overflow;
- refuses to begin if an evicted in-flight write is still unresolved;
- creates a 96 MiB low-memory-priority child filled with deterministic,
  low-compressibility private data;
- trims the child's working set so Windows can make it cold/pageable;
- creates bounded, adaptive user-mode pressure while preserving a 256 MiB
  physical safety floor and 1024 MiB commit reserve;
- checks verifier telemetry every 64 MiB instead of blindly filling memory;
- stops early on fresh pagefile evidence, a tracked match, history pressure,
  table exhaustion, mismatch, or unresolved dropped-in-flight history;
- keeps allocated pressure resident while the child re-reads and verifies every
  target byte;
- frees all user-mode allocations before exit.

It never resets H3-B5 counters and never silently retries or escalates.

A verifier pass requires:

- positive `ShadowReadPages` delta;
- positive `ShadowMatches` delta;
- zero additional `ShadowMismatches`;
- `PagefileTableFull == 0`.

Before the full pressure pass, run:

`StateRAMH3BPressure.exe --preflight`

The preflight applies no sustained memory pressure. It checks the live Version 3
ABI, pagefile identity state, mismatch/table/in-flight safety gates, an 8 MiB
allocation + working-set trim + full deterministic reread, and then checks that
no verifier mismatch appeared.

This harness is only for proving H3-B5 write-to-later-read semantics. A pass does
not itself save RAM or prove the final 4 GB -> 8 GB StateRAM objective.
