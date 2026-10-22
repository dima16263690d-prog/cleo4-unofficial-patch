#pragma once

#include <cstdint>
#include <cstddef>

// The exact byte patches RenderFix applies to GTA SA 1.0 US.
//
// This is the single source of truth for both the plugin and its host test.
// Every value is copied from a long-standing, well-tested mod:
//   - frame-limit byte 0xC1704C            : SA-MixSets, FramerateVigilante
//   - frame-delay removal 0x53E923/9F/A5   : SilentPatch "No framedelay"
// The host test (Tests/renderfix_plan_test.cpp) asserts these values, because a
// wrong address or byte silently corrupts the game's code.
namespace RenderFixPlan
{
    // GTA SA 1.0 US. RenderFix refuses to patch any other game version.
    constexpr int kGtaSa10Us = 0; // CLEO GV_US10

    // Engine frame limiter: one byte the game compares its target frame period
    // against. Writing N caps the frame rate at N FPS.
    constexpr uintptr_t kFrameLimitByte = 0xC1704C;

    // Frame-delay removal. The engine sleeps ~14 ms per frame here; these three
    // edits remove that sleep (SilentPatch "No framedelay").
    constexpr uintptr_t kFrameDelayPatch1 = 0x53E923; // 2 bytes -> EB 43 (jmp)
    constexpr uintptr_t kFrameDelayPatch2 = 0x53E99F; // 1 byte  -> 0x10
    constexpr uintptr_t kFrameDelayNop = 0x53E9A5;    // 1 byte  -> 0x90 (NOP)

    constexpr uint8_t kFrameDelayJump[2] = { 0xEB, 0x43 };
    constexpr uint8_t kFrameDelayArg = 0x10;
    constexpr uint8_t kNop = 0x90;

    // The limiter byte is unsigned, so only 1..255 is representable.
    constexpr int kFrameLimitMin = 1;
    constexpr int kFrameLimitMax = 255;

    constexpr bool IsValidFrameLimit(int fps)
    {
        return fps >= kFrameLimitMin && fps <= kFrameLimitMax;
    }

    // Minimum refresh rate (FramerateVigilante "RefreshRate"). The two-byte
    // instruction at 0x74612A loads a constant; +2 is its immediate operand.
    constexpr uintptr_t kMinRefreshRateByte = 0x74612C;
    constexpr bool IsValidRefreshRate(int hz)
    {
        return hz >= 1 && hz <= 255;
    }

    // ---- game-state addresses (GTA SA 1.0 US, from plugin-sdk) ----
    // Read-only; RenderFix never writes these. Used by the auto-FPS limiter.
    constexpr uintptr_t kCutsceneRunningByte = 0xB5F851; // CCutsceneMgr::ms_running
    constexpr uintptr_t kWideScreenByte = 0xB6F065;      // TheCamera.m_bWideScreenOn (0xB6F028 + 0x3D)
    constexpr uintptr_t kUserPauseByte = 0xB7CB49;       // CTimer::m_UserPause
    constexpr uintptr_t kCurrAreaInt = 0xB72914;         // CGame::currArea
    constexpr uintptr_t kActiveScriptsPtr = 0xA8B42C;    // CTheScripts::pActiveScripts

    // ---- Auto FPS limit (ported from FramerateVigilante's AutoLimitFPS) ----
    // The engine was tuned for 30 FPS; a handful of activities break (physics,
    // scripted timing, minigame logic) at a high cap. FramerateVigilante drops
    // the cap only while one of those is running. The limits below are the exact
    // values from that mod's source.
    constexpr int kAutoFpsCutscene = 60;
    constexpr int kAutoFpsScriptedScene = 80;
    constexpr int kAutoFpsPauseMenu = 60;
    constexpr int kAutoFpsMinigame = 30;
    constexpr int kAutoFpsMissionInterior = 50;
    constexpr int kAutoFpsSchool = 80;

    // Upper bound on threads scanned per frame (GTA SA allows 96 scripts; the
    // active queue is far shorter). Bounds the per-frame loop.
    constexpr int kAutoFpsMaxScripts = 96;

    struct AutoFpsOptions
    {
        bool forCutscenes = true;
        bool forScriptedScenes = true;
        bool forPauseMenu = true;
        bool forMinigames = true;
        bool forMissions = true;
        bool forSchools = true;
    };

    struct AutoFpsState
    {
        bool cutsceneRunning = false;
        bool wideScreenOn = false;   // scripted scene (borders on)
        bool pauseMenu = false;
        int  currArea = 0;           // 0 = outside/interior marker used by DRUGS1
        const char* const* scriptNames = nullptr;
        int  scriptNameCount = 0;
    };

    // Case-sensitive, NUL-terminated compare that never reads past `want`.
    inline bool ScriptNameIs(const char* name, const char* want)
    {
        if (name == nullptr)
            return false;
        int i = 0;
        for (; want[i] != '\0'; ++i)
            if (name[i] != want[i])
                return false;
        return name[i] == '\0';
    }

    // Returns the FPS cap to force right now, or 0 for "no override". Faithful to
    // FramerateVigilante: a cutscene beats a scripted scene beats the pause menu;
    // otherwise the active script list is scanned and the last match wins.
    inline int DecideAutoFpsLimit(const AutoFpsState& s, const AutoFpsOptions& o)
    {
        if (s.cutsceneRunning)
            return o.forCutscenes ? kAutoFpsCutscene : 0;
        if (s.wideScreenOn)
            return o.forScriptedScenes ? kAutoFpsScriptedScene : 0;
        if (s.pauseMenu)
            return o.forPauseMenu ? kAutoFpsPauseMenu : 0;

        int cap = 0;
        for (int i = 0; i < s.scriptNameCount; ++i)
        {
            const char* n = s.scriptNames[i];
            if (o.forMinigames && (ScriptNameIs(n, "POOL2") || ScriptNameIs(n, "GFSEX")))
                cap = kAutoFpsMinigame;
            else if (o.forMissions && ScriptNameIs(n, "DRUGS1") && s.currArea != 0)
                cap = kAutoFpsMissionInterior;
            else if (o.forSchools &&
                     (ScriptNameIs(n, "DSKOOL") || ScriptNameIs(n, "BOAT") || ScriptNameIs(n, "BSKOOL")))
                cap = kAutoFpsSchool;
        }
        return cap;
    }

    // The limiter byte to write, given the base (steady-state) cap and the current
    // override. Never raises above the base: an override can only lower it, and an
    // override with no base cap is the override itself.
    inline int EffectiveFpsCap(int base, int overrideCap)
    {
        if (overrideCap <= 0)
            return base;
        if (base <= 0)
            return overrideCap;
        return base < overrideCap ? base : overrideCap;
    }
}
