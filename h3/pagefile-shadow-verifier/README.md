# StateRAM H3-B: write-through pagefile shadow verifier

H3-B moves one step beyond the H3-A observer without changing paging semantics.

The normal Windows pagefile remains authoritative. The minifilter does not block,
redirect, suppress, complete, rewrite, or compress any pagefile I/O.

When a pagefile write completes successfully, H3-B fingerprints each full,
4 KiB-aligned page using two independent 64-bit FNV-style fingerprints and stores
only:

- pagefile file-object identity,
- pagefile byte offset,
- the two fingerprints.

No page contents are retained.

When a later pagefile read of a tracked offset completes successfully, H3-B
fingerprints the returned 4 KiB page and compares it with the most recently
completed tracked write for that same pagefile offset.

## Bounded state

The shadow table has 32,768 direct-mapped entries and is allocated from nonpaged
pool. Collisions replace the old entry and increment ShadowReplacements.
Reads whose current offset is not present increment ShadowUntracked.

Resetting telemetry advances a logical generation, so old entries are ignored
without zeroing the whole table on the paging path.

## Safety choices

- only NTFS volumes are attached;
- the first install is DEMAND_START with automatic attachment suppressed;
- an InstanceQueryTeardown callback allows explicit detach;
- exact pagefile tracking still depends on seeing SL_OPEN_PAGING_FILE, so the
  decisive verifier run is a single F7 boot-time experiment;
- completed I/O is observed only; I/O status/data are never modified;
- post-operation hashing is skipped above APC_LEVEL rather than doing expensive
  work at unsafe IRQL;
- MDL-backed buffers are mapped with MmGetSystemAddressForMdlSafe; unsupported
  buffers are counted and skipped.

A ShadowMismatches value greater than zero is a stop condition for this
experiment. H3-B is verification only and is not evidence of 4->8 performance.
