#pragma once
#include "CCodeInjector.h"
#include "CCustomOpcodeSystem.h"
#include "CCustomScript.h"

namespace CLEO
{
    const char cleo_dir[] = "./cleo";
    const char cs_mask[] = "./*.cs";
    const char cs4_mask[] = "./*.cs4";
    const char cs3_mask[] = "./*.cs3";

    class CScriptEngine : VInjectible
    {
        friend class CCustomScript;
        std::list<CCustomScript *> CustomScripts;
        std::list<CCustomScript *> ScriptsWaitingForDelete;
        std::set<unsigned long> InactiveScriptHashes;

        // Central runtime lifecycle state. This is the preparation layer for
        // the future scheduler/worker architecture; execution remains on the
        // GTA game thread in this stage.
        bool scriptsLoaded;
        CCustomScript *CustomMission;

        CCustomScript			*	LoadScript(const char *szFilePath);

    public:
        static SCRIPT_VAR			CleoVariables[0x400];

        // Script lifecycle, following the useful CLEO 5 engine separation.
        void							GameBegin(bool bLoadMode = false);
        void							GameEnd();

        inline CCustomScript		*	GetCustomMission() { return CustomMission; }
        void							LoadCustomScripts(bool bMode = false);
        void							SaveState();
        CRunningScript			*	FindScriptNamed(const char *);
        CCustomScript			*	FindCustomScriptNamed(const char*);
        CCustomScript			*	CreateCustomScript(CRunningScript *fromThread, const char *scriptName, int label);
        bool							IsActiveScriptPtr(const CRunningScript *script) const;
        bool							IsValidScriptPtr(const CRunningScript *script) const;
        void							AddCustomScript(CCustomScript*);
        void							RemoveScript(CRunningScript*);
        void							RemoveCustomScript(CCustomScript*);
        void							RemoveAllCustomScripts();
        void							UnregisterAllScripts();
        void							ReregisterAllScripts();
        void							RestorePendingChildScript(CCustomScript *parent, CCustomScript *child, int label);
        void							RestorePendingScmFunctions(CCustomScript *script);
        void							RestorePendingChildTree(CCustomScript *parent);
        inline size_t				WorkingScriptsCount() { return CustomScripts.size(); }
        virtual void					Inject(CCodeInjector&);

        CScriptEngine()
        {
            scriptsLoaded = false;
            CustomMission = nullptr;
        }

        ~CScriptEngine()
        {
            TRACE("Unloading scripts...");
            RemoveAllCustomScripts();
        }

        void DrawScriptStuff(char bBeforeFade);

      private:
        void DeleteWaitingScripts();
    };

    extern void(__thiscall * AddScriptToQueue)(CRunningScript *, CRunningScript **queue);
    extern void(__thiscall * RemoveScriptFromQueue)(CRunningScript *, CRunningScript **queue);
    extern void(__thiscall * StopScript)(CRunningScript *);
    extern char(__thiscall * ScriptOpcodeHandler00)(CRunningScript *, WORD opcode);
    extern void(__thiscall * GetScriptParams)(CRunningScript *, int count);
    extern void(__thiscall * TransmitScriptParams)(CRunningScript *, CRunningScript *);
    extern void(__thiscall * SetScriptParams)(CRunningScript *, int count);
    extern void(__thiscall * SetScriptCondResult)(CRunningScript *, bool);
    extern SCRIPT_VAR * (__thiscall * GetScriptParamPointer1)(CRunningScript *);
    extern void(__thiscall * GetScriptStringParam)(CRunningScript *, char* buf, BYTE len);
    extern SCRIPT_VAR * (__thiscall * GetScriptParamPointer2)(CRunningScript *, int __unused__);

    inline SCRIPT_VAR * GetScriptParamPointer(CRunningScript *thread)
    {
        SCRIPT_VAR* ptr = GetScriptParamPointer2(thread, 0);
        return ptr;
    }

    extern "C" {
        extern SCRIPT_VAR *opcodeParams;
        extern SCRIPT_VAR *missionLocals;
        extern CRunningScript *staticThreads;
    }

    extern BYTE *scmBlock, *missionBlock;
    extern CCustomScript *lastScriptCreated;

	extern float VectorSqrMagnitude(CVector vector);
}

