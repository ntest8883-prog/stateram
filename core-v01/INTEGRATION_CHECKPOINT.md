# StateRAM Core v0.1 — Windows integration checkpoint

Frozen source commit validated: `fe47d55737e73b97eeaa9ddcdee542af0831a7f4`

GitHub Actions run: `36068046793`

Status: **PASS**

Validated on a disposable Windows GitHub runner:

- source reconstruction: PASS
- native x64 Release build: PASS
- integrated Core self-check: PASS
- 3.75 GiB logical Workbench headless integration smoke: PASS
- Microsoft Defender custom directory scan: PASS, no threats found
- SHA-256 manifest generated: PASS
- candidate artifact packaging: PASS

Workbench smoke evidence:

- logical application state: 3840 MiB
- raw resident after explicit trim: 8 MiB
- sparse overlay: 432 KiB
- promotions exercised: 67
- streaming evictions exercised: 35
- smoke result: PASS

Core self-check evidence:

- logical state: 72 MiB
- raw resident: 4 MiB
- overlay: 8 KiB
- promotions: 5
- streaming evictions: 20
- write generations: 1
- result: PASS

Important boundary:

This is an **implementation integration checkpoint**, not the final 4→8 proof.
The final claim requires the agreed A/B/C evaluation on the guarded 4 GiB target machine:
A) correctness, B) capacity, C) experience.
