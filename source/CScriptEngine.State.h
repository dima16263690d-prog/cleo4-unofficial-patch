#pragma once

// Internal persistence records shared by save/load and child-script restoration.
#include "CScriptEngine.Internal.h"
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>

namespace CLEO
{
    // Legacy CLEO variable/script save records.
    struct CleoSafeHeader
    {
        const static unsigned sign;
        unsigned signature;
        unsigned n_saved_threads;
        unsigned n_stopped_threads;
    };

    struct ThreadSavingInfo
    {
        unsigned long hash;
        SCRIPT_VAR tls[32];
        unsigned timers[2];
        bool condResult;
        unsigned sleepTime;
        eLogicalOperation logicalOp;
        bool notFlag;
        ptrdiff_t ip_diff;
        char threadName[8];

        ThreadSavingInfo(CCustomScript *cs) :
            hash(cs->cleoState.dwChecksum), condResult(cs->bCondResult),
            logicalOp(cs->LogicalOp), notFlag(cs->NotFlag != false), ip_diff(cs->CurrentIP - reinterpret_cast<BYTE*>(cs->BaseIP))
        {
            sleepTime = cs->WakeTime >= *GameTimer ? 0 : cs->WakeTime - *GameTimer;
            std::copy(cs->LocalVar, cs->LocalVar + 32, tls);
            std::copy(cs->Timers, cs->Timers + 2, timers);
            std::copy(cs->Name, cs->Name + 8, threadName);
        }

        void Apply(CCustomScript *cs)
        {
            cs->cleoState.dwChecksum = hash;
            std::copy(tls, tls + 32, cs->LocalVar);
            std::copy(timers, timers + 2, cs->Timers);
            cs->bCondResult = condResult;
            cs->WakeTime = *GameTimer + sleepTime;
            cs->LogicalOp = logicalOp;
            cs->NotFlag = notFlag;
            cs->CurrentIP = reinterpret_cast<BYTE*>(cs->BaseIP) + ip_diff;
            std::copy(threadName, threadName + 8, cs->Name);
            cs->cleoState.bSaveEnabled = true;
        }

        ThreadSavingInfo() { }
    };

    struct ChildSaveHeader
    {
        const static unsigned sign;
        const static unsigned format_version;
        unsigned signature;
        unsigned version;
        unsigned n_children;
    };

    struct ChildThreadSavingInfo
    {
        unsigned node_id;
        unsigned parent_node_id;
        int label;
        unsigned ordinal;
        ThreadSavingInfo state;

        ChildThreadSavingInfo() : node_id(0), parent_node_id(0), label(0), ordinal(0) {}
        ChildThreadSavingInfo(CCustomScript *cs, unsigned parentId, unsigned nodeId, unsigned childOrdinal)
            : node_id(nodeId), parent_node_id(parentId), label(cs->GetChildLabel()), ordinal(childOrdinal), state(cs)
        {
        }

        void Apply(CCustomScript *cs)
        {
            state.Apply(cs);
            // Child streams are represented by the sidecar, not by the legacy
            // hash-only saved-thread list.
            cs->enable_saving(false);
        }
    };

    struct SavedScmStringParam
    {
        DWORD oldPointer;
        std::string value;
    };

    struct ScmFunctionSaveInfo
    {
        unsigned node_id;
        unsigned depth;
        BYTE callArgCount;
        int32_t callOffset;
        int32_t retnOffset;
        int32_t savedBaseOffset;
        uint32_t savedCodeSize;
        int32_t savedStackOffsets[8];
        WORD savedSP;
        SCRIPT_VAR savedTls[32];
        bool savedCondResult;
        int32_t savedLogicalOp;
        bool savedNotFlag;
        std::string savedScriptFileDir;
        std::string savedScriptFileName;
        std::vector<SavedScmStringParam> stringParams;
    };

    struct ScmFunctionSaveHeader
    {
        const static unsigned sign;
        const static unsigned format_version;
        unsigned signature;
        unsigned version;
        unsigned n_functions;
    };

    extern std::vector<ChildThreadSavingInfo> pendingChildSaves;
    extern std::vector<ScmFunctionSaveInfo> pendingScmFunctionSaves;
}
