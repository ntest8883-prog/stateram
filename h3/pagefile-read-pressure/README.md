# StateRAM H3-B controlled pagefile-read pressure harness

This is a user-mode validation helper for the H3-B verifier. It does **not** modify paging data or the pagefile.

The harness:

- queries the already-running H3-B Version 2 telemetry;
- refuses to run if mismatches already exist or boot-time pagefiles were not observed;
- creates a 96 MiB low-memory-priority child target filled with deterministic, low-compressibility private data;
- trims that child's working set;
- creates bounded, adaptive user-mode memory pressure while reserving commit headroom;
- stops pressure allocation if physical-memory or commit safety thresholds are reached;
- keeps pressure resident while the child re-reads and verifies every target byte;
- queries H3-B again and reports deltas;
- never resets H3-B counters;
- never changes driver/service/boot configuration;
- frees all user-mode allocations before exit.

A verifier pass requires a positive ShadowReadPages delta, a positive ShadowMatches delta, and zero additional ShadowMismatches.

The tool is intentionally single-pass so it cannot silently escalate into repeated thrashing. If it cannot generate a tracked read, the output is diagnostic rather than an automatic retry.

Before the full pressure pass, run:

`StateRAMH3BPressure.exe --preflight`

This performs a no-pressure machine compatibility check: H3-B query, pagefile-state sanity, 8 MiB allocation/trim/re-read data verification, and confirms no new verifier mismatches were introduced.
