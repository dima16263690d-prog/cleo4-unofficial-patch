#pragma once

#include "CCustomOpcodeSystem.h"

namespace CLEO
{
    using FastOpcodeHandler = OpcodeResult(__thiscall *)(CRunningScript *, unsigned short);

    class CFastOpcodeExecutor
    {
    public:
        static constexpr unsigned short kOpcodeCount = 0x8000;

        // Build the flat opcode -> existing handler map once during injection.
        // The existing GTA/CLEO handlers remain the actual implementations.
        static void Initialize(const FastOpcodeHandler *groupedHandlers, size_t groupedCount);

        // Fast opcode path used by the existing DebugUtils dispatch boundary.
        // It preserves the before/after callbacks and calls the original handler.
        static __forceinline OpcodeResult __fastcall Dispatch(CRunningScript *thread, unsigned short opcode);

    private:
        static FastOpcodeHandler s_groupHandlers[kOpcodeCount];
    };
}
