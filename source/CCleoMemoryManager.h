#pragma once

#include <Windows.h>
#include <cstddef>
#include <mutex>
#include <unordered_map>

namespace CLEO
{
    class CRunningScript;
    class CCleoMemoryManager
    {
    public:
        struct AllocationInfo
        {
            int count = 0;
            size_t size = 0;
        };

        struct Stats
        {
            size_t totalBlocks = 0;
            size_t totalBytes = 0;
            size_t peakBytes = 0;
            int configuredBlockLimit = 2000;
            int configuredSizeLimit = 16 * 1024 * 1024;
        };

        CCleoMemoryManager();
        CCleoMemoryManager(const CCleoMemoryManager&) = delete;
        CCleoMemoryManager& operator=(const CCleoMemoryManager&) = delete;

        // CLEO5-style memory operations.
        // Memory is tracked globally and additionally attributed to its script.
        void* Allocate(CRunningScript* owner, size_t size);
        bool Free(CRunningScript* owner, void* address);
        bool Forget(CRunningScript* owner, void* address);

        // Release every tracked allocation belonging to one script.
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

        int m_configLimitAllocationCount = 2000;
        int m_configLimitAllocationSize = 16 * 1024 * 1024;

        void RegisterMemoryAllocationLocked(CRunningScript* owner, void* address, size_t size);
        bool UnregisterMemoryAllocationLocked(void* address, bool freeMemory);
        void LogScriptWarningLocked(CRunningScript* owner, const char* reason) const;
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
}
