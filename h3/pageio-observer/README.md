# StateRAM H3 page-I/O observer

This is the first H3 integration harness. It is **observation-only**: it does not modify, block, redirect, compress, or complete any filesystem I/O.

The driver registers Filter Manager callbacks for IRP_MJ_READ and IRP_MJ_WRITE with registration flags set to zero so paging I/O is included. It counts all observed read/write operations and bytes, IRP_PAGING_IO operations, and pagefile-specific operations only when it has observed a successful paging-file create marked with SL_OPEN_PAGING_FILE.

Important limitation: when loaded on-demand after boot, Windows pagefiles are normally already open, so pagefile-specific counters may remain zero even while generic paging-I/O counters increase. The same driver can later be used in a boot-time experiment to capture the pagefile create and distinguish exact pagefile traffic.

The installation script uses a temporary local-lab-only altitude and suppresses automatic attachment. It attaches only to C: and D: when explicitly started. A production minifilter would require an altitude allocated for the product by Microsoft.

No existing filters are disabled or changed.
