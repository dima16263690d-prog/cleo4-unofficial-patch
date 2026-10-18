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
(plus a "script memory leak report" block when [DebugUtils.Logs] LeakTracker=1)

cleo_frametime.log:
//////////////////////// frame time ////////////////////////
================= errors (interval) =================

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
LeakTracker=0
LeakInterval=30
LeakMinGrowthKB=64
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
MinFps=0
LowFpsWarn=0
LowFpsWarn01=0
SustainedLowIntervals=0
FrameBudgetMs=0

[DebugUtils.Errors]
; OFF by default (opt-in): the RenderWare poll runs every frame.
Enabled=0
RenderWare=0
ModuleScan=0
; Name the exact file behind a failed open (data-only patch, no code detour).
FileWatch=0
; Print the "failed file opens" block (1 = on). An empty FileWatchNames does not
; silence it; set FileWatchFailures=0 to stop the block while keeping the hook.
FileWatchFailures=1
; Name the mod behind E_RW_READTEXMASK (data-only patch, no code detour). The
; error is caught inside RwTextureRead, so the mod's stack is still present.
TextureWatch=0

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
- frame-quality statistics from a bounded ring of every period: p50/p95/p99/p99.9
  percentiles, jitter_ms (standard deviation of the period), fps_1%low and
  fps_0.1%low (the average FPS of the slowest 1% / 0.1% of frames), and spike
  bands (how many frames fell below 30/20/10/4 FPS, with their share);
- an interval-over-interval trend line (fps_prev vs fps_now and the delta), so a
  gradual slowdown is visible before it shows inside one interval;
- MinFps / LowFpsWarn / LowFpsWarn01 thresholds: when the interval's average FPS,
  its 1%-low FPS, or its 0.1%-low FPS drops below the configured value, a
  greppable `[fps] WARNING` line is written (all default to 0 = off). LowFpsWarn01
  targets long hitches, which the 1%-low figure can hide;
- SustainedLowIntervals: when MinFps is set and the average FPS stays below it for
  this many consecutive intervals, a single `[fps] SUSTAINED LOW` line is written
  (and a `[fps] recovered` line when it comes back). One slow interval is noise
  (a load hitch, an alt-tab); a run is a real regression. 0 = off;
- FrameBudgetMs: a frame budget in ms (0 = off). The report then states how many
  frames met it and the share, the single number that answers "does this build
  hold the target FPS?";
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

### Error diagnostics (RenderWare / CLEO)

[DebugUtils.Errors] reports CLEO and RenderWare errors inside cleo_frametime.log,
so an error can be tied to the frame it happened in. It is **off by default**
(opt-in): the RenderWare poll runs every frame and the module scan walks the
stack, so it costs a little even when nothing is wrong — set `Enabled=1` when
hunting errors. No game code is patched: the monitor only observes (and, for
RenderWare, restores the error state it read).

The report's `first` / `last` columns are the uptime (seconds) when each error
was first and most recently seen, so it is clear when an error started and
whether it is still recurring (the old single `uptime` column showed only the
last occurrence, which made a repeating error look like it moved). File/resource
errors (E_RW_BADOPEN, E_RW_READTEXMASK, E_RW_READ, E_RW_STREAMOPEN,
E_RW_BADFORMAT) also get a `hint:` line naming them as a missing or corrupt
texture/model/.txd — the "which mod causes it" pointer.

The report is deduplicated: an interval that produced no new error prints
nothing, and a recurring error is shown in full (row + meaning + hint) only when
it first appears or its count grows. Rows that did not change are collapsed into
one `[errors] unchanged since last report: ...` line, so a stable error does not
re-print its explanation every interval. The `meaning:`/`hint:` details are shown
once per record (and again in the final `game_end` dump).

When the script active at the error is `plg-sdk`, the report adds a note that
this is the plugin-sdk script-command bridge (`CallCommandById` builds a
throwaway `CRunningScript` named `"plg-sdk"`), so the error came from a plugin
calling a game command rather than from a user CLEO script.

### Naming the exact file (FileWatch)

`E_RW_BADOPEN` ("Error opening the file %s") does not record the filename in the
error itself, so the log alone cannot say *which* texture or `.txd` is missing.
Set `[DebugUtils.Errors] FileWatch=1` and DebugUtils captures every RenderWare
file open and reports the exact paths that failed:

```
================= failed file opens =================
#   path                                                   count first  last script
1   ...\models\cutscene\something.txd                         12    45    186 plg-sdk
[filewatch] 1 distinct path(s) failed to open; these are the files behind E_RW_BADOPEN / E_RW_READTEXMASK
```

The path is taken from the argument RenderWare passes to its own file-open
function, so it is exactly what the game tried to load. When a plugin triggered
the load through plugin-sdk's script bridge, the script column shows `plg-sdk`
(the bridge), which is the same attribution the error report uses. With
`ModuleScan=1` each failed path also gets a `requested by:` line naming the
module(s) on the stack at the failed open — the concrete `.asi`/plugin that
asked for the file — and a bare filename (no directory) is flagged with a hint
that it is looked up in the game folder.

This is a *data-only* patch: RenderWare reaches the OS through a file-function
table (`RwFileFunctions`, from `RwOsGetFileInterface`), and DebugUtils swaps that
table's `rwfopen` pointer for its own wrapper, which calls the original and
records the name on a null result. No instruction is overwritten and no
trampoline is used, so it carries none of the risk of a code detour; the original
pointer is restored on game end and on unload. The table address is GTA SA 1.0 US
only, and the wrapper refuses to install if the pointer it would replace is not
executable code (a wrong-layout guard). Off by default.

The `failed file opens` block is gated by `FileWatch` and `FileWatchFailures`,
not by `FileWatchNames`: emptying the watchlist silences the *watched* list but
not the failed-open report. Set `FileWatchFailures=0` to keep the hook (and the
watchlist) but stop the failed-open block, e.g. once you have named the culprit.

Frames in a `requested by:` chain that come from the Windows directory
(`user32.dll`, `kernel32.dll`, a system-supplied `d3d9.dll`, ...) are dropped as
OS noise. A mod that ships its own `d3d9.dll` in the game folder is kept, since
its path is not under `Windows`. So a chain like
`d3d9.dll+0x5FE17 <- SilentPatchSA.asi+0xE104 <- modloader.asi+0x1F1F0`
means a D3D9 proxy (`d3d9.dll`) asked RenderWare for the file, called from a mod
loader — the `d3d9.dll` frame is the immediate requester, not a system DLL.

Each chain prints `chain via:` naming the mechanism that produced it, so the
reader knows how much to trust it:

- `unwind` — `RtlCaptureStackBackTrace`, a real unwind. Most trustworthy.
- `scan-e8` — a stack scan where a candidate word is kept only if the five bytes
  before it decode to a near `CALL` (the shape of a real return address). This is
  the workhorse on 32-bit, where the frame-pointer unwind above often stops inside
  the game EXE.
- `scan` — the permissive raw stack scan, used only when the two above found
  nothing. Treat it as a hint, not proof: it can pick up stale stack values.

32-bit Windows has no PE unwind tables (they are an x64/ARM64 feature) and the
game/mods ship no PDBs, so DbgHelp's `StackWalk64` cannot do better than the
frame-pointer walk here — it is deliberately not used.

Frames whose address is outside its module image, or whose preceding bytes do not
decode to a call, are dropped; when those bytes cannot be read the frame is kept
rather than guessed away.

### Finding who loads a specific file (FileWatchNames)

`E_RW_BADOPEN` only fires when a file is *missing*. To answer "which script or
`.asi` loads `particle.txd`" — or to pin down a missing `lunar.png` — list the
name(s) in `[DebugUtils.Errors] FileWatchNames` (comma-separated). Each entry is
a plain substring, a glob mask (`*`, `?`), or a bare name, so you can watch one
file, a whole kind, or several at once:

```
[DebugUtils.Errors] FileWatch=1
[DebugUtils.Errors] FileWatchNames=particle.txd,*.png
```

Every open of a matching file is recorded whether it succeeded or failed:

```
================= watched file opens =================
#   path                           count first  last result script
1   models\particle.txd                1    12    12 ok     plg-sdk
      requested by: mymod.asi+0x1a2b
      looked in: C:\GTA San Andreas\models\particle.txd
2   lunar.png                          1    40    40 FAILED plg-sdk
      requested by: mymod.asi+0x2f10
      looked in: C:\GTA San Andreas\lunar.png
[filewatch] watchlist: 2 path(s) matched (FileWatchNames)
```

- `result` is `ok` (opened) or `FAILED` (missing — this is what raises
  `E_RW_BADOPEN` / `E_RW_READTEXMASK`).
- `requested by:` (needs `ModuleScan=1`) names the module on the stack at the
  open, with the offset inside it (`mymod.asi+0x1a2b`) — the concrete `.asi`
  plugin that asked for the file. The chain is captured with a real stack unwind
  (`RtlCaptureStackBackTrace`) at the moment of the open, so the nearest frame is
  the actual caller; a bounded stack scan is used only as a fallback.
- `looked in:` is the absolute path the game tried — the exact folder to place a
  missing file into, or the path a mod got wrong.
- `script` is the CLEO script active then (`plg-sdk` means a plugin called a game
  command through the plugin-sdk bridge).

The same `requested by:` / `looked in:` lines are added to the *failed file
opens* block, so a missing file (as with `lunar.png`) comes with both its exact
path and the mod that requested it.

Masks are matched against the file name and the full path: `*.txd` watches every
texture dictionary, `*.png` every PNG, `lunar.*` a specific basename.

Note: files read from inside IMG archives are not opened through RenderWare's
file functions (the IMG is opened once and seeked), so this watch sees loose
files only.

### Naming the mod behind E_RW_READTEXMASK (TextureWatch)

`E_RW_READTEXMASK` ("Unable to read Texture/Mask") is raised by RenderWare's
*name-based* reader `RwTextureRead` when its read callback returns NULL. The error
poll runs one frame later, when the raising stack is already gone, so it can only
report the error, never the mod that caused it.

Set `[DebugUtils.Errors] TextureWatch=1` and DebugUtils wraps that read callback
(`RwTextureGetReadCallBack`/`RwTextureSetReadCallBack`, SA 1.0 US `0x7F3520`/
`0x7F3500`) and, as a secondary, the stream reader `RwGlobals.stdFunc[26]`
(SA `0x4CD820`, the TXD-internal path). When a read fails, the requester chain is
captured *at that moment* — while the mod's frame is still on the stack:

```
================= failed texture reads =================
texture                            count first  last script
lunar.png                              3     5    53 ~plg-sdk
      requested by: mymod.asi+0x1a2b <- gta_sa.exe+0x730ed0
      chain via: unwind
[texturewatch] 1 failed texture read(s); these are the textures behind E_RW_READTEXMASK
```

- `texture` is the exact name that failed to read, so a missing/corrupt texture
  is named directly.
- `requested by:` is the concrete module that asked RenderWare to read the
  texture (needs `ModuleScan=1`), with the offset inside it — the "which mod"
  answer the error poll cannot give.
- `chain via:` names the mechanism that produced the chain — `unwind`
  (`RtlCaptureStackBackTrace`), `scan-e8` (a stack scan validated against the
  preceding `CALL` opcode, the workhorse on 32-bit), or `scan` (permissive
  fallback).

Like `FileWatch`, this is a **data-only patch**: the read callback pointer is
swapped through RenderWare's own public API and the `stdFunc[26]` entry is swapped
in place; no code is overwritten, and both are restored on unload. It is
GTA SA 1.0 US only and refuses to install on any other version.

- RenderWare: RenderWare keeps the last error as an `RwError {pluginID,
  errorCode}` pair in a global; the plugin-sdk resolves `RwErrorGet` at a fixed
  address (GTA SA 1.0 US). The monitor polls it once per frame and reports any
  *change* of the pair — the only way to see a RenderWare error that did not
  (yet) crash the process. `RwErrorGet` copies *and clears* the error, so the
  monitor calls `RwErrorSet` right after to put the pair back: it is transparent
  and the game's own error handling still sees the error. The report names common
  errors (E_RW_NOMEM, E_RW_NULLP, ...) and the RenderWare core plugin's errors
  (e.g. `E_RW_READTEXMASK` — "Unable to read Texture"), and prints each error's
  meaning on a `meaning:` line; errors from other plugins are shown as
  `RW_PLUGIN_ERR` with the raw pluginID + code. Because the error is noticed on
  the next frame, the script column is prefixed with `~` to mark it approximate,
  and no "last opcode" line is printed for RenderWare rows. Set RenderWare=0 to
  disable the poll; on non-1.0-US builds it disables itself and says so.
- CLEO: `CLEO_DEBUG_ERROR` / `CLEO_DEBUG_WARNING` messages are counted, and the
  stack at that moment is scanned (ModuleScan=1) for the first return address in
  a module that is neither the game, the core, nor DebugUtils. That module is the
  mod/plugin/loader that raised the error. The report lists the error with its
  count, the script and opcode active, and the suspect module (plus the call
  chain when more than one module is seen).

Attribution is deliberately conservative and labelled as such: a module seen on
the error stack (or the script active then) is a strong suspect, not proof. The
point is to point a human at the right DLL or .cs file, not to claim a root
cause.

### Per-script memory-leak tracker

[DebugUtils.Logs] LeakTracker=1 samples CLEO's script-memory arena (the
allocate_memory 0AC8 / free_allocated_memory 0AC9 blocks) per script on an
interval and keeps a per-script history, so a script whose live allocation only
ever grows can be named. It is off by default because it allocates and samples.

For each script the report gives first/last live bytes, the growth, that growth
as a percentage of the arena and of the process's private bytes, the growth rate
(bytes/second, the equivalent percent-per-minute of the script's own first
sample, and the percent-per-minute of the whole arena), the block count, the
peak, and first/last-seen uptime. The verdict is one of:

- LEAK — grew on every sample and was never released (the strong signature);
- SUSPECTED — net growth but not monotonic, or too few samples yet;
- STABLE — normal churn (grew and shrank) or below LeakMinGrowthKB;
- RELEASED — it held memory earlier and has since freed it.

The report opens the per-script table with a single `[leak] TOP SUSPECT:` line
naming the one script to look at first (the largest LEAK/SUSPECTED growth, or
"none"), so the answer is one line rather than a table to scan. When no script
is leaking but the process is, the line names the process growth instead
(`TOP SUSPECT: process private (no script is leaking) - grew X (+P% of private)
at R MB/min`) and follows it with an estimate of how long until the 2 GB
address-space limit is reached at that rate.

A closing `[leak]` line gives the exact total growth and its percentage of the
arena and of the process, which is the single "how much, in percent" answer.

The report also states how much the *whole process* grew since the first sample
(`[leak] process private X -> Y = +Z in Ns (+P%, +P%/min, ...)`). This matters
because the script arena can be perfectly flat while the process still grows — a
streaming-pool, texture, or GPU/driver leak. In that case a `[leak] NOTE:` line
says the growth is NOT a script allocate_memory leak and points at the memory
summary and module tables, so a flat arena is never misread as "no leak". When
the streaming pool grew over the same window, a `[leak] of that, X is the
streaming pool filling (world load, not a leak); Y is unattributed` line
separates the world-loading fill from the part actually worth chasing — otherwise
the first minutes of any level load read as a leak. The process baseline is taken
at the first interval sample after the level has loaded, not at uptime 0, so
load-time allocation is not counted as growth.

A trend line separates a real leak from a pool/streaming fill that is simply
filling up: `[leak] this interval +X MB (N B/s); average since first M B/s ->
verdict`, where the verdict is `steady (leak-like)`, `decelerating (likely a
pool/streaming fill, not a leak)`, `accelerating (worsening)`, or `shrinking`.
A leak keeps up with (or exceeds) the average rate; a pool fill slows down as it
saturates. To name the exact DLL behind process growth that is not in the CLEO
arena, set `[DebugUtils.Logs] ModuleMemoryMap=1` — its per-module resident
deltas point at the owner.

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

The RenderWare error watch is a single `RwErrorGet` + `RwErrorSet` pair per frame while the frame-time monitor is on; it patches no code and leaves the game's error state exactly as it found it. The stack scan that names the suspect module for a CLEO error runs only when an error/warning is actually logged, not per frame. The per-script leak tracker is off by default and, when on, only walks the script-memory owner map on its own interval.


### Language and CrashInfo localization

Configure the crash window language in `DebugUtils.ini`:

```ini
[DebugUtils.General]
Language=en
```

Use `Language=ru` for the Russian interface. The full verified database remains in `CrashInfo/CLEO-CrashList.txt` as the English baseline, while `CrashInfo/CLEO-CrashList-RU.txt` contains Russian text overrides. Untranslated entries automatically fall back to English, so switching to Russian never removes a known crash entry.
