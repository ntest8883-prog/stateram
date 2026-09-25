# StateRAM Runtime Host v0.2

This is the beginning of the always-running StateRAM system layer.

It is **not** a file explorer and it does **not** claim to make arbitrary Windows applications faster yet.

## Why it exists

Core v0.1 proved that one cooperative application can keep large logical state while retaining only a smaller raw working set.

A real 4→8 experience needs more than one isolated library instance. Several StateRAM-enabled applications must cooperate around the same physical 4 GiB machine.

The Runtime Host provides that coordination.

```text
                 StateRAM Runtime Host
                         │
             system memory pressure
             foreground application
             global StateRAM raw budget
                         │
          ┌──────────────┼──────────────┐
          │              │              │
       adapter A      adapter B      adapter C
          │              │              │
       Core v0.1      Core v0.1      Core v0.1
          │              │              │
     application     application     application
```

## What the host does

The current per-user host:

- watches Windows physical-memory availability and memory load;
- detects the foreground process when Windows exposes one;
- accepts explicit interactive hints from adapters;
- tracks connected StateRAM-enabled processes;
- creates one global StateRAM raw-residency budget;
- gives foreground/interactive clients a larger share;
- reduces background-client raw targets first;
- tells each client when it should stream state out of raw residency.

The runtime client library automatically calls `sr_trim()` when the host recommends a smaller raw target.

This is how StateRAM starts becoming a **machine-level memory system** rather than a collection of unrelated demo programs.

## Safety

This v0.2 host is deliberately user-mode:

- no kernel driver;
- no process injection;
- no pagefile changes;
- no registry changes;
- no boot changes;
- no administrator privileges required.

The host only controls applications that explicitly connect through the StateRAM runtime client API.

## Important limitation

Ordinary Chrome, Word, VS Code, games, etc. do not automatically benefit yet.

The path to the actual product is:

```text
Core v0.1
   ↓
Runtime Host v0.2
   ↓
real application adapters/plugins
   ↓
increasingly transparent integration
   ↓
4 GiB + StateRAM should feel close to the 8 GiB reference
```

The Runtime Host is therefore infrastructure for the real goal, not the goal itself.

## End-product direction

The intended mature product is:

```text
Windows login
    ↓
StateRAM runtime starts automatically
    ↓
StateRAM-aware adapters/apps connect
    ↓
foreground state gets priority
background/cold state becomes cheaper
    ↓
less HDD paging and fewer memory-pressure stalls
```

A small settings/status application may eventually control the runtime, but the memory system itself should normally run in the background without user interaction.
