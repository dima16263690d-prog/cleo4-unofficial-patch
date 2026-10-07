#include "stdafx.h"
#include "CCustomOpcodeSystem.h"
#include "CFastOpcodeExecutor.h"
#include "CFastScriptExecutor.h"
#include "CDebugBridge.h"
#include "CScriptEngine.h"

namespace CLEO
{
    // Legacy executor used for active 0AB1/0AB2 scopes. It keeps the exact
    // pre-fast-loop opcode dispatch semantics for function execution.
    extern char ExecuteLegacyScriptLoop(CRunningScript *thread);

    extern WORD last_opcode;
    extern WORD last_custom_opcode;
    extern char last_thread[9];
    extern CRunningScript *last_script;
    extern ptrdiff_t last_off;

    char CFastScriptExecutor::Execute()
    {
        CCustomScript *thread;
        OpcodeResult res;

        _asm mov thread, esi

        last_script = thread;

        try
        {
            do
            {
                // Once 0AB1 has entered a function scope, hand execution back
                // to the legacy loop. This preserves the previously working
                // 0AD1/wait/0AB2 behavior while normal scripts stay on fast-loop.
                if (thread->GetScmFunction() != 0)
                    return ExecuteLegacyScriptLoop(thread);

                // Read the opcode directly from the current IP. This keeps the
                // legacy execution order intact while removing the indirect
                // newOpcodeHandlerTable[opcode / 100] dispatch from the hot loop.
                BYTE *const ip = thread->GetBytePointer();
                WORD opcode = thread->ReadDataWord();

                // Crash/debug state keeps the same information as the legacy
                // loop, but the relatively expensive offset/name work is only
                // performed while crash snapshots are enabled.
                if (IsCrashSnapshotEnabled())
                {
                    const ptrdiff_t off =
                        thread->IsCustom()
                            ? ip - thread->GetBasePointer()
                            : ip - scmBlock;

                    last_opcode = opcode;
                    last_off = off;
                    memcpy(last_thread, thread->GetName(), 8);
                    last_thread[8] = '\0';
                }
                else
                {
                    last_opcode = opcode;
                }

                thread->SetNotFlag((opcode & 0x8000) != 0);
                opcode &= 0x7FFF;

                // Directly enter the already prepared flat opcode dispatcher.
                // No second table lookup and no opcode / 100 operation here.
                res = CFastOpcodeExecutor::Dispatch(thread, opcode);
            }
            while (res == OR_CONTINUE);
        }
        catch (const char *e)
        {
            // Crash snapshots may be disabled in the hot loop, so last_thread
            // can legitimately contain an older script name. Build the error
            // message from the current thread only when an exception happens.
            char scriptName[9] = "none";
            if (thread != nullptr)
            {
                memcpy(scriptName, thread->GetName(), 8);
                scriptName[8] = '\0';
            }

            char str[128];
            sprintf(
                str,
                "%s encountered while parsing opcode '%04X' in script '%s'",
                e,
                last_opcode,
                scriptName
            );
            Error(str);
        }

        return 0;
    }
}
