#include "RenderFix.CLEO.h"

#include "rwplcore.h"

#include <cstddef>
#include <cstdint>

namespace
{
    // RenderWare globals for GTA San Andreas 1.0 US. The module is enabled only
    // after CLEO confirms the executable version, avoiding global init reads.
    constexpr uintptr_t kRwEngineInstanceAddress = 0xC97B24;

    constexpr RwRenderState kTrackedStates[] =
    {
        rwRENDERSTATETEXTURERASTER,
        rwRENDERSTATETEXTUREADDRESS,
        rwRENDERSTATETEXTUREPERSPECTIVE,
        rwRENDERSTATEZTESTENABLE,
        rwRENDERSTATESHADEMODE,
        rwRENDERSTATEZWRITEENABLE,
        rwRENDERSTATETEXTUREFILTER,
        rwRENDERSTATESRCBLEND,
        rwRENDERSTATEDESTBLEND,
        rwRENDERSTATEVERTEXALPHAENABLE,
        rwRENDERSTATEFOGENABLE,
        rwRENDERSTATEALPHATESTFUNCTION
    };

    constexpr size_t kTrackedStateCount = sizeof(kTrackedStates) / sizeof(kTrackedStates[0]);

    struct RenderStateSnapshot
    {
        RwRenderState state = rwRENDERSTATENARENDERSTATE;
        void* value = nullptr;
        bool valid = false;
    };

    RenderStateSnapshot g_snapshot[kTrackedStateCount]{};
    bool g_enabled = false;
    bool g_stateGuardEnabled = false;
    bool g_hasFrameSnapshot = false;

    RwGlobals* GetRwGlobals()
    {
        auto** slot = reinterpret_cast<RwGlobals**>(kRwEngineInstanceAddress);
        return slot != nullptr ? *slot : nullptr;
    }

    bool CaptureStates()
    {
        RwGlobals* globals = GetRwGlobals();
        if (globals == nullptr ||
            globals->dOpenDevice.fpRenderStateGet == nullptr ||
            globals->dOpenDevice.fpRenderStateSet == nullptr)
            return false;

        bool anyCaptured = false;
        for (size_t i = 0; i < kTrackedStateCount; ++i)
        {
            g_snapshot[i].state = kTrackedStates[i];
            g_snapshot[i].value = nullptr;
            g_snapshot[i].valid =
                globals->dOpenDevice.fpRenderStateGet(kTrackedStates[i], &g_snapshot[i].value) != FALSE;
            anyCaptured = anyCaptured || g_snapshot[i].valid;
        }
        return anyCaptured;
    }

    void RestoreStates()
    {
        RwGlobals* globals = GetRwGlobals();
        if (globals == nullptr || globals->dOpenDevice.fpRenderStateSet == nullptr)
            return;

        for (size_t i = 0; i < kTrackedStateCount; ++i)
        {
            if (g_snapshot[i].valid)
            {
                globals->dOpenDevice.fpRenderStateSet(
                    g_snapshot[i].state,
                    g_snapshot[i].value
                );
                g_snapshot[i].valid = false;
            }
        }
    }
}

void RenderFixCleo::Configure(bool enabled, bool renderStateGuard)
{
    g_enabled = enabled;
    g_stateGuardEnabled = enabled && renderStateGuard;
    if (!g_stateGuardEnabled)
        g_hasFrameSnapshot = false;
}

void RenderFixCleo::OnFrameBegin()
{
    if (!g_enabled || !g_stateGuardEnabled || g_hasFrameSnapshot)
        return;

    g_hasFrameSnapshot = CaptureStates();
}

void RenderFixCleo::OnFrameEnd()
{
    if (!g_enabled || !g_stateGuardEnabled || !g_hasFrameSnapshot)
        return;

    RestoreStates();
    g_hasFrameSnapshot = false;
}
