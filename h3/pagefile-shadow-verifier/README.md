# StateRAM H3-B5: canonical pagefile shadow verifier

H3-B5 is a write-through verifier. Windows' normal pagefiles remain authoritative.
The minifilter does not block, redirect, suppress, complete, rewrite, compress, or
otherwise alter pagefile I/O.

## Why H3-B5 exists

The H3-B4 target run produced 1,396 tracked read comparisons: 1,363 matches and
33 mismatches. The same run reported six distinct paging-file FILE_OBJECTs even
though the target is configured with pagefiles on only two partitions.

H3-B4 incorrectly treated FILE_OBJECT identity as pagefile identity. A newer
overlapping write through another FILE_OBJECT for the same underlying pagefile
could therefore escape the old object's write history and make a stale
fingerprint look like a mismatch.

H3-B5 separates:

- **paging-file object identity**: each observed FILE_OBJECT;
- **canonical paging-file identity**: the minifilter instance/volume that owns
  the paging file.

Windows supports one Pagefile.sys per partition, so all paging-file FILE_OBJECTs
observed by this minifilter instance on one volume are aliases of the same
pagefile. History and shadow entries are keyed by that canonical identity plus
byte offset, while FILE_OBJECTs remain tracked only so ordinary files on the
same volume are never mistaken for the pagefile.

## Read-buffer correction

In a post-read callback, if Filter Manager sets
`FLTFL_CALLBACK_DATA_NEW_SYSTEM_BUFFER`, the original pre-operation buffer is
not necessarily the buffer containing the data returned by the file system.
H3-B5 checks this flag and uses `FltGetNewSystemBufferAddress` before hashing.

The verifier also records:

- cross-FILE_OBJECT comparisons/matches/mismatches;
- new-system-buffer reads/comparisons/mismatches;
- canonical pagefile identities and FILE_OBJECT aliases;
- dynamic pagefile discoveries;
- history expiry/drop and race-invalidated samples.

## Reboot avoidance

Boot-time `SL_OPEN_PAGING_FILE` detection is retained, but H3-B5 adds a
DEMAND_START fallback for an already-running Windows session. For an unknown
FILE_OBJECT seen in an IRP-based paging read/write pre-operation callback,
H3-B5 calls `FsRtlIsPagingFile` at IRQL <= APC_LEVEL. If Windows identifies the
object as a paging file, the driver registers it and maps it to that volume's
canonical identity.

This allows the verifier logic to be exercised after a manual load in the same
Windows boot, rather than requiring another reboot merely to rediscover the
already-open pagefiles.

## Concurrent-write correctness

H3-B5 does not use pre-write sequence numbers as proof of final storage order.
If two overlapping writes are simultaneously in flight, both ranges remember
that uncertainty and neither completion may publish a fingerprint. This remains
true even if their post-operation callbacks happen in an apparently convenient
order.

If an in-flight range is evicted from the bounded history ring before its
completion arrives, the corresponding canonical pagefile enters a conservative
unknown-write state. New samples and comparisons are suppressed until that late
completion retires the barrier. If pagefile-object/identity capacity is ever
exceeded, verification fails closed and `PagefileTableFull` becomes a stop
signal.

## Bounded verifier state

- 32,768 direct-mapped shadow entries in nonpaged pool.
- 256 write-history range records per canonical pagefile identity.
- At most four 4 KiB pages fingerprinted per completed I/O.
- Two independent 64-bit FNV-style page fingerprints.
- History overflow/expiry makes a sample **untracked**, never a match.
- Newer overlapping writes invalidate older samples before comparison.

The history scan remains bounded and is verifier-stage code, not the eventual
production data path.

## Success rule

`ShadowMismatches > 0` is still an immediate stop condition.

A useful H3-B5 validation requires real pagefile writes, later tracked reads,
positive matches, and zero mismatches. Cross-object and buffer-source telemetry
is diagnostic; it does not lower the success criterion.

H3-B5 still saves no RAM and is not evidence that 4 GB behaves like 8 GB. It
only validates pagefile write-to-later-read semantics needed for the later H3
integration work.
