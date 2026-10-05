# Debug Diagnostics Architecture

Debugging is a separate optional CLEO plugin. The legacy CDebug/CDiagnosticLog file writers have been removed from the CLEO core.

## Runtime boundary

CLEO core exposes only lightweight Debug API calls and lifecycle callbacks. The core does not open debug files, format script log lines for disk, or maintain a debug logger object.

The only permanent core-side work is:

- a fast callback-pointer check for core diagnostic messages;
- lifecycle callback dispatch;
- an allocation-free opcode observation dispatch;
- low-opcode callback routing for DebugUtils functions.

All file I/O, formatting, CrashInfo lookup, breakpoint bookkeeping and script-log writing belong to DebugUtils.

## Three logs

### cleo/debug/cleo_core.log

CLEO runtime diagnostics:

- CLEO module information
- exported CLEO API addresses
- GTA SA 1.0 US pActiveScripts address
- CRunningScript layout
- active queue snapshots
- process memory counters
- runtime lifecycle
- thread deletion information
- core warnings/errors/diagnostic messages forwarded through the Debug API

### cleo/debug/cleo_script.log

Detailed script/opcode trace.

The file exists even when tracing is disabled, but script tracing is disabled by default to minimize game-thread overhead. Enable:

[DebugUtils.ScriptLog]
Enabled=1

When enabled, script events are placed into a bounded single-producer/background-writer queue. File I/O is performed by the DebugUtils writer thread, not by the CLEO script-processing thread.

The queue drops events if the writer falls behind; the drop count is reported to cleo_core.log.

### cleo/debug/gta_crashinfo.log

Crash diagnostics:

- exception code/address
- module
- registers
- exception parameters
- memory region
- process memory
- EBP backtrace
- active script queue
- last script/opcode/offset/result
- optional exact-address match in CrashInfo

## CLEO Crash Database

DebugUtils owns a project-local crash database.

Path:

cleo\\cleo_plugins\\CrashInfo\\CLEO-CrashList.txt

The database is bundled into DebugUtils.cleo and is created locally at startup when missing. It is not downloaded or synchronized automatically.

The matching layer is our own implementation and supports exact addresses, faulting modules, wildcard signatures, and Backtrace address/module conditions.

Only signatures that we reproduce or verify in our own GTA SA 1.0 US/CLEO testing are added to the database.

## CLEO 5-style DebugUtils opcodes

The standalone plugin provides:

- 00C3 debug_on
- 00C4 debug_off
- 2100 breakpoint
- 2101 trace
- 2102 log_to_file
- optional 0662 printstring
- optional 0663 printint
- optional 0664 printfloat

The current opcode behavior follows the published CLEO 5 DebugUtils design. In particular, 2101 and 2102 support formatted varargs, while 2102 does not require debug mode.

## Plugin location

New optional plugins are loaded from:

cleo\cleo_plugins\

Legacy CLEO 4 plugin placement in:

cleo\

remains supported.

## Isolation rules

DebugUtils does not:

- become part of the Memory Engine
- enable worker threads in the runtime scheduler
- add the GTA-CLEO bridge
- replace the legacy .cs/.cs3/.cs4 execution engine

Diagnostics are now a prerequisite for further scheduler/runtime changes.
