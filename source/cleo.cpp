#include "stdafx.h"
#include "cleo.h"
#include "CDebugCallbackSystem.h"

namespace CLEO
{
    CCleoInstance CleoInstance;
    CCleoInstance& GetInstance() { return CleoInstance; }

    void __cdecl CCleoInstance::OnUpdateGameLogics()
    {
        NotifyGameProcessBefore();

        GetInstance().SoundSystem.Update();
        GetInstance().UpdateGameLogics();

        NotifyGameProcessAfter();
    }
}
