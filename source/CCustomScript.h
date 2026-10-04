#pragma once
#include "CTheScripts.h"
#include <list>
#include <string>
#include <vector>

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

        DWORD dwChecksum;
        BYTE *ownedBuffer;
        bool bSaveEnabled;
        bool bOK;
        DWORD LastSearchPed, LastSearchCar, LastSearchObj;
        CLEO_Version CompatVer;
        size_t CodeSize;
        std::string ScriptFileDir;
        std::string ScriptFileName;
        CCustomScript *parentThread;
        int childLabel;
        DWORD savedNodeId;
        BYTE UseTextCommands;
        int NumDraws;
        int NumTexts;
		std::list<CCustomScript*> childThreads;
        std::list<RwTexture*> script_textures;
        std::vector<BYTE> script_draws;
        std::vector<BYTE> script_texts;

    public:
		inline RwTexture* GetScriptTextureById(unsigned int id)
		{
			if (script_textures.size() > id)
			{
				auto it = script_textures.begin();
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
        inline bool IsOK() { return bOK; }
        inline void enable_saving(bool en = true) { bSaveEnabled = en; }
        inline void SetCompatibility(CLEO_Version ver) { CompatVer = ver; }
        inline CLEO_Version GetCompatibility() { return CompatVer; }
        inline size_t GetCodeSize() const { return CodeSize; }
        inline void SetCodeSize(size_t size) { CodeSize = size; }
        inline const std::string& GetScriptFileDir() const { return ScriptFileDir; }
        inline void SetScriptFileDir(const char *dir) { ScriptFileDir = dir ? dir : ""; }
        inline const std::string& GetScriptFileName() const { return ScriptFileName; }
        inline void SetScriptFileName(const char *name) { ScriptFileName = name ? name : ""; }
        inline int GetChildLabel() { return childLabel; }
        inline DWORD& GetLastSearchPed() { return LastSearchPed; }
        inline DWORD& GetLastSearchVehicle() { return LastSearchCar; }
        inline DWORD& GetLastSearchObject() { return LastSearchObj; }
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
