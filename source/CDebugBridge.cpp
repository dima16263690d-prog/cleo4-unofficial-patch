#include "stdafx.h"
#include "CDebugBridge.h"

#include <windows.h>
#include <cstdarg>

namespace
{
    volatile PVOID g_debugLogCallback = nullptr;

    // Crash context is thread-local because the unhandled exception filter
    // runs on the thread that actually faulted. This avoids the old global
    // sequence lock and two interlocked increments on every opcode.
    thread_local CLEO_CrashSnapshot g_threadCrashSnapshot =
    {
        0,
        0xFFFFFFFFu,
        0,
        -1,
        0,
        "none"
    };

}

namespace CLEO
{
    volatile LONG g_crashSnapshotEnabled = 0;
}


extern "C" BOOL WINAPI CLEO_DebugSetLogCallback(CLEO_DebugLogCallback callback)
{
    InterlockedExchangePointer(
        &g_debugLogCallback,
        reinterpret_cast<PVOID>(callback)
    );
    return TRUE;
}

extern "C" void __cdecl CLEO_DebugLog(int level, const char *format, ...)
{
    auto callback = reinterpret_cast<CLEO_DebugLogCallback>(
        InterlockedCompareExchangePointer(
            &g_debugLogCallback,
            nullptr,
            nullptr
        )
    );

    if (callback == nullptr || format == nullptr)
        return;

    va_list args;
    va_start(args, format);
    callback(level, format, args);
    va_end(args);
}

void Error(const char *message)
{
    CLEO_DebugLog(CLEO_DEBUG_ERROR, "%s", message ? message : "");
}

void Warning(const char *message)
{
    CLEO_DebugLog(CLEO_DEBUG_WARNING, "%s", message ? message : "");
}


extern "C" BOOL WINAPI CLEO_DebugSetCrashSnapshotEnabled(BOOL enabled)
{
    InterlockedExchange(
        &CLEO::g_crashSnapshotEnabled,
        enabled ? 1 : 0
    );
    return TRUE;
}

extern "C" void WINAPI CLEO_DebugRecordCrashOpcode(
    uintptr_t scriptPtr,
    const char* scriptName,
    DWORD opcode,
    DWORD opcodeOffset,
    LONG result
)
{
    if (CLEO::g_crashSnapshotEnabled == 0)
        return;

    g_threadCrashSnapshot.scriptPtr = scriptPtr;
    g_threadCrashSnapshot.opcode = opcode & 0x7FFF;
    g_threadCrashSnapshot.opcodeOffset = opcodeOffset;
    g_threadCrashSnapshot.opcodeResult = result;
    g_threadCrashSnapshot.gameTick = GetTickCount();

    if (scriptName != nullptr)
    {
        memcpy(
            g_threadCrashSnapshot.scriptName,
            scriptName,
            8
        );
        g_threadCrashSnapshot.scriptName[8] = '\0';
    }
    else
    {
        memcpy(
            g_threadCrashSnapshot.scriptName,
            "none",
            5
        );
        g_threadCrashSnapshot.scriptName[5] = '\0';
    }
}
extern "C" BOOL WINAPI CLEO_DebugGetCrashSnapshot(
    CLEO_CrashSnapshot* snapshot
)
{
    if (snapshot == nullptr || CLEO::g_crashSnapshotEnabled == 0)
        return FALSE;

    memcpy(
        snapshot,
        &g_threadCrashSnapshot,
        sizeof(CLEO_CrashSnapshot)
    );

    return g_threadCrashSnapshot.scriptPtr != 0 ||
           g_threadCrashSnapshot.opcode != 0xFFFFFFFFu;
}
