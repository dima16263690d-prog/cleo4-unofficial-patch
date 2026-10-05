#pragma once

#include "../cleo_sdk/CLEO_Debug.h"

#ifdef DEBUGIT
#define TRACE(format,...) CLEO_DebugLog(CLEO_DEBUG_INFO, format __VA_OPT__(,) __VA_ARGS__)
#define DIAG(format,...) CLEO_DebugLog(CLEO_DEBUG_DIAGNOSTIC, format __VA_OPT__(,) __VA_ARGS__)
#define SCRIPT_TRACE(format,...) CLEO_DebugLog(CLEO_DEBUG_DIAGNOSTIC, "[SCRIPT] " format __VA_OPT__(,) __VA_ARGS__)
#define MEMORY_TRACE(format,...) CLEO_DebugLog(CLEO_DEBUG_DIAGNOSTIC, "[MEMORY] " format __VA_OPT__(,) __VA_ARGS__)
#else
#define TRACE(...) __noop
#define DIAG(...) __noop
#define SCRIPT_TRACE(...) __noop
#define MEMORY_TRACE(...) __noop
#endif

void Warning(const char *message);
void Error(const char *message);


namespace CLEO
{
    extern volatile LONG g_crashSnapshotEnabled;

    inline bool IsCrashSnapshotEnabled()
    {
        return g_crashSnapshotEnabled != 0;
    }
}
