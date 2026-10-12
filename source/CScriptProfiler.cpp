#include "stdafx.h"
#include "CScriptProfiler.h"
#include <cstring>

namespace CLEO
{
    // Off by default: profiling is a diagnostic aid, not a runtime cost we
    // impose on every opcode. Plugins toggle it through the public API.
    volatile LONG g_scriptProfilerEnabled = 0;

    CScriptProfiler& CScriptProfiler::Get()
    {
        static CScriptProfiler instance;
        return instance;
    }

    bool CScriptProfiler::SetEnabled(bool enabled)
    {
        const LONG previous = g_scriptProfilerEnabled;
        g_scriptProfilerEnabled = enabled ? 1 : 0;

        // Leaving a session active could leave stale frames on the stack.
        if (!enabled && previous)
            Reset();

        return previous != 0;
    }

    unsigned long long CScriptProfiler::NowUs()
    {
        LARGE_INTEGER counter;
        LARGE_INTEGER frequency;
        QueryPerformanceCounter(&counter);
        QueryPerformanceFrequency(&frequency);

        if (frequency.QuadPart == 0)
            return 0;

        // Convert to microseconds without overflowing 64 bits.
        return (static_cast<unsigned long long>(counter.QuadPart) * 1000000ull) /
               static_cast<unsigned long long>(frequency.QuadPart);
    }

    CScriptProfiler::Entry& CScriptProfiler::EntryFor(CRunningScript* script)
    {
        const uintptr_t key = reinterpret_cast<uintptr_t>(script);

        auto inserted = m_entries.emplace(key, Entry{});
        Entry& entry = inserted.first->second;

        if (inserted.second)
        {
            m_order.push_back(key);

            const char* name = script->GetName();
            memcpy(entry.name, name, 8);
            entry.name[8] = '\0';
        }

        return entry;
    }

    void CScriptProfiler::Reset()
    {
        m_entries.clear();
        m_order.clear();
        m_stack.clear();
        m_currentEntry = nullptr;
        m_totalOpcodes = 0;
        m_totalUs = 0;
    }

    void CScriptProfiler::BeginScript(CRunningScript* script)
    {
        if (g_scriptProfilerEnabled == 0 || script == nullptr)
            return;

        Entry& entry = EntryFor(script);
        ++entry.ticks;

        m_currentEntry = &entry;
        m_stack.push_back(Frame{
            reinterpret_cast<uintptr_t>(script),
            &entry,
            NowUs(),
            0
        });
    }

    void CScriptProfiler::EndScript(CRunningScript* script)
    {
        if (g_scriptProfilerEnabled == 0 || script == nullptr || m_stack.empty())
            return;

        Frame frame = m_stack.back();
        m_stack.pop_back();

        const unsigned long long elapsed = NowUs() - frame.startUs;
        const unsigned long long self = elapsed >= frame.childUs ? elapsed - frame.childUs : 0;

        frame.entry->totalUs += elapsed;
        frame.entry->selfUs += self;

        m_totalUs += self;

        // Attribute this script's time to its caller as child time, so the
        // caller's self time excludes the nested work.
        if (!m_stack.empty())
            m_stack.back().childUs += elapsed;

        m_currentEntry = m_stack.empty() ? nullptr : m_stack.back().entry;
    }

    void CScriptProfiler::OnOpcode(CRunningScript* script)
    {
        if (g_scriptProfilerEnabled == 0)
            return;

        ++m_totalOpcodes;

        Entry* entry = m_currentEntry;
        if (entry == nullptr)
        {
            // Opcode executed outside a tracked Process() frame (for example a
            // script started from an opcode). Resolve lazily.
            if (script == nullptr)
                return;
            entry = &EntryFor(script);
            m_currentEntry = entry;
        }

        ++entry->opcodes;
    }

    void CScriptProfiler::OnScriptDeleted(CRunningScript* script)
    {
        if (script == nullptr)
            return;

        const uintptr_t key = reinterpret_cast<uintptr_t>(script);
        m_entries.erase(key);

        m_order.erase(
            std::remove(m_order.begin(), m_order.end(), key),
            m_order.end()
        );

        m_stack.erase(
            std::remove_if(m_stack.begin(), m_stack.end(),
                [key](const Frame& frame) { return frame.scriptPtr == key; }),
            m_stack.end()
        );

        m_currentEntry = m_stack.empty() ? nullptr : m_stack.back().entry;
    }

    bool CScriptProfiler::GetSnapshot(size_t index, Snapshot& out) const
    {
        if (index >= m_order.size())
            return false;

        const uintptr_t key = m_order[index];
        auto it = m_entries.find(key);
        if (it == m_entries.end())
            return false;

        const Entry& entry = it->second;
        out.scriptPtr = key;
        memcpy(out.name, entry.name, 9);
        out.totalUs = entry.totalUs;
        out.selfUs = entry.selfUs;
        out.opcodes = entry.opcodes;
        out.ticks = entry.ticks;
        return true;
    }
}
