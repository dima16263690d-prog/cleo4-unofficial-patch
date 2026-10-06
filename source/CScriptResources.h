#pragma once

#include "CTheScripts.h"
#include <list>
#include <vector>

namespace CLEO
{
    class CCustomScript;

    // Resources owned by one CLEO script. These are kept separate from the
    // GTA script ABI and from the script execution state.
    struct CScriptResources
    {
        BYTE UseTextCommands;
        int NumDraws;
        int NumTexts;

        std::list<RwTexture*> script_textures;
        std::vector<BYTE> script_draws;
        std::vector<BYTE> script_texts;
    };
}
