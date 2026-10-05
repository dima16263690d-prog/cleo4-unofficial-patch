# CLEO 4 DebugUtils

Standalone diagnostic plugin inspired by the CLEO 5 DebugUtils split. It fully replaces the legacy in-core CDebug and CDiagnosticLog writers.

## Installation

Place the plugin in:

cleo\cleo_plugins\DebugUtils.cleo

The CLEO 4 core still accepts legacy optional plugins from cleo\.

Place the configuration file in:

cleo\cleo_plugins\DebugUtils.ini

## Logs

DebugUtils owns all diagnostic files:

- cleo\debug\cleo_core.log — ядро, API, lifecycle, ошибки
- cleo\debug\cleo_script.log — скрипты, 0AB1/0AB2 и opcode checks
- cleo\debug\cleo_memory.log — память и VirtualProtect
- cleo\debug\cleo_diagnostic.log — низкоуровневая диагностика, по умолчанию выключена
- cleo\debug\gta_crashinfo.log — аварийные отчёты

The old cleo.log debug writer is removed from the core.

## Log architecture

Each log uses ordered visual sections instead of one mixed stream:

//////////////////////// module ////////////////////////
//////////////////////// game / api ////////////////////////
//////////////////////// script thread ////////////////////////

cleo_script.log:
//////////////////////// scripts ////////////////////////
//////////////////////// script execution ////////////////////////
//////////////////////// function call check (0AB1 / 0AB2) ////////////////////////
//////////////////////// opcode check ////////////////////////

cleo_memory.log:
//////////////////////// memory ////////////////////////

cleo_diagnostic.log:
//////////////////////// diagnostic ////////////////////////

Repeated identical lines are collapsed into a single [repeat] count entry. High-frequency 0AB1/0AB2 function traces are disabled unless FunctionTrace=1. Low-level memory protection tracing is disabled unless MemoryTrace=1.

The core log rotates at 1 MiB instead of using an 8 KiB hard stop. Repeated consecutive identical messages are collapsed into a single [repeat] record.

Memory diagnostics are opt-in. When Memory=0 there is no periodic memory scan. When enabled, samples stay compact and record only measured process/CLEO categories.

## Performance model

The CLEO core performs no debug file I/O.

Core diagnostic calls go through a small function-pointer bridge. When DebugUtils is not loaded, the call returns immediately. When DebugUtils is loaded, core messages are received by the plugin.

Script/opcode tracing is the expensive mode and is disabled by default. When enabled, trace lines are buffered in a bounded single-producer/background-writer queue, so disk I/O does not happen inside the script execution callback.

## Configuration

[DebugUtils.ScriptLog]
Enabled=1
OpcodeTrace=0
FunctionTrace=0
Deduplicate=1

[DebugUtils.Logs]
Memory=0
MemoryTrace=0
Diagnostic=0

[DebugUtils.Limits]
Command=0
Time=0

[DebugUtils.General]
LegacyDebugOpcodes=0

The script log can be enabled with Enabled=1. The hang guard limits can be disabled by setting the corresponding value to 0.

## CLEO 5-style functions

00C3 debug_on
00C4 debug_off
2100 breakpoint
2101 trace
2102 log_to_file

Optional Rockstar debug opcodes can be enabled with LegacyDebugOpcodes=1:

0662 printstring
0663 printint
0664 printfloat

The 2101/2102 string parameters use CLEO opcode formatting, including varargs.

## CLEO Crash Database

DebugUtils uses its own local crash database:

cleo\\cleo_plugins\\CrashInfo\\CLEO-CrashList.txt

The verified database is bundled inside DebugUtils.cleo and is loaded only when a real unhandled crash is analysed. There is no runtime network download or manual synchronization.

The matching engine supports:

- exact fault addresses and module/RVA context;
- wildcard signatures as a final fallback;
- Backtrace address/module rules;
- last-command and script-context rules.

When a crash has no specific verified match, DebugUtils automatically appends an
UNVERIFIED candidate to the separate local auto database. AUTO candidates are
never loaded as verified matches, so one observed crash cannot promote itself.


## Crash diagnostics bridge

The crash subsystem is optional and is controlled by `cleo/cleo_plugins/DebugUtils.ini`:

```ini
[DebugUtils.Crash]
Enabled=1
Window=1
Backtrace=1
OpcodeHistory=0
MaxFrames=32
```

When `Enabled=0`, DebugUtils does not install its VEH/unhandled-exception
crash hooks and CLEO does not maintain the crash snapshot.

When enabled, the CLEO core exposes a small bridge snapshot containing only
the last script pointer/name, opcode, script offset, result state and game tick.
DebugUtils reads this snapshot only when a crash occurs. It does not walk the
GTA active-script queue and does not write per-opcode crash data to disk.

Crash reports contain the fault address, GTA module/RVA, access target, registers,
instruction bytes, StackWalk64 backtrace and the last CLEO execution context. The interactive crash window can copy the report, open the log, or
terminate GTA.

The fault address comes directly from the Windows exception record. Additional
GTA hooks are not required to discover the faulting instruction; hooks should
only be added later for a specifically reproduced function-level investigation.


## Diagnostic performance contract

DebugUtils is an observer. In normal gameplay it does not parse the CrashInfo database, load DbgHelp, walk memory, write crash reports, or perform forensic stack analysis. The CLEO crash snapshot is thread-local. DebugUtils opcode callbacks used for tracing or guards are not registered unless those modes are explicitly enabled.

The core log rotates at 1 MiB instead of using the old 8 KiB hard stop. Repeated consecutive identical messages are collapsed into a single repeat record. Crash reports are written as one structured report with explicit sections: exception, fault, CPU, instruction, memory, process memory, stack walk, CLEO context, matcher, and diagnosis.

The verified database is CLEO-CrashList.txt. Automatically discovered observations are stored in CLEO-CrashAuto.txt and remain UNVERIFIED until reproduced.


### Language and CrashInfo localization

Configure the crash window language in `DebugUtils.ini`:

```ini
[DebugUtils.General]
Language=en
```

Use `Language=ru` for the Russian interface. The full verified database remains in `CrashInfo/CLEO-CrashList.txt` as the English baseline, while `CrashInfo/CLEO-CrashList-RU.txt` contains Russian text overrides. Untranslated entries automatically fall back to English, so switching to Russian never removes a known crash entry.
