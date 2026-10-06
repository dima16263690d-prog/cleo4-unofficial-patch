#pragma once

#include "CTheScripts.h"
#include <cstddef>
#include <string>

namespace CLEO
{
    // CLEO-owned script state. CRunningScript remains the GTA ABI base;
    // this structure contains only data owned by the CLEO layer.
    struct CCleoScriptState
    {
        DWORD dwChecksum;
        BYTE *ownedBuffer;
        bool bSaveEnabled;
        bool bOK;

        DWORD LastSearchPed;
        DWORD LastSearchCar;
        DWORD LastSearchObj;

        CLEO_Version CompatVer;
        size_t CodeSize;

        std::string ScriptFileDir;
        std::string ScriptFileName;

        DWORD savedNodeId;
    };
}
