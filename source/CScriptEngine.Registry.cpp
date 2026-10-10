#include "stdafx.h"
#include "cleo.h"
#include "CCustomScript.h"
#include "CScriptEngine.Internal.h"
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

        for (auto script = *activeThreadQueue; script != nullptr; script = script->GetNext())
        {
            if (_stricmp(script->GetName(), name) == 0)
                return script;
        }

        return nullptr;
    }

    CCustomScript *CScriptEngine::FindCustomScriptNamed(const char *name)
    {
        if (name == nullptr)
            return nullptr;

        const auto key = NormalizeCustomScriptName(name);
        auto it = m_customScriptsByName.find(key);
        if (it != m_customScriptsByName.end())
        {
            for (auto *cs : it->second)
            {
                if (cs != nullptr)
                    return cs;
            }
        }

        // Compatibility fallback: linear scan (legacy behavior).
        for (auto *cs : CustomScripts)
        {
            if (cs != nullptr && NormalizeCustomScriptName(cs->Name) == key)
                return cs;
        }

        if (CustomMission != nullptr && NormalizeCustomScriptName(CustomMission->Name) == key)
            return CustomMission;

        return nullptr;
    }

    CCustomScript *CScriptEngine::CreateCustomScript(CRunningScript *fromThread, const char *scriptName, int label)
    {
        if (scriptName == nullptr)
            return nullptr;

        CCustomScript *parent = fromThread ? reinterpret_cast<CCustomScript*>(fromThread) : nullptr;

        if (label != 0 && (parent == nullptr || !parent->IsCustom()))
        {
            TRACE("[engine] CreateCustomScript rejected: label child requires a custom parent");
            if (fromThread)
                SetScriptCondResult(fromThread, false);
            if (fromThread)
            {
                while (*fromThread->GetBytePointer())
                    GetScriptParams(fromThread, 1);
                fromThread->ReadDataByte();
            }
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
            {
                while (*fromThread->GetBytePointer())
                    GetScriptParams(fromThread, 1);
                fromThread->ReadDataByte();
            }

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

        if (m_customScriptRegistry.find(script) != m_customScriptRegistry.end())
            return m_activeCustomScriptRegistry.find(script) != m_activeCustomScriptRegistry.end();

        for (auto current = *activeThreadQueue; current != nullptr; current = current->GetNext())
        {
            if (current == script)
                return true;
        }
        return false;
    }

    bool CScriptEngine::IsValidScriptPtr(const CRunningScript *script) const
    {
        if (script == nullptr)
            return false;

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

        return false;
    }

    void CScriptEngine::RemoveScript(CRunningScript *script)
    {
        if (script == nullptr)
            return;

        CCustomScript *custom = reinterpret_cast<CCustomScript*>(script);
        if (custom->IsCustom())
            RemoveCustomScript(custom);
        else
        {
            RemoveScriptFromQueue(script, activeThreadQueue);
            AddScriptToQueue(script, inactiveThreadQueue);
            StopScript(script);
        }
    }

    void CScriptEngine::AddCustomScript(CCustomScript *cs)
    {
        if (cs == nullptr || !cs->cleoState.bOK)
            return;

        if (!m_customScriptRegistry.insert(cs).second)
        {
            TRACE("[engine] Duplicate custom-script registration rejected: %.*s", 8, cs->Name);
            return;
        }

        m_activeCustomScriptRegistry.insert(cs);

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

        AddScriptToQueue(cs, activeThreadQueue);
        cs->SetActive(true);
    }

    void CScriptEngine::RemoveCustomScript(CCustomScript *cs)
    {
        if (cs == nullptr)
            return;

        if (m_customScriptRegistry.find(cs) == m_customScriptRegistry.end() ||
            !m_pendingDeleteRegistry.insert(cs).second)
        {
            return;
        }

        m_activeCustomScriptRegistry.erase(cs);
        UpdateDrawableScript(cs, false);

        if (cs->parentThread != nullptr)
        {
            cs->parentThread->childThreads.remove(cs);
            cs->parentThread = nullptr;
        }

        while (!cs->childThreads.empty())
            RemoveCustomScript(cs->childThreads.front());

        RemoveScriptFromQueue(cs, activeThreadQueue);
        cs->SetActive(false);

        if (cs->IsMission())
        {
            CustomMission = nullptr;
        }
        else
        {
            CustomScripts.remove(cs);
            auto key = NormalizeCustomScriptName(cs->Name);
            auto it = m_customScriptsByName.find(key);
            if (it != m_customScriptsByName.end())
            {
                auto& vec = it->second;
                vec.erase(std::remove(vec.begin(), vec.end(), cs), vec.end());
                if (vec.empty())
                    m_customScriptsByName.erase(it);
            }
        }

        ScriptsWaitingForDelete.push_back(cs);
        TRACE("Unregistering custom script named %.*s", 8, cs->Name);
    }

    void CScriptEngine::RemoveAllCustomScripts(void)
    {
        while (CustomMission != nullptr)
            RemoveCustomScript(CustomMission);

        while (!CustomScripts.empty())
            RemoveCustomScript(CustomScripts.back());

        DeleteWaitingScripts();
    }

    void CScriptEngine::DeleteWaitingScripts()
    {
        if (ScriptsWaitingForDelete.empty())
            return;

        for (auto *cs : ScriptsWaitingForDelete)
        {
            TRACE("Deleting inactive script named %.*s", 8, cs->Name);
            m_customScriptRegistry.erase(cs);
            m_pendingDeleteRegistry.erase(cs);
            delete cs;
        }
        ScriptsWaitingForDelete.clear();
    }

    void CScriptEngine::UnregisterAllScripts()
    {
        TRACE("Unregistering all custom scripts");
        for (auto *cs : CustomScripts)
        {
            RemoveScriptFromQueue(cs, activeThreadQueue);
            cs->SetActive(false);
            m_activeCustomScriptRegistry.erase(cs);
        }
        if (CustomMission != nullptr)
        {
            RemoveScriptFromQueue(CustomMission, activeThreadQueue);
            CustomMission->SetActive(false);
            m_activeCustomScriptRegistry.erase(CustomMission);
        }
    }

    void CScriptEngine::ReregisterAllScripts()
    {
        TRACE("Reregistering all custom scripts");
        for (auto *cs : CustomScripts)
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
