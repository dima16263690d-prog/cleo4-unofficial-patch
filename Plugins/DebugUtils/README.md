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
- cleo\debug\cleo_frametime.log — время кадра и фризы (свой файл, по умолчанию выключен)
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

cleo_frametime.log:
//////////////////////// frame time ////////////////////////

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

[DebugUtils.FrameTime]
Enabled=0
Interval=10
WarnMs=50
TopStutter=15
Histogram=1
MaxLogMB=16

[DebugUtils.General]
LegacyDebugOpcodes=0

The script log can be enabled with Enabled=1. The hang guard limits can be disabled by setting the corresponding value to 0.

### Frame-time monitor

[DebugUtils.FrameTime] measures frame timing to locate stutter. It uses
QueryPerformanceCounter around the per-frame game-logic tick (the same call CLEO
already wraps) and writes to its own cleo_frametime.log, so the other logs stay
unchanged. It is off by default; while disabled no clock is read.

Each report shows, for the interval:

- period_ms min/avg/max and fps_avg — the whole frame, regardless of cause;
- logic_ms avg/max — just the game-logic tick. A frame with a high period but a
  low logic time points at streaming/driver/render, not at scripts; a high logic
  time points at the scripts (or CLEO opcodes);
- stutter count/share/lost_ms — frames at or above WarnMs, and the time lost;
- a period histogram with fixed edges (16.6/20/25/33.3/50/100/250 ms);
- the worst frames of the interval, with their logic_ms, active scripts and
  uptime, so a hitch can be tied to a moment and a script count;
- streaming budget/usage (GTA SA 1.0 only, Streaming=1): the streaming budget,
  average and peak bytes held, peak as a share of the budget, plus pending model
  requests (ms_numModelsRequested: avg/max, and the share of frames with a
  request in flight), and stream_MB/req columns in the worst-frames table. This
  answers whether stutters line up with streaming pressure (used near the budget,
  or requests in flight) or not (used flat and req=0 = the cause is
  render/driver/vsync, not streaming).

Streaming budget (GTA SA 1.0 only, StreamLimitMB): this ini key owns the
`CStreaming::ms_memoryAvailable` setting and forwards it to the core through
`CLEO_SetStreamingMemoryLimit()`. 0 (default) leaves the game/mod value
untouched; -1 sets auto (1024 MB with Large Address Aware, else 512 MB); >0 sets
that many MB. The value is only ever raised, never lowered. Because it is read
from this file, an A/B test needs no rebuild: run with 0, then with -1.

Usage: set Enabled=1, play until it stutters, then read cleo_frametime.log. The
period-vs-logic split is the whole point — it says whether to optimise scripts or
streaming. MaxLogMB caps the file (default 16 MB) so it cannot grow forever.

The core log starts with a system header (GPU description, vendor/device id,
driver string and version, desktop resolution/refresh, OS build, CPU count, RAM)
queried from D3D9, so a stutter report says which card/driver it came from
without a follow-up question.

Localized ini: the comments in DebugUtils.ini are written in the language of
`[DebugUtils.General] Language` (en or ru). Set Language and restart the game;
the file is rewritten with the descriptions in that language, and every value you
set is preserved. The previous file is kept once as DebugUtils.ini.bak. A config
from an older build (missing keys) is also rewritten so new options are
documented.

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

The verified database is bundled inside DebugUtils.cleo and is loaded only when a real unhandled crash is analysed. There is no runtime network download.

The matching engine supports:

- exact fault addresses and module/RVA context;
- wildcard signatures as a final fallback;
- Backtrace address/module rules;
- last-command and script-context rules.

The parser understands the numbered CrashInfo schema: `Problem N:` / `Solution N:`
pairs, `Issue N:` / `About N:`, `Type:`, and the scenario-qualified
`During gameplay:` / `When trying to start the game:` lines. Plain-text entries
under `Other problems:` (a bare `Error:` description with no address) keep their
wording as the issue text instead of being discarded.

### Updating the database

Refresh the bundled list from the upstream JuniorDjjr/CrashInfo project:

```bash
python tools/Update-CrashInfo.py            # GTA SA 1.0 US English baseline
python tools/Update-CrashInfo.py --dry-run  # report only
python tools/Update-CrashInfo.py --source EN-CrashList.txt   # offline refresh
python tools/Update-CrashInfo.py --with-satellites           # + other languages
```

The script downloads `Lists/GTA-SA-10US/EN-CrashList.txt`, regenerates the
DebugUtils header (version marker, source URL, update date) and preserves the
local-notes footer verbatim. `--with-satellites` additionally mirrors the other
languages/versions under `CrashInfo/upstream/` for localization or diffing.

When a crash has no specific verified match, DebugUtils automatically records an
UNVERIFIED candidate in the separate local auto database. AUTO candidates are
never loaded as verified matches, so one observed crash cannot promote itself.
Each auto entry now tracks a hit count and first/last-seen timestamps, so a
recurring fingerprint is counted rather than silently ignored.


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

Opcode numbers are resolved to their canonical SCRLog/Sanny Builder name for the
crash report, the crash dialog and the live opcode trace. The name table
(`DebugUtils.OpcodeNames.h`) is generated from scrlog `keywords.txt` (LINK/2012,
MIT) plus the CLEO+ `opcodes.txt` names for the 0Dxx/0Exx opcodes the vanilla list
does not cover. Regenerate it with `python tools/Generate-OpcodeNames.py`; it is a
sorted static array resolved by binary search, so it adds no startup cost and no
runtime file I/O.

The core log rotates at 1 MiB instead of using the old 8 KiB hard stop. Repeated consecutive identical messages are collapsed into a single repeat record. Crash reports are written as one structured report with explicit sections: exception, fault, CPU, instruction, memory, process memory, stack walk, CLEO context, matcher, and diagnosis.

The verified database is CLEO-CrashList.txt. Automatically discovered observations are stored in CLEO-CrashAuto.txt and remain UNVERIFIED until reproduced.


### Language and CrashInfo localization

Configure the crash window language in `DebugUtils.ini`:

```ini
[DebugUtils.General]
Language=en
```

Use `Language=ru` for the Russian interface. The full verified database remains in `CrashInfo/CLEO-CrashList.txt` as the English baseline, while `CrashInfo/CLEO-CrashList-RU.txt` contains Russian text overrides. Untranslated entries automatically fall back to English, so switching to Russian never removes a known crash entry.
