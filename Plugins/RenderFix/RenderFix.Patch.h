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

        // Per-activity caps, configurable from the ini. The constants above are
        // the FramerateVigilante defaults. Each value must stay in 1..255 to fit
        // the limiter byte; an out-of-range value is ignored when written.
        int cutsceneFps = kAutoFpsCutscene;
        int scriptedSceneFps = kAutoFpsScriptedScene;
        int pauseMenuFps = kAutoFpsPauseMenu;
        int minigameFps = kAutoFpsMinigame;
        int missionInteriorFps = kAutoFpsMissionInterior;
        int schoolFps = kAutoFpsSchool;
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
            return o.forCutscenes ? o.cutsceneFps : 0;
        if (s.wideScreenOn)
            return o.forScriptedScenes ? o.scriptedSceneFps : 0;
        if (s.pauseMenu)
            return o.forPauseMenu ? o.pauseMenuFps : 0;

        int cap = 0;
        for (int i = 0; i < s.scriptNameCount; ++i)
        {
            const char* n = s.scriptNames[i];
            if (o.forMinigames && (ScriptNameIs(n, "POOL2") || ScriptNameIs(n, "GFSEX")))
                cap = o.minigameFps;
            else if (o.forMissions && ScriptNameIs(n, "DRUGS1") && s.currArea != 0)
                cap = o.missionInteriorFps;
            else if (o.forSchools &&
                     (ScriptNameIs(n, "DSKOOL") || ScriptNameIs(n, "BOAT") || ScriptNameIs(n, "BSKOOL")))
                cap = o.schoolFps;
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

    // -----------------------------------------------------------------------
    // Core Fixes (ported from SilentPatch SA, MIT, (c) Adrian Zdanowicz).
    // Each is an isolated, long-standing byte edit. All OFF by default.
    // -----------------------------------------------------------------------

    // Stop the game re-initialising the DirectInput mouse device every frame.
    // SilentPatch turns each of these three calls into a short jump (0xEB).
    constexpr uintptr_t kMouseReinitPatch[3] = { 0x576CCC, 0x576EBA, 0x576F8A };
    constexpr int kMouseReinitPatchCount = 3;
    constexpr uint8_t kMouseReinitByte = 0xEB;

    // RefFix: the reflection renderer loads a scale of 0 and divides by it.
    // SilentPatch points the value at a 1.0f constant and clears a flag byte.
    constexpr uintptr_t kRefFixPtr = 0x6FB97A;  // -> &const float 1.0f
    constexpr uintptr_t kRefFixFlag = 0x6FB9A0; // -> 0
    constexpr float kRefFixValue = 1.0f;

    // DOUBLE_RWHEELS: fix the RW matrix flags for double rear wheels.
    // ~(rwMATRIXTYPEMASK | rwMATRIXINTERNALIDENTITY) = ~(0x3 | 0x20000).
    constexpr uintptr_t kDoubleRwheelsWord = 0x4C9290; // WORD 0xE281
    constexpr uintptr_t kDoubleRwheelsInt = 0x4C9292;  // int mask
    constexpr uint16_t kDoubleRwheelsOp = 0xE281;
    constexpr int kDoubleRwheelsMask = ~(0x00000003 | 0x00020000);

    // -----------------------------------------------------------------------
    // FPS overlay. The engine calls CHud::Draw (0x58FAE0) through a 5-byte CALL
    // at 0x53E4FF (this is plugin-sdk's drawHudEvent site). We repoint that call
    // at our wrapper, which runs the original and then prints the FPS. A
    // call-for-call swap needs no trampoline and is exactly reversible.
    // -----------------------------------------------------------------------
    constexpr uintptr_t kHudDrawCallSite = 0x53E4FF;
    constexpr uintptr_t kHudDrawOriginal = 0x58FAE0; // CHud::Draw
    constexpr uint8_t kCallOpcode = 0xE8;

    // CFont (GTA SA 1.0 US). Called through raw pointers so the plugin keeps
    // linking only cleo.lib, with no plugin-sdk/plugin.lib dependency.
    constexpr uintptr_t kFontPrintString = 0x71A700;     // PrintString(x, y, text)
    constexpr uintptr_t kFontSetScale = 0x719380;        // SetScale(w, h)
    constexpr uintptr_t kFontSetColor = 0x719430;        // SetColor(CRGBA)
    constexpr uintptr_t kFontSetDropColor = 0x719510;    // SetDropColor(CRGBA)
    constexpr uintptr_t kFontSetDropShadow = 0x719570;   // SetDropShadowPosition(short)
    constexpr uintptr_t kFontSetStyle = 0x719490;        // SetFontStyle(short)
    constexpr uintptr_t kFontSetProportional = 0x7195B0; // SetProportional(bool)
    constexpr uintptr_t kFontSetBackground = 0x7195C0;   // SetBackground(bool, bool)
    constexpr uintptr_t kFontSetJustify = 0x719600;      // SetJustify(bool)
    constexpr uintptr_t kFontSetOrientation = 0x719610;  // SetOrientation(eFontAlignment)
    constexpr uintptr_t kFontSetRightJustifyWrap = 0x7194F0; // SetRightJustifyWrap(float)

    // RsGlobal (0xC17040): appName, maximumWidth (+4), maximumHeight (+8), ...
    // Read directly, not through the SDK, to avoid a plugin.lib dependency.
    constexpr uintptr_t kRsGlobalBase = 0xC17040;

    // CFont enums (1.0 US). ALIGN_RIGHT right-justifies the string at x.
    constexpr int kFontAlignRight = 2;
    constexpr int kFontGothic = 0;

    // Packs a CRGBA into the 4 bytes the game pushes for a by-value colour, so
    // the colour can be passed through a plain 32-bit parameter (same ABI).
    inline uint32_t Rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
    {
        return static_cast<uint32_t>(r) |
               (static_cast<uint32_t>(g) << 8) |
               (static_cast<uint32_t>(b) << 16) |
               (static_cast<uint32_t>(a) << 24);
    }
}
