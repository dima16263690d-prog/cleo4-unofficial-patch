#include "stdafx.h"
#include "cleo.h"
#include "CCustomScript.h"
#include "ScmFunction.h"
#include "CDebugCallbackSystem.h"
#include "CCleoMemoryManager.h"
#include <cstdint>
#include <algorithm>
#include <cctype>

namespace CLEO
{
    static std::string NormalizeCustomScriptName(const char* name)
    {
        std::string key = name ? name : "";
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return key;
    }

    DWORD FUNC_AddScriptToQueue;
    DWORD FUNC_RemoveScriptFromQueue;
    DWORD FUNC_StopScript;
    DWORD FUNC_ScriptOpcodeHandler00;
    DWORD FUNC_GetScriptParams;
    DWORD FUNC_TransmitScriptParams;
    DWORD FUNC_SetScriptParams;
    DWORD FUNC_SetScriptCondResult;
    DWORD FUNC_GetScriptParamPointer1;
    DWORD FUNC_GetScriptStringParam;
    DWORD FUNC_GetScriptParamPointer2;

    void(__thiscall * AddScriptToQueue)(CRunningScript *, CRunningScript **queue);
    void(__thiscall * RemoveScriptFromQueue)(CRunningScript *, CRunningScript **queue);
    void(__thiscall * StopScript)(CRunningScript *);
    char(__thiscall * ScriptOpcodeHandler00)(CRunningScript *, WORD opcode);
    void(__thiscall * GetScriptParams)(CRunningScript *, int count);
    void(__thiscall * TransmitScriptParams)(CRunningScript *, CRunningScript *);
    void(__thiscall * SetScriptParams)(CRunningScript *, int count);
    void(__thiscall * SetScriptCondResult)(CRunningScript *, bool);
    SCRIPT_VAR *	(__thiscall * GetScriptParamPointer1)(CRunningScript *);
    void(__thiscall * GetScriptStringParam)(CRunningScript *, char* buf, BYTE len);
    SCRIPT_VAR *	(__thiscall * GetScriptParamPointer2)(CRunningScript *, int __unused__);

	void RunScriptDeleteDelegate(CRunningScript *script);

    void __fastcall _AddScriptToQueue(CRunningScript *pScript, int dummy, CRunningScript **queue)
    {
        _asm
        {
            push queue
            mov ecx, pScript
            call FUNC_AddScriptToQueue
        }
    }

    void __fastcall _RemoveScriptFromQueue(CRunningScript *pScript, int dummy, CRunningScript **queue)
    {
        _asm
        {
            push queue
            mov ecx, pScript
            call FUNC_RemoveScriptFromQueue
        }
    }

    void __fastcall _StopScript(CRunningScript *pScript)
    {
        _asm
        {
            mov ecx, pScript
            call FUNC_StopScript
        }
    }

    char __fastcall _ScriptOpcodeHandler00(CRunningScript *pScript, int dummy, WORD opcode)
    {
        int result;
        _asm
        {
            push opcode
            mov ecx, pScript
            call FUNC_ScriptOpcodeHandler00
            mov result, eax
        }
        return result;
    }

    void __fastcall _GetScriptParams(CRunningScript *pScript, int dummy, int count)
    {
        _asm
        {
            mov ecx, pScript
            push count
            call FUNC_GetScriptParams
        }
    }

    void __fastcall _TransmitScriptParams(CRunningScript *pScript, int dummy, CRunningScript *pScriptB)
    {
        _asm
        {
            mov ecx, pScript
            push pScriptB
            call FUNC_TransmitScriptParams
        }
    }

    void __fastcall _SetScriptParams(CRunningScript *pScript, int dummy, int count)
    {
        _asm
        {
            mov ecx, pScript
            push count
            call FUNC_SetScriptParams
        }
    }

    void __fastcall _SetScriptCondResult(CRunningScript *pScript, int dummy, int val)
    {
        _asm
        {
            mov ecx, pScript
            push val
            call FUNC_SetScriptCondResult
        }
    }

    SCRIPT_VAR * __fastcall _GetScriptParamPointer1(CRunningScript *pScript)
    {
        SCRIPT_VAR *result;
        _asm
        {
            mov ecx, pScript
            call FUNC_GetScriptParamPointer1
            mov result, eax
        }
        return (SCRIPT_VAR*)((size_t)result + pScript->GetBasePointer());
    }

    void __fastcall _GetScriptStringParam(CRunningScript *pScript, int dummy, char *buf, int len)
    {
        _asm
        {
            mov ecx, pScript
            push len
            push buf
            call FUNC_GetScriptStringParam
        }
    }

    SCRIPT_VAR * __fastcall _GetScriptParamPointer2(CRunningScript *pScript, int dummy, int unused)
    {
        _asm
        {
            mov ecx, pScript
            push unused
            call FUNC_GetScriptParamPointer2
        }
    }

    void(__cdecl * InitScm)();
    void(__cdecl * SaveScmData)();
    void(__cdecl * LoadScmData)();
    void(__cdecl * DrawScriptStuff)(char bBeforeFade);
    void(__cdecl * DrawScriptStuff_H)(char bBeforeFade);

    DWORD* GameTimer;
    extern "C" {
        SCRIPT_VAR *opcodeParams;
        SCRIPT_VAR *missionLocals;
        CRunningScript *staticThreads;
    }

    BYTE *scmBlock;
    BYTE *MissionLoaded;
    BYTE *missionBlock;
    BOOL *onMissionFlag;
    CTexture *scriptSprites;
    BYTE *scriptDraws;
    WORD *numScriptDraws;
    WORD *numScriptTexts;
    BYTE *useTextCommands;
    BYTE *scriptTexts;

    CRunningScript **inactiveThreadQueue, **activeThreadQueue;
	CCustomScript *lastScriptCreated = nullptr;

    // -------------------------------------------------------------------------
    // Script engine lifecycle
    // -------------------------------------------------------------------------
    // Keep GTA responsible for script execution and pActiveScripts ordering.
    // CScriptEngine owns the transition into/out of the CLEO runtime. This is
    // intentionally single-threaded for the first test baseline.
    void CScriptEngine::GameBegin(bool bLoadMode)
    {
        if (scriptsLoaded)
            return;

        if (activeThreadQueue == nullptr || *activeThreadQueue == nullptr)
            return;

        scriptsLoaded = true;

        // Preserve GTA's native processing order. CLEO scripts are built into
        // a separate temporary queue and attached after the native tail.
        CRunningScript *nativeHead = *activeThreadQueue;
        CRunningScript *nativeTail = nativeHead;
        size_t nativeCount = 1;

        while (nativeTail->GetNext() != nullptr)
        {
            nativeTail = nativeTail->GetNext();
            ++nativeCount;
        }

        TRACE("[engine] GameBegin: preserving native script order");

        *activeThreadQueue = nullptr;
        LoadCustomScripts(bLoadMode);

        if (*activeThreadQueue != nullptr)
        {
            nativeTail->SetNext(*activeThreadQueue);
            (*activeThreadQueue)->SetPrev(nativeTail);
        }

        *activeThreadQueue = nativeHead;

        const size_t customCount = CustomScripts.size() + (CustomMission != nullptr ? 1u : 0u);
        TRACE("[engine] Queue composed: native=%u custom=%u total=%u",
            static_cast<unsigned>(nativeCount),
            static_cast<unsigned>(customCount),
            static_cast<unsigned>(nativeCount + customCount));
        TRACE("[engine] GameBegin complete: CLEO scripts appended after native scripts");
        NotifyGameBegin();
    }

    void CScriptEngine::GameEnd()
    {
        if (!scriptsLoaded && CustomMission == nullptr && CustomScripts.empty())
            return;

        TRACE("[engine] GameEnd: stopping custom runtime");

        // All custom shutdown goes through the central lifecycle path.
        RemoveAllCustomScripts();

        // CLEO-style memory cleanup happens after scripts are destroyed,
        // so per-script ownership can be released before the final leak scan.
        GetSmartMemoryEngine().Memory().OnGameEnd();
        CompactProcessHeaps();

        scriptsLoaded = false;
        NotifyGameEnd();
    }

// TRUNCATED_WILL_FAIL
}
