#pragma once
#include "CTheScripts.h"
#include "CCleoScriptState.h"
#include "CScriptResources.h"
#include <list>
#include <string>

namespace CLEO
{
    class CScriptEngine;
    struct ScmFunction;
    struct ThreadSavingInfo;

    class CCustomScript : public CRunningScript
    {
        friend class CScriptEngine;
        friend struct ScmFunction;
        friend struct ThreadSavingInfo;

        // GTA CRunningScript remains the ABI/base state. CLEO-owned
        // state and resources are grouped without changing that base.
        CCleoScriptState cleoState;

        CCustomScript *parentThread;
        int childLabel;
        std::list<CCustomScript*> childThreads;

        CScriptResources resources;

    public:
		inline RwTexture* GetScriptTextureById(unsigned int id)
		{
			if (resources.script_textures.size() > id)
			{
				auto it = resources.script_textures.begin();
				std::advance(it, id);
				return *it;
			}
			return nullptr;
		}

        inline SCRIPT_VAR * GetVarsPtr() { return LocalVar; }
        inline WORD GetScmFunction() { return MemRead<WORD>(reinterpret_cast<BYTE*>(this) + 0xDD); }
        inline void SetScmFunction(WORD id) { MemWrite<WORD>(reinterpret_cast<BYTE*>(this) + 0xDD, id); }
        inline void SetNotFlag(bool b) { NotFlag = b; }
        inline char GetNotFlag() { return NotFlag; }
        inline void IsCustom(bool b) { MemWrite<BYTE>(reinterpret_cast<BYTE*>(this) + 0xDF, b); }
        inline bool IsCustom() { return MemRead<bool>(reinterpret_cast<BYTE*>(this) + 0xDF); }
        inline bool IsOK() { return cleoState.bOK; }
        inline void enable_saving(bool en = true) { cleoState.bSaveEnabled = en; }
        inline void SetCompatibility(CLEO_Version ver) { cleoState.CompatVer = ver; }
        inline CLEO_Version GetCompatibility() { return cleoState.CompatVer; }
        inline size_t GetCodeSize() const { return cleoState.CodeSize; }
        inline void SetCodeSize(size_t size) { cleoState.CodeSize = size; }
        inline const std::string& GetScriptFileDir() const { return cleoState.ScriptFileDir; }
        inline void SetScriptFileDir(const char *dir) { cleoState.ScriptFileDir = dir ? dir : ""; }
        inline const std::string& GetScriptFileName() const { return cleoState.ScriptFileName; }
        inline void SetScriptFileName(const char *name) { cleoState.ScriptFileName = name ? name : ""; }
        inline int GetChildLabel() { return childLabel; }
        inline DWORD& GetLastSearchPed() { return cleoState.LastSearchPed; }
        inline DWORD& GetLastSearchVehicle() { return cleoState.LastSearchCar; }
        inline DWORD& GetLastSearchObject() { return cleoState.LastSearchObj; }
		CCustomScript(const char *szFileName, bool bIsMiss = false, CCustomScript *parent = nullptr, int label = 0);
        ~CCustomScript();

        void Process();
        void Draw(char bBeforeFade);

        void StoreScriptSpecifics();
        void RestoreScriptSpecifics();
        void StoreScriptTextures();
        void RestoreScriptTextures();
        void StoreScriptDraws();
        void RestoreScriptDraws();

        void StoreScriptCustoms();
        void RestoreScriptCustoms();
    };
}
