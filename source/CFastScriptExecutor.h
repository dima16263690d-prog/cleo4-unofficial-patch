#pragma once

namespace CLEO
{
    class CFastScriptExecutor
    {
    public:
        // Replacement for the GTA/CLEO hot ScriptExecutionLoop on SA 1.0 US.
        // The caller provides the current script in ESI, matching the legacy hook ABI.
        static char Execute();
    };
}
