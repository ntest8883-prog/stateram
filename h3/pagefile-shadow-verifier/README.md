# StateRAM H3-D1: one-shot guarded payload intervention

H3-D1 is the first StateRAM pagefile **intervention** experiment.

It inherits the H3-B5.1/H3-C2 canonical pagefile tracking, bounded write history,
two-hash verifier, and retained byte-for-byte payload mirror. Unlike H3-C2,
H3-D1 can complete **one** eligible pagefile read from the retained payload
instead of sending that one read to the lower file-system/pagefile path.

This is intentionally a tiny proof step. It is not a RAM-expansion claim and it
is not a production cache.

## Default state

The driver is passive after load. It observes normal Windows pagefile I/O and
builds the same verifier/payload evidence as H3-C2.

It cannot be armed until the current driver session has already observed at
least one normal pagefile read whose returned bytes match a retained payload
byte-for-byte.

## Intervention gates

A D1 intervention requires all of the following:

- explicit user-space `arm` command;
- no previous intervention in the current generation;
- no shadow mismatch;
- no payload mismatch;
- no pagefile table overflow or compromised tracking state;
- no unresolved dropped in-flight write;
- a known canonical pagefile identity;
- an exactly 4 KiB, page-aligned IRP paging read;
- a current shadow entry for that offset;
- bounded write history proving the write is still current;
- an exact retained payload for the same write sequence;
- both stored payload hashes matching the shadow hashes again immediately before use;
- a second history validation after payload copy;
- an MDL-backed destination buffer.

If any condition fails, the request falls through to ordinary Windows pagefile
I/O. A verifier mismatch while armed immediately disarms D1.

After one successful intervention, D1 automatically disarms and will not serve
another page in that generation.

## Protocol

H3-D1 uses protocol version 5.

`Query-StateRAMH3B.ps1` supports:

```powershell
-Command query
-Command reset
-Command arm
-Command disarm
```

Important telemetry:

- `InterventionEligible`
- `InterventionArmed`
- `InterventionAttempts`
- `InterventionServedPages`
- `InterventionFallbacks`
- `InterventionGuardRejects`
- `InterventionPayloadMisses`
- `InterventionHashRejects`
- `InterventionCapacity` (always 1 for D1)

## Acceptance rule

The D1 milestone is only interesting if all of these are true after an armed
test:

- `InterventionServedPages = 1`
- `ShadowMismatches = 0`
- `PayloadMismatches = 0`
- `InterventionHashRejects = 0`
- the machine remains stable and the workload's data verification passes.

A pass would prove that StateRAM can safely substitute one previously captured
page into a real Windows paging read under these guards. It would **not** yet
prove useful memory expansion, sustained paging acceleration, or 4 GB behaving
like 8 GB.

## Reboot policy

The package remains DEMAND_START and is designed for manual unload/reload in the
same boot. Do not reboot merely to move between H3-C2 and H3-D1 unless Windows
itself refuses a clean unload/load.
