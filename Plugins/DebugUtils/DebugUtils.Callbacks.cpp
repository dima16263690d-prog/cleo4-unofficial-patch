#include "DebugUtils.h"
#include "DebugUtils.Internal.h"
#include "resource.h"
#include <windows.h>
#include <psapi.h>
#include <shellapi.h>
#include <dbghelp.h>
#pragma comment(lib, "Shell32.lib")
#include <TlHelp32.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <fstream>
#include <sstream>
#include <regex>
#include <CTimer.h>

void DebugUtils::RegisterCallbacks()
{
    struct Registration
    {
        CLEO_CallbackId id;
        uintptr_t fn;
    } callbacks[] =
    {
        { CLEO_CB_GAME_BEGIN, reinterpret_cast<uintptr_t>(&DebugUtils::OnGameBegin) },
        { CLEO_CB_GAME_END, reinterpret_cast<uintptr_t>(&DebugUtils::OnGameEnd) },
        { CLEO_CB_GAME_PROCESS_BEFORE, reinterpret_cast<uintptr_t>(&DebugUtils::OnGameProcessBefore) },
        { CLEO_CB_GAME_PROCESS_AFTER, reinterpret_cast<uintptr_t>(&DebugUtils::OnGameProcessAfter) },
        { CLEO_CB_SCRIPT_PROCESS_BEFORE, reinterpret_cast<uintptr_t>(&DebugUtils::OnScriptProcessBefore) },
        { CLEO_CB_SCRIPT_DELETED, reinterpret_cast<uintptr_t>(&DebugUtils::OnScriptDeleted) }
    };

    for (const auto& callback : callbacks)
    {
        if (!CLEO_RegisterCallback(callback.id, callback.fn))
        {
            if (s_instance != nullptr)
                s_instance->WriteCore("[callback] register failed: %s", CallbackName(callback.id));
        }
        else
        {
            if (s_instance != nullptr)
                s_instance->WriteCore("[callback] registered: %s", CallbackName(callback.id));
        }
    }

    const bool needOpcodeCallbacks =
        s_instance != nullptr &&
        (s_instance->m_scriptOpcodeTrace ||
         s_instance->m_functionTrace ||
         s_instance->m_crashOpcodeHistory ||
         s_instance->m_commandLimit > 0 ||
         s_instance->m_timeLimitSeconds > 0);

    if (needOpcodeCallbacks)
    {
        const struct
        {
            CLEO_CallbackId id;
            uintptr_t fn;
        } opcodeCallbacks[] =
        {
            { CLEO_CB_SCRIPT_OPCODE_PROCESS_BEFORE, reinterpret_cast<uintptr_t>(&DebugUtils::OnScriptOpcodeBefore) },
            { CLEO_CB_SCRIPT_OPCODE_PROCESS_AFTER, reinterpret_cast<uintptr_t>(&DebugUtils::OnScriptOpcodeAfter) }
        };

        for (const auto& callback : opcodeCallbacks)
        {
            if (!CLEO_RegisterCallback(callback.id, callback.fn))
                s_instance->WriteCore("[callback] register failed: %s", CallbackName(callback.id));
            else
                s_instance->WriteCore("[callback] registered: %s", CallbackName(callback.id));
        }

        s_instance->WriteCore(
            "[callback] opcode observer=enabled trace=%d history=%d guard=%d",
            (s_instance->m_scriptOpcodeTrace || s_instance->m_functionTrace) ? 1 : 0,
            s_instance->m_crashOpcodeHistory ? 1 : 0,
            (s_instance->m_commandLimit > 0 || s_instance->m_timeLimitSeconds > 0) ? 1 : 0
        );
    }
    else if (s_instance != nullptr)
    {
        s_instance->WriteCore("[callback] opcode observer=crash-only (DebugUtils callback bypassed)");
    }
}

void DebugUtils::UnregisterCallbacks()
{
    struct Registration
    {
        CLEO_CallbackId id;
        uintptr_t fn;
    } callbacks[] =
    {
        { CLEO_CB_GAME_BEGIN, reinterpret_cast<uintptr_t>(&DebugUtils::OnGameBegin) },
        { CLEO_CB_GAME_END, reinterpret_cast<uintptr_t>(&DebugUtils::OnGameEnd) },
        { CLEO_CB_GAME_PROCESS_BEFORE, reinterpret_cast<uintptr_t>(&DebugUtils::OnGameProcessBefore) },
        { CLEO_CB_GAME_PROCESS_AFTER, reinterpret_cast<uintptr_t>(&DebugUtils::OnGameProcessAfter) },
        { CLEO_CB_SCRIPT_PROCESS_BEFORE, reinterpret_cast<uintptr_t>(&DebugUtils::OnScriptProcessBefore) },
        { CLEO_CB_SCRIPT_PROCESS_AFTER, reinterpret_cast<uintptr_t>(&DebugUtils::OnScriptProcessAfter) },
        { CLEO_CB_SCRIPT_OPCODE_PROCESS_BEFORE, reinterpret_cast<uintptr_t>(&DebugUtils::OnScriptOpcodeBefore) },
        { CLEO_CB_SCRIPT_OPCODE_PROCESS_AFTER, reinterpret_cast<uintptr_t>(&DebugUtils::OnScriptOpcodeAfter) },
        { CLEO_CB_SCRIPT_DELETED, reinterpret_cast<uintptr_t>(&DebugUtils::OnScriptDeleted) }
    };

    for (const auto& callback : callbacks)
        CLEO_UnregisterCallback(callback.id, callback.fn);
}

void __stdcall DebugUtils::OnGameBegin()
{
    if (!s_instance) return;
    s_instance->m_scriptCommands = 0;
    s_instance->m_currentScriptPtr = 0;
    s_instance->m_currentScriptCommands = 0;
    s_instance->m_debugScripts.clear();
    s_instance->m_breakpoints.clear();
    s_instance->m_keysReleased = true;
    s_instance->m_lastProfilerTick = GetTickCount();
    s_instance->m_lastMemoryLogTick = GetTickCount();
    s_instance->m_lastModuleMapTick = s_instance->m_lastMemoryLogTick;
    s_instance->m_memorySessionStartTick = s_instance->m_lastMemoryLogTick;
    s_instance->m_lastFrameTimeReportTick = s_instance->m_lastMemoryLogTick;
    // Drop the previous game's interval trend: otherwise the new game's first
    // frame-time report compares FPS against the old game's final interval and can
    // print a misleading slowdown.
    s_instance->m_frameTimeHaveLastInterval = false;
    s_instance->m_frameTimeLastIntervalAvgPeriodMs = 0.0;
    s_instance->m_frameTimeHaveLast = false;
    s_instance->m_frameTimeLastPeriodMs = 0.0;
    s_instance->m_frameTimeScriptsLast = 0;
    s_instance->ResetFrameTimeInterval();
    s_instance->m_lastPrivateUsage = 0;
    s_instance->m_lastMemoryQueueCount = 0;
    s_instance->m_lastMemoryCustomCount = 0;
    s_instance->m_memoryBaselineReady = false;
    s_instance->m_peakPrivateUsage = 0;
    s_instance->m_memoryWarnIssued = false;
    s_instance->m_memoryRotateActive = false;
    s_instance->m_memorySummaryHeaderWritten = false;
    // Reset the leak tracker's process baseline for the new session; the first
    // sample captures it. Also reset its timer: otherwise (defaulting to 0) the
    // first sample fires at uptime 0-1s, during level loading, where memory
    // swings wildly and the load-time allocation would be miscounted as a leak.
    s_instance->m_leakFirstPrivateBytes = 0;
    s_instance->m_leakFirstPrivateUptimeSec = 0;
    s_instance->m_leakLastPrivateBytes = 0;
    s_instance->m_leakLastUptimeSec = 0;
    s_instance->m_lastLeakTick = s_instance->m_lastMemoryLogTick;
    // Drop the previous session's per-script leak history: the script pointers are
    // recycled by a new game, and keeping them would attribute the old run's growth
    // to whatever script lands on the same pointer now.
    s_instance->m_leakScripts.clear();
    s_instance->m_leakRetired.clear();
    s_instance->m_leakSeenThisSample.clear();
    s_instance->m_leakHaveStreamBaseline = false;
    s_instance->m_leakFirstStreamUsedBytes = 0;
    // Error records are cumulative for the session; clear them and the one-shot
    // note flag so a new game starts from a clean error table.
    {
        std::lock_guard<std::mutex> lock(s_instance->m_errorMutex);
        s_instance->m_errorRecords.clear();
        s_instance->m_errorOrder.clear();
        s_instance->m_errorModulesSeen.clear();
    }
    s_instance->m_errorNotePrinted = false;
    s_instance->m_rwErrorPrimed = false;
    s_instance->m_rwErrorAddressCheckDone = false;
    s_instance->m_rwErrorAddressesValid = false;
    s_instance->m_rwErrorWatchDisabled = false;
    s_instance->m_cleoErrorCount = 0;
    s_instance->m_cleoWarningCount = 0;
    s_instance->m_lastReportedCleoErrors = 0;
    s_instance->m_lastReportedCleoWarnings = 0;
    {
        std::lock_guard<std::mutex> lock(s_instance->m_fileOpenMutex);
        s_instance->m_fileOpenFailures.clear();
        s_instance->m_fileOpenOrder.clear();
    }
    {
        std::lock_guard<std::mutex> lock(s_instance->m_fileWatchRecordMutex);
        s_instance->m_fileWatchRecords.clear();
        s_instance->m_fileWatchRecordOrder.clear();
    }
    {
        std::lock_guard<std::mutex> lock(s_instance->m_textureReadMutex);
        s_instance->m_textureReadFailures.clear();
        s_instance->m_textureReadOrder.clear();
    }
    s_instance->InstallFileOpenWatch();
    s_instance->InstallTextureReadWatch();
    s_instance->InstallStreamingModelWatch();
    s_instance->WriteCore("//////////////////////// game begin ////////////////////////");
    s_instance->m_seenScripts.clear();
    s_instance->m_lastScriptMessage.clear();
    s_instance->m_lastScriptRepeatCount = 0;
    s_instance->WriteScript("//////////////////////// script execution ////////////////////////");
    if (s_instance->m_functionTrace)
        s_instance->WriteScript("//////////////////////// function call check (0AB1 / 0AB2) ////////////////////////");
    if (s_instance->m_scriptOpcodeTrace)
        s_instance->WriteScript("//////////////////////// opcode check ////////////////////////");
    if (s_instance->m_memoryLogEnabled)
        s_instance->WriteCoreMemorySummary();
}

void __stdcall DebugUtils::OnGameEnd()
{
    if (!s_instance) return;
    s_instance->WriteCore("//////////////////////// game end ////////////////////////");
    if (s_instance->m_memoryLogEnabled)
        s_instance->WriteCoreMemorySummary();
    s_instance->WriteProfilerReport("game_end");
    s_instance->WriteFrameTimeReport("game_end");
    s_instance->WriteErrorReport("game_end");
    s_instance->WriteFileOpenReport(true);
    s_instance->WriteWatchedOpenReport(true);
    s_instance->WriteTextureReadReport(true);
    s_instance->RemoveFileOpenWatch();
    s_instance->RemoveTextureReadWatch();
    s_instance->RemoveStreamingModelWatch();
    // Do NOT sample here: CLEO dispatches GAME_END only after it has already run
    // RemoveAllCustomScripts() and Memory().OnGameEnd(), so the owner arena is
    // empty and a sample would retire every tracked script as RELEASED with 0
    // bytes - erasing the evidence. Report the last live interval sample instead.
    s_instance->WriteLeakReport("game_end");
    s_instance->FlushScriptRepeat();
    s_instance->WriteScript("[game_end] runtime stopped");
}

void __stdcall DebugUtils::OnGameProcessBefore()
{
    if (!s_instance)
        return;

    // Start the frame-time sample before any early return below, so the period
    // between consecutive frames is measured even when no breakpoint is set.
    s_instance->BeginFrameTimeFrame();

    if (s_instance->m_breakpoints.empty())
        return;

    bool anyKeyDown = false;
    for (int key = VK_F5; key <= VK_F12; ++key)
    {
        if (GetAsyncKeyState(key) & 0x8000)
        {
            anyKeyDown = true;
            break;
        }
    }

    if (anyKeyDown)
    {
        if (!s_instance->m_keysReleased)
            return;

        s_instance->m_keysReleased = false;

        const size_t count = std::min<size_t>(s_instance->m_breakpoints.size(), 8);
        for (size_t i = 0; i < count; ++i)
        {
            const int key = VK_F5 + static_cast<int>(i);
            if (!(GetAsyncKeyState(key) & 0x8000))
                continue;

            s_instance->WriteCore(
                "[breakpoint] continued script='%.8s' key=F%d",
                s_instance->m_breakpoints[i].name.c_str(),
                5 + static_cast<int>(i)
            );

            if (CTimer::m_CodePause)
                CTimer::m_CodePause = false;

            s_instance->m_breakpoints.erase(
                s_instance->m_breakpoints.begin() + static_cast<ptrdiff_t>(i)
            );
            break;
        }
    }
    else
    {
        s_instance->m_keysReleased = true;
    }
}

void __stdcall DebugUtils::OnGameProcessAfter()
{
    if (!s_instance) return;

    // Close the frame-time sample and flush a report when the interval elapses.
    s_instance->EndFrameTimeFrame();

    const DWORD now = GetTickCount();
    if (s_instance->m_memoryLogEnabled &&
        now - s_instance->m_lastMemoryLogTick >= 10000)
    {
        s_instance->m_lastMemoryLogTick = now;
        s_instance->WriteCoreMemorySummary();
    }

    if (s_instance->m_profilerEnabled &&
        now - s_instance->m_lastProfilerTick >=
            s_instance->m_profilerIntervalSeconds * 1000u)
    {
        s_instance->m_lastProfilerTick = now;
        s_instance->WriteProfilerReport("interval");
    }

    if (s_instance->m_memoryLogEnabled && s_instance->m_memoryModuleMap &&
        now - s_instance->m_lastModuleMapTick >=
            s_instance->m_memoryModuleMapIntervalSeconds * 1000u)
    {
        s_instance->m_lastModuleMapTick = now;
        s_instance->WriteModuleMemoryMap();
    }

    // Per-script leak tracker: sample the script-memory arena on its own cadence
    // and flush a report. Both calls are game-thread only, like this callback.
    if (s_instance->m_leakTrackerEnabled &&
        now - s_instance->m_lastLeakTick >= s_instance->m_leakIntervalSeconds * 1000u)
    {
        s_instance->m_lastLeakTick = now;
        s_instance->SampleScriptMemoryLeaks();
        s_instance->WriteLeakReport("interval");
    }

    // Data-only watches install once; a mod can displace them later. Re-check on a
    // slow cadence so a silently-disabled watch is reported instead of looking
    // like "nothing failed". Self-gated (returns unless a watch is installed and
    // 5s elapsed), so calling it every frame is cheap.
    s_instance->VerifyWatches();
}

BOOL __stdcall DebugUtils::OnScriptProcessBefore(CScriptThread* thread)
{
    if (!s_instance || !thread)
        return TRUE;

    const bool timingGuardEnabled =
        s_instance->m_commandLimit > 0 ||
        s_instance->m_timeLimitSeconds > 0;

    if (timingGuardEnabled)
    {
        s_instance->m_currentScriptPtr = reinterpret_cast<uintptr_t>(thread);
        s_instance->m_currentScriptStartTick = GetTickCount();
        s_instance->m_currentScriptCommands = 0;
    }

    const uintptr_t scriptPtr = reinterpret_cast<uintptr_t>(thread);

    // Record the first observation before breakpoint handling. A breakpoint
    // may return FALSE and skip the rest of the script-process callback chain,
    // but the script must still receive exactly one [script_begin] record.
    if (s_instance->m_seenScripts.insert(scriptPtr).second)
    {
        s_instance->WriteScript(
            "[script_begin] ptr=%p name='%.8s' ip=%p base=%p off=0x%zX active=%d external=%d mission=%d",
            thread,
            thread->threadName,
            thread->ip,
            thread->baseIp,
            ScriptOffset(thread),
            thread->isActive ? 1 : 0,
            thread->external ? 1 : 0,
            thread->missionFlag ? 1 : 0
        );
    }

    for (const auto& breakpoint : s_instance->m_breakpoints)
    {
        if (breakpoint.scriptPtr == scriptPtr)
            return FALSE;
    }

    return TRUE;
}

void __stdcall DebugUtils::OnScriptProcessAfter(CScriptThread* thread)
{
    if (!s_instance || !thread)
        return;

    s_instance->m_currentScriptPtr = 0;
}

int __stdcall DebugUtils::OnScriptOpcodeBefore(CScriptThread* thread, DWORD opcode)
{
    if (!s_instance || !thread)
        return 0;

    ++s_instance->m_scriptCommands;
    ++s_instance->m_currentScriptCommands;

    const DWORD normalized = opcode & 0x7FFF;

    if (s_instance->m_commandLimit > 0 && s_instance->m_currentScriptCommands > s_instance->m_commandLimit)
    {
        s_instance->WriteCore(
            "[hang_guard] script='%.8s' command_limit=%u",
            thread->threadName,
            static_cast<unsigned>(s_instance->m_commandLimit)
        );
        return CLEO_DEBUG_OPCODE_INTERRUPT;
    }

    if ((s_instance->m_currentScriptCommands % 1000u) == 0)
    {
        const DWORD elapsed = GetTickCount() - s_instance->m_currentScriptStartTick;
        if (s_instance->m_timeLimitSeconds > 0 && elapsed > s_instance->m_timeLimitSeconds * 1000u)
        {
            s_instance->WriteScript(
                "[hang_guard] script='%.8s' elapsed_ms=%u time_limit_seconds=%u",
                thread->threadName,
                static_cast<unsigned>(elapsed),
                static_cast<unsigned>(s_instance->m_timeLimitSeconds)
            );
            s_instance->m_currentScriptStartTick = GetTickCount();
            return CLEO_DEBUG_OPCODE_INTERRUPT;
        }
    }

    if (s_instance->m_scriptOpcodeTrace ||
        s_instance->m_functionTrace ||
        s_instance->m_crashOpcodeHistory)
    {
        s_instance->RecordOpcode(thread, opcode, 0xFFFFFFFF);
    }

    const bool notFlag = thread->notFlag != 0;

    if (s_instance->m_scriptOpcodeTrace ||
        s_instance->m_debugScripts.find(reinterpret_cast<uintptr_t>(thread)) != s_instance->m_debugScripts.end())
    {
        s_instance->WriteScript(
            "[opcode_before] script='%.8s' ptr=%p opcode=0x%04X name=%s group=%u not=%d ip=%p off=0x%zX",
            thread->threadName,
            thread,
            normalized,
            OpcodeName(normalized) ? OpcodeName(normalized) : "unknown",
            static_cast<unsigned>(normalized / 100),
            notFlag ? 1 : 0,
            thread->ip,
            ScriptOffset(thread) >= 2 ? ScriptOffset(thread) - 2 : 0
        );
    }

    return 0;
}

int __stdcall DebugUtils::OnScriptOpcodeAfter(CScriptThread* thread, DWORD opcode, int result)
{
    if (!s_instance || !thread)
        return 0;

    if (s_instance->m_scriptOpcodeTrace ||
        s_instance->m_functionTrace ||
        s_instance->m_crashOpcodeHistory)
    {
        s_instance->RecordOpcode(thread, opcode, static_cast<DWORD>(result));
    }

    if (s_instance->m_scriptOpcodeTrace ||
        s_instance->m_debugScripts.find(reinterpret_cast<uintptr_t>(thread)) != s_instance->m_debugScripts.end())
    {
        s_instance->WriteScript(
            "[opcode_after] script='%.8s' ptr=%p opcode=0x%04X result=%d cond=%d ip=%p off=0x%zX",
            thread->threadName,
            thread,
            opcode & 0x7FFF,
            result,
            thread->condResult ? 1 : 0,
            thread->ip,
            ScriptOffset(thread)
        );
    }

    return 0;
}

void __stdcall DebugUtils::OnScriptDeleted(CScriptThread* thread)
{
    if (!s_instance || !thread)
        return;

    const uintptr_t scriptPtr = reinterpret_cast<uintptr_t>(thread);
    s_instance->m_debugScripts.erase(scriptPtr);
    s_instance->m_seenScripts.erase(scriptPtr);
    if (s_instance->m_currentScriptPtr == scriptPtr)
        s_instance->m_currentScriptPtr = 0;

    // Retire the leak state now, at the moment the script is destroyed, instead of
    // waiting for the next sample. CLEO can reuse the freed script slot before the
    // next 5-30s sample; without this, the new script would inherit the old one's
    // first-bytes baseline and growth streak and be reported as a false leak.
    if (s_instance->m_leakTrackerEnabled)
        s_instance->RetireLeakScript(scriptPtr);

    s_instance->m_breakpoints.erase(
        std::remove_if(
            s_instance->m_breakpoints.begin(),
            s_instance->m_breakpoints.end(),
            [thread](const BreakpointInfo& bp)
            {
                return bp.scriptPtr == reinterpret_cast<uintptr_t>(thread);
            }
        ),
        s_instance->m_breakpoints.end()
    );

    s_instance->WriteCore(
        "[thread_delete] ptr=%p name='%.8s' base=%p ip=%p off=0x%zX",
        thread,
        thread->threadName,
        thread->baseIp,
        thread->ip,
        ScriptOffset(thread)
    );
}
