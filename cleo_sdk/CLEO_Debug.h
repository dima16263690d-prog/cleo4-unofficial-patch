#pragma once

#include <windows.h>
#include <stdint.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum CLEO_CallbackId
{
    CLEO_CB_GAME_BEGIN = 0,
    CLEO_CB_GAME_END = 1,
    CLEO_CB_GAME_PROCESS_BEFORE = 2,
    CLEO_CB_GAME_PROCESS_AFTER = 3,
    CLEO_CB_SCRIPT_PROCESS_BEFORE = 4,
    CLEO_CB_SCRIPT_PROCESS_AFTER = 5,
    CLEO_CB_SCRIPT_OPCODE_PROCESS_BEFORE = 6,
    CLEO_CB_SCRIPT_OPCODE_PROCESS_AFTER = 7,
    CLEO_CB_SCRIPT_DELETED = 8,
    // Plugin SDK 2.0 additions. Object lifetime notifications let plugins
    // attach and release per-script state without walking the GTA queue.
    CLEO_CB_SCRIPT_CREATED = 9,
    CLEO_CB_SCRIPT_OPCODE_REGISTERED = 10
} CLEO_CallbackId;

// ---------------------------------------------------------------------------
// Plugin SDK 2.0 shared types.
// These live in the debug/SDK header (included by CLEO.h) so the core can use
// them without pulling in the public SCRIPT_VAR union, which differs from the
// engine's internal one.
// ---------------------------------------------------------------------------

typedef enum CLEO_OpcodeParamKind
{
    CLEO_OPARAM_ANY = 0,
    CLEO_OPARAM_INT = 1,
    CLEO_OPARAM_FLOAT = 2,
    CLEO_OPARAM_STRING = 3,
    CLEO_OPARAM_VAR = 4
} CLEO_OpcodeParamKind;

typedef struct CLEO_OpcodeInfo
{
    WORD opcode;
    WORD paramCount;
    CLEO_OpcodeParamKind paramKind[16];
    const char* name;       // borrowed pointer; owned by the registering plugin
    const char* params;     // borrowed pointer; owned by the registering plugin
    const char* description;// borrowed pointer; owned by the registering plugin
    uintptr_t handler;
    DWORD pluginModule;     // HMODULE of the registering module (0 = core)
    BOOL legacy;            // TRUE when registered through the old CLEO_RegisterOpcode
} CLEO_OpcodeInfo;

// Script execution profiler. Times are in milliseconds of wall-clock time on
// the game thread. "self" excludes time spent inside child scripts.
typedef struct CLEO_ScriptProfile
{
    uintptr_t scriptPtr;
    char scriptName[9];
    unsigned long long totalMs;
    unsigned long long selfMs;
    unsigned long long opcodes;
    unsigned long long ticks;
    // Appended at the end to keep older field offsets stable. Self time minus
    // the profiler's own instrumentation cost; meaningful only while overhead
    // measurement is on (CLEO_SetProfilerOverheadEnabled).
    unsigned long long netMs;
} CLEO_ScriptProfile;

// One entry of the top-opcodes histogram (CLEO_GetTopOpcodes). `reserved`
// keeps the struct 16 bytes and is always zero.
typedef struct CLEO_OpcodeCount
{
    WORD opcode;
    WORD reserved;
    unsigned long long count;
} CLEO_OpcodeCount;

typedef enum CLEO_DebugLogLevel
{
    CLEO_DEBUG_INFO = 0,
    CLEO_DEBUG_WARNING = 1,
    CLEO_DEBUG_ERROR = 2,
    CLEO_DEBUG_DIAGNOSTIC = 3
} CLEO_DebugLogLevel;

typedef void (__cdecl *CLEO_DebugLogCallback)(int level, const char* format, va_list args);

typedef enum CLEO_DebugOpcodeAction
{
    CLEO_DEBUG_OPCODE_PASSTHROUGH = 0,
    CLEO_DEBUG_OPCODE_HANDLED = 1,
    CLEO_DEBUG_OPCODE_INTERRUPT = 2
} CLEO_DebugOpcodeAction;

typedef struct CLEO_CrashSnapshot
{
    uintptr_t scriptPtr;
    DWORD opcode;
    DWORD opcodeOffset;
    LONG opcodeResult;
    DWORD gameTick;
    char scriptName[9];
} CLEO_CrashSnapshot;

// Aggregate view of memory currently tracked for scripts via the
// allocate_memory / free_allocated_memory opcodes.
typedef struct CLEO_MemoryStats
{
    unsigned long long totalBlocks;
    unsigned long long totalBytes;
    unsigned long long peakBytes;
    // Fast-memory (VirtualAlloc arenas) detail. Additive: older consumers that
    // only read the first three fields keep working.
    unsigned long long reservedBytes;
    unsigned long long committedBytes;
    unsigned long long recyclableBytes;
} CLEO_MemoryStats;

typedef BOOL (__stdcall *CLEO_ScriptProcessBeforeCallback)(void* thread);
typedef void (__stdcall *CLEO_ScriptProcessAfterCallback)(void* thread);
typedef int (__stdcall *CLEO_ScriptOpcodeProcessBeforeCallback)(void* thread, DWORD opcode);
typedef int (__stdcall *CLEO_ScriptOpcodeProcessAfterCallback)(void* thread, DWORD opcode, int result);
typedef void (__stdcall *CLEO_GameCallback)();
typedef void (__stdcall *CLEO_ScriptDeletedCallback)(void* thread);

// Release callback for the Plugin SDK 2.0 resource registry. Lives here so the
// core can see it via its explicit "CLEO_Debug.h" include regardless of how
// each translation unit reaches the public CLEO.h.
typedef void (__stdcall *CLEO_PluginResourceReleaseFn)(void* resource);

BOOL WINAPI CLEO_RegisterCallback(int callbackId, uintptr_t callback);
BOOL WINAPI CLEO_UnregisterCallback(int callbackId, uintptr_t callback);
BOOL WINAPI CLEO_DebugSetLogCallback(CLEO_DebugLogCallback callback);
void __cdecl CLEO_DebugLog(int level, const char* format, ...);

BOOL WINAPI CLEO_DebugSetCrashSnapshotEnabled(BOOL enabled);
BOOL WINAPI CLEO_DebugGetMemoryStats(CLEO_MemoryStats* stats);
void WINAPI CLEO_DebugRecordCrashOpcode(
    uintptr_t scriptPtr,
    const char* scriptName,
    DWORD opcode,
    DWORD opcodeOffset,
    LONG result
);
void WINAPI CLEO_DebugRecordCrashOpcodeResult(LONG result);
BOOL WINAPI CLEO_DebugGetCrashSnapshot(CLEO_CrashSnapshot* snapshot);

#ifdef __cplusplus
}
#endif
