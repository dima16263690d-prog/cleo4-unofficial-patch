#pragma once

#include "CLEO.h"
#include "../cleo_sdk/CLEO_Debug.h"
#include "CTheScripts.h"
#include <windows.h>
#include <vector>

namespace CLEO
{
    typedef BOOL (__stdcall *ScriptProcessBeforeCallback)(CRunningScript*);
    using eCallbackId = int;
    typedef void (__stdcall *ScriptProcessAfterCallback)(CRunningScript*);
    typedef int (__stdcall *ScriptOpcodeProcessBeforeCallback)(CRunningScript*, DWORD);
    typedef int (__stdcall *ScriptOpcodeProcessAfterCallback)(CRunningScript*, DWORD, int);
    typedef void (__stdcall *GameCallback)();
    typedef void (__stdcall *ScriptDeletedCallback)(CRunningScript*);
    typedef void (__stdcall *ScriptOpcodeRegisteredCallback)(WORD opcode);
    typedef void (__stdcall *ScriptCreatedCallback)(CRunningScript*);

    BOOL RegisterCallback(eCallbackId id, uintptr_t callback);
    BOOL UnregisterCallback(eCallbackId id, uintptr_t callback);

    // Drop every subscription that belongs to `module`. Called before the
    // module is unloaded so no callback can fire into freed code. Idempotent.
    void ReleaseCallbacksForModule(HMODULE module);

    // True when any plugin has registered a per-opcode observer. Used to keep
    // the hot opcode loop free of callback work when nobody is listening.
    bool ScriptOpcodeCallbacksActive();

    void NotifyGameBegin();
    void NotifyGameEnd();
    void NotifyGameProcessBefore();
    void NotifyGameProcessAfter();
    bool NotifyScriptProcessBefore(CRunningScript* script);
    void NotifyScriptProcessAfter(CRunningScript* script);
    int NotifyScriptOpcodeProcessBefore(CRunningScript* script, DWORD opcode);
    OpcodeResult NotifyScriptOpcodeProcessAfter(CRunningScript* script, DWORD opcode, OpcodeResult result);
    void NotifyScriptDeleted(CRunningScript* script);

    // Plugin SDK 2.0: announce a newly registered opcode to interested plugins.
    void NotifyScriptOpcodeRegistered(WORD opcode);
    void NotifyScriptCreated(CRunningScript* script);
}
