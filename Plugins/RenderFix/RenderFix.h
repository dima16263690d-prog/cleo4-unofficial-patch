#pragma once

#include "CLEO.h"
#include "CLEO_Debug.h"
#include "RenderFix.Patch.h"

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
inline constexpr const char* kRenderFixBuildId = "renderfix-1";

class RenderFix
{
public:
    RenderFix();
    ~RenderFix();

private:
    static RenderFix* s_instance;

    // ---- configuration (RenderFix.ini) ----
    bool m_enabled = false;
    int m_frameLimit = 0;            // FPS cap; 0 = leave the engine value untouched
    bool m_removeFrameDelay = false; // drop the engine's ~14 ms per-frame sleep
    int m_streamingMemoryMb = 0;     // streaming budget; 0 = untouched, -1 = auto
    bool m_logEnabled = true;

    // Auto FPS limit (ported from FramerateVigilante). When enabled, the cap is
    // lowered only while a cutscene / scripted scene / minigame / etc. runs.
    bool m_autoFpsEnabled = false;
    int m_minRefreshRate = 0;        // 0 = leave the engine's refresh rate alone
    RenderFixPlan::AutoFpsOptions m_autoFpsOptions;

    // ---- runtime state ----
    bool m_supportedVersion = false;
    bool m_pendingReapply = false;
    int m_autoFpsCurrent = -1;       // last cap written by the auto limiter; -1 = none
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

    void WriteLog(const char* format, ...);

    static bool PatchBytes(uintptr_t address, const void* data, size_t size);
    static bool NopBytes(uintptr_t address, size_t size);
    static uint8_t ReadByte(uintptr_t address);

    std::string ConfigPath() const;
    std::string LogPath() const;

    std::string BuildConfigText(const std::map<std::string, std::string>& raw) const;
    void WriteConfigFile(const std::string& path,
                         const std::map<std::string, std::string>& raw) const;

    static void OnGameBegin();
    static void OnGameProcessBefore();
};
