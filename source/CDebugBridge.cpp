#include "stdafx.h"
#include "CDebugBridge.h"

#include <windows.h>
#include <cstdarg>

namespace
{
    volatile PVOID g_debugLogCallback = nullptr;

    volatile LONG g_crashSnapshotSequence = 0;
    CLEO_CrashSnapshot g_crashSnapshot = {};
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

    InterlockedIncrement(&g_crashSnapshotSequence);

    g_crashSnapshot.scriptPtr = scriptPtr;
    g_crashSnapshot.opcode = opcode & 0x7FFF;
    g_crashSnapshot.opcodeOffset = opcodeOffset;
    g_crashSnapshot.opcodeResult = result;
    g_crashSnapshot.gameTick = GetTickCount();

    if (scriptName != nullptr)
    {
        memcpy(g_crashSnapshot.scriptName, scriptName, 8);
        g_crashSnapshot.scriptName[8] = '\0';
    }
    else
    {
        memcpy(g_crashSnapshot.scriptName, "none", 5);
        g_crashSnapshot.scriptName[5] = '\0';
    }

    InterlockedIncrement(&g_crashSnapshotSequence);
}

extern "C" BOOL WINAPI CLEO_DebugGetCrashSnapshot(CLEO_CrashSnapshot* snapshot)
{
    if (snapshot == nullptr)
        return FALSE;

    for (unsigned attempt = 0; attempt < 4; ++attempt)
    {
        const LONG begin = g_crashSnapshotSequence;

        if (begin & 1)
            continue;

        memcpy(snapshot, &g_crashSnapshot, sizeof(CLEO_CrashSnapshot));
        MemoryBarrier();

        const LONG end = g_crashSnapshotSequence;

        if (begin == end && !(end & 1))
            return TRUE;
    }

    return FALSE;
}
