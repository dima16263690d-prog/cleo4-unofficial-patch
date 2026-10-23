#include "RenderFix.h"
#include "RenderFix.Patch.h"

#include <cstdint>
#include <cstring>

using namespace RenderFixPlan;

namespace
{
    constexpr uintptr_t kStreamingMemoryAvailable = 0x8A5A80; // CStreaming::ms_memoryAvailable, GTA SA 1.0 US

    bool IsStreamingAddressAccessible()
    {
        static bool checked = false;
        static bool accessible = false;
        if (checked)
            return accessible;

        checked = true;
        MEMORY_BASIC_INFORMATION mbi{};
        const auto* address = reinterpret_cast<const void*>(kStreamingMemoryAvailable);
        if (VirtualQuery(address, &mbi, sizeof(mbi)) == 0 ||
            mbi.State != MEM_COMMIT ||
            (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0)
            return false;

        accessible = true;
        return true;
    }

    bool IsLargeAddressAwareExecutable()
    {
        const auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleA(nullptr));
        if (base == nullptr)
            return false;

        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 64 || dos->e_lfanew > 4096)
            return false;

        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
            return false;

        return (nt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) != 0;
    }
}

void RenderFix::UpdateAutoFpsLimit()
{
    if (!m_supportedVersion || !m_gtaEnabled || !m_autoFpsEnabled || !IsValidFrameLimit(m_frameLimit))
        return;

    AutoFpsState state;
    state.cutsceneRunning = ReadByte(kCutsceneRunningByte) != 0;
    state.wideScreenOn = ReadByte(kWideScreenByte) != 0;
    state.pauseMenu = ReadByte(kUserPauseByte) != 0;
    state.currArea = *reinterpret_cast<const int*>(kCurrAreaInt);

    // threadName is a fixed 8 bytes and not NUL-terminated, so copy each name
    // into a NUL-terminated buffer. Fixed storage: this runs every frame.
    char nameBuf[kAutoFpsMaxScripts][9] = {};
    const char* names[kAutoFpsMaxScripts] = {};
    int count = 0;
    const auto* script =
        *reinterpret_cast<const CScriptThread* const*>(kActiveScriptsPtr);
    for (; script != nullptr && count < kAutoFpsMaxScripts; script = script->next)
    {
        nameBuf[count][8] = '\0';
        memcpy(nameBuf[count], script->threadName, 8);
        names[count] = nameBuf[count];
        ++count;
    }

    state.scriptNames = names;
    state.scriptNameCount = count;

    const int overrideCap = DecideAutoFpsLimit(state, m_autoFpsOptions);

    const int desired = EffectiveFpsCap(m_frameLimit, overrideCap);

    if (desired == m_autoFpsCurrent)
        return;

    if (desired > 0 && desired <= kFrameLimitMax)
    {
        const uint8_t value = static_cast<uint8_t>(desired);
        if (PatchBytes(kFrameLimitByte, &value, sizeof(value)))
        {
            WriteLog(
                "[renderfix] auto FPS limit %d%s (0x%08X)",
                desired,
                overrideCap > 0 ? " [activity override]" : " [restored]",
                kFrameLimitByte
            );
            m_autoFpsCurrent = desired;
        }
    }
}


void RenderFix::ApplyFixes()
{
    if (!m_supportedVersion || !m_gtaEnabled)
        return;

    WriteLog(
        "[renderfix] applying frame_limit=%d remove_frame_delay=%d "
        "core_fixes=%d show_fps=%d",
        m_frameLimit,
        m_removeFrameDelay ? 1 : 0,
        (m_mouseReinitFix || m_refFix || m_doubleRwheelsFix) ? 1 : 0,
        m_showFps ? 1 : 0
    );

    ApplyFrameLimit();
    ApplyFrameDelayRemoval();
    ApplyMinRefreshRate();
    ApplyCoreFixes();
    ApplyFpsOverlay();

    ApplyStreamingMemory();

    // The engine re-initialises the limiter shortly after the scripts start, so
    // the value written at game begin can be overwritten. Re-apply it once on
    // the first processed frame (this is what SA-MixSets and FramerateVigilante
    // do as well).
    m_pendingReapply = IsValidFrameLimit(m_frameLimit);

    WriteLog("[renderfix] done");
}


void RenderFix::ApplyMinRefreshRate()
{
    if (m_minRefreshRate <= 0)
        return;

    if (!IsValidRefreshRate(m_minRefreshRate))
    {
        WriteLog(
            "[renderfix] MinRefreshRate=%d is out of range (%d..%d); ignored",
            m_minRefreshRate,
            kFrameLimitMin,
            kFrameLimitMax
        );
        return;
    }

    const uint8_t value = static_cast<uint8_t>(m_minRefreshRate);
    if (PatchBytes(kMinRefreshRateByte, &value, sizeof(value)))
        WriteLog("[renderfix] minimum refresh rate set to %d Hz (0x%08X)",
            m_minRefreshRate, kMinRefreshRateByte);
    else
        WriteLog("[renderfix] refresh-rate patch failed (0x%08X)", kMinRefreshRateByte);
}


void RenderFix::ReapplyFrameLimit()
{
    m_pendingReapply = false;

    const uint8_t value = static_cast<uint8_t>(m_frameLimit);
    if (PatchBytes(kFrameLimitByte, &value, sizeof(value)))
    {
        // Keep the auto limiter's cache in sync; otherwise it would treat the
        // value we just wrote as a change and write it again next frame.
        m_autoFpsCurrent = m_frameLimit;
        WriteLog("[renderfix] frame limit re-applied on first frame: %u FPS", m_frameLimit);
    }
    else
    {
        WriteLog("[renderfix] frame limit re-apply failed");
    }
}


void RenderFix::ApplyFrameLimit()
{
    if (m_frameLimit <= 0)
        return;

    // The limiter byte is unsigned; values above 255 cannot be represented.
    if (!IsValidFrameLimit(m_frameLimit))
    {
        WriteLog(
            "[renderfix] FrameLimit=%d is out of range (%d..%d); ignored",
            m_frameLimit,
            kFrameLimitMin,
            kFrameLimitMax
        );
        return;
    }

    const uint8_t previous = ReadByte(kFrameLimitByte);
    const uint8_t value = static_cast<uint8_t>(m_frameLimit);

    if (!PatchBytes(kFrameLimitByte, &value, sizeof(value)))
    {
        WriteLog("[renderfix] frame limit: write to 0x%08X failed", kFrameLimitByte);
        return;
    }

    WriteLog(
        "[renderfix] frame limit %u -> %u FPS (0x%08X)",
        static_cast<unsigned>(previous),
        static_cast<unsigned>(value),
        kFrameLimitByte
    );

    m_autoFpsCurrent = m_frameLimit;
}


void RenderFix::ApplyFrameDelayRemoval()
{
    if (!m_removeFrameDelay)
        return;

    const bool ok =
        PatchBytes(kFrameDelayPatch1, kFrameDelayJump, sizeof(kFrameDelayJump)) &&
        PatchBytes(kFrameDelayPatch2, &kFrameDelayArg, sizeof(kFrameDelayArg)) &&
        NopBytes(kFrameDelayNop, 1);

    if (ok)
        WriteLog("[renderfix] removed the ~14 ms per-frame delay");
    else
        WriteLog("[renderfix] frame-delay patch failed (0x%08X)", kFrameDelayPatch1);
}


void RenderFix::ApplyCoreFixes()
{
    if (m_mouseReinitFix)
    {
        bool ok = true;
        for (int i = 0; i < kMouseReinitPatchCount; ++i)
            ok = PatchBytes(kMouseReinitPatch[i], &kMouseReinitByte, 1) && ok;

        WriteLog("[renderfix] mouse re-init fix %s (0x%08X..0x%08X)",
            ok ? "applied" : "PARTIAL",
            kMouseReinitPatch[0], kMouseReinitPatch[kMouseReinitPatchCount - 1]);
    }

    if (m_refFix)
    {
        // Point the reflection scale pointer at a static 1.0f and clear the flag.
        const float* valuePtr = &kRefFixValue;
        const bool ok =
            PatchBytes(kRefFixPtr, &valuePtr, sizeof(valuePtr)) &&
            PatchBytes(kRefFixFlag, "\0", 1);

        WriteLog("[renderfix] RefFix %s", ok ? "applied" : "FAILED");
    }

    if (m_doubleRwheelsFix)
    {
        const bool ok =
            PatchBytes(kDoubleRwheelsWord, &kDoubleRwheelsOp, sizeof(kDoubleRwheelsOp)) &&
            PatchBytes(kDoubleRwheelsInt, &kDoubleRwheelsMask, sizeof(kDoubleRwheelsMask));

        WriteLog("[renderfix] DOUBLE_RWHEELS fix %s", ok ? "applied" : "FAILED");
    }
}


void RenderFix::ApplyFpsOverlay()
{
    if (!m_showFps)
        return;

    // Only install when the call still targets CHud::Draw. If another plugin has
    // already repointed it, we skip rather than clobber its hook; the overlay is
    // a convenience, not worth breaking another mod for.
    if (!PatchCall(
            kHudDrawCallSite,
            kHudDrawOriginal,
            reinterpret_cast<uintptr_t>(&RenderFix::HudDrawWrapper)))
    {
        WriteLog(
            "[renderfix] FPS overlay: CHud::Draw call at 0x%08X was already "
            "hooked by another plugin; overlay skipped",
            kHudDrawCallSite);
        return;
    }

    m_fpsOverlayInstalled = true;
    WriteLog("[renderfix] FPS overlay installed (CHud::Draw call 0x%08X)",
        kHudDrawCallSite);
}


void __cdecl RenderFix::HudDrawWrapper()
{
    // Run the game's own HUD first, unchanged.
    using HudDrawFn = void(__cdecl*)();
    reinterpret_cast<HudDrawFn>(kHudDrawOriginal)();

    RenderFix* self = s_instance;
    if (self == nullptr)
        return;

    // Measure over a ~500 ms window so the number is stable, not jittery.
    // GetTickCount (not GetTickCount64) keeps the XP-era API floor; unsigned
    // subtraction stays correct across its 49-day wrap.
    const DWORD now = GetTickCount();
    if (self->m_fpsWindowStartMs == 0)
    {
        self->m_fpsWindowStartMs = now;
        self->m_fpsFrameCount = 0;
    }

    ++self->m_fpsFrameCount;

    const DWORD elapsed = now - static_cast<DWORD>(self->m_fpsWindowStartMs);
    if (elapsed >= 500)
    {
        self->m_fpsLastValue = static_cast<int>(
            (self->m_fpsFrameCount * 1000ull) / (elapsed == 0 ? 1 : elapsed));
        self->m_fpsFrameCount = 0;
        self->m_fpsWindowStartMs = now;
    }

    if (self->m_fpsLastValue <= 0)
        return;

    // The engine only calls CHud::Draw during gameplay, so no extra state check
    // is needed here: menus and loading screens never reach this code.
    const int screenW = *reinterpret_cast<const int*>(kRsGlobalBase + 4);
    const int screenH = *reinterpret_cast<const int*>(kRsGlobalBase + 8);

    char text[16] = {};
    std::snprintf(text, sizeof(text), "%d", self->m_fpsLastValue);

    using SetScaleFn = void(__cdecl*)(float, float);
    using SetColorFn = void(__cdecl*)(uint32_t);
    using SetShadowFn = void(__cdecl*)(short);
    using SetStyleFn = void(__cdecl*)(short);
    using SetPropFn = void(__cdecl*)(bool);
    using SetBgFn = void(__cdecl*)(bool, bool);
    using SetJustifyFn = void(__cdecl*)(bool);
    using SetOrientFn = void(__cdecl*)(int);
    using SetWrapFn = void(__cdecl*)(float);
    using PrintFn = void(__cdecl*)(float, float, const char*);

    reinterpret_cast<SetStyleFn>(kFontSetStyle)(kFontGothic);
    reinterpret_cast<SetPropFn>(kFontSetProportional)(true);
    reinterpret_cast<SetJustifyFn>(kFontSetJustify)(false);
    reinterpret_cast<SetBgFn>(kFontSetBackground)(false, false);
    reinterpret_cast<SetOrientFn>(kFontSetOrientation)(kFontAlignRight);
    reinterpret_cast<SetWrapFn>(kFontSetRightJustifyWrap)(static_cast<float>(screenW));
    reinterpret_cast<SetScaleFn>(kFontSetScale)(0.55f, 1.1f);
    reinterpret_cast<SetShadowFn>(kFontSetDropShadow)(1);
    reinterpret_cast<SetColorFn>(kFontSetDropColor)(Rgba(0, 0, 0, 255));
    reinterpret_cast<SetColorFn>(kFontSetColor)(Rgba(255, 255, 255, 255));

    // Right-justified near the bottom-right corner, inside the safe area.
    const float x = static_cast<float>(screenW) - 12.0f;
    const float y = static_cast<float>(screenH) - 26.0f;
    reinterpret_cast<PrintFn>(kFontPrintString)(x, y, text);
}


void RenderFix::ApplyStreamingMemory()
{
    if (m_streamingMemoryMb == 0)
        return;

    if (!m_supportedVersion || !m_gtaEnabled)
        return;

    if (m_streamingMemoryMb < -1)
    {
        WriteLog("[renderfix] invalid Streaming.MemoryMB=%d; expected -1, 0 or a positive value",
            m_streamingMemoryMb);
        return;
    }

    const int effectiveMb = m_streamingMemoryMb == -1
        ? (IsLargeAddressAwareExecutable() ? 1024 : 512)
        : m_streamingMemoryMb;

    // The game stores this budget as bytes in a 32-bit field.
    const uint32_t desiredBytes = effectiveMb > 2047
        ? 0x7FFFFFFFu
        : static_cast<uint32_t>(effectiveMb) * 1024u * 1024u;

    if (!IsStreamingAddressAccessible())
    {
        WriteLog("[renderfix] CStreaming::ms_memoryAvailable is not safely accessible");
        return;
    }

    auto* value = reinterpret_cast<uint32_t*>(kStreamingMemoryAvailable);
    const uint32_t previousBytes = *value;
    if (desiredBytes <= previousBytes)
        return;


    DWORD oldProtect = 0;
    if (!VirtualProtect(value, sizeof(*value), PAGE_READWRITE, &oldProtect))
    {
        WriteLog("[renderfix] cannot change protection for CStreaming::ms_memoryAvailable");
        return;
    }

    *value = desiredBytes;

    DWORD ignored = 0;
    if (!VirtualProtect(value, sizeof(*value), oldProtect, &ignored))
        WriteLog("[renderfix] warning: could not restore streaming-budget page protection");

    WriteLog(
        "[renderfix] streaming budget raised from %u MB to %u MB (config=%d, bytes=0x%08X)",
        previousBytes / (1024u * 1024u),
        desiredBytes / (1024u * 1024u),
        m_streamingMemoryMb,
        desiredBytes
    );
}


