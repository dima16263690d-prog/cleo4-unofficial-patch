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
#include "crc32.h"
#include "CCleoMemoryManager.h"

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
            // CLEO 4 runtime directories. Keep resource ownership simple:
            // .cs/.cs3/.cs4 -> ScriptEngine
            // .cleo          -> PluginSystem
            // cleo_saves     -> save sidecar data
            // cleo_text      -> text subsystem
            //
            // No cleo_modules directory: this patch intentionally keeps the
            // classic CLEO 4 script model without a separate .s module layer.
            CreateDirectoryA("cleo", nullptr);
            CreateDirectoryA("cleo\\cleo_plugins", nullptr);
            CreateDirectoryA("cleo\\cleo_saves", nullptr);
            CreateDirectoryA("cleo\\cleo_text", nullptr);

            ConfigureProcessMemory();

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

            // Plugins are loaded only after the CLEO core is injected.
            // CPluginSystem performs CLEO 5-style discovery and load ordering.
            PluginSystem.LoadPlugins();

            m_bStarted = true;
        }

        void Stop()
        {
            if (!m_bStarted) return;

            PluginSystem.UnloadPlugins();
            ScriptEngine.GameEnd();
            m_bStarted = false;
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
    };

    CCleoInstance& GetInstance();
}

#endif
