# StateRAM Windows true-reclamation subtest

This is a controlled Windows user-mode experiment for the still-open **real reclamation** gate.

It is intentionally different from the H1-C/H1-D private-frame EPT harnesses:

1. Reserve and commit a managed virtual region.
2. Fill every 4 KiB page with deterministic mixed data.
3. Compress only pages that produce a real payload reduction using the Windows XPRESS_HUFF Compression API.
4. Call `VirtualFree(..., MEM_DECOMMIT)` on each successfully compressed page.
5. Verify those virtual pages are now `MEM_RESERVE`.
6. Access selected decommitted pages normally. A vectored exception handler recommits exactly that page, decompresses its saved content, verifies integrity, and resumes the faulting instruction.
7. Modify restored pages, evict them again, and fault them back a second time to verify dirty-data preservation.
8. Report process working set and private-commit measurements before and after eviction.

The test does **not** claim arbitrary-application transparency. Its purpose is narrower and decisive: prove on Windows that StateRAM can keep logical data while actually decommitting its original storage so Windows can reuse that physical capacity.

The test also refuses to count incompressible pages as reclaimed. They remain resident.

Example:

```powershell
StateRAMTrueReclaim.exe --mib 256 --sample 512
```
