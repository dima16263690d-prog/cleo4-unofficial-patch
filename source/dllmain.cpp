#include "stdafx.h"
#include "cleo.h"
#include "CDebugBridge.h"

#include <psapi.h>

#pragma comment(lib, "psapi.lib")

namespace
{
    // 0 = not started, 1 = running, 2 = done
    volatile LONG g_cleoInitState = 0;

    bool ModuleExportsUltimateASILoader(HMODULE module)
    {
        return module != nullptr &&
            GetProcAddress(module, "IsUltimateASILoader") != nullptr;
    }

    // Fast path: known Ultimate ASI Loader / proxy DLL names used with GTA SA.
    // Includes dinput8.dll (most common UAL name) and other.dll (custom proxy).
    bool IsKnownAsiLoaderProxyPresent()
    {
        static const wchar_t* const kProxyNames[] =
        {
            L"dinput8.dll",
            L"other.dll",
            L"vorbisFile.dll",
            L"vorbishooked.dll",
            L"dsound.dll",
            L"dinput.dll",
            L"d3d8.dll",
            L"d3d9.dll",
            L"d3d11.dll",
            L"ddraw.dll",
            L"winmm.dll",
            L"version.dll",
            L"wininet.dll",
            L"winhttp.dll",
            L"msimg32.dll",
            L"xlive.dll",
        };

        for (const wchar_t* name : kProxyNames)
        {
            if (ModuleExportsUltimateASILoader(GetModuleHandleW(name)))
                return true;
        }

        return false;
    }

    // Ultimate ASI Loader exports IsUltimateASILoader from its proxy module.
    // Prefer known proxy names (dinput8.dll, other.dll, ...), then scan all
    // loaded modules. Avoid stack walking inside DllMain.
    bool IsUltimateASILoaderPresent()
    {
        if (IsKnownAsiLoaderProxyPresent())
            return true;

        HMODULE modules[512];
        DWORD bytesNeeded = 0;

        if (!EnumProcessModules(
                GetCurrentProcess(),
                modules,
                sizeof(modules),
                &bytesNeeded))
        {
            return false;
        }

        const DWORD count = bytesNeeded / sizeof(HMODULE);
        for (DWORD i = 0; i < count; ++i)
        {
            if (ModuleExportsUltimateASILoader(modules[i]))
                return true;
        }

        return false;
    }

    void InitializeCleoOnce()
    {
        if (InterlockedCompareExchange(&g_cleoInitState, 1, 0) != 0)
            return;

        const auto gameVersion = CLEO::GetInstance().VersionManager.GetGameVersion();

        TRACE(
            "Started on game of version: %s",
            (gameVersion == CLEO::GV_US10) ? "SA 1.0 us" :
            (gameVersion == CLEO::GV_EU11) ? "SA 1.01 eu" :
            (gameVersion == CLEO::GV_EU10) ? "SA 1.0 eu" :
            (gameVersion == CLEO::GV_STEAM) ? "SA 3.0 steam" :
            "<!unknown!>"
        );

        if (gameVersion != CLEO::GV_US10 &&
            gameVersion != CLEO::GV_EU11 &&
            gameVersion != CLEO::GV_EU10 &&
            gameVersion != CLEO::GV_STEAM)
        {
            Error(
                "Unknown game version.\n"
                "The list of all supported executables:\n\n"
                "  1) gta_sa.exe, original 1.0 us, 14 405 632 bytes;\n"
                "  2) gta_sa.exe, public no-dvd 1.0 us, 14 383 616 bytes;\n"
                "  3) gta_sa_compact.exe, listener's executable, 5 189 632 bytes;\n"
                "  4) gta_sa.exe, original 1.01 eu, 14 405 632 bytes;\n"
                "  5) gta_sa.exe, public no-dvd 1.01 eu, 15 806 464 bytes;\n"
                "  6) gta_sa.exe, 1C localization, 15 806 464 bytes;\n"
                "  7) gta_sa.exe, original 1.0 eu, unknown size;\n"
                "  8) gta_sa.exe, public no-dvd 1.0eu, 14 386 176 bytes;\n"
                "  9) gta_sa.exe, original 3.0 steam executable, unknown size;\n"
                " 10) gta_sa.exe, decrypted 3.0 steam executable, 5 697 536 bytes."
            );
        }

        CLEO::GetInstance().Start();
        InterlockedExchange(&g_cleoInitState, 2);
    }
}

// Ultimate ASI Loader calls this after LoadLibrary (outside the Windows loader
// lock when DontLoadFromDllMain=1, which is its default).
// Must remain exported: see source/cleo.def (InitializeASI).
extern "C" __declspec(dllexport) void InitializeASI()
{
    InitializeCleoOnce();
}

extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        // Silent's ASI Loader / other classic loaders only call LoadLibrary and
        // never invoke InitializeASI. Init from DllMain for them.
        //
        // Ultimate ASI Loader (dinput8.dll, other.dll, vorbisFile.dll, ...):
        // skip heavy init here (loader lock) and let InitializeASI run after
        // LoadLibrary returns.
        if (!IsUltimateASILoaderPresent())
            InitializeCleoOnce();
    }

    // CCleoInstance's global destructor keeps the existing Stop() cleanup.
    return TRUE;
}
