#include "stdafx.h"
#include "CDebugBridge.h"

#include <windows.h>
#include <cstdarg>

namespace
{
    volatile PVOID g_debugLogCallback = nullptr;
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
