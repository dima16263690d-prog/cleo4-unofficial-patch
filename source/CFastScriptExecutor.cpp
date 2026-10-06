#include "stdafx.h"
#include "CCustomOpcodeSystem.h"
#include "CFastOpcodeExecutor.h"
#include "CFastScriptExecutor.h"
#include "CDebugBridge.h"
#include "CScriptEngine.h"

namespace CLEO
{
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
            char str[128];
            sprintf(
                str,
                "%s encountered while parsing opcode '%04X' in script '%s'",
                e,
                last_opcode,
                last_thread
            );
            Error(str);
        }

        return 0;
    }
}
