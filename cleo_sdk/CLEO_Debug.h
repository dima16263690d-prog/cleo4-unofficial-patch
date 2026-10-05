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
    CLEO_CB_SCRIPT_DELETED = 8
} CLEO_CallbackId;

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

typedef BOOL (__stdcall *CLEO_ScriptProcessBeforeCallback)(void* thread);
typedef void (__stdcall *CLEO_ScriptProcessAfterCallback)(void* thread);
typedef int (__stdcall *CLEO_ScriptOpcodeProcessBeforeCallback)(void* thread, DWORD opcode);
typedef int (__stdcall *CLEO_ScriptOpcodeProcessAfterCallback)(void* thread, DWORD opcode, int result);
typedef void (__stdcall *CLEO_GameCallback)();
typedef void (__stdcall *CLEO_ScriptDeletedCallback)(void* thread);

BOOL WINAPI CLEO_RegisterCallback(int callbackId, uintptr_t callback);
BOOL WINAPI CLEO_UnregisterCallback(int callbackId, uintptr_t callback);
BOOL WINAPI CLEO_DebugSetLogCallback(CLEO_DebugLogCallback callback);
void __cdecl CLEO_DebugLog(int level, const char* format, ...);

BOOL WINAPI CLEO_DebugSetCrashSnapshotEnabled(BOOL enabled);
void WINAPI CLEO_DebugRecordCrashOpcode(
    uintptr_t scriptPtr,
    const char* scriptName,
    DWORD opcode,
    DWORD opcodeOffset,
    LONG result
);
BOOL WINAPI CLEO_DebugGetCrashSnapshot(CLEO_CrashSnapshot* snapshot);

#ifdef __cplusplus
}
#endif
