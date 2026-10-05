# DebugUtils crash/diagnostic test matrix

Target branch: test-xx02
Target: GTA San Andreas 1.0 US

## Normal / lightweight mode

Expected:
- CrashInfo database is not parsed during startup.
- DbgHelp.dll is not loaded during normal gameplay.
- Opcode trace callbacks are not registered when tracing/history/guards are disabled.
- Command and time hang guards are disabled by default.
- Periodic memory scanning is disabled by default.
- Core log uses 1 MiB rotation, not the old 8 KiB hard stop.
- Consecutive identical core/script messages are collapsed.

## Handled first-chance exception

Expected:
- VEH records only a fixed exception snapshot.
- No crash report is written.
- No CrashInfo lookup occurs.
- No StackWalk64 call occurs.
- Game continues if the original exception handler handles the exception.

## Real unhandled crash

Expected:
1. Unhandled filter captures the authoritative exception context.
2. CrashInfo verified database is initialized lazily.
3. StackWalk64 runs only in the crash path.
4. One structured gta_crashinfo.log is written.
5. Report sections are ordered:
   EXCEPTION -> FAULT -> CPU -> INSTRUCTION -> MEMORY -> PROCESS MEMORY -> STACK WALK -> CLEO CONTEXT -> CRASH MATCHER -> DIAGNOSIS.
6. Last CLEO opcode/script are reported as context, not automatically blamed.
7. CLEO involvement stays NOT_PROVEN unless evidence supports a stronger statement.

## Unknown crash

Expected:
- A new fingerprint is appended to CrashInfo/CLEO-CrashAuto.txt.
- Status remains UNVERIFIED.
- The record is never loaded as a verified CrashInfo match.
- Repeating the same fingerprint does not create another AUTO entry.

## Known crash

Expected:
- Verified CLEO-CrashList.txt entry wins over wildcard fallback.
- Exact address has higher priority than module-only/context matches.
- Different diagnoses for the same address remain separate.
- Exact duplicate records are removed when the database is loaded.

## Access violation

Expected:
- Exception code and access type are recorded.
- Access target address is recorded.
- VirtualQuery information for the fault address and access target is recorded when available.

## Regression checks

Required after build:
- GTA starts and reaches gameplay.
- Existing CLEO scripts still execute with unchanged opcode semantics.
- No zlib1.dll is added or modified by DebugUtils.
- DebugUtils has no WinHTTP dependency.
- The existing crash window still works.
- The crash log contains no duplicate raw-hook report.


## Null-EIP regression test

Expected:
- A crash with `EIP=0x00000000` is classified as `NULL-EIP`, not `EXACT-ADDRESS`.
- A verified database entry keyed only by `0x00000000` is not treated as an ordinary exact-address match.
- The crash name is `Null Instruction Execution`.
- Diagnosis reports `execution_control=INVALID` and `probable_fault=NULL-EIP instruction execution`.
- Last script/opcode remain context only and are not automatically blamed.

## Valid-EIP StackWalk test

Use the supplied Sanny Builder test source `Tests/DebugUtilsStackWalkTest.txt`.
Expected:
- The crash address is non-zero.
- `[STACK WALK]` contains more than frame `#00` when DbgHelp can unwind the current call chain.
- Frame modules are resolved where possible instead of all frames being `<unknown>`.
- The report still keeps CLEO script/opcode information separate from crash causality.


## Language / localization test

INI:
- `[DebugUtils.General]`
- `Language=en` for the English crash window.
- `Language=ru` for the Russian crash window.

CrashInfo:
- `CLEO-CrashList.txt` is the complete English verified baseline.
- `CLEO-CrashList-RU.txt` contains Russian text overrides.
- With `Language=ru`, DebugUtils loads the English baseline and applies the Russian overrides.
- Entries without a Russian override remain available and use the English text.

Expected:
- Startup core log contains `[debugutils] language=en` or `language=ru`.
- Crash window buttons and labels follow the selected language.
- A localized CrashInfo entry shows Russian Issue/About/Solution text.
