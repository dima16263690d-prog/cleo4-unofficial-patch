#include "stdafx.h"
#include "CScriptRuntime.h"
#include "CCustomOpcodeSystem.h"
#include "CDebugCallbackSystem.h"

namespace CLEO
{
    bool CScriptRuntime::DispatchScript(CRunningScript *script)
    {
        if (script == nullptr)
            return true;

        // Keep the existing callback boundary unchanged.
        if (!NotifyScriptProcessBefore(script))
            return false;

        // Preserve the exact legacy execution route.
        CCustomScript *customScript = reinterpret_cast<CCustomScript*>(script);
        if (customScript->IsCustom())
            customScript->Process();
        else
            ProcessScript(script);

        NotifyScriptProcessAfter(script);
        return true;
    }
}
