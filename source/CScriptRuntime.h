#pragma once

#include "CCustomScript.h"

namespace CLEO
{
    class CScriptRuntime
    {
    public:
        // Runtime boundary for one GTA script. This layer only routes the
        // existing execution path; it does not change legacy execution.
        bool DispatchScript(CRunningScript *script);
    };
}
