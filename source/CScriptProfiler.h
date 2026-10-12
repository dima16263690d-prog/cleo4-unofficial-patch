#pragma once

// Per-script execution profiler.
//
// Times the CCustomScript::Process() call for every script and counts the
// opcodes executed at the dispatch boundary. The runtime is single-threaded
// (GTA's active-script queue runs on the game thread), so no locking is done
// on the hot path; snapshots are meant to be read from the game thread too.

#include "CTheScripts.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <windows.h>

namespace CLEO
{
    // Zero-overhead enable flag read by the hot paths. Declared before the
    // class so IsEnabled() can use it.
    extern volatile LONG g_scriptProfilerEnabled;

    class CScriptProfiler
    {
    public:
        struct Snapshot
        {
            uintptr_t scriptPtr;
            char name[9];
            unsigned long long totalUs;
            unsigned long long selfUs;
            unsigned long long opcodes;
            unsigned long long ticks;
        };

        static CScriptProfiler& Get();

        bool IsEnabled() const { return g_scriptProfilerEnabled != 0; }
        // Returns the previous enabled state so callers can restore it.
        bool SetEnabled(bool enabled);
        void Reset();

        void BeginScript(CRunningScript* script);
        void EndScript(CRunningScript* script);
        void OnOpcode(CRunningScript* script);

        // Drop a script's record so deleted scripts do not linger.
        void OnScriptDeleted(CRunningScript* script);

        // True when the script already has an open timing frame. Used to avoid
        // double-counting a direct ProcessScript() call that bypasses the hook.
        bool InFrame(CRunningScript* script) const
        {
            const uintptr_t key = reinterpret_cast<uintptr_t>(script);
            for (const Frame& frame : m_stack)
                if (frame.scriptPtr == key)
                    return true;
            return false;
        }

        size_t GetCount() const { return m_order.size(); }
        bool GetSnapshot(size_t index, Snapshot& out) const;

        unsigned long long GetTotalOpcodeCount() const { return m_totalOpcodes; }
        unsigned long long GetTotalUs() const { return m_totalUs; }

    private:
        struct Entry
        {
            char name[9] = "none";
            unsigned long long totalUs = 0;
            unsigned long long selfUs = 0;
            unsigned long long opcodes = 0;
            unsigned long long ticks = 0;
        };

        struct Frame
        {
            uintptr_t scriptPtr;
            Entry* entry;
            unsigned long long startUs;
            unsigned long long childUs;
        };

        Entry& EntryFor(CRunningScript* script);

        static unsigned long long NowUs();

        std::unordered_map<uintptr_t, Entry> m_entries;
        std::vector<uintptr_t> m_order;
        std::vector<Frame> m_stack;

        // Cached current-script entry so OnOpcode() never touches the map.
        Entry* m_currentEntry = nullptr;

        unsigned long long m_totalOpcodes = 0;
        unsigned long long m_totalUs = 0;
    };

}
