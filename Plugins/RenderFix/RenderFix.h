#pragma once

#include "CLEO.h"
#include "CLEO_Debug.h"
#include "RenderFix.Patch.h"
#include "RenderFix.CLEO.h"

#include <windows.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <string>

// RenderFix applies optional frame-pacing and renderer tweaks for GTA San
// Andreas 1.0 US. It is the "fixer" counterpart to DebugUtils, which only
// observes. Every tweak is opt-in from RenderFix.ini so a change can be
// isolated and reverted without rebuilding.
//
// The address constants are all 1.0 US specific, so ApplyFixes() refuses to run
// on any other game version.
inline constexpr const char* kRenderFixBuildId = "renderfix-2";

class RenderFix
{
public:
    RenderFix();
    ~RenderFix();

private:
    static RenderFix* s_instance;

    // ---- configuration (RenderFix.ini) ----
    bool m_enabled = false;
    bool m_gtaEnabled = true;
    bool m_cleoEnabled = true;
    bool m_cleoRenderStateGuard = true;
    int m_frameLimit = 0;            // FPS cap; 0 = leave the engine value untouched
    bool m_removeFrameDelay = false; // drop the engine's ~14 ms per-frame sleep
    int m_streamingMemoryMb = 0;     // streaming budget; 0 = untouched, -1 = auto
    bool m_logEnabled = true;
    bool m_languageRussian = false;  // ini comment language: en (default) or ru

    // Auto FPS limit (ported from FramerateVigilante). When enabled, the cap is
    // lowered only while a cutscene / scripted scene / minigame / etc. runs.
    bool m_autoFpsEnabled = false;
    int m_minRefreshRate = 0;        // 0 = leave the engine's refresh rate alone
    RenderFixPlan::AutoFpsOptions m_autoFpsOptions;

    // Core Fixes (ported from SilentPatch SA). Each is an isolated byte edit.
    bool m_mouseReinitFix = false;
    bool m_refFix = false;
    bool m_doubleRwheelsFix = false;

    // FPS overlay drawn in the bottom-right corner of the screen.
    bool m_showFps = false;

    // ---- runtime state ----
    bool m_supportedVersion = false;
    bool m_pendingReapply = false;
    bool m_fpsOverlayInstalled = false;
    int m_autoFpsCurrent = -1;       // last cap written by the auto limiter; -1 = none

    // FPS overlay state. Frame counting is only updated from the draw wrapper,
    // which runs on the game thread, so no lock is needed.
    unsigned m_fpsFrameCount = 0;
    unsigned long long m_fpsWindowStartMs = 0;
    int m_fpsLastValue = 0;
    std::ofstream m_log;
    std::mutex m_logMutex;

    void LoadConfig();
    void ApplyFixes();
    void ReapplyFrameLimit();
    void UpdateAutoFpsLimit();

    void ApplyFrameLimit();
    void ApplyFrameDelayRemoval();
    void ApplyStreamingMemory();
    void ApplyMinRefreshRate();
    void ApplyCoreFixes();
    void ApplyFpsOverlay();

    void WriteLog(const char* format, ...);

    static bool PatchBytes(uintptr_t address, const void* data, size_t size);
    static bool NopBytes(uintptr_t address, size_t size);
    static uint8_t ReadByte(uintptr_t address);
    static bool PatchCall(uintptr_t callSite, uintptr_t expectedTarget,
                          uintptr_t newTarget);

    std::string ConfigPath() const;
    std::string LogPath() const;

    std::string BuildConfigText(bool ru,
                                const std::map<std::string, std::string>& raw) const;
    void WriteConfigFile(const std::string& path, bool ru,
                         const std::map<std::string, std::string>& raw) const;

    static void OnGameBegin();
    static void OnGameProcessBefore();

    // The CHud::Draw replacement: runs the original, then the overlay.
    static void __cdecl HudDrawWrapper();
};
