# AI Runtime Architecture Notes — CLEO 4 Runtime Engine

## Current stage

Branch: `runtime-engine-cleo5-baseline`

Goal of this stage: prepare the CLEO 4 script engine for a future high-performance scheduler and real multithreading without changing the existing opcode/execution engine yet.

The target is stable operation with very large numbers of logical CLEO scripts (1000+ as a benchmark target), smooth frame behavior, correct start/stop order, and safe lifecycle management.

## Important scope decisions

- GTA↔CLEO bridge is **not part of this architecture**.
- Opcode `0E6F` is **not being redesigned here**. It is relevant only as an example that one CLEO file can produce multiple logical script threads.
- Memory management is a **separate future block**. It will later be able to manage per-script CLEO/GTA memory and Windows allocation requests, but it is deliberately not mixed into this engine patch.
- Multithreading is **not enabled in this stage**. All script execution remains on the GTA game thread.

## Reference: CLEO 5

The official CLEO 5 source was inspected at:

- `source/CScriptEngine.h`
- `source/CScriptEngine.cpp`
- `source/CCustomScript.h`
- `source/CCustomScript.cpp`

Useful CLEO 5 architecture:

```text
GTA pActiveScripts
  |
  +-- native scripts
  |
  +-- CLEO scripts
        |
        +-- CCustomScript : CRunningScript
```

CLEO 5 centralizes script lifecycle in `CScriptEngine`:

```text
GameBegin
  -> preserve native queue order
  -> temporarily build CLEO queue
  -> append CLEO scripts after native scripts

Load/ Create
  -> CCustomScript
  -> AddCustomScript
  -> register in engine
  -> add to GTA active list
  -> activate

Remove
  -> detach parent
  -> remove child subtree
  -> SetActive(false)
  -> remove from pActiveScripts
  -> remove from engine registry
  -> defer deletion
  -> delete after the runtime boundary
```

CLEO 5 also exposes explicit lookup/validation functions and separate unregister/reregister operations for save handling.

Important finding: CLEO 5 does **not** provide a worker-pool scheduler that makes CLEO scripts run in parallel. Its `HOOK_ProcessScript` wraps the original GTA `ProcessScript` with callbacks.

## What was changed in our CLEO 4 engine

### 1. Central lifecycle

Added:

- `CScriptEngine::GameBegin(bool bLoadMode)`
- `CScriptEngine::GameEnd()`

The existing `OnInitScm1/2/3` and new-game path now go through these lifecycle functions instead of directly manipulating custom scripts.

### 2. Native queue order

`GameBegin()` preserves the existing GTA native script order and builds the CLEO part separately before attaching it to the tail.

Target queue:

```text
pActiveScripts

[NATIVE 1]
[NATIVE 2]
[NATIVE 3]
[NATIVE 4]
[CLEO 1]
[CLEO 2]
[CLEO 3]
[CLEO 4]
...
```

This ordering is intentional. It gives us a stable single-threaded baseline and a future place to branch CLEO work into a scheduler.

### 3. Central script control

Added engine-level operations:

- `CreateCustomScript()`
- `RemoveScript()`
- `IsActiveScriptPtr()`
- `IsValidScriptPtr()`

The goal is for script lifecycle decisions to belong to `CScriptEngine`, not to scattered opcode/hook code.

### 4. Safe deferred deletion

The custom-script removal order is now explicitly:

```text
RemoveCustomScript()
        |
        v
remove from parent
        |
        v
remove children
        |
        v
SetActive(false)
        |
        v
remove from pActiveScripts
        |
        v
remove from registry
        |
        v
Pending Delete
        |
        v
DELETE
```

This prevents immediate destruction from being mixed with queue/registry mutation.

### 5. Native script behavior

Native scripts continue to use the GTA lifecycle:

```text
active queue
  -> inactive queue
  -> StopScript()
```

The patch does not replace the GTA native runtime.

## What is intentionally not changed yet

- `CCustomScript::Process()`
- opcode dispatch / `ScriptExecutionLoop()`
- legacy `.cs/.cs3/.cs4` execution semantics
- `ScmFunction` / `0AB1/0AB2`
- child-state save sidecars already present in this branch
- memory engine
- VM
- worker threads

## Next stages

### Stage 2 — CCustomScript

Refactor `CCustomScript` so the script object has a clearer execution context and lifecycle state, while retaining the GTA `CRunningScript` ABI.

### Stage 3 — execution context / VM preparation

Separate script state from engine lifecycle sufficiently that a future scheduler can execute a script context without rewriting the whole engine.

### Stage 4 — scheduler

Introduce a scheduler above the GTA active list. First mode remains sequential and must be behaviorally identical to the baseline.

### Stage 5 — worker pool

Only after the sequential scheduler is stable, test worker threads and determine which operations can safely execute away from the GTA game thread.

### Separate Memory Engine

Later, build an independent memory subsystem for per-script accounting, CLEO/GTA memory management, and Windows memory allocation requests.

## Testing target

Before enabling any multithreading:

1. Build Debug / Win32 for GTA SA 1.0.0.0 US.
2. Start a new game and load an existing save.
3. Verify normal `.cs`, `.cs3`, and `.cs4` scripts.
4. Verify native scripts stay at the front of `pActiveScripts`.
5. Verify `004E` stops custom and native scripts through the centralized lifecycle.
6. Verify parent/child scripts are removed without stale pointers.
7. Verify save/load still restores working scripts.
8. Stress progressively: 100 -> 500 -> 1000 logical scripts.
9. Record frame time and runtime logs before changing execution architecture.

## Current conclusion

The engine now has the correct architectural boundary for the next experiment:

```text
GTA pActiveScripts
        |
        v
   CScriptEngine
        |
        +-- lifecycle
        +-- registry
        +-- safe removal
        +-- validation
        |
        v
   CCustomScript
        |
        v
   existing execution engine

Future:
        CScriptEngine
             |
          Scheduler
             |
        Worker Pool
```

The purpose of this branch is to establish a stable, measurable, single-threaded baseline before touching `CCustomScript`, VM design, memory workers, or true multithreading.
