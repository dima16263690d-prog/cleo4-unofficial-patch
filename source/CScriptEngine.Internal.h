#pragma once

// Shared symbols for split CScriptEngine translation units.
// Internal only — not part of the public CLEO API.

#include "CScriptEngine.h"
#include "CCustomScript.h"
#include <string>
#include <algorithm>
#include <cctype>

namespace CLEO
{
    inline std::string NormalizeCustomScriptName(const char* name)
    {
        std::string key = name ? name : "";
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return key;
    }

    extern DWORD FUNC_AddScriptToQueue;
    extern DWORD FUNC_RemoveScriptFromQueue;
    extern DWORD FUNC_StopScript;
    extern DWORD FUNC_ScriptOpcodeHandler00;
    extern DWORD FUNC_GetScriptParams;
    extern DWORD FUNC_TransmitScriptParams;
    extern DWORD FUNC_SetScriptParams;
    extern DWORD FUNC_SetScriptCondResult;
    extern DWORD FUNC_GetScriptParamPointer1;
    extern DWORD FUNC_GetScriptStringParam;
    extern DWORD FUNC_GetScriptParamPointer2;

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

    extern void(__cdecl * InitScm)();
    extern void(__cdecl * SaveScmData)();
    extern void(__cdecl * LoadScmData)();
    extern void(__cdecl * DrawScriptStuff)(char bBeforeFade);
    extern void(__cdecl * DrawScriptStuff_H)(char bBeforeFade);

    extern DWORD* GameTimer;
    extern BYTE *scmBlock;
    extern BYTE *MissionLoaded;
    extern BYTE *missionBlock;
    extern BOOL *onMissionFlag;
    extern CTexture *scriptSprites;
    extern BYTE *scriptDraws;
    extern WORD *numScriptDraws;
    extern WORD *numScriptTexts;
    extern BYTE *useTextCommands;
    extern BYTE *scriptTexts;

    extern CRunningScript **inactiveThreadQueue;
    extern CRunningScript **activeThreadQueue;
    extern CCustomScript *lastScriptCreated;

    void RunScriptDeleteDelegate(CRunningScript *script);
}
