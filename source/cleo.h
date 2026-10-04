#ifndef __CLEO_H
#define __CLEO_H

#include "CCodeInjector.h"
#include "CHookSystem.h"
#include "CGameVersionManager.h"
#include "CDebugBridge.h"
#include "CDmaFix.h"
#include "CGameMenu.h"
#include "CPluginSystem.h"
#include "CScriptEngine.h"
#include "CCustomOpcodeSystem.h"
#include "CTextManager.h"
#include "CSoundSystem.h"
#include "FileEnumerator.h"
#include "crc32.h"

namespace CLEO
{
    class CCleoInstance
    {
        bool            m_bStarted;

    public:
        CCleoInstance()
        {
            m_bStarted = false;
        }

        virtual ~CCleoInstance()
        {
            Stop();
        }

        void(__cdecl * UpdateGameLogics)();
        static void __cdecl OnUpdateGameLogics();

        void Start()
        {
            // CLEO runtime directories. Keep resource ownership separated:
            // .cs/.cs3/.cs4 -> ScriptEngine
            // .cleo          -> PluginSystem
            // cleo_modules   -> module resources
            // cleo_saves     -> save sidecar data
            CreateDirectoryA("cleo", nullptr);
            CreateDirectoryA("cleo\\cleo_modules", nullptr);
            CreateDirectoryA("cleo\\cleo_plugins", nullptr);
            CreateDirectoryA("cleo\\cleo_saves", nullptr);

            // Existing CLEO 4 text subsystem still owns cleo_text.
            CreateDirectoryA("cleo\\cleo_text", nullptr);

            CodeInjector.OpenReadWriteAccess(); // must do this earlier to ensure plugins write access on init
            GameMenu.Inject(CodeInjector);
            DmaFix.Inject(CodeInjector);
            UpdateGameLogics = VersionManager.TranslateMemoryAddress(MA_UPDATE_GAME_LOGICS_FUNCTION);
            HookSystem.InstallCall(
                CodeInjector,
                "UpdateGameLogics",
                VersionManager.TranslateMemoryAddress(MA_CALL_UPDATE_GAME_LOGICS),
                (size_t)&OnUpdateGameLogics
            );
            TextManager.Inject(CodeInjector);
            SoundSystem.Inject(CodeInjector);
            OpcodeSystem.Inject(CodeInjector);
            ScriptEngine.Inject(CodeInjector);

            // Load plugins only after the core CLEO subsystems have been
            // injected. This keeps plugin loading separate from script
            // execution while preserving legacy CLEO 4 plugin placement.
            PluginSystem.LoadPlugins();

            m_bStarted = true;
        }

        void Stop()
        {
            if (!m_bStarted) return;
        }

        CDmaFix                    DmaFix;
        CGameMenu                 GameMenu;
        CHookSystem               HookSystem;
        CCodeInjector             CodeInjector;
        CGameVersionManager       VersionManager;
        CScriptEngine              ScriptEngine;
        CTextManager              TextManager;
        CCustomOpcodeSystem        OpcodeSystem;
        CSoundSystem               SoundSystem;
        CPluginSystem              PluginSystem;
        //CLegacy                   Legacy;
    };

    CCleoInstance& GetInstance();
}

#endif
