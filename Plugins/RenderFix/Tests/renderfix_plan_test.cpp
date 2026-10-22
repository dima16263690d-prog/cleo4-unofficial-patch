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

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
