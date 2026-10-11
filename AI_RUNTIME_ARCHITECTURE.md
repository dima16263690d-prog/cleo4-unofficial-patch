# CLEO 4.4.4 — Current Runtime Architecture

## Scope

This document describes the active single-threaded runtime. The project does not use or prepare a separate Scheduler or worker pool. GTA SA continues to own script execution order through its active-script queue.

The goal is to improve the existing CLEO 4.4.4 engine's performance, memory handling, and stability while preserving legacy .cs, .cs3, and .cs4 scripts, opcodes, and plugin behavior.

## Execution path

The processing hook keeps the original execution model:

```text
GTA ProcessScript hook
        |
        +-- delete scripts queued for deferred destruction
        +-- initialize CLEO when GTA's active queue is ready
        +-- ScriptProcessBefore callback
        |
        +-- native GTA script -> GTA ProcessScript
        |
        +-- CLEO script -> CCustomScript::Process()
        |
        +-- ScriptProcessAfter callback
```

There is no independent scheduler. There is no parallel execution of legacy CLEO scripts. Calls to GTA APIs, RenderWare, shared SCM state, and legacy opcode handlers stay on the GTA game thread.

## Script lifecycle

`CScriptEngine` owns custom-script registration and lifecycle:

- Preserve native GTA scripts' order and append CLEO scripts to the active queue.
- Register custom script objects and their names.
- Track active and pending-delete custom scripts.
- Remove parent/child script trees safely.
- Defer object destruction until a safe script-processing boundary.
- Keep native GTA script lifecycle on the game's existing path.

The `CRunningScript` base state and legacy opcode execution semantics remain intact.

## Current targeted optimizations

- Hash-based registries for CLEO script pointer checks.
- Indexed lookup for custom script names, with a compatibility fallback.
- A drawable-script list containing only scripts with persistent draw/text resources.
- A precomputed opcode dispatch table for the fast execution path.
- Per-script memory accounting, ownership-based cleanup, and leak reporting.
- Crash context capture and diagnostic callbacks through DebugUtils.

These are code-level optimizations. Their effect on frame time must be measured in a real GTA SA 1.0 US build; this document does not claim a measured FPS gain.

## Compatibility and validation

The control baseline is the existing working collection of 65 CLEO scripts.

Any engine change should preserve:

1. Existing opcode behavior and legacy script compatibility.
2. Native GTA execution order and script lifecycle.
3. Child-script and save/load behavior.
4. Drawing/text resource state.
5. Script and memory cleanup.
6. DebugUtils callbacks and crash diagnostics.

Build and in-game validation are required before claiming a change is safe. Keep experimental changes isolated from the established working branch.
