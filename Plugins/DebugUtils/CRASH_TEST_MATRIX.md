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
