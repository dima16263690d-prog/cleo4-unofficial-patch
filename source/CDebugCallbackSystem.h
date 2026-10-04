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

    BOOL RegisterCallback(eCallbackId id, uintptr_t callback);
    BOOL UnregisterCallback(eCallbackId id, uintptr_t callback);

    void NotifyGameBegin();
    void NotifyGameEnd();
    void NotifyGameProcessBefore();
    void NotifyGameProcessAfter();
    bool NotifyScriptProcessBefore(CRunningScript* script);
    void NotifyScriptProcessAfter(CRunningScript* script);
    int NotifyScriptOpcodeProcessBefore(CRunningScript* script, DWORD opcode);
    OpcodeResult NotifyScriptOpcodeProcessAfter(CRunningScript* script, DWORD opcode, OpcodeResult result);
    void NotifyScriptDeleted(CRunningScript* script);
}
