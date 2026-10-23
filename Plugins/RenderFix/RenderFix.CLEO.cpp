#include "RenderFix.CLEO.h"

#include "Events.h"
#include "rwplcore.h"

#include <cstddef>

namespace
{
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
    bool g_eventsRegistered = false;
    bool g_hasFrameSnapshot = false;

    RwGlobals* GetRwGlobals()
    {
        return static_cast<RwGlobals*>(RwEngineInstance);
    }

    void InvalidateSnapshot()
    {
        g_hasFrameSnapshot = false;
        for (size_t i = 0; i < kTrackedStateCount; ++i)
            g_snapshot[i].valid = false;
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
        {
            InvalidateSnapshot();
            return;
        }

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
        g_hasFrameSnapshot = false;
    }
}

void RenderFixCleo::Configure(bool enabled, bool renderStateGuard)
{
    g_enabled = enabled;
    g_stateGuardEnabled = enabled && renderStateGuard;
    InvalidateSnapshot();

    // Bind to the actual RenderWare drawing event, not CLEO's script-processing
    // callbacks. This brackets the game's drawing pass, where CLEO+ drawing
    // callbacks execute, and avoids restoring renderer state around script logic.
    if (g_stateGuardEnabled && !g_eventsRegistered)
    {
        Events::drawingEvent.before += [] {
            RenderFixCleo::OnFrameBegin();
        };
        Events::drawingEvent.after += [] {
            RenderFixCleo::OnFrameEnd();
        };

        // A device loss/reset or RenderWare reinitialization invalidates any
        // in-flight snapshot. Never replay stale raster/device state after reset.
        Events::d3dLostEvent += [] {
            InvalidateSnapshot();
        };
        Events::d3dResetEvent += [] {
            InvalidateSnapshot();
        };
        Events::initRwEvent += [] {
            InvalidateSnapshot();
        };
        g_eventsRegistered = true;
    }
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
}
