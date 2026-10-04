#include "stdafx.h"
#include "cleo.h"
#include "CCustomScript.h"
#include "ScmFunction.h"
#include "CDebugCallbackSystem.h"
#include <cstdint>

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
        scriptsLoaded = false;
        NotifyGameEnd();
    }

    // called to initialise the scripts (after the main.scm has actually had a chance to set up)
    void OnInitScm1(void)
    {
        TRACE("Scripts initialized");
        GetInstance().ScriptEngine.GameEnd();
        InitScm();
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

    struct CleoSafeHeader
    {
        const static unsigned sign;
        unsigned signature;
        unsigned n_saved_threads;
        unsigned n_stopped_threads;
    };

    const unsigned CleoSafeHeader::sign = 0x31345653;

    struct ThreadSavingInfo
    {
        unsigned long hash;
        SCRIPT_VAR tls[32];
        unsigned timers[2];
        bool condResult;
        unsigned sleepTime;
        eLogicalOperation logicalOp;
        bool notFlag;
        ptrdiff_t ip_diff;
        char threadName[8];

        ThreadSavingInfo(CCustomScript *cs) :
            hash(cs->dwChecksum), condResult(cs->bCondResult),
            logicalOp(cs->LogicalOp), notFlag(cs->NotFlag != false), ip_diff(cs->CurrentIP - reinterpret_cast<BYTE*>(cs->BaseIP))
        {
            sleepTime = cs->WakeTime >= *GameTimer ? 0 : cs->WakeTime - *GameTimer;
            std::copy(cs->LocalVar, cs->LocalVar + 32, tls);
            std::copy(cs->Timers, cs->Timers + 2, timers);
            std::copy(cs->Name, cs->Name + 8, threadName);
        }

        void Apply(CCustomScript *cs)
        {
            cs->dwChecksum = hash;
            std::copy(tls, tls + 32, cs->LocalVar);
            std::copy(timers, timers + 2, cs->Timers);
            cs->bCondResult = condResult;
            cs->WakeTime = *GameTimer + sleepTime;
            cs->LogicalOp = logicalOp;
            cs->NotFlag = notFlag;
            cs->CurrentIP = reinterpret_cast<BYTE*>(cs->BaseIP) + ip_diff;
            std::copy(threadName, threadName + 8, cs->Name);
            cs->bSaveEnabled = true;
        }

        ThreadSavingInfo() { }
    };


    // Stage 3: child custom-script state is stored in a sidecar file so the
    // legacy cs*.sav binary layout remains byte-for-byte compatible.
    struct ChildSaveHeader
    {
        const static unsigned sign;
        const static unsigned format_version;
        unsigned signature;
        unsigned version;
        unsigned n_children;
    };

    const unsigned ChildSaveHeader::sign = 0x31484343; // CCH1
    const unsigned ChildSaveHeader::format_version = 1;

    struct ChildThreadSavingInfo
    {
        unsigned node_id;
        unsigned parent_node_id;
        int label;
        unsigned ordinal;
        ThreadSavingInfo state;

        ChildThreadSavingInfo() : node_id(0), parent_node_id(0), label(0), ordinal(0) {}
        ChildThreadSavingInfo(CCustomScript *cs, unsigned parentId, unsigned nodeId, unsigned childOrdinal)
            : node_id(nodeId), parent_node_id(parentId), label(cs->GetChildLabel()), ordinal(childOrdinal), state(cs)
        {
        }

        void Apply(CCustomScript *cs)
        {
            state.Apply(cs);
            // Child streams are represented by the sidecar, not by the legacy
            // hash-only saved-thread list.
            cs->enable_saving(false);
        }
    };

    // Active 0AB1 execution scopes are stored in a separate sidecar so
    // the legacy cs*.sav layout remains unchanged. The current script locals
    // and IP are already covered by ThreadSavingInfo; this file persists the
    // ScmFunction caller snapshots needed when execution reaches 0AB2 later.
    struct SavedScmStringParam
    {
        DWORD oldPointer;
        std::string value;
    };

    struct ScmFunctionSaveInfo
    {
        unsigned node_id;
        unsigned depth;
        BYTE callArgCount;
        int32_t callOffset;
        int32_t retnOffset;
        int32_t savedBaseOffset;
        uint32_t savedCodeSize;
        int32_t savedStackOffsets[8];
        WORD savedSP;
        SCRIPT_VAR savedTls[32];
        bool savedCondResult;
        int32_t savedLogicalOp;
        bool savedNotFlag;
        std::string savedScriptFileDir;
        std::string savedScriptFileName;
        std::vector<SavedScmStringParam> stringParams;
    };

    struct ScmFunctionSaveHeader
    {
        const static unsigned sign;
        const static unsigned format_version;
        unsigned signature;
        unsigned version;
        unsigned n_functions;
    };

    const unsigned ScmFunctionSaveHeader::sign = 0x31465343; // CSF1
    const unsigned ScmFunctionSaveHeader::format_version = 1;

    std::vector<ScmFunctionSaveInfo> pendingScmFunctionSaves;

    template<typename T>
    void inline ReadBinary(std::istream& stream, T& value);

    template<typename T>
    void inline ReadBinary(std::istream& stream, T* value, size_t size);

    template<typename T>
    void inline WriteBinary(std::ostream& stream, const T& value);

    template<typename T>
    void inline WriteBinary(std::ostream& stream, const T* value, size_t size);

    inline int32_t SavePointerOffset(const BYTE *ptr, const BYTE *base)
    {
        if (!ptr)
            return INT32_MIN;

        return static_cast<int32_t>(
            reinterpret_cast<intptr_t>(ptr) - reinterpret_cast<intptr_t>(base)
        );
    }

    inline BYTE *RestorePointerOffset(BYTE *base, int32_t offset)
    {
        return offset == INT32_MIN ? nullptr : base + offset;
    }

    void SaveStringBinary(std::ostream& stream, const std::string& value)
    {
        uint32_t length = static_cast<uint32_t>(value.size());
        WriteBinary(stream, length);
        if (length)
            stream.write(value.data(), length);
    }

    bool LoadStringBinary(std::istream& stream, std::string& value)
    {
        uint32_t length = 0;
        ReadBinary(stream, length);

        // Refuse obviously corrupt/hostile lengths before allocating.
        if (length > 16 * 1024 * 1024)
            return false;

        value.resize(length);
        if (length)
            stream.read(&value[0], length);

        return stream.good();
    }

    void CollectScmFunctionSaves(
        CCustomScript *cs,
        unsigned nodeId,
        std::vector<ScmFunctionSaveInfo>& output)
    {
        if (!cs || nodeId == 0)
            return;

        std::vector<ScmFunction*> chain;
        std::set<WORD> visited;

        WORD id = cs->GetScmFunction();
        while (id < ScmFunction::store_size && ScmFunction::Store[id] && visited.insert(id).second)
        {
            chain.push_back(ScmFunction::Store[id]);
            id = ScmFunction::Store[id]->prevScmFunctionId;
        }

        std::reverse(chain.begin(), chain.end());

        const BYTE *base = cs->GetBasePointer();

        for (size_t depth = 0; depth < chain.size(); ++depth)
        {
            ScmFunction *fn = chain[depth];

            ScmFunctionSaveInfo saved{};
            saved.node_id = nodeId;
            saved.depth = static_cast<unsigned>(depth);
            saved.callArgCount = fn->callArgCount;
            saved.callOffset = SavePointerOffset(fn->callIP, base);
            saved.retnOffset = SavePointerOffset(fn->retnAddress, base);
            saved.savedBaseOffset = fn->savedBaseIP
                ? static_cast<int32_t>(
                    reinterpret_cast<intptr_t>(fn->savedBaseIP) -
                    reinterpret_cast<intptr_t>(base))
                : INT32_MIN;
            saved.savedCodeSize = static_cast<uint32_t>(fn->savedCodeSize);
            saved.savedSP = fn->savedSP;
            saved.savedCondResult = fn->savedCondResult;
            saved.savedLogicalOp = static_cast<int32_t>(fn->savedLogicalOp);
            saved.savedNotFlag = fn->savedNotFlag;
            saved.savedScriptFileDir = fn->savedScriptFileDir;
            saved.savedScriptFileName = fn->savedScriptFileName;

            for (size_t i = 0; i < 8; ++i)
                saved.savedStackOffsets[i] = SavePointerOffset(fn->savedStack[i], base);

            std::copy(fn->savedTls, fn->savedTls + 32, saved.savedTls);

            for (const auto& stringValue : fn->stringParams)
            {
                SavedScmStringParam stringParam{};
                stringParam.oldPointer = static_cast<DWORD>(
                    reinterpret_cast<uintptr_t>(stringValue.c_str()));
                stringParam.value = stringValue;
                saved.stringParams.push_back(std::move(stringParam));
            }

            output.push_back(std::move(saved));
        }
    }

    SCRIPT_VAR CScriptEngine::CleoVariables[0x400];

    template<typename T>
    void inline ReadBinary(std::istream& s, T& buf)
    {
        s.read(reinterpret_cast<char *>(&buf), sizeof(T));
    }

    template<typename T>
    void inline ReadBinary(std::istream& s, T *buf, size_t size)
    {
        s.read(reinterpret_cast<char *>(buf), sizeof(T) * size);
    }

    template<typename T>
    void inline WriteBinary(std::ostream& s, const T& data)
    {
        s.write(reinterpret_cast<const char *>(&data), sizeof(T));
    }

    template<typename T>
    void inline WriteBinary(std::ostream& s, const T*data, size_t size)
    {
        s.write(reinterpret_cast<const char *>(data), sizeof(T) * size);
    }

    void __fastcall HOOK_ProcessScript(CCustomScript * pScript, int)
    {
        // Match CLEO 5 lifecycle: destroy scripts deferred by the previous
        // processing boundary before attempting to initialize the runtime.
        GetInstance().ScriptEngine.DeleteWaitingScripts();

        // CLEO 5 retries GameBegin from the script-processing hook because
        // pActiveScripts may not be ready during the initial SCM callbacks.
        GetInstance().ScriptEngine.GameBegin();

        if (pScript == nullptr)
            return;

        if (!NotifyScriptProcessBefore(reinterpret_cast<CRunningScript*>(pScript)))
            return;

        if (pScript->IsCustom())
            pScript->Process();
        else
            ProcessScript(pScript);

        NotifyScriptProcessAfter(reinterpret_cast<CRunningScript*>(pScript));
    }

    void HOOK_DrawScriptStuff(char bBeforeFade)
    {
        GetInstance().ScriptEngine.DrawScriptStuff(bBeforeFade);

        // restore SCM textures and return to the overwritten func (which may != DrawScriptSprites)
        return bBeforeFade ? DrawScriptStuff_H(bBeforeFade) : DrawScriptStuff(bBeforeFade);
    }

    void CScriptEngine::DrawScriptStuff(char bBeforeFade)
    {
        for (auto i = CustomScripts.begin(); i != CustomScripts.end(); ++i)
        {
            auto script = *i;
            script->Draw(bBeforeFade);
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

    CleoSafeHeader safe_header;
    ThreadSavingInfo *safe_info;
    unsigned long *stopped_info;
    std::unique_ptr<ThreadSavingInfo[]> safe_info_utilizer;
    std::unique_ptr<unsigned long[]> stopped_info_utilizer;
    std::vector<ChildThreadSavingInfo> pendingChildSaves;
    std::vector<bool> safeInfoUsed;

    void CScriptEngine::RestorePendingChildScript(CCustomScript *parent, CCustomScript *child, int label)
    {
        if (!parent || !child || parent->savedNodeId == 0)
            return;

        unsigned ordinal = 0;
        for (auto sibling : parent->childThreads)
        {
            if (sibling->childLabel != label)
                continue;

            if (sibling == child)
                break;

            ++ordinal;
        }

        for (auto it = pendingChildSaves.begin(); it != pendingChildSaves.end(); ++it)
        {
            if (it->parent_node_id != parent->savedNodeId ||
                it->label != label ||
                it->ordinal != ordinal)
            {
                continue;
            }

            const unsigned nodeId = it->node_id;
            it->Apply(child);
            child->savedNodeId = nodeId;

            TRACE("Restored custom child script '%s' from sidecar parent=%08X node=%u label=%d ordinal=%u",
                child->Name, parent->savedNodeId, nodeId, label, ordinal);

            pendingChildSaves.erase(it);
            return;
        }
    }

    void CScriptEngine::RestorePendingScmFunctions(CCustomScript *cs)
    {
        if (!cs || cs->savedNodeId == 0)
            return;

        std::vector<const ScmFunctionSaveInfo*> savedStates;
        for (const auto& saved : pendingScmFunctionSaves)
        {
            if (saved.node_id == cs->savedNodeId)
                savedStates.push_back(&saved);
        }

        if (savedStates.empty())
            return;

        std::sort(savedStates.begin(), savedStates.end(),
            [](const ScmFunctionSaveInfo* a, const ScmFunctionSaveInfo* b) {
                return a->depth < b->depth;
            });

        BYTE *base = cs->GetBasePointer();
        WORD prevId = 0;
        std::vector<DWORD> restoredStringOldPointers;
        std::vector<DWORD> restoredStringNewPointers;
        std::vector<ScmFunction*> restoredFunctions;

        for (const auto* saved : savedStates)
        {
            ScmFunction *fn = ScmFunction::CreateRestored();
            if (!fn)
                throw std::bad_alloc();

            fn->prevScmFunctionId = prevId;
            fn->callArgCount = saved->callArgCount;
            fn->callIP = RestorePointerOffset(base, saved->callOffset);
            fn->retnAddress = RestorePointerOffset(base, saved->retnOffset);
            fn->savedBaseIP = RestorePointerOffset(base, saved->savedBaseOffset);
            fn->savedCodeSize = saved->savedCodeSize;
            fn->savedSP = saved->savedSP;
            fn->savedCondResult = saved->savedCondResult;
            fn->savedLogicalOp = static_cast<eLogicalOperation>(saved->savedLogicalOp);
            fn->savedNotFlag = saved->savedNotFlag;
            fn->savedScriptFileDir = saved->savedScriptFileDir;
            fn->savedScriptFileName = saved->savedScriptFileName;

            for (size_t i = 0; i < 8; ++i)
                fn->savedStack[i] = RestorePointerOffset(base, saved->savedStackOffsets[i]);

            std::copy(saved->savedTls, saved->savedTls + 32, fn->savedTls);

            for (const auto& stringParam : saved->stringParams)
            {
                fn->stringParams.push_back(stringParam.value);
                auto& restored = fn->stringParams.back();
                restoredStringOldPointers.push_back(stringParam.oldPointer);
                restoredStringNewPointers.push_back(
                    static_cast<DWORD>(reinterpret_cast<uintptr_t>(restored.c_str()))
                );
            }

            restoredFunctions.push_back(fn);
            prevId = fn->thisScmFunctionId;
        }

        // Rebind saved/current string locals that previously pointed into
        // std::string storage owned by the pre-save ScmFunction objects.
        auto restoreStringPointer = [&](SCRIPT_VAR& var)
        {
            for (size_t i = 0; i < restoredStringOldPointers.size(); ++i)
            {
                if (var.dwParam == restoredStringOldPointers[i])
                {
                    var.dwParam = restoredStringNewPointers[i];
                    break;
                }
            }
        };

        // Rebind saved/current string locals that previously pointed into
        // std::string storage owned by the pre-save ScmFunction objects.
        for (auto* fn : restoredFunctions)
        {
            for (auto& var : fn->savedTls)
                restoreStringPointer(var);
        }

        SCRIPT_VAR *currentLocals = cs->IsMission() ? missionLocals : cs->LocalVar;
        for (size_t i = 0; i < 32; ++i)
            restoreStringPointer(currentLocals[i]);

        cs->SetScmFunction(prevId);

        pendingScmFunctionSaves.erase(
            std::remove_if(
                pendingScmFunctionSaves.begin(),
                pendingScmFunctionSaves.end(),
                [cs](const ScmFunctionSaveInfo& saved) {
                    return saved.node_id == cs->savedNodeId;
                }),
            pendingScmFunctionSaves.end()
        );

        TRACE("[CLEO][LOAD][ScmFunction] restored %u active scopes for '%.8s' node=%u",
            static_cast<unsigned>(restoredFunctions.size()),
            cs->GetName(),
            cs->savedNodeId);
    }

    void CScriptEngine::RestorePendingChildTree(CCustomScript *parent)
    {
        if (!parent || parent->savedNodeId == 0)
            return;

        for (;;)
        {
            size_t found = pendingChildSaves.size();

            // Restore children in ordinal order for each label. This keeps
            // duplicate labels (for example two 0E6F streams at the same
            // label) deterministic.
            for (size_t i = 0; i < pendingChildSaves.size(); ++i)
            {
                if (pendingChildSaves[i].parent_node_id != parent->savedNodeId)
                    continue;

                bool lowerOrdinalPending = false;
                for (size_t j = 0; j < pendingChildSaves.size(); ++j)
                {
                    if (pendingChildSaves[j].parent_node_id == parent->savedNodeId &&
                        pendingChildSaves[j].label == pendingChildSaves[i].label &&
                        pendingChildSaves[j].ordinal < pendingChildSaves[i].ordinal)
                    {
                        lowerOrdinalPending = true;
                        break;
                    }
                }

                if (!lowerOrdinalPending)
                {
                    found = i;
                    break;
                }
            }

            if (found == pendingChildSaves.size())
                break;

            ChildThreadSavingInfo saved = pendingChildSaves[found];
            pendingChildSaves.erase(pendingChildSaves.begin() + found);

            auto child = new CCustomScript(parent->Name, false, parent, saved.label);
            if (!child || !child->IsOK())
            {
                if (child)
                    delete child;

                DIAG("[CLEO][ERROR][CUSTOM] restore failed parent_node=%08X node=%u label=%d ordinal=%u",
                    parent->savedNodeId, saved.node_id, saved.label, saved.ordinal);
                continue;
            }

            AddCustomScript(child);
            saved.Apply(child);
            child->savedNodeId = saved.node_id;
            RestorePendingScmFunctions(child);

            RestorePendingChildTree(child);
        }
    }

    void CScriptEngine::LoadCustomScripts(bool load_mode)
    {
        char safe_name[MAX_PATH];

        // steam offset is different, so get it manually for now
        CGameVersionManager& gvm = GetInstance().VersionManager;
        int nSlot = gvm.GetGameVersion() != GV_STEAM ? *(BYTE*)&MenuManager->m_nSelectedSaveGame : *((BYTE*)MenuManager + 0x15B);

        sprintf(safe_name, "./cleo/cleo_saves/cs%d.sav", nSlot);

        safe_info = nullptr;
        stopped_info = nullptr;
        safe_header.n_saved_threads = safe_header.n_stopped_threads = 0;
        pendingChildSaves.clear();
        pendingScmFunctionSaves.clear();
        safeInfoUsed.clear();

        if (load_mode)
        {
            // load cleo saving file
            try
            {
                TRACE("Loading cleo safe %s", safe_name);
                std::ifstream ss(safe_name, std::ios::binary);
                if (ss.is_open())
                {
                    ss.exceptions(std::ios::eofbit | std::ios::badbit | std::ios::failbit);
                    ReadBinary(ss, safe_header);
                    if (safe_header.signature != CleoSafeHeader::sign)
                        throw std::runtime_error("Invalid file format");
                    safe_info = new ThreadSavingInfo[safe_header.n_saved_threads];
                    safe_info_utilizer.reset(safe_info);
                    stopped_info = new unsigned long[safe_header.n_stopped_threads];
                    stopped_info_utilizer.reset(stopped_info);
                    ReadBinary(ss, CleoVariables, 0x400);
                    ReadBinary(ss, safe_info, safe_header.n_saved_threads);
                    ReadBinary(ss, stopped_info, safe_header.n_stopped_threads);
                    safeInfoUsed.assign(safe_header.n_saved_threads, false);
                    for (size_t i = 0; i < safe_header.n_stopped_threads; ++i)
                        InactiveScriptHashes.insert(stopped_info[i]);
                    TRACE("Finished. Loaded %u cleo variables, %u saved threads info, %u stopped threads info",
                        0x400, safe_header.n_saved_threads, safe_header.n_stopped_threads);
                }
                else
                {
                    memset(CleoVariables, 0, sizeof(CleoVariables));
                }
            }
            catch (std::exception& ex)
            {
                TRACE("Loading of cleo safe %s failed: %s", safe_name, ex.what());
                safe_header.n_saved_threads = safe_header.n_stopped_threads = 0;
                memset(CleoVariables, 0, sizeof(CleoVariables));
            }
        }
        else
        {
            memset(CleoVariables, 0, sizeof(CleoVariables));
        }

        if (load_mode)
        {
            try
            {
                int nSlot = gvm.GetGameVersion() != GV_STEAM ? *(BYTE*)&MenuManager->m_nSelectedSaveGame : *((BYTE*)MenuManager + 0x15B);
                char child_safe_name[MAX_PATH];
                _snprintf_s(child_safe_name, sizeof(child_safe_name), _TRUNCATE,
                    "./cleo/cleo_saves/cs%d.children.sav", nSlot);

                std::ifstream cs(child_safe_name, std::ios::binary);
                if (cs.is_open())
                {
                    cs.exceptions(std::ios::eofbit | std::ios::badbit | std::ios::failbit);

                    ChildSaveHeader header{};
                    ReadBinary(cs, header);
                    if (header.signature != ChildSaveHeader::sign || header.version != ChildSaveHeader::format_version)
                        throw std::runtime_error("Invalid child save format");

                    pendingChildSaves.resize(header.n_children);
                    if (header.n_children)
                        ReadBinary(cs, pendingChildSaves.data(), header.n_children);

                    DIAG("[CLEO][LOAD][CUSTOM] loaded child states=%u file=%s", header.n_children, child_safe_name);
                }
            }
            catch (std::exception& ex)
            {
                pendingChildSaves.clear();
                DIAG("[CLEO][ERROR][LOAD] child state load failed: %s", ex.what());
            }
        }

        try
        {
            char function_safe_name[MAX_PATH];
            _snprintf_s(function_safe_name, sizeof(function_safe_name), _TRUNCATE,
                "./cleo/cleo_saves/cs%d.functions.sav", nSlot);

            std::ifstream fs(function_safe_name, std::ios::binary);
            if (fs.is_open())
            {
                fs.exceptions(std::ios::eofbit | std::ios::badbit | std::ios::failbit);

                ScmFunctionSaveHeader header{};
                ReadBinary(fs, header);
                if (header.signature != ScmFunctionSaveHeader::sign ||
                    header.version != ScmFunctionSaveHeader::format_version)
                    throw std::runtime_error("Invalid ScmFunction save format");

                pendingScmFunctionSaves.reserve(header.n_functions);

                for (unsigned i = 0; i < header.n_functions; ++i)
                {
                    ScmFunctionSaveInfo saved{};
                    uint32_t stringCount = 0;

                    ReadBinary(fs, saved.node_id);
                    ReadBinary(fs, saved.depth);
                    ReadBinary(fs, saved.callArgCount);
                    ReadBinary(fs, saved.callOffset);
                    ReadBinary(fs, saved.retnOffset);
                    ReadBinary(fs, saved.savedBaseOffset);
                    ReadBinary(fs, saved.savedCodeSize);
                    ReadBinary(fs, saved.savedStackOffsets, 8);
                    ReadBinary(fs, saved.savedSP);
                    ReadBinary(fs, saved.savedTls, 32);
                    ReadBinary(fs, saved.savedCondResult);
                    ReadBinary(fs, saved.savedLogicalOp);
                    ReadBinary(fs, saved.savedNotFlag);
                    if (!LoadStringBinary(fs, saved.savedScriptFileDir) ||
                        !LoadStringBinary(fs, saved.savedScriptFileName))
                        throw std::runtime_error("Invalid ScmFunction string state");

                    ReadBinary(fs, stringCount);
                    if (stringCount > 1024)
                        throw std::runtime_error("Invalid ScmFunction string count");

                    saved.stringParams.reserve(stringCount);
                    for (uint32_t s = 0; s < stringCount; ++s)
                    {
                        SavedScmStringParam param{};
                        ReadBinary(fs, param.oldPointer);
                        if (!LoadStringBinary(fs, param.value))
                            throw std::runtime_error("Invalid ScmFunction string parameter");
                        saved.stringParams.push_back(std::move(param));
                    }

                    pendingScmFunctionSaves.push_back(std::move(saved));
                }

                DIAG("[CLEO][LOAD][ScmFunction] loaded function states=%u file=%s",
                    header.n_functions, function_safe_name);
            }
        }
        catch (std::exception& ex)
        {
            pendingScmFunctionSaves.clear();
            DIAG("[CLEO][ERROR][LOAD] ScmFunction state load failed: %s", ex.what());
        }

        char cwd[MAX_PATH];
        _getcwd(cwd, sizeof(cwd));
        _chdir(cleo_dir);

        TRACE("Searching for cleo scripts");

        FilesWalk(cs_mask, [this](const char *filename) {
            auto cs = LoadScript(filename);
            if (cs)
                RestorePendingChildTree(cs);
        });
        FilesWalk(cs4_mask, [this](const char *filename) {
            auto cs = LoadScript(filename);
            if (cs)
            {
                cs->SetCompatibility(CLEO_VER_4);
                RestorePendingChildTree(cs);
            }
        });
        FilesWalk(cs3_mask, [this](const char *filename) {
            auto cs = LoadScript(filename);
            if (cs)
            {
                cs->SetCompatibility(CLEO_VER_3);
                RestorePendingChildTree(cs);
            }
        });

        _chdir(cwd);
    }

    CCustomScript * CScriptEngine::LoadScript(const char * szFilePath)
    {
        auto cs = new CCustomScript(szFilePath);

        if (!cs || !cs->bOK)
        {
            TRACE("Loading of custom script %s failed", szFilePath);
            if (cs) delete cs;
            return nullptr;
        }

        // check whether the script is in stop-list
        if (stopped_info)
        {
            for (size_t i = 0; i < safe_header.n_stopped_threads; ++i)
            {
                if (stopped_info[i] == cs->dwChecksum)
                {
                    TRACE("Custom script %s found in the stop-list", szFilePath);
                    InactiveScriptHashes.insert(stopped_info[i]);
                    delete cs;
                    return nullptr;
                }
            }
        }

        // check whether the script is in safe-list
        if (safe_info)
        {
            for (size_t i = 0; i < safe_header.n_saved_threads; ++i)
            {
                if (safeInfoUsed.size() > i && safeInfoUsed[i])
                    continue;

                if (safe_info[i].hash == cs->dwChecksum)
                {
                    TRACE("Custom script %s found in the safe-list", szFilePath);
                    safe_info[i].Apply(cs);

                    if (safeInfoUsed.size() > i)
                    {
                        safeInfoUsed[i] = true;
                        cs->savedNodeId = 0x80000000u | static_cast<unsigned>(i + 1);
                    }
                    break;
                }
            }
        }

        AddCustomScript(cs);
        RestorePendingScmFunctions(cs);
        return cs;
    }

    void CScriptEngine::SaveState()
    {
        try
        {
            std::list<CCustomScript *> savedThreads;
            std::for_each(CustomScripts.begin(), CustomScripts.end(), [this, &savedThreads](CCustomScript *cs) {
                if ((cs->bSaveEnabled || !cs->childThreads.empty()) && cs->parentThread == nullptr)
                    savedThreads.push_back(cs);
            });

            CleoSafeHeader header = { CleoSafeHeader::sign, savedThreads.size(), InactiveScriptHashes.size() };

            // Assign stable node ids for this save operation. Root scripts use
            // the legacy saved-thread index; child scripts use a separate id
            // space stored only in the sidecar file.
            unsigned rootIndex = 0;
            for (auto cs : savedThreads)
                cs->savedNodeId = 0x80000000u | (++rootIndex);

            std::vector<ChildThreadSavingInfo> childSaves;
            std::vector<ScmFunctionSaveInfo> functionSaves;
            unsigned nextChildNodeId = 1;

            auto collectChildren = [&](auto&& self, CCustomScript *parent, unsigned parentNodeId) -> void
            {
                for (auto child : parent->childThreads)
                {
                    unsigned ordinal = 0;
                    for (auto sibling : parent->childThreads)
                    {
                        if (sibling->childLabel != child->childLabel)
                            continue;
                        if (sibling == child)
                            break;
                        ++ordinal;
                    }

                    const unsigned nodeId = nextChildNodeId++;
                    child->savedNodeId = nodeId;
                    childSaves.emplace_back(child, parentNodeId, nodeId, ordinal);
                    CollectScmFunctionSaves(child, nodeId, functionSaves);
                    self(self, child, nodeId);
                }
            };

            for (auto root : savedThreads)
            {
                CollectScmFunctionSaves(root, root->savedNodeId, functionSaves);
                collectChildren(collectChildren, root, root->savedNodeId);
            }

            // steam offset is different, so get it manually for now
            CGameVersionManager& gvm = GetInstance().VersionManager;
            int nSlot = gvm.GetGameVersion() != GV_STEAM ? *(BYTE*)&MenuManager->m_nSelectedSaveGame : *((BYTE*)MenuManager + 0x15B);

            char safe_name[MAX_PATH];
            char child_safe_name[MAX_PATH];
            sprintf(safe_name, "./cleo/cleo_saves/cs%d.sav", nSlot);
            _snprintf_s(child_safe_name, sizeof(child_safe_name), _TRUNCATE,
                "./cleo/cleo_saves/cs%d.children.sav", nSlot);
            TRACE("Saving script engine state to the file %s", safe_name);

            CreateDirectory("cleo", NULL);
            CreateDirectory("cleo/cleo_saves", NULL);
            std::ofstream ss(safe_name, std::ios::binary);
            if (ss.is_open())
            {
                ss.exceptions(std::ios::failbit | std::ios::badbit);

                WriteBinary(ss, header);
                WriteBinary(ss, CleoVariables, 0x400);

                std::for_each(savedThreads.begin(), savedThreads.end(), [&savedThreads, &ss](CCustomScript *cs)
                {
                    ThreadSavingInfo savingInfo(cs);
                    WriteBinary(ss, savingInfo);
                });

                std::for_each(InactiveScriptHashes.begin(), InactiveScriptHashes.end(), [&ss](unsigned long hash) {
                    WriteBinary(ss, hash);
                });

                TRACE("Done. Saved %u cleo variables, %u saved threads, %u stopped threads",
                    0x400, header.n_saved_threads, header.n_stopped_threads);
            }
            else
            {
                TRACE("Failed to write save file '%s'!", safe_name);
            }

            try
            {
                std::ofstream childFile(child_safe_name, std::ios::binary);
                if (childFile.is_open())
                {
                    childFile.exceptions(std::ios::failbit | std::ios::badbit);

                    ChildSaveHeader childHeader = {
                        ChildSaveHeader::sign,
                        ChildSaveHeader::format_version,
                        childSaves.size()
                    };

                    WriteBinary(childFile, childHeader);
                    if (!childSaves.empty())
                        WriteBinary(childFile, childSaves.data(), childSaves.size());

                    DIAG("[CLEO][SAVE][CUSTOM] saved child states=%u file=%s",
                        childHeader.n_children, child_safe_name);
                }
                else
                {
                    DIAG("[CLEO][ERROR][SAVE] child state file write failed file=%s", child_safe_name);
                }
            }
            catch (std::exception& ex)
            {
                DIAG("[CLEO][ERROR][SAVE] child state save failed: %s", ex.what());
            }

            try
            {
                char function_safe_name[MAX_PATH];
                _snprintf_s(function_safe_name, sizeof(function_safe_name), _TRUNCATE,
                    "./cleo/cleo_saves/cs%d.functions.sav", nSlot);

                std::ofstream functionFile(function_safe_name, std::ios::binary);

                if (functionFile.is_open())
                {
                    functionFile.exceptions(std::ios::failbit | std::ios::badbit);

                    ScmFunctionSaveHeader functionHeader = {
                        ScmFunctionSaveHeader::sign,
                        ScmFunctionSaveHeader::format_version,
                        static_cast<unsigned>(functionSaves.size())
                    };

                    WriteBinary(functionFile, functionHeader);

                    for (const auto& saved : functionSaves)
                    {
                        WriteBinary(functionFile, saved.node_id);
                        WriteBinary(functionFile, saved.depth);
                        WriteBinary(functionFile, saved.callArgCount);
                        WriteBinary(functionFile, saved.callOffset);
                        WriteBinary(functionFile, saved.retnOffset);
                        WriteBinary(functionFile, saved.savedBaseOffset);
                        WriteBinary(functionFile, saved.savedCodeSize);
                        WriteBinary(functionFile, saved.savedStackOffsets, 8);
                        WriteBinary(functionFile, saved.savedSP);
                        WriteBinary(functionFile, saved.savedTls, 32);
                        WriteBinary(functionFile, saved.savedCondResult);
                        WriteBinary(functionFile, saved.savedLogicalOp);
                        WriteBinary(functionFile, saved.savedNotFlag);
                        SaveStringBinary(functionFile, saved.savedScriptFileDir);
                        SaveStringBinary(functionFile, saved.savedScriptFileName);

                        const uint32_t stringCount = static_cast<uint32_t>(saved.stringParams.size());
                        WriteBinary(functionFile, stringCount);
                        for (const auto& stringParam : saved.stringParams)
                        {
                            WriteBinary(functionFile, stringParam.oldPointer);
                            SaveStringBinary(functionFile, stringParam.value);
                        }
                    }

                    DIAG("[CLEO][SAVE][ScmFunction] saved function states=%u file=%s",
                        functionHeader.n_functions, function_safe_name);
                }
                else
                {
                    DIAG("[CLEO][ERROR][SAVE] ScmFunction state file write failed slot=%d", nSlot);
                }
            }
            catch (std::exception& ex)
            {
                DIAG("[CLEO][ERROR][SAVE] ScmFunction state save failed: %s", ex.what());
            }
        }
        catch (std::exception& ex)
        {
            TRACE("Saving failed. %s", ex.what());
        }
    }

    CRunningScript *CScriptEngine::FindScriptNamed(const char *name)
    {
        if (name == nullptr || activeThreadQueue == nullptr)
            return nullptr;

        // pActiveScripts remains the authoritative execution order:
        // native scripts first, CLEO scripts after them.
        for (auto script = *activeThreadQueue; script; script = script->GetNext())
        {
            if (_stricmp(name, script->GetName()) == 0)
                return script;
        }

        return nullptr;
    }

    CCustomScript *CScriptEngine::FindCustomScriptNamed(const char *name)
    {
        if (name == nullptr)
            return nullptr;

        if (CustomMission && _stricmp(name, CustomMission->Name) == 0)
            return CustomMission;

        for (auto cs : CustomScripts)
        {
            if (_stricmp(name, cs->Name) == 0)
                return cs;
        }

        return nullptr;
    }

    static void SkipUnusedScriptParameters(CRunningScript *thread)
    {
        if (thread == nullptr)
            return;

        while (*thread->GetBytePointer())
            GetScriptParams(thread, 1);

        thread->ReadDataByte();
    }

    CCustomScript *CScriptEngine::CreateCustomScript(CRunningScript *fromThread, const char *scriptName, int label)
    {
        if (scriptName == nullptr)
            return nullptr;

        CCustomScript *parent = fromThread ? reinterpret_cast<CCustomScript*>(fromThread) : nullptr;

        if (label != 0 && (parent == nullptr || !parent->IsCustom()))
        {
            TRACE("[engine] CreateCustomScript rejected: label child requires a custom parent");
            if (fromThread)
                SetScriptCondResult(fromThread, false);
            if (fromThread)
                SkipUnusedScriptParameters(fromThread);
            return nullptr;
        }

        char cwd[MAX_PATH];
        _getcwd(cwd, sizeof(cwd));
        _chdir(cleo_dir);

        CCustomScript *cs = new CCustomScript(scriptName, false, parent, label);

        if (fromThread)
            SetScriptCondResult(fromThread, cs != nullptr && cs->bOK);

        if (cs == nullptr || !cs->bOK)
        {
            if (cs)
                delete cs;

            if (fromThread)
                SkipUnusedScriptParameters(fromThread);

            TRACE("[engine] CreateCustomScript failed: %s", scriptName);
            _chdir(cwd);
            return nullptr;
        }

        AddCustomScript(cs);

        if (fromThread)
            TransmitScriptParams(fromThread, cs);

        _chdir(cwd);
        return cs;
    }

    bool CScriptEngine::IsActiveScriptPtr(const CRunningScript *script) const
    {
        if (script == nullptr || activeThreadQueue == nullptr)
            return false;

        for (auto current = *activeThreadQueue; current != nullptr; current = current->GetNext())
        {
            if (current == script)
                return current->IsActive();
        }

        return false;
    }

    bool CScriptEngine::IsValidScriptPtr(const CRunningScript *script) const
    {
        if (script == nullptr)
            return false;

        if (activeThreadQueue != nullptr)
        {
            for (auto current = *activeThreadQueue; current != nullptr; current = current->GetNext())
            {
                if (current == script)
                    return true;
            }
        }

        if (inactiveThreadQueue != nullptr)
        {
            for (auto current = *inactiveThreadQueue; current != nullptr; current = current->GetNext())
            {
                if (current == script)
                    return true;
            }
        }

        for (auto current : CustomScripts)
        {
            if (current == script)
                return true;
        }

        for (auto current : ScriptsWaitingForDelete)
        {
            if (current == script)
                return true;
        }

        return false;
    }

    void CScriptEngine::RemoveScript(CRunningScript *script)
    {
        if (script == nullptr)
            return;

        CCustomScript *custom = reinterpret_cast<CCustomScript*>(script);

        if (custom->IsCustom())
        {
            if (custom->IsMission())
                *MissionLoaded = false;

            RemoveCustomScript(custom);
            return;
        }

        // Native GTA script lifecycle stays unchanged: active -> inactive -> stop.
        if (activeThreadQueue != nullptr)
            RemoveScriptFromQueue(script, activeThreadQueue);
        if (inactiveThreadQueue != nullptr)
            AddScriptToQueue(script, inactiveThreadQueue);
        StopScript(script);
    }

    void CScriptEngine::AddCustomScript(CCustomScript *cs)
    {
        if (cs == nullptr || !cs->bOK)
            return;

        if (cs->IsMission())
        {
            TRACE("Registering custom mission named %.*s", 8, cs->Name);
            CustomMission = cs;
        }
        else
        {
            TRACE("Registering custom script named %.*s", 8, cs->Name);
            CustomScripts.push_back(cs);
        }

        // Registry -> GTA queue -> active state.
        AddScriptToQueue(cs, activeThreadQueue);
        cs->SetActive(true);
    }

    void CScriptEngine::RemoveCustomScript(CCustomScript *cs)
    {
        if (cs == nullptr)
            return;

        const bool wasChild = cs->parentThread != nullptr;

        // 1. Break the parent relation first.
        if (cs->parentThread != nullptr)
        {
            cs->parentThread->childThreads.remove(cs);
            cs->parentThread = nullptr;
        }

        // 2. Tear down the complete child subtree before this node.
        while (!cs->childThreads.empty())
        {
            CCustomScript *child = cs->childThreads.front();
            RemoveCustomScript(child);
        }

        // A saved child must not become an independently stopped root.
        if (cs != CustomMission && cs->bSaveEnabled && !wasChild)
        {
            InactiveScriptHashes.insert(cs->dwChecksum);
            TRACE("Stopping custom script named %.*s", 8, cs->Name);
        }

        // 3. Mark inactive first.
        cs->SetActive(false);

        // 4. Remove the script from GTA's active execution list.
        if (activeThreadQueue != nullptr)
            RemoveScriptFromQueue(cs, activeThreadQueue);

        // 5. Remove the script from CLEO's registry.
        if (cs == CustomMission)
        {
            TRACE("Unregistering custom mission named %.*s", 8, cs->Name);
            CustomMission = nullptr;
            *MissionLoaded = false;
        }
        else
        {
            TRACE("Unregistering custom script named %.*s", 8, cs->Name);
            CustomScripts.remove(cs);
        }

        // 6. Defer the actual delete until the current runtime boundary is safe.
        ScriptsWaitingForDelete.push_back(cs);
    }

    void CScriptEngine::RemoveAllCustomScripts(void)
    {
        TRACE("[engine] RemoveAllCustomScripts");

        InactiveScriptHashes.clear();

        if (CustomMission != nullptr)
            RemoveCustomScript(CustomMission);

        while (!CustomScripts.empty())
            RemoveCustomScript(CustomScripts.back());

        DeleteWaitingScripts();
    }

    void CScriptEngine::DeleteWaitingScripts()
    {
        // Destruction is deliberately separated from queue/registry removal.
        std::list<CCustomScript *> waiting;
        waiting.swap(ScriptsWaitingForDelete);

        for (auto cs : waiting)
        {
            TRACE("Deleting inactive script named %.*s", 8, cs->Name);
            delete cs;
        }
    }

    void CScriptEngine::UnregisterAllScripts()
    {
        TRACE("Unregistering all custom scripts");

        for (auto cs : CustomScripts)
        {
            if (activeThreadQueue != nullptr)
                RemoveScriptFromQueue(cs, activeThreadQueue);
            cs->SetActive(false);
        }

        if (CustomMission != nullptr)
        {
            if (activeThreadQueue != nullptr)
                RemoveScriptFromQueue(CustomMission, activeThreadQueue);
            CustomMission->SetActive(false);
        }
    }

    void CScriptEngine::ReregisterAllScripts()
    {
        TRACE("Reregistering all custom scripts");

        for (auto cs : CustomScripts)
        {
            AddScriptToQueue(cs, activeThreadQueue);
            cs->SetActive(true);
        }

        if (CustomMission != nullptr)
        {
            AddScriptToQueue(CustomMission, activeThreadQueue);
            CustomMission->SetActive(true);
        }
    }



	float VectorSqrMagnitude(CVector vector) { return vector.x * vector.x + vector.y * vector.y + vector.z * vector.z; }
}