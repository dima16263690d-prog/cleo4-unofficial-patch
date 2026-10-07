#pragma once

#include <Windows.h>
#include <cstddef>
#include <mutex>
#include <unordered_map>

namespace CLEO
{
    class CCleoMemoryManager
    {
    public:
        static constexpr size_t WorkingMark = size_t(1) << 30;
        static constexpr size_t AbsoluteLimit = size_t(2) << 30;

        struct Stats
        {
            size_t reserved = 0;
            size_t committed = 0;
            size_t used = 0;
            size_t free = AbsoluteLimit;
            size_t allocationCount = 0;
            size_t peakUsed = 0;
        };

        CCleoMemoryManager() = default;
        CCleoMemoryManager(const CCleoMemoryManager&) = delete;
        CCleoMemoryManager& operator=(const CCleoMemoryManager&) = delete;

        void* Allocate(const void* owner, size_t size);
        bool Free(void* address);
        size_t ReleaseOwner(const void* owner);

        Stats GetStats() const;
        bool Owns(void* address) const;
        void Maintenance();

    private:
        struct Allocation
        {
            size_t requested = 0;
            size_t committed = 0;
            const void* owner = nullptr;
        };

        mutable std::mutex m_mutex;
        std::unordered_map<void*, Allocation> m_allocations;

        size_t m_reserved = 0;
        size_t m_committed = 0;
        size_t m_used = 0;
        size_t m_peakUsed = 0;
        size_t m_pageSize = 4096;
        bool m_workingMarkReported = false;

        size_t PageSize() const { return m_pageSize; }
        size_t AlignToPage(size_t size) const;
        void LogStatsLocked(const char* reason) const;
    };

    class CSmartMemoryEngine
    {
    public:
        CCleoMemoryManager& Memory() { return m_memory; }
        const CCleoMemoryManager& Memory() const { return m_memory; }

        void Tick();
        CCleoMemoryManager::Stats GetStats() const;

    private:
        CCleoMemoryManager m_memory;
        unsigned long long m_tick = 0;
    };

    CSmartMemoryEngine& GetSmartMemoryEngine();
}
