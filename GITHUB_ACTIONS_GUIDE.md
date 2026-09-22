# Run StateRAM on GitHub Actions

This repository contains the StateRAM-12 real-kernel candidate plus a
conservative GitHub Actions probe.

## Why GitHub Actions

For a public repository, GitHub's standard `ubuntu-24.04` runner is currently
a fresh VM with 4 vCPUs and 16 GiB RAM. It also provides passwordless sudo.

That makes it materially different from Kaggle's seccomp-filtered notebook
container.

## First run — capability only

The workflow:

```text
.github/workflows/stateram-probe.yml
```

does this:

1. prints kernel, RAM, process-security and cgroup information;
2. builds StateRAM locally on the VM;
3. runs `userfaultfd_probe` as the normal runner user;
4. runs it again through `sudo`;
5. checks for `UFFDIO_API OK`;
6. only if that succeeds, runs a tiny 64 MiB real StateRAM reconstruction test;
7. checks whether a writable child cgroup and `memory.max` are available;
8. uploads all logs/CSV files as a workflow artifact.

It does **not** attempt the 4 GiB → 12 GiB benchmark yet.

## GitHub UI steps

1. Create a new repository.
2. Upload the **contents** of this repository ZIP, preserving `.github/workflows/`.
3. Open the repository's **Actions** tab.
4. Select **StateRAM real-kernel probe**.
5. Click **Run workflow**.
6. Open the completed run and inspect the `Probe userfaultfd` step.
7. The decisive success line is:

```text
UFFDIO_API OK
```

8. Download the artifact `stateram-real-kernel-probe` if you want the full logs.

## What happens next

If both of these succeed:

```text
UFFDIO_API OK
memory.max_write=OK
```

and the 64 MiB StateRAM smoke test completes correctly, the next workflow
should scale real testing progressively:

```text
64 MiB
256 MiB
1 GiB
4→8 GiB
4→12 GiB
```

Only then should the 12 GiB StateRAM run be compared with the large-RAM
reference.
