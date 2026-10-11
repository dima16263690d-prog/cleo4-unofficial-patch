#include "stdafx.h"
#include "cleo.h"
#include "CCustomScript.h"
#include "ScmFunction.h"
#include "CDebugCallbackSystem.h"
#include "CCleoMemoryManager.h"
#include <cstdint>
#include <algorithm>

namespace CLEO
{
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
        SCRIPT_VAR *result;
        _asm
        {
            mov ecx, pScript
            push unused
            call FUNC_GetScriptParamPointer2
            mov result, eax
        }
        return result;
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

    // GTA's streaming budget for models/textures (CStreaming::ms_memoryAvailable)
    // defaults to 50 MB. Raise it once the game has initialised streaming;
    // never lower a value another mod already set.
    static void ApplyStreamingMemoryLimit()
    {
        DWORD *memoryAvailable = nullptr;
        switch (GetInstance().VersionManager.GetGameVersion())
        {
        case GV_US10:
        case GV_EU10:
            memoryAvailable = reinterpret_cast<DWORD*>(0x8A5A80);
            break;
        default:
            TRACE("[MEMORY] STREAMING limit unchanged: address unknown for this game version");
            return;
        }

        // 1 GB only fits safely in the 4 GB address space given by LAA.
        const bool largeAddressAware = IsGameLargeAddressAware();
        const DWORD desired = (largeAddressAware ? 1024u : 512u) * 1024 * 1024;
        const DWORD current = *memoryAvailable;

        if (current < desired)
            *memoryAvailable = desired;

        TRACE("[MEMORY] STREAMING limit %u -> %u MB (large_address_aware=%d)",
            current / (1024 * 1024),
            *memoryAvailable / (1024 * 1024),
            largeAddressAware ? 1 : 0);
    }

    // called to initialise the scripts (after the main.scm has actually had a chance to set up)
    void OnInitScm1(void)
    {
        TRACE("Scripts initialized");
        GetInstance().ScriptEngine.GameEnd();
        InitScm();
        ApplyStreamingMemoryLimit();
        GetInstance().TextManager.ClearDynamicFxts();
        GetInstance().OpcodeSystem.FinalizeScriptObjects();
        GetInstance().SoundSystem.UnloadAllStreams();
    }

    // called on first load before the others
    void OnInitScm2(void)
    {
        TRACE("Scripts exclusively initialized");
        GetInstance().ScriptEngine.GameEnd();
        InitScm();
        ApplyStreamingMemoryLimit();
        GetInstance().TextManager.ClearDynamicFxts();
        GetInstance().OpcodeSystem.FinalizeScriptObjects();
        GetInstance().SoundSystem.UnloadAllStreams();
    }

    // called to load the scripts
    void OnInitScm3(void)
    {
        TRACE("Scripts loaded");
        GetInstance().ScriptEngine.GameEnd();
        InitScm();
        ApplyStreamingMemoryLimit();
        GetInstance().TextManager.ClearDynamicFxts();
        GetInstance().OpcodeSystem.FinalizeScriptObjects();
        GetInstance().SoundSystem.UnloadAllStreams();
    }

    extern "C" void __stdcall opcode_004E(CCustomScript *pScript)
    {
        if (pScript == nullptr)
            return;

        if (pScript->IsCustom() && !pScript->IsMission())
        {
            TRACE("[004E] Incorrect usage of opcode in script '%s'.", pScript->GetName());
        }

        GetInstance().ScriptEngine.RemoveScript(reinterpret_cast<CRunningScript*>(pScript));
    }

    extern "C" void __declspec(naked) opcode_004E_hook(void)
    {
        __asm
        {
            push esi
            call opcode_004E
            pop edi
            mov al, 1
            pop esi
            mov ecx, [esp + 0x14]
            mov fs : 0, ecx
            add esp, 32
            ret 0x4
        }
    }

    void OnNewGame(void)
    {
        static struct CGangWeapons {
            BYTE _f0;
            BYTE _f1; // -
            DWORD weapon1;
            DWORD weapon2;
            DWORD weapon3;
        } *gangWeapons((CGangWeapons *)0xC0B870);	// 1.01 eu specific
        TRACE("New game started");
        gangWeapons[0].weapon1 = 22;
        gangWeapons[0].weapon2 = 28;
        gangWeapons[0].weapon3 = 0;

        gangWeapons[1].weapon1 = 22;
        gangWeapons[1].weapon2 = 0;
        gangWeapons[1].weapon3 = 0;

        gangWeapons[2].weapon1 = 22;
        gangWeapons[2].weapon2 = 0;
        gangWeapons[2].weapon3 = 0;

        gangWeapons[4].weapon1 = 24;
        gangWeapons[4].weapon2 = 28;
        gangWeapons[4].weapon3 = 0;

        gangWeapons[5].weapon1 = 24;
        gangWeapons[5].weapon2 = 0;
        gangWeapons[5].weapon3 = 0;

        gangWeapons[6].weapon1 = 22;
        gangWeapons[6].weapon2 = 30;
        gangWeapons[6].weapon3 = 0;

        gangWeapons[7].weapon1 = 22;
        gangWeapons[7].weapon2 = 28;
        gangWeapons[7].weapon3 = 0;
        GetInstance().TextManager.ClearDynamicFxts();
        GetInstance().OpcodeSystem.FinalizeScriptObjects();
        GetInstance().ScriptEngine.GameEnd();
        GetInstance().SoundSystem.UnloadAllStreams();
    }

    void OnLoadScmData(void)
    {
        TRACE(__FUNCSIG__);
        LoadScmData();
    }

    void OnSaveScmData(void)
    {
        TRACE(__FUNCSIG__);
        GetInstance().ScriptEngine.SaveState();
        GetInstance().ScriptEngine.UnregisterAllScripts();
        SaveScmData();
        GetInstance().ScriptEngine.ReregisterAllScripts();
    }

    void __fastcall HOOK_ProcessScript(CCustomScript * pScript, int)
    {
        // Destroy scripts deferred by the previous processing boundary.
        GetInstance().ScriptEngine.DeleteWaitingScripts();

        // Wait until GTA's active script queue is ready before loading CLEO scripts.
        GetInstance().ScriptEngine.GameBegin();

        if (pScript == nullptr)
            return;

        CRunningScript *script = reinterpret_cast<CRunningScript*>(pScript);

        // Keep DebugUtils/plugin callbacks at the existing processing boundary.
        if (!NotifyScriptProcessBefore(script))
            return;

        // Native GTA scripts retain GTA's original executor. CLEO scripts keep
        // their existing CCustomScript::Process() path.
        CCustomScript *customScript = reinterpret_cast<CCustomScript*>(script);
        if (customScript->IsCustom())
            customScript->Process();
        else
            ProcessScript(script);

        NotifyScriptProcessAfter(script);
    }

    void HOOK_DrawScriptStuff(char bBeforeFade)
    {
        GetInstance().ScriptEngine.DrawScriptStuff(bBeforeFade);

        // restore SCM textures and return to the overwritten func (which may != DrawScriptSprites)
        return bBeforeFade ? DrawScriptStuff_H(bBeforeFade) : DrawScriptStuff(bBeforeFade);
    }

    void CScriptEngine::UpdateDrawableScript(CCustomScript* script, bool drawable)
    {
        if (script == nullptr)
            return;

        auto existing = std::find(m_drawableScripts.begin(), m_drawableScripts.end(), script);

        if (!drawable)
        {
            if (existing != m_drawableScripts.end())
                m_drawableScripts.erase(existing);
            return;
        }

        // No draw data transition means no list work. The list contains only
        // scripts with persistent draw/text state, in CustomScripts order.
        if (existing != m_drawableScripts.end())
            return;

        const auto scriptPosition = std::find(CustomScripts.begin(), CustomScripts.end(), script);
        if (scriptPosition == CustomScripts.end())
            return; // custom missions are drawn separately

        auto nextScript = scriptPosition;
        ++nextScript;
        for (; nextScript != CustomScripts.end(); ++nextScript)
        {
            const auto nextDrawable = std::find(
                m_drawableScripts.begin(), m_drawableScripts.end(), *nextScript);
            if (nextDrawable != m_drawableScripts.end())
            {
                m_drawableScripts.insert(nextDrawable, script);
                return;
            }
        }

        m_drawableScripts.push_back(script);
    }

    void CScriptEngine::DrawScriptStuff(char bBeforeFade)
    {
        // Draw only scripts that currently own draw/text records. Advance the
        // iterator first because Draw() may clear its records and unregister
        // itself from m_drawableScripts.
        for (auto it = m_drawableScripts.begin(); it != m_drawableScripts.end(); )
        {
            auto current = it++;
            if (*current != nullptr)
                (*current)->Draw(bBeforeFade);
        }

        if (auto script = GetCustomMission())
            script->Draw(bBeforeFade);
    }
    void CScriptEngine::Inject(CCodeInjector& inj)
    {
        TRACE("Injecting ScriptEngine...");
        CGameVersionManager& gvm = GetInstance().VersionManager;

        // Global Events crashfix
        //inj.MemoryWrite(0xA9AF6C, 0, 4);

        // Dirty hacks to keep compatibility with plugins + overcome VS thiscall restrictions
        FUNC_AddScriptToQueue = gvm.TranslateMemoryAddress(MA_ADD_SCRIPT_TO_QUEUE_FUNCTION);
        FUNC_RemoveScriptFromQueue = gvm.TranslateMemoryAddress(MA_REMOVE_SCRIPT_FROM_QUEUE_FUNCTION);
        FUNC_StopScript = gvm.TranslateMemoryAddress(MA_STOP_SCRIPT_FUNCTION);
        FUNC_ScriptOpcodeHandler00 = gvm.TranslateMemoryAddress(MA_SCRIPT_OPCODE_HANDLER0_FUNCTION);
        FUNC_GetScriptParams = gvm.TranslateMemoryAddress(MA_GET_SCRIPT_PARAMS_FUNCTION);
        FUNC_TransmitScriptParams = gvm.TranslateMemoryAddress(MA_TRANSMIT_SCRIPT_PARAMS_FUNCTION);
        FUNC_SetScriptParams = gvm.TranslateMemoryAddress(MA_SET_SCRIPT_PARAMS_FUNCTION);
        FUNC_SetScriptCondResult = gvm.TranslateMemoryAddress(MA_SET_SCRIPT_COND_RESULT_FUNCTION);
        FUNC_GetScriptParamPointer1 = gvm.TranslateMemoryAddress(MA_GET_SCRIPT_PARAM_POINTER1_FUNCTION);
        FUNC_GetScriptStringParam = gvm.TranslateMemoryAddress(MA_GET_SCRIPT_STRING_PARAM_FUNCTION);
        FUNC_GetScriptParamPointer2 = gvm.TranslateMemoryAddress(MA_GET_SCRIPT_PARAM_POINTER2_FUNCTION);

        AddScriptToQueue = reinterpret_cast<void(__thiscall*)(CRunningScript*, CRunningScript**)>(_AddScriptToQueue);
        RemoveScriptFromQueue = reinterpret_cast<void(__thiscall*)(CRunningScript*, CRunningScript**)>(_RemoveScriptFromQueue);
        StopScript = reinterpret_cast<void(__thiscall*)(CRunningScript*)>(_StopScript);
        ScriptOpcodeHandler00 = reinterpret_cast<char(__thiscall*)(CRunningScript*, WORD)>(_ScriptOpcodeHandler00);
        GetScriptParams = reinterpret_cast<void(__thiscall*)(CRunningScript*, int)>(_GetScriptParams);
        TransmitScriptParams = reinterpret_cast<void(__thiscall*)(CRunningScript*, CRunningScript*)>(_TransmitScriptParams);
        SetScriptParams = reinterpret_cast<void(__thiscall*)(CRunningScript*, int)>(_SetScriptParams);
        SetScriptCondResult = reinterpret_cast<void(__thiscall*)(CRunningScript*, bool)>(_SetScriptCondResult);
        GetScriptParamPointer1 = reinterpret_cast<SCRIPT_VAR * (__thiscall*)(CRunningScript*)>(_GetScriptParamPointer1);
        GetScriptStringParam = reinterpret_cast<void(__thiscall*)(CRunningScript*, char*, BYTE)>(_GetScriptStringParam);
        GetScriptParamPointer2 = reinterpret_cast<SCRIPT_VAR * (__thiscall*)(CRunningScript*, int)>(_GetScriptParamPointer2);

        InitScm = gvm.TranslateMemoryAddress(MA_INIT_SCM_FUNCTION);
        SaveScmData = gvm.TranslateMemoryAddress(MA_SAVE_SCM_DATA_FUNCTION);
        LoadScmData = gvm.TranslateMemoryAddress(MA_LOAD_SCM_DATA_FUNCTION);

        GameTimer = gvm.TranslateMemoryAddress(MA_GAME_TIMER);
        opcodeParams = gvm.TranslateMemoryAddress(MA_OPCODE_PARAMS);
        missionLocals = gvm.TranslateMemoryAddress(MA_MISSION_LOCALS);
        scmBlock = gvm.TranslateMemoryAddress(MA_SCM_BLOCK);
        MissionLoaded = gvm.TranslateMemoryAddress(MA_MISSION_LOADED);
        missionBlock = gvm.TranslateMemoryAddress(MA_MISSION_BLOCK);
        onMissionFlag = gvm.TranslateMemoryAddress(MA_ON_MISSION_FLAG);

        // Protect script dependencies
        auto addr = gvm.TranslateMemoryAddress(MA_CALL_PROCESS_SCRIPT);
        inj.MemoryReadOffset(addr.address + 1, ProcessScript);
        inj.ReplaceFunction(HOOK_ProcessScript, addr);

        scriptSprites = gvm.TranslateMemoryAddress(MA_SCRIPT_SPRITE_ARRAY);
        scriptDraws = gvm.TranslateMemoryAddress(MA_SCRIPT_DRAW_ARRAY);
        scriptTexts = gvm.TranslateMemoryAddress(MA_SCRIPT_TEXT_ARRAY);
        numScriptDraws = gvm.TranslateMemoryAddress(MA_NUM_SCRIPT_DRAWS);
        numScriptTexts = gvm.TranslateMemoryAddress(MA_NUM_SCRIPT_TEXTS);
        useTextCommands = gvm.TranslateMemoryAddress(MA_USE_TEXT_COMMANDS);

        inj.MemoryReadOffset(gvm.TranslateMemoryAddress(MA_CALL_DRAW_SCRIPT_TEXTS_AFTER_FADE).address + 1, CLEO::DrawScriptStuff);
        inj.MemoryReadOffset(gvm.TranslateMemoryAddress(MA_CALL_DRAW_SCRIPT_TEXTS_BEFORE_FADE).address + 1, DrawScriptStuff_H);
        inj.ReplaceFunction(HOOK_DrawScriptStuff, gvm.TranslateMemoryAddress(MA_CALL_DRAW_SCRIPT_TEXTS_AFTER_FADE));
        inj.ReplaceFunction(HOOK_DrawScriptStuff, gvm.TranslateMemoryAddress(MA_CALL_DRAW_SCRIPT_TEXTS_BEFORE_FADE));
        inj.MemoryWrite(gvm.TranslateMemoryAddress(MA_CODE_JUMP_FOR_TXD_STORE), OP_RET);

        inactiveThreadQueue = gvm.TranslateMemoryAddress(MA_INACTIVE_THREAD_QUEUE);
        activeThreadQueue = gvm.TranslateMemoryAddress(MA_ACTIVE_THREAD_QUEUE);
        staticThreads = gvm.TranslateMemoryAddress(MA_STATIC_THREADS);

        if (gvm.GetGameVersion() == GV_EU11)
        {
            GetInstance().HookSystem.InstallCall(
                inj,
                "OnInitScm3",
                gvm.TranslateMemoryAddress(MA_CALL_INIT_SCM3),
                (size_t)OnInitScm3
            );
            inj.InjectFunction(OnNewGame, 0x5DEEA0);	// GV_EU11 specific
        }
        else
        {
            GetInstance().HookSystem.InstallCall(
                inj,
                "OnInitScm1",
                gvm.TranslateMemoryAddress(MA_CALL_INIT_SCM1),
                (size_t)OnInitScm1
            );
            GetInstance().HookSystem.InstallCall(
                inj,
                "OnInitScm2",
                gvm.TranslateMemoryAddress(MA_CALL_INIT_SCM2),
                (size_t)OnInitScm2
            );
            GetInstance().HookSystem.InstallCall(
                inj,
                "OnInitScm3",
                gvm.TranslateMemoryAddress(MA_CALL_INIT_SCM3),
                (size_t)OnInitScm3
            );
        }

        inj.ReplaceFunction(OnLoadScmData, gvm.TranslateMemoryAddress(MA_CALL_LOAD_SCM_DATA));
        inj.ReplaceFunction(OnSaveScmData, gvm.TranslateMemoryAddress(MA_CALL_SAVE_SCM_DATA));
        inj.InjectFunction(&opcode_004E_hook, gvm.TranslateMemoryAddress(MA_OPCODE_004E));
    }

	float VectorSqrMagnitude(CVector vector) { return vector.x * vector.x + vector.y * vector.y + vector.z * vector.z; }
}