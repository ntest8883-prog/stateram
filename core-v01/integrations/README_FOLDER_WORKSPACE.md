# StateRAM Folder Workspace — first real v0.2 integration

This is the first post-v0.1 integration that uses **real user files** rather than synthetic semantic objects.

It is intentionally simple and read-only.

## What it does

The application opens a real folder and registers supported text/code files as StateRAM `FILE_REF` objects.

Supported examples include:

- Markdown and text;
- C/C++ headers and sources;
- Python, JavaScript, TypeScript, Java, C#, Go, Rust;
- JSON, YAML, TOML, INI/configuration;
- HTML/CSS/SQL/shell/PowerShell;
- logs and CSV.

StateRAM does **not** preload the folder.

Each file remains represented primarily by its real file backing. Requested ranges are promoted on demand, recently useful raw segments may remain resident, and cold segments can be streamed back out of raw residency.

The application provides:

```text
files
open N
search TEXT
stats
trim
quit
```

So it is already a useful folder/code corpus browser and search tool.

## Why this matters

Core v0.1 proved the cooperative architecture using a controlled Workbench.

This integration crosses the next boundary:

```text
synthetic application state
        ↓
actual files on disk
        ↓
StateRAM FILE_REF objects
        ↓
on-demand promotion
        ↓
real search / preview
        ↓
bounded raw working set
```

That is product/integration engineering rather than another memory benchmark.

## Correctness boundary for external changes

The initial integration treats each registered file as an immutable snapshot for the lifetime of the session.

Before every backing read, the provider checks the file's size and Windows last-write timestamp.

If the backing file changed externally, reconstruction **fails closed** rather than silently serving a mixture of old and new generations.

Restart/reopen the workspace after external edits.

A future SDK revision can provide general version/invalidation APIs for long-lived mutable external backing sources.

## Safety

Use:

```text
StateRAM_FolderWorkspace_Safe.exe <folder>
```

or the packaged helper script.

The safe launcher:

- refuses Administrator execution;
- requires at least 1024 MiB available RAM and <=80% memory load before starting;
- runs one child process inside a Windows Job Object;
- enforces a 768 MiB process-memory cap;
- aborts on system low-memory notification, <640 MiB available RAM, or >=88% memory load;
- changes no registry, pagefile, driver, boot, or system configuration.

## Scope

This integration is **read-only** with respect to user files.

Its purpose is to make StateRAM useful against genuine folders immediately while keeping file correctness and safety simple.

The next product decision should be driven by use, not another numbered test phase: editing/version invalidation, richer indexes, previews, or an adapter/plugin for an existing application.
