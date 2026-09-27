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


## H3-B2 DISPATCH-safe verifier

The first boot-time H3-B run on the target HP identified the real paging files
and observed 559 pagefile reads plus 537 pagefile writes, but all 1,096
completion callbacks arrived above APC_LEVEL. The original verifier therefore
skipped every fingerprint operation.

H3-B2 keeps the I/O path write-through and observational, but permits bounded
fingerprinting at DISPATCH_LEVEL only when the completed I/O buffer is backed by
an MDL or a system buffer. MmGetSystemAddressForMdlSafe is documented for use
through DISPATCH_LEVEL. Raw buffer fallbacks remain restricted to IRQL <=
APC_LEVEL.

To keep elevated-IRQL work bounded on the target's dual-core Celeron, H3-B2
fingerprints at most four 4 KiB pages per completed I/O. This is a sampled
integrity verifier, not a performance implementation. ShadowMismatches > 0
remains a stop condition.


## H3-B3 stale-sample protection

The H3-B2 target run proved DISPATCH_LEVEL hashing works, but produced two
mismatches. Because H3-B2 sampled only the first four pages of a completed
write, an older fingerprint outside that sampled subset could survive a later
large write and be compared against newer pagefile data.

H3-B3 makes the verifier deliberately conservative. Each known pagefile file
object has an epoch. Every observed pagefile write advances that pagefile's
epoch before the write is sent down the stack. A sampled fingerprint is valid
only for the same epoch. Reads capture the current epoch in pre-operation and
are verified only if that epoch is still current in post-operation and after
hashing. Thus any intervening write invalidates older samples rather than
turning stale verifier state into a false mismatch.

This reduces match coverage under heavy concurrent pagefile writes, which is
acceptable for this correctness gate. A mismatch remains a stop condition.
The Windows pagefile remains authoritative and H3-B3 still does not modify,
suppress, redirect, complete, or compress pagefile I/O.
