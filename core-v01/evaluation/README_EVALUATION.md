# StateRAM Core v0.1 — bounded 4→8 evaluation

This is the **only planned local evaluation package** before the Core v0.1 4→8 verdict.

It runs the agreed three stages in one guarded process:

## A — correctness

- six semantic workspaces;
- 3.75 GiB total logical application state;
- deterministic foreground document/history writes;
- repeated dormancy/trim cycles;
- recipe + delta reconstruction;
- exact verification of every recorded foreground patch;
- exact reads from derived indexes and preview state.

Pass condition: zero lost or corrupted checked state.

## B — capacity

- all six 640 MiB workspaces remain registered simultaneously;
- logical application state must equal exactly 3840 MiB;
- samples span every semantic object across the whole logical range;
- on-demand promotion and streaming eviction must both occur;
- the broker must stay within a 640 MiB steady-state raw-residency ceiling; the capacity stage then forces a safe 32 MiB trim target to prove that streaming eviction actually occurs, with post-trim raw residency required to stay <=160 MiB.

This is a cooperative semantic capacity test. It does not claim that 3.75 GiB of arbitrary incompressible simultaneously-hot bytes are stored in 640 MiB. The ceiling is intentionally aligned with the Core v0.1 steady-state 4-to-8 design budget rather than forcing the prewarmer to stay artificially empty.

## C — experience

A repeatable interaction loop measures:

- workspace-visible-document wake;
- foreground edit;
- deep search-index request;
- preview request;
- file-backed asset request.

Local gates:

- switch P95 < 50 ms
- edit P95 < 50 ms
- search P95 < 150 ms
- preview P95 < 150 ms
- file-backed asset P95 < 150 ms
- no measured interactive operation >=500 ms

The exact distributions are written to CSV so the final judgment can use the real P50/P95/P99/max values rather than only a boolean gate.

## Why there is no full raw 3.75 GiB OFF run on the 4 GiB PC

A conventional full-raw control would intentionally force the target machine toward the condition we are trying to avoid. That violates the project safety rule:

> The PC is never allowed to become the experiment. Only StateRAM is.

Therefore the local evaluation does **not** deliberately materialize 3.75 GiB of raw application state.

The capacity comparison is instead explicit: the same 3840 MiB logical application model is maintained while StateRAM is constrained by a 1280 MiB hard process-commit safety envelope and by the Core's lower raw-residency budget.

This means the final 4→8 claim, if the evaluation passes, is:

> A cooperative semantic application can carry an 8-GiB-class logical workload on this 4 GiB PC while keeping foreground interaction responsive and preserving exact application state.

It is **not** a claim that Windows has acquired 8 GiB of physical RAM or that all arbitrary 8 GiB workloads will behave this way.

## Safety

The only local entry point is:

    StateRAM_4to8_Evaluator_Safe.exe

The launcher:

- refuses Administrator elevation;
- refuses to start below 1024 MiB available RAM or above 80% memory load;
- places the evaluator in a Job Object;
- enforces a 1280 MiB hard process-memory limit;
- allows only one child process;
- kills the child if the launcher exits;
- aborts below 640 MiB available RAM or at >=88% memory load;
- aborts sustained abnormal CPU pressure;
- has a 20-minute hard timeout;
- changes no pagefile, registry, driver, boot or security settings.

## Output

Send ChatGPT:

- `stateram_4to8_evaluation.json`
- `stateram_4to8_interactions.csv`
- `stateram_4to8_guard.json`

The verdict is based on those three files together.
