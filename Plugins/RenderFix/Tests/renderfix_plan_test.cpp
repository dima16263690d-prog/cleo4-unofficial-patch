// Host unit test for RenderFix.Patch.h.
//
// RenderFix writes raw bytes into gta_sa.exe. A wrong address or a swapped byte
// silently corrupts the game's code, so the patch plan is the one part worth a
// real test. It has no Windows dependency, so it builds and runs on the host:
//
//   g++ -std=c++17 -I Plugins/RenderFix Plugins/RenderFix/Tests/renderfix_plan_test.cpp -o /tmp/renderfix_plan_test
//   /tmp/renderfix_plan_test
//
// Exit code 0 = all checks passed, 1 = at least one failed.

#include "RenderFix.Patch.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

using namespace RenderFixPlan;

static int g_failures = 0;
static int g_checks = 0;

static void Check(const char* what, bool ok)
{
    ++g_checks;
    if (!ok)
    {
        ++g_failures;
        std::printf("FAIL %s\n", what);
    }
    else
    {
        std::printf("ok   %s\n", what);
    }
}

static void CheckEq(const char* what, long long got, long long want)
{
    ++g_checks;
    if (got != want)
    {
        ++g_failures;
        std::printf("FAIL %-44s got %lld, want %lld\n", what, got, want);
    }
    else
    {
        std::printf("ok   %-44s = %lld\n", what, got);
    }
}

// Reference: SilentPatch writes Patch<WORD>(0x53E923, 0x43EB). A WORD is stored
// little-endian, so the in-memory bytes must be EB 43.
static uint16_t AsLittleEndianWord(const uint8_t* bytes)
{
    return static_cast<uint16_t>(bytes[0] | (bytes[1] << 8));
}

int main()
{
    // ---- frame-delay jump encoding ----
    // The single most dangerous value: a wrong byte here corrupts the game's
    // frame loop. Assert it decodes to exactly the WORD SilentPatch patches.
    CheckEq("frame-delay jump bytes == SilentPatch WORD 0x43EB",
            AsLittleEndianWord(kFrameDelayJump), 0x43EB);
    CheckEq("frame-delay jump is a short jmp (opcode 0xEB)",
            kFrameDelayJump[0], 0xEB);
    CheckEq("frame-delay jump target offset", kFrameDelayJump[1], 0x43);
    CheckEq("frame-delay argument byte", kFrameDelayArg, 0x10);
    CheckEq("frame-delay NOP byte", kNop, 0x90);

    // ---- frame-limit byte validation ----
    CheckEq("frame limit 0 is invalid (means 'leave untouched')",
            IsValidFrameLimit(0), 0);
    CheckEq("frame limit 1 is valid", IsValidFrameLimit(1), 1);
    CheckEq("frame limit 60 is valid", IsValidFrameLimit(60), 1);
    CheckEq("frame limit 255 is valid", IsValidFrameLimit(255), 1);
    CheckEq("frame limit 256 is invalid (does not fit a byte)",
            IsValidFrameLimit(256), 0);
    CheckEq("negative frame limit is invalid", IsValidFrameLimit(-1), 0);

    // ---- addresses must be inside gta_sa.exe's image range ----
    // The executable is based at 0x400000; a value outside [0x400000, 0x1000000)
    // would be a typo, not a real code/data address.
    const auto inImage = [](uintptr_t a) {
        return a >= 0x400000u && a < 0x1000000u;
    };
    Check("frame-limit byte address is in the image", inImage(kFrameLimitByte));
    Check("frame-delay patch 1 address is in the image", inImage(kFrameDelayPatch1));
    Check("frame-delay patch 2 address is in the image", inImage(kFrameDelayPatch2));
    Check("frame-delay NOP address is in the image", inImage(kFrameDelayNop));

    // The three frame-delay edits are consecutive instructions in one routine;
    // they must stay close together or the addresses are wrong.
    Check("frame-delay edits are close together",
          kFrameDelayPatch2 > kFrameDelayPatch1 &&
          kFrameDelayPatch2 - kFrameDelayPatch1 < 0x100 &&
          kFrameDelayNop > kFrameDelayPatch2 &&
          kFrameDelayNop - kFrameDelayPatch2 < 0x100);

    // ---- game-state addresses (read-only, must be in the image) ----
    Check("cutscene ms_running address is in the image", inImage(kCutsceneRunningByte));
    Check("widescreen byte address is in the image", inImage(kWideScreenByte));
    Check("user-pause address is in the image", inImage(kUserPauseByte));
    Check("currArea address is in the image", inImage(kCurrAreaInt));
    Check("active-scripts pointer is in the image", inImage(kActiveScriptsPtr));
    Check("min-refresh-rate byte address is in the image", inImage(kMinRefreshRateByte));

    // The wide-screen byte must be exactly TheCamera (0xB6F028) + 0x3D; a wrong
    // offset would read unrelated camera state as "scripted scene".
    CheckEq("widescreen byte == TheCamera + 0x3D", kWideScreenByte, 0xB6F028 + 0x3D);

    // ---- refresh-rate validation ----
    CheckEq("refresh rate 0 is invalid (means 'untouched')", IsValidRefreshRate(0), 0);
    CheckEq("refresh rate 60 is valid", IsValidRefreshRate(60), 1);
    CheckEq("refresh rate 144 is valid", IsValidRefreshRate(144), 1);
    CheckEq("refresh rate 256 is invalid", IsValidRefreshRate(256), 0);

    // ---- Auto FPS limit decision (ported from FramerateVigilante) ----
    {
        AutoFpsOptions opt;

        AutoFpsState s;
        CheckEq("idle: no override", DecideAutoFpsLimit(s, opt), 0);

        s.cutsceneRunning = true;
        CheckEq("cutscene -> 60", DecideAutoFpsLimit(s, opt), 60);
        s.cutsceneRunning = false;

        s.wideScreenOn = true;
        CheckEq("scripted scene -> 80", DecideAutoFpsLimit(s, opt), 80);
        s.wideScreenOn = false;

        s.pauseMenu = true;
        CheckEq("pause menu -> 60", DecideAutoFpsLimit(s, opt), 60);
        s.pauseMenu = false;

        // Cutscene beats a scripted scene beats the pause menu (FV precedence).
        AutoFpsState both;
        both.cutsceneRunning = true;
        both.wideScreenOn = true;
        both.pauseMenu = true;
        CheckEq("cutscene beats scene and pause", DecideAutoFpsLimit(both, opt), 60);

        const char* pool[] = { "POOL2" };
        AutoFpsState mg;
        mg.scriptNames = pool;
        mg.scriptNameCount = 1;
        CheckEq("pool minigame -> 30", DecideAutoFpsLimit(mg, opt), 30);

        const char* gfsex[] = { "GFSEX" };
        mg.scriptNames = gfsex;
        CheckEq("hot coffee -> 30", DecideAutoFpsLimit(mg, opt), 30);

        const char* dskool[] = { "DSKOOL" };
        mg.scriptNames = dskool;
        CheckEq("driving school -> 80", DecideAutoFpsLimit(mg, opt), 80);

        const char* boat[] = { "BOAT" };
        mg.scriptNames = boat;
        CheckEq("boat school -> 80", DecideAutoFpsLimit(mg, opt), 80);

        // DRUGS1 only caps inside an interior (currArea != 0).
        const char* drugs[] = { "DRUGS1" };
        mg.scriptNames = drugs;
        mg.currArea = 0;
        CheckEq("DRUGS1 outside -> no override", DecideAutoFpsLimit(mg, opt), 0);
        mg.currArea = 1;
        CheckEq("DRUGS1 inside -> 50", DecideAutoFpsLimit(mg, opt), 50);

        // A name that merely starts with a known one must not match.
        const char* notPool[] = { "POOL2X" };
        mg.currArea = 0;
        mg.scriptNames = notPool;
        CheckEq("POOL2X does not match POOL2", DecideAutoFpsLimit(mg, opt), 0);

        // Options can disable individual overrides.
        AutoFpsOptions noCut = opt;
        noCut.forCutscenes = false;
        AutoFpsState cut;
        cut.cutsceneRunning = true;
        CheckEq("cutscene override disabled -> none", DecideAutoFpsLimit(cut, noCut), 0);

        AutoFpsOptions noMini = opt;
        noMini.forMinigames = false;
        mg.scriptNames = pool;
        CheckEq("minigame override disabled -> none", DecideAutoFpsLimit(mg, noMini), 0);
    }

    // ---- Effective cap: an override can only lower the base ----
    CheckEq("no base, no override -> 0", EffectiveFpsCap(0, 0), 0);
    CheckEq("base 60, no override -> 60", EffectiveFpsCap(60, 0), 60);
    CheckEq("no base, override 30 -> 30", EffectiveFpsCap(0, 30), 30);
    CheckEq("base 60, override 30 -> 30", EffectiveFpsCap(60, 30), 30);
    CheckEq("base 30, override 60 -> 30 (never raise)", EffectiveFpsCap(30, 60), 30);
    CheckEq("base 60, override 80 -> 60 (never raise)", EffectiveFpsCap(60, 80), 60);

    // ---- configurable auto-FPS caps ----
    {
        AutoFpsOptions opt;
        CheckEq("default cutscene cap is 60", opt.cutsceneFps, 60);
        CheckEq("default minigame cap is 30", opt.minigameFps, 30);

        opt.cutsceneFps = 45;
        AutoFpsState s;
        s.cutsceneRunning = true;
        CheckEq("custom cutscene cap is used", DecideAutoFpsLimit(s, opt), 45);

        opt.minigameFps = 24;
        const char* pool[] = { "POOL2" };
        AutoFpsState mg;
        mg.scriptNames = pool;
        mg.scriptNameCount = 1;
        CheckEq("custom minigame cap is used", DecideAutoFpsLimit(mg, opt), 24);
    }

    // ---- Core Fixes (SilentPatch ports) ----
    CheckEq("mouse re-init patch count", kMouseReinitPatchCount, 3);
    CheckEq("mouse re-init byte is a short jmp", kMouseReinitByte, 0xEB);
    Check("mouse re-init addresses are in the image",
          inImage(kMouseReinitPatch[0]) && inImage(kMouseReinitPatch[1]) &&
          inImage(kMouseReinitPatch[2]));
    Check("mouse re-init addresses are distinct",
          kMouseReinitPatch[0] != kMouseReinitPatch[1] &&
          kMouseReinitPatch[1] != kMouseReinitPatch[2]);

    Check("RefFix pointer address is in the image", inImage(kRefFixPtr));
    Check("RefFix flag address is in the image", inImage(kRefFixFlag));
    CheckEq("RefFix value is 1.0f", kRefFixValue == 1.0f, 1);

    // ~(rwMATRIXTYPEMASK | rwMATRIXINTERNALIDENTITY) == ~(0x3 | 0x20000).
    CheckEq("DOUBLE_RWHEELS mask matches the RW flags", kDoubleRwheelsMask,
            static_cast<int>(~(0x00000003u | 0x00020000u)));
    CheckEq("DOUBLE_RWHEELS opcode word", kDoubleRwheelsOp, 0xE281);
    Check("DOUBLE_RWHEELS addresses are in the image",
          inImage(kDoubleRwheelsWord) && inImage(kDoubleRwheelsInt));

    // ---- FPS overlay ----
    Check("CHud::Draw call site is in the image", inImage(kHudDrawCallSite));
    CheckEq("call opcode is 0xE8", kCallOpcode, 0xE8);
    Check("CHud::Draw original is in the image", inImage(kHudDrawOriginal));
    // The overlay repoints a call; the call site and its target must differ.
    Check("call site differs from CHud::Draw", kHudDrawCallSite != kHudDrawOriginal);

    // CFont entry points must all be in the image and distinct.
    Check("CFont PrintString address is in the image", inImage(kFontPrintString));
    Check("CFont SetScale address is in the image", inImage(kFontSetScale));
    Check("CFont SetColor address is in the image", inImage(kFontSetColor));
    Check("CFont SetDropShadow address is in the image", inImage(kFontSetDropShadow));
    Check("CFont PrintString differs from SetScale",
          kFontPrintString != kFontSetScale);
    Check("RsGlobal base is in the image", inImage(kRsGlobalBase));

    // RGBA packing must match the in-memory CRGBA layout (r,g,b,a little-endian).
    CheckEq("Rgba packs r in the low byte", Rgba(0x11, 0x22, 0x33, 0x44) & 0xFF, 0x11);
    CheckEq("Rgba packs g in byte 1", (Rgba(0x11, 0x22, 0x33, 0x44) >> 8) & 0xFF, 0x22);
    CheckEq("Rgba packs b in byte 2", (Rgba(0x11, 0x22, 0x33, 0x44) >> 16) & 0xFF, 0x33);
    CheckEq("Rgba packs a in byte 3", (Rgba(0x11, 0x22, 0x33, 0x44) >> 24) & 0xFF, 0x44);

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
