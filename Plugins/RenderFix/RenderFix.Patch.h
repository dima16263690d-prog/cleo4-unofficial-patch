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
}
