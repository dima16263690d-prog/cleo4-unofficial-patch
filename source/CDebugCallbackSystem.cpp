#include "stdafx.h"
#include "CDebugCallbackSystem.h"
#include "CCustomOpcodeSystem.h"
#include "CDebugBridge.h"

namespace CLEO
{
    struct CallbackRegistry
    {
        PVOID callbacks[CLEO_CB_SCRIPT_DELETED + 1] = {};
    };

    static CallbackRegistry g_callbacks;

    static uintptr_t GetCallback(eCallbackId id)
    {
        if (id < 0 || id > CLEO_CB_SCRIPT_DELETED)
            return 0;

        return reinterpret_cast<uintptr_t>(g_callbacks.callbacks[id]);
    }

    BOOL RegisterCallback(eCallbackId id, uintptr_t callback)
    {
        if (id < 0 || id > CLEO_CB_SCRIPT_DELETED || callback == 0)
            return FALSE;

        const PVOID previous = InterlockedCompareExchangePointer(
            reinterpret_cast<PVOID volatile*>(&g_callbacks.callbacks[id]),
            reinterpret_cast<PVOID>(callback),
            nullptr
        );

        return previous == nullptr ? TRUE : FALSE;
    }

    BOOL UnregisterCallback(eCallbackId id, uintptr_t callback)
    {
        if (id < 0 || id > CLEO_CB_SCRIPT_DELETED || callback == 0)
            return FALSE;

        const PVOID previous = InterlockedCompareExchangePointer(
            reinterpret_cast<PVOID volatile*>(&g_callbacks.callbacks[id]),
            nullptr,
            reinterpret_cast<PVOID>(callback)
        );

        return previous == reinterpret_cast<PVOID>(callback);
    }

    void NotifyGameBegin()
    {
        if (auto callback = GetCallback(CLEO_CB_GAME_BEGIN))
            reinterpret_cast<GameCallback>(callback)();
    }

    void NotifyGameEnd()
    {
        if (auto callback = GetCallback(CLEO_CB_GAME_END))
            reinterpret_cast<GameCallback>(callback)();
    }

    void NotifyGameProcessBefore()
    {
        if (auto callback = GetCallback(CLEO_CB_GAME_PROCESS_BEFORE))
            reinterpret_cast<GameCallback>(callback)();
    }

    void NotifyGameProcessAfter()
    {
        if (auto callback = GetCallback(CLEO_CB_GAME_PROCESS_AFTER))
            reinterpret_cast<GameCallback>(callback)();
    }

    bool NotifyScriptProcessBefore(CRunningScript* script)
    {
        if (auto callback = GetCallback(CLEO_CB_SCRIPT_PROCESS_BEFORE))
            return reinterpret_cast<ScriptProcessBeforeCallback>(callback)(script) != FALSE;

        return true;
    }

    void NotifyScriptProcessAfter(CRunningScript* script)
    {
        if (auto callback = GetCallback(CLEO_CB_SCRIPT_PROCESS_AFTER))
            reinterpret_cast<ScriptProcessAfterCallback>(callback)(script);
    }

    int NotifyScriptOpcodeProcessBefore(CRunningScript* script, DWORD opcode)
    {
        if (g_crashSnapshotEnabled != 0 && script != nullptr)
        {
            DWORD offset = 0;
            if (script->GetBytePointer() != nullptr && script->GetBasePointer() != nullptr &&
                script->GetBytePointer() >= script->GetBasePointer() + 2)
            {
                offset = static_cast<DWORD>(script->GetBytePointer() - script->GetBasePointer() - 2);
            }

            CLEO_DebugRecordCrashOpcode(
                reinterpret_cast<uintptr_t>(script),
                script->GetName(),
                opcode,
                offset,
                -1
            );
        }

        if (auto callback = GetCallback(CLEO_CB_SCRIPT_OPCODE_PROCESS_BEFORE))
            return reinterpret_cast<ScriptOpcodeProcessBeforeCallback>(callback)(script, opcode);

        return CLEO_DEBUG_OPCODE_PASSTHROUGH;
    }

    OpcodeResult NotifyScriptOpcodeProcessAfter(CRunningScript* script, DWORD opcode, OpcodeResult result)
    {
        if (g_crashSnapshotEnabled != 0 && script != nullptr)
        {
            DWORD offset = 0;
            if (script->GetBytePointer() != nullptr && script->GetBasePointer() != nullptr &&
                script->GetBytePointer() >= script->GetBasePointer() + 2)
            {
                offset = static_cast<DWORD>(script->GetBytePointer() - script->GetBasePointer() - 2);
            }

            CLEO_DebugRecordCrashOpcodeResult(static_cast<LONG>(result));
        }

        if (auto callback = GetCallback(CLEO_CB_SCRIPT_OPCODE_PROCESS_AFTER))
        {
            const int callbackResult =
                reinterpret_cast<ScriptOpcodeProcessAfterCallback>(callback)(script, opcode, result);

            if (callbackResult != 0)
                return OR_INTERRUPT;
        }

        return result;
    }

    void NotifyScriptDeleted(CRunningScript* script)
    {
        if (auto callback = GetCallback(CLEO_CB_SCRIPT_DELETED))
            reinterpret_cast<ScriptDeletedCallback>(callback)(script);
    }
}
