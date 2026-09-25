# StateRAM H0 — Baseline hypervisor gate

This branch is an isolated feasibility experiment for the transparent StateRAM research path.

## H0-A goal

Build the unmodified Windows/NT SimpleVisor virtualization core with a current Microsoft WDK toolchain before adding any StateRAM-specific EPT logic.

Upstream source:

- Repository: ionescu007/SimpleVisor
- Pinned upstream commit: 989d33b1bc6569965d7aad3bd50a8d35fa4c359e
- Original source headers and upstream README/license are preserved under h0/simplevisor/.

## What H0-A does not do

H0-A does **not**:

- change EPT permissions for StateRAM;
- compress or evict pages;
- alter Windows boot configuration;
- enable TESTSIGNING;
- install or start a driver on the target PC.

The GitHub Actions job only compiles the x64 NT driver and uploads the resulting build artifact.

## Gates

1. **H0-A — build baseline**: current CI can compile the pinned SimpleVisor NT source.
2. **H0-B — load/unload baseline**: manually load on the HP 650 with a reversible, demand-start test procedure.
3. **H0-C — controlled EPT trap**: only after H0-B passes, modify one controlled 4 KiB test mapping and verify a recoverable EPT violation.

No later gate is treated as proven by an earlier one.
