#pragma once

#include <Windows.h>
#include "CTheScripts.h"
#include <cstddef>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace CLEO
{
    class CCleoMemoryManager
    {
    public:
        struct AllocationInfo
        {
            int count = 0;
            size_t size = 0;
            std::unordered_set<void*> blocks;
        };

        struct Stats
        {
            size_t totalBlocks = 0;
            size_t totalBytes = 0;
            size_t peakBytes = 0;
        };

        CCleoMemoryManager();
        CCleoMemoryManager(const CCleoMemoryManager&) = delete;
        CCleoMemoryManager& operator=(const CCleoMemoryManager&) = delete;

        // CLEO5-style memory operations.
        // Every request is honored at its full size (a script that needs more
        // than any fixed budget simply gets it); memory is tracked per script so
        // a script that stops without freeing is reclaimed automatically.
        void* Allocate(CRunningScript* owner, size_t size);
        bool Free(CRunningScript* owner, void* address);

        // Release every tracked allocation belonging to one script.
        // Called when a script stops (and again from its destructor as a net).
        size_t ReleaseOwner(CRunningScript* owner);

        // Called once when GTA/CLEO ends a game session.
        void OnGameEnd();

        Stats GetStats() const;

    private:
        struct Allocation
        {
            size_t size = 0;
            CRunningScript* owner = nullptr;
        };

        mutable std::mutex m_mutex;
        std::unordered_map<void*, Allocation> m_allocations;
        std::unordered_map<CRunningScript*, AllocationInfo> m_scriptAllocations;

        size_t m_totalBytes = 0;
        size_t m_peakBytes = 0;

        void RegisterMemoryAllocationLocked(CRunningScript* owner, void* address, size_t size);
        bool UnregisterMemoryAllocationLocked(void* address);
        void LogRemainingLocked() const;

    };

    class CSmartMemoryEngine
    {
    public:
        CCleoMemoryManager& Memory() { return m_memory; }
        const CCleoMemoryManager& Memory() const { return m_memory; }

    private:
        CCleoMemoryManager m_memory;
    };

    CSmartMemoryEngine& GetSmartMemoryEngine();

    // Large Address Aware flag of gta_sa.exe (4 GB address space on x64 Windows).
    bool IsGameLargeAddressAware();

    // Ask Windows to keep more of the game resident (soft working-set limits).
    void ConfigureProcessMemory();

    // Return free heap pages to Windows after a game session was torn down.
    void CompactProcessHeaps();
}
