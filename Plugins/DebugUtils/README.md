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

//////////////////////// MODULE ////////////////////////
//////////////////////// GAME / API ////////////////////////
//////////////////////// SCRIPT THREAD ////////////////////////

cleo_script.log:
//////////////////////// SCRIPTS ////////////////////////
//////////////////////// SCRIPT EXECUTION ////////////////////////
//////////////////////// OPCODE CHECK ////////////////////////
//////////////////////// FUNCTION CALL CHECK ////////////////////////

cleo_memory.log:
//////////////////////// MEMORY ////////////////////////

cleo_diagnostic.log:
//////////////////////// DIAGNOSTIC ////////////////////////

Repeated identical lines are collapsed into a single [REPEAT] count entry. High-frequency 0AB1/0AB2 function traces are disabled unless FunctionTrace=1.

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
Memory=1
Diagnostic=0

[DebugUtils.Limits]
Command=2000000
Time=5

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

## CrashInfo

Reference database:

https://github.com/JuniorDjjr/CrashInfo/blob/main/Lists/GTA-SA-10US/EN-CrashList.txt

Development download helper:

powershell -ExecutionPolicy Bypass -File tools\Get-CrashInfo.ps1 -GtaPath "C:\Games\GTA San Andreas"

Expected local database:

cleo\debug\CrashInfo\EN-CrashList.txt
