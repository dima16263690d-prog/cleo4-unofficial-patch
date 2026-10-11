#include "stdafx.h"
#include "cleo.h"
#include "CCustomScript.h"
#include "ScmFunction.h"
#include "CScriptEngine.Internal.h"
#include "CScriptEngine.State.h"
#include "CDebugCallbackSystem.h"
#include "CCleoMemoryManager.h"
#include <algorithm>
#include <cstdint>
#include <new>
#include <vector>

namespace CLEO
{
    static inline BYTE *RestorePointerOffset(BYTE *base, int32_t offset)
    {
        return offset == INT32_MIN ? nullptr : base + offset;
    }

    // -------------------------------------------------------------------------
    // Script engine lifecycle
    // -------------------------------------------------------------------------
    // Keep GTA responsible for script execution and pActiveScripts ordering.
    // CScriptEngine owns the transition into/out of the CLEO runtime. This is
    // intentionally single-threaded for the first test baseline.

    void CScriptEngine::GameBegin(bool bLoadMode)
    {
        if (scriptsLoaded)
            return;

        if (activeThreadQueue == nullptr || *activeThreadQueue == nullptr)
            return;

        scriptsLoaded = true;

        // Preserve GTA's native processing order. CLEO scripts are built into
        // a separate temporary queue and attached after the native tail.
        CRunningScript *nativeHead = *activeThreadQueue;
        CRunningScript *nativeTail = nativeHead;
        size_t nativeCount = 1;

        while (nativeTail->GetNext() != nullptr)
        {
            nativeTail = nativeTail->GetNext();
            ++nativeCount;
        }

        TRACE("[engine] GameBegin: preserving native script order");

        *activeThreadQueue = nullptr;
        LoadCustomScripts(bLoadMode);

        if (*activeThreadQueue != nullptr)
        {
            nativeTail->SetNext(*activeThreadQueue);
            (*activeThreadQueue)->SetPrev(nativeTail);
        }

        *activeThreadQueue = nativeHead;

        const size_t customCount = CustomScripts.size() + (CustomMission != nullptr ? 1u : 0u);
        TRACE("[engine] Queue composed: native=%u custom=%u total=%u",
            static_cast<unsigned>(nativeCount),
            static_cast<unsigned>(customCount),
            static_cast<unsigned>(nativeCount + customCount));
        TRACE("[engine] GameBegin complete: CLEO scripts appended after native scripts");
        NotifyGameBegin();
    }

    void CScriptEngine::GameEnd()
    {
        if (!scriptsLoaded && CustomMission == nullptr && CustomScripts.empty())
            return;

        TRACE("[engine] GameEnd: stopping custom runtime");

        // All custom shutdown goes through the central lifecycle path.
        RemoveAllCustomScripts();

        // CLEO-style memory cleanup happens after scripts are destroyed,
        // so per-script ownership can be released before the final leak scan.
        GetSmartMemoryEngine().Memory().OnGameEnd();
        CompactProcessHeaps();

        scriptsLoaded = false;
        NotifyGameEnd();
    }

    void CScriptEngine::RestorePendingChildScript(CCustomScript *parent, CCustomScript *child, int label)
    {
        if (!parent || !child || parent->cleoState.savedNodeId == 0)
            return;

        unsigned ordinal = 0;
        for (auto sibling : parent->childThreads)
        {
            if (sibling->childLabel != label)
                continue;

            if (sibling == child)
                break;

            ++ordinal;
        }

        for (auto it = pendingChildSaves.begin(); it != pendingChildSaves.end(); ++it)
        {
            if (it->parent_node_id != parent->cleoState.savedNodeId ||
                it->label != label ||
                it->ordinal != ordinal)
            {
                continue;
            }

            const unsigned nodeId = it->node_id;
            it->Apply(child);
            child->cleoState.savedNodeId = nodeId;

            TRACE("Restored custom child script '%s' from sidecar parent=%08X node=%u label=%d ordinal=%u",
                child->Name, parent->cleoState.savedNodeId, nodeId, label, ordinal);

            pendingChildSaves.erase(it);
            return;
        }
    }

    void CScriptEngine::RestorePendingScmFunctions(CCustomScript *cs)
    {
        if (!cs || cs->cleoState.savedNodeId == 0)
            return;

        std::vector<const ScmFunctionSaveInfo*> savedStates;
        for (const auto& saved : pendingScmFunctionSaves)
        {
            if (saved.node_id == cs->cleoState.savedNodeId)
                savedStates.push_back(&saved);
        }

        if (savedStates.empty())
            return;

        std::sort(savedStates.begin(), savedStates.end(),
            [](const ScmFunctionSaveInfo* a, const ScmFunctionSaveInfo* b) {
                return a->depth < b->depth;
            });

        BYTE *base = cs->GetBasePointer();
        WORD prevId = 0;
        std::vector<DWORD> restoredStringOldPointers;
        std::vector<DWORD> restoredStringNewPointers;
        std::vector<ScmFunction*> restoredFunctions;

        for (const auto* saved : savedStates)
        {
            ScmFunction *fn = ScmFunction::CreateRestored();
            if (!fn)
                throw std::bad_alloc();

            fn->prevScmFunctionId = prevId;
            fn->callArgCount = saved->callArgCount;
            fn->callIP = RestorePointerOffset(base, saved->callOffset);
            fn->retnAddress = RestorePointerOffset(base, saved->retnOffset);
            fn->savedBaseIP = RestorePointerOffset(base, saved->savedBaseOffset);
            fn->savedCodeSize = saved->savedCodeSize;
            fn->savedSP = saved->savedSP;
            fn->savedCondResult = saved->savedCondResult;
            fn->savedLogicalOp = static_cast<eLogicalOperation>(saved->savedLogicalOp);
            fn->savedNotFlag = saved->savedNotFlag;
            fn->savedScriptFileDir = saved->savedScriptFileDir;
            fn->savedScriptFileName = saved->savedScriptFileName;

            for (size_t i = 0; i < 8; ++i)
                fn->savedStack[i] = RestorePointerOffset(base, saved->savedStackOffsets[i]);

            std::copy(saved->savedTls, saved->savedTls + 32, fn->savedTls);

            for (const auto& stringParam : saved->stringParams)
            {
                fn->stringParams.push_back(stringParam.value);
                auto& restored = fn->stringParams.back();
                restoredStringOldPointers.push_back(stringParam.oldPointer);
                restoredStringNewPointers.push_back(
                    static_cast<DWORD>(reinterpret_cast<uintptr_t>(restored.c_str()))
                );
            }

            restoredFunctions.push_back(fn);
            prevId = fn->thisScmFunctionId;
        }

        // Rebind saved/current string locals that previously pointed into
        // std::string storage owned by the pre-save ScmFunction objects.
        auto restoreStringPointer = [&](SCRIPT_VAR& var)
        {
            for (size_t i = 0; i < restoredStringOldPointers.size(); ++i)
            {
                if (var.dwParam == restoredStringOldPointers[i])
                {
                    var.dwParam = restoredStringNewPointers[i];
                    break;
                }
            }
        };

        // Rebind saved/current string locals that previously pointed into
        // std::string storage owned by the pre-save ScmFunction objects.
        for (auto* fn : restoredFunctions)
        {
            for (auto& var : fn->savedTls)
                restoreStringPointer(var);
        }

        SCRIPT_VAR *currentLocals = cs->IsMission() ? missionLocals : cs->LocalVar;
        for (size_t i = 0; i < 32; ++i)
            restoreStringPointer(currentLocals[i]);

        cs->SetScmFunction(prevId);

        pendingScmFunctionSaves.erase(
            std::remove_if(
                pendingScmFunctionSaves.begin(),
                pendingScmFunctionSaves.end(),
                [cs](const ScmFunctionSaveInfo& saved) {
                    return saved.node_id == cs->cleoState.savedNodeId;
                }),
            pendingScmFunctionSaves.end()
        );

        TRACE("[CLEO][LOAD][ScmFunction] restored %u active scopes for '%.8s' node=%u",
            static_cast<unsigned>(restoredFunctions.size()),
            cs->GetName(),
            cs->cleoState.savedNodeId);
    }

    void CScriptEngine::RestorePendingChildTree(CCustomScript *parent)
    {
        if (!parent || parent->cleoState.savedNodeId == 0)
            return;

        for (;;)
        {
            size_t found = pendingChildSaves.size();

            // Restore children in ordinal order for each label. This keeps
            // duplicate labels (for example two 0E6F streams at the same
            // label) deterministic.
            for (size_t i = 0; i < pendingChildSaves.size(); ++i)
            {
                if (pendingChildSaves[i].parent_node_id != parent->cleoState.savedNodeId)
                    continue;

                bool lowerOrdinalPending = false;
                for (size_t j = 0; j < pendingChildSaves.size(); ++j)
                {
                    if (pendingChildSaves[j].parent_node_id == parent->cleoState.savedNodeId &&
                        pendingChildSaves[j].label == pendingChildSaves[i].label &&
                        pendingChildSaves[j].ordinal < pendingChildSaves[i].ordinal)
                    {
                        lowerOrdinalPending = true;
                        break;
                    }
                }

                if (!lowerOrdinalPending)
                {
                    found = i;
                    break;
                }
            }

            if (found == pendingChildSaves.size())
                break;

            ChildThreadSavingInfo saved = pendingChildSaves[found];
            pendingChildSaves.erase(pendingChildSaves.begin() + found);

            auto child = new CCustomScript(parent->Name, false, parent, saved.label);
            if (!child || !child->IsOK())
            {
                if (child)
                    delete child;

                DIAG("[CLEO][ERROR][CUSTOM] restore failed parent_node=%08X node=%u label=%d ordinal=%u",
                    parent->cleoState.savedNodeId, saved.node_id, saved.label, saved.ordinal);
                continue;
            }

            AddCustomScript(child);
            saved.Apply(child);
            child->cleoState.savedNodeId = saved.node_id;
            RestorePendingScmFunctions(child);

            RestorePendingChildTree(child);
        }
    }

}
