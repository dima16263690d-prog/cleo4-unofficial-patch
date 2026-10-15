#include "stdafx.h"
#include "cleo.h"
#include "CCustomScript.h"
#include "CScriptEngine.Internal.h"
#include "CDebugCallbackSystem.h"
#include <cstdint>
#include <algorithm>
#include <cctype>
#include <string>

namespace CLEO
{

    CRunningScript *CScriptEngine::FindScriptNamed(const char *name)
    {
        if (name == nullptr || activeThreadQueue == nullptr)
            return nullptr;

        // pActiveScripts remains the authoritative execution order:
        // native scripts first, CLEO scripts after them.
        for (auto script = *activeThreadQueue; script; script = script->GetNext())
        {
            if (_stricmp(name, script->GetName()) == 0)
                return script;
        }

        return nullptr;
    }

    CCustomScript *CScriptEngine::FindCustomScriptByPtr(const CRunningScript* script) const
    {
        if (script == nullptr)
            return nullptr;

        auto it = m_customScriptByPtr.find(script);
        return it == m_customScriptByPtr.end() ? nullptr : it->second;
    }

    CCustomScript *CScriptEngine::FindCustomScriptNamed(const char *name)
    {
        if (name == nullptr)
            return nullptr;

        // Preserve legacy precedence: the custom mission is searched first.
        if (CustomMission && _stricmp(name, CustomMission->Name) == 0)
            return CustomMission;

        // Most lookups are now average O(1). Confirm with _stricmp so the
        // cache never changes the legacy case-insensitive matching semantics.
        const auto key = NormalizeCustomScriptName(name);
        const auto indexed = m_customScriptsByName.find(key);
        if (indexed != m_customScriptsByName.end())
        {
            for (auto cs : indexed->second)
            {
                if (cs != nullptr &&
                    m_customScriptRegistry.find(cs) != m_customScriptRegistry.end() &&
                    _stricmp(name, cs->Name) == 0)
                {
                    return cs;
                }
            }
        }

        // Compatibility fallback for scripts whose public Name storage was
        // modified directly by a plugin or memory opcode after registration.
        for (auto cs : CustomScripts)
        {
            if (_stricmp(name, cs->Name) == 0)
                return cs;
        }

        return nullptr;
    }

    static void SkipUnusedScriptParameters(CRunningScript *thread)
    {
        if (thread == nullptr)
            return;

        while (*thread->GetBytePointer())
            GetScriptParams(thread, 1);

        thread->ReadDataByte();
    }

    CCustomScript *CScriptEngine::CreateCustomScript(CRunningScript *fromThread, const char *scriptName, int label)
    {
        if (scriptName == nullptr)
            return nullptr;

        // Never inspect CLEO-only fields through an unchecked cast: native
        // GTA threads have the same base pointer type but not CCustomScript state.
        CCustomScript *parent = label != 0 ? FindCustomScriptByPtr(fromThread) : nullptr;

        if (label != 0 && (parent == nullptr || !parent->IsCustom()))
        {
            TRACE("[engine] CreateCustomScript rejected: label child requires a registered custom parent");
            if (fromThread)
                SetScriptCondResult(fromThread, false);
            if (fromThread)
                SkipUnusedScriptParameters(fromThread);
            return nullptr;
        }

        char cwd[MAX_PATH];
        _getcwd(cwd, sizeof(cwd));
        _chdir(cleo_dir);

        CCustomScript *cs = new CCustomScript(scriptName, false, parent, label);

        if (fromThread)
            SetScriptCondResult(fromThread, cs != nullptr && cs->cleoState.bOK);

        if (cs == nullptr || !cs->cleoState.bOK)
        {
            if (cs)
                delete cs;

            if (fromThread)
                SkipUnusedScriptParameters(fromThread);

            TRACE("[engine] CreateCustomScript failed: %s", scriptName);
            _chdir(cwd);
            return nullptr;
        }

        AddCustomScript(cs);

        if (fromThread)
            TransmitScriptParams(fromThread, cs);

        _chdir(cwd);
        return cs;
    }

    bool CScriptEngine::IsActiveScriptPtr(const CRunningScript *script) const
    {
        if (script == nullptr || activeThreadQueue == nullptr)
            return false;

        // CLEO script state is tracked by lifecycle operations, avoiding a
        // full GTA active-queue walk for the common custom-script case.
        if (m_customScriptRegistry.find(script) != m_customScriptRegistry.end())
            return m_activeCustomScriptRegistry.find(script) != m_activeCustomScriptRegistry.end();

        for (auto current = *activeThreadQueue; current != nullptr; current = current->GetNext())
        {
            if (current == script)
                return current->IsActive();
        }

        return false;
    }

    bool CScriptEngine::IsValidScriptPtr(const CRunningScript *script) const
    {
        if (script == nullptr)
            return false;

        // Includes active, temporarily unregistered and pending-delete CLEO
        // scripts, matching the lifetime currently represented by the lists.
        if (m_customScriptRegistry.find(script) != m_customScriptRegistry.end())
            return true;

        if (activeThreadQueue != nullptr)
        {
            for (auto current = *activeThreadQueue; current != nullptr; current = current->GetNext())
            {
                if (current == script)
                    return true;
            }
        }

        if (inactiveThreadQueue != nullptr)
        {
            for (auto current = *inactiveThreadQueue; current != nullptr; current = current->GetNext())
            {
                if (current == script)
                    return true;
            }
        }

        for (auto current : CustomScripts)
        {
            if (current == script)
                return true;
        }

        for (auto current : ScriptsWaitingForDelete)
        {
            if (current == script)
                return true;
        }

        return false;
    }

    void CScriptEngine::RemoveScript(CRunningScript *script)
    {
        if (script == nullptr)
            return;

        CCustomScript *custom = reinterpret_cast<CCustomScript*>(script);

        if (custom->IsCustom())
        {
            if (custom->IsMission())
                *MissionLoaded = false;

            RemoveCustomScript(custom);
            return;
        }

        // Native GTA script lifecycle stays unchanged: active -> inactive -> stop.
        if (activeThreadQueue != nullptr)
            RemoveScriptFromQueue(script, activeThreadQueue);
        if (inactiveThreadQueue != nullptr)
            AddScriptToQueue(script, inactiveThreadQueue);
        StopScript(script);
    }

    void CScriptEngine::AddCustomScript(CCustomScript *cs)
    {
        if (cs == nullptr || !cs->cleoState.bOK)
            return;

        // Registration is idempotent: a script must never be inserted into
        // the GTA queue or custom lists twice.
        if (!m_customScriptRegistry.insert(cs).second)
        {
            TRACE("[engine] Duplicate custom-script registration rejected: %.*s", 8, cs->Name);
            return;
        }

        m_activeCustomScriptRegistry.insert(cs);
        m_customScriptByPtr[cs] = cs;

        if (cs->IsMission())
        {
            TRACE("Registering custom mission named %.*s", 8, cs->Name);
            CustomMission = cs;
        }
        else
        {
            TRACE("Registering custom script named %.*s", 8, cs->Name);
            CustomScripts.push_back(cs);
            m_customScriptsByName[NormalizeCustomScriptName(cs->Name)].push_back(cs);
        }

        // Registry -> GTA queue -> active state.
        AddScriptToQueue(cs, activeThreadQueue);
        cs->SetActive(true);

        // Notify plugins that a new script object is live (SDK 2.0 lifetime).
        NotifyScriptCreated(cs);
    }

    void CScriptEngine::RemoveCustomScript(CCustomScript *cs)
    {
        if (cs == nullptr)
            return;

        // Prevent repeated stop requests from enqueueing the same object for
        // deletion more than once. Keep it in the main registry until the
        // deferred destructor boundary, preserving pointer-validity semantics.
        if (m_customScriptRegistry.find(cs) == m_customScriptRegistry.end() ||
            !m_pendingDeleteRegistry.insert(cs).second)
        {
            return;
        }

        m_activeCustomScriptRegistry.erase(cs);
        UpdateDrawableScript(cs, false);
        const bool wasChild = cs->parentThread != nullptr;

        // 1. Break the parent relation first.
        if (cs->parentThread != nullptr)
        {
            cs->parentThread->childThreads.remove(cs);
            cs->parentThread = nullptr;
        }

        // 2. Tear down the complete child subtree before this node.
        while (!cs->childThreads.empty())
        {
            CCustomScript *child = cs->childThreads.front();
            RemoveCustomScript(child);
        }

        // A saved child must not become an independently stopped root.
        if (cs != CustomMission && cs->cleoState.bSaveEnabled && !wasChild)
        {
            InactiveScriptHashes.insert(cs->cleoState.dwChecksum);
            TRACE("Stopping custom script named %.*s", 8, cs->Name);
        }

        // 3. Mark inactive first.
        cs->SetActive(false);

        // 4. Remove the script from GTA's active execution list.
        if (activeThreadQueue != nullptr)
            RemoveScriptFromQueue(cs, activeThreadQueue);

        // 5. Remove the script from CLEO's registry.
        if (cs == CustomMission)
        {
            TRACE("Unregistering custom mission named %.*s", 8, cs->Name);
            CustomMission = nullptr;
            *MissionLoaded = false;
        }
        else
        {
            TRACE("Unregistering custom script named %.*s", 8, cs->Name);

            // Remove the pointer from every name bucket. This also cleans up
            // stale cache entries if a plugin changed the public script name.
            for (auto it = m_customScriptsByName.begin(); it != m_customScriptsByName.end(); )
            {
                auto& scripts = it->second;
                scripts.erase(std::remove(scripts.begin(), scripts.end(), cs), scripts.end());
                if (scripts.empty())
                    it = m_customScriptsByName.erase(it);
                else
                    ++it;
            }

            CustomScripts.remove(cs);
        }

        // 6. Defer the actual delete until the current runtime boundary is safe.
        ScriptsWaitingForDelete.push_back(cs);
    }

    void CScriptEngine::RemoveAllCustomScripts(void)
    {
        TRACE("[engine] RemoveAllCustomScripts");

        InactiveScriptHashes.clear();

        if (CustomMission != nullptr)
            RemoveCustomScript(CustomMission);

        while (!CustomScripts.empty())
            RemoveCustomScript(CustomScripts.back());

        DeleteWaitingScripts();
    }

    void CScriptEngine::DeleteWaitingScripts()
    {
        // This hook runs for every GTA ProcessScript call. Avoid even the
        // temporary list/swap work on the overwhelmingly common empty path.
        if (ScriptsWaitingForDelete.empty())
            return;

        // Destruction is deliberately separated from queue/registry removal.
        std::list<CCustomScript *> waiting;
        waiting.swap(ScriptsWaitingForDelete);

        for (auto cs : waiting)
        {
            // Remove the pointer before destruction callbacks run, so they
            // cannot treat an object being destroyed as a valid script.
            m_pendingDeleteRegistry.erase(cs);
            m_activeCustomScriptRegistry.erase(cs);
            m_customScriptRegistry.erase(cs);
            m_customScriptByPtr.erase(cs);

            TRACE("Deleting inactive script named %.*s", 8, cs->Name);
            delete cs;
        }
    }

    void CScriptEngine::UnregisterAllScripts()
    {
        TRACE("Unregistering all custom scripts");

        for (auto cs : CustomScripts)
        {
            if (activeThreadQueue != nullptr)
                RemoveScriptFromQueue(cs, activeThreadQueue);
            cs->SetActive(false);
            m_activeCustomScriptRegistry.erase(cs);
        }

        if (CustomMission != nullptr)
        {
            if (activeThreadQueue != nullptr)
                RemoveScriptFromQueue(CustomMission, activeThreadQueue);
            CustomMission->SetActive(false);
            m_activeCustomScriptRegistry.erase(CustomMission);
        }
    }

    void CScriptEngine::ReregisterAllScripts()
    {
        TRACE("Reregistering all custom scripts");

        for (auto cs : CustomScripts)
        {
            AddScriptToQueue(cs, activeThreadQueue);
            cs->SetActive(true);
            m_activeCustomScriptRegistry.insert(cs);
        }

        if (CustomMission != nullptr)
        {
            AddScriptToQueue(CustomMission, activeThreadQueue);
            CustomMission->SetActive(true);
            m_activeCustomScriptRegistry.insert(CustomMission);
        }
    }

}
