#include "stdafx.h"
#include "CFastOpcodeExecutor.h"
#include "CDebugCallbackSystem.h"

namespace CLEO
{
    extern CustomOpcodeHandler lowOpcodeHandlers[0x0AF0];
    extern CustomOpcodeHandler customOpcodeHandlers[100];
    extern CustomOpcodeHandler extraOpcodeHandlers[100][300];

    extern WORD last_custom_opcode;
    extern CRunningScript *last_script;

    FastOpcodeHandler CFastOpcodeExecutor::s_groupHandlers[
        CFastOpcodeExecutor::kOpcodeCount
    ] = {};

    void CFastOpcodeExecutor::Initialize(
        const FastOpcodeHandler *groupedHandlers,
        size_t groupedCount
    )
    {
        if (groupedHandlers == nullptr || groupedCount == 0)
            return;

        // The legacy dispatch uses opcode / 100. Precompute that mapping once
        // so the hot opcode path performs a direct table lookup instead.
        for (unsigned int opcode = 0; opcode < kOpcodeCount; ++opcode)
        {
            const size_t group = opcode / 100;
            s_groupHandlers[opcode] =
                group < groupedCount ? groupedHandlers[group] : nullptr;
        }
    }

    __forceinline OpcodeResult __fastcall CFastOpcodeExecutor::Dispatch(
        CRunningScript *thread,
        unsigned short opcode
    )
    {
        const int action = NotifyScriptOpcodeProcessBefore(thread, opcode);
        if (action == CLEO_DEBUG_OPCODE_HANDLED)
            return OR_CONTINUE;
        if (action == CLEO_DEBUG_OPCODE_INTERRUPT)
            return OR_INTERRUPT;

        OpcodeResult result = OR_INTERRUPT;

        // Keep the existing dynamically registered low-opcode path unchanged.
        if (opcode < 0x0AF0 && lowOpcodeHandlers[opcode] != nullptr)
        {
            result = lowOpcodeHandlers[opcode](thread);
        }
        // Built-in CLEO opcodes get a direct handler call. This removes the
        // group dispatcher and its opcode-range calculation for 0A8C..0AEF.
        else if (opcode >= 0x0A8C && opcode <= 0x0AEF)
        {
            last_custom_opcode = opcode;
            last_script = thread;
            result = customOpcodeHandlers[opcode - 0x0A8C](thread);
        }
        // User-registered opcodes above 0x0AEF keep the existing handler
        // storage and semantics. Only the standard opcode path uses the
        // precomputed flat table below.
        else if (opcode >= 0x0AF0 &&
                 opcode / 100 >= 28 &&
                 extraOpcodeHandlers[opcode % 100][opcode / 100 - 28] != nullptr)
        {
            last_custom_opcode = opcode;
            last_script = thread;
            result = extraOpcodeHandlers[opcode % 100][opcode / 100 - 28](thread);
        }
        else
        {
            const FastOpcodeHandler handler = s_groupHandlers[opcode];
            if (handler != nullptr)
                result = handler(thread, opcode);
        }

        return NotifyScriptOpcodeProcessAfter(thread, opcode, result);
    }
}
