#include "stdafx.h"
#include "CCleoMemoryManager.h"
#include "CDebugBridge.h"

namespace CLEO
{
    size_t CCleoMemoryManager::AlignToPage(size_t size) const
    {
        if (size == 0)
            return 0;

        const size_t page = m_pageSize ? m_pageSize : 4096;
        const size_t remainder = size % page;
        if (remainder == 0)
            return size;

        const size_t aligned = size + (page - remainder);
        if (aligned < size)
            return 0;

        return aligned;
    }

    void CCleoMemoryManager::LogStatsLocked(const char* reason) const
    {
        MEMORY_TRACE("%s reserved=%llu MB committed=%llu MB used=%llu MB free=%llu MB allocations=%llu",
            reason,
            static_cast<unsigned long long>(m_reserved / (1024 * 1024)),
            static_cast<unsigned long long>(m_committed / (1024 * 1024)),
            static_cast<unsigned long long>(m_used / (1024 * 1024)),
            static_cast<unsigned long long>((AbsoluteLimit - m_used) / (1024 * 1024)),
            static_cast<unsigned long long>(m_allocations.size()));
    }

    void* CCleoMemoryManager::Allocate(const void* owner, size_t size)
    {
        if (size == 0)
            return nullptr;

        const size_t committed = AlignToPage(size);
        if (committed == 0 || committed > AbsoluteLimit)
            return nullptr;

        {
            std::lock_guard<std::mutex> lock(m_mutex);

            if (size > AbsoluteLimit - m_used ||
                committed > AbsoluteLimit - m_committed)
            {
                MEMORY_TRACE("REJECT size=%llu MB limit=2048 MB used=%llu MB committed=%llu MB",
                    static_cast<unsigned long long>(size / (1024 * 1024)),
                    static_cast<unsigned long long>(m_used / (1024 * 1024)),
                    static_cast<unsigned long long>(m_committed / (1024 * 1024)));
                return nullptr;
            }
        }

        void* memory = VirtualAlloc(nullptr, committed, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!memory)
        {
            MEMORY_TRACE("OS_ALLOC_FAIL size=%llu KB error=%lu",
                static_cast<unsigned long long>(committed / 1024),
                static_cast<unsigned long>(GetLastError()));
            return nullptr;
        }

        std::lock_guard<std::mutex> lock(m_mutex);

        m_allocations.emplace(memory, Allocation{ size, committed, owner });
        m_reserved += committed;
        m_committed += committed;
        m_used += size;
        if (m_used > m_peakUsed)
            m_peakUsed = m_used;

        if (!m_workingMarkReported && m_used >= WorkingMark)
        {
            m_workingMarkReported = true;
            LogStatsLocked("WORKING_MARK");
        }

        return memory;
    }

    bool CCleoMemoryManager::Free(void* address)
    {
        if (!address)
            return true;

        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_allocations.find(address);
        if (it == m_allocations.end())
        {
            MEMORY_TRACE("FREE_REJECT address=0x%08X reason=unknown",
                static_cast<unsigned int>(reinterpret_cast<uintptr_t>(address)));
            return false;
        }

        const Allocation allocation = it->second;
        if (!VirtualFree(address, 0, MEM_RELEASE))
        {
            MEMORY_TRACE("FREE_FAIL address=0x%08X error=%lu",
                static_cast<unsigned int>(reinterpret_cast<uintptr_t>(address)),
                static_cast<unsigned long>(GetLastError()));
            return false;
        }

        m_reserved -= allocation.committed;
        m_committed -= allocation.committed;
        m_used -= allocation.requested;
        m_allocations.erase(it);

        if (m_used < WorkingMark)
            m_workingMarkReported = false;

        return true;
    }

    size_t CCleoMemoryManager::ReleaseOwner(const void* owner)
    {
        if (!owner)
            return 0;

        std::lock_guard<std::mutex> lock(m_mutex);
        size_t released = 0;

        for (auto it = m_allocations.begin(); it != m_allocations.end(); )
        {
            if (it->second.owner != owner)
            {
                ++it;
                continue;
            }

            void* address = it->first;
            const Allocation allocation = it->second;

            if (VirtualFree(address, 0, MEM_RELEASE))
            {
                m_reserved -= allocation.committed;
                m_committed -= allocation.committed;
                m_used -= allocation.requested;
                ++released;
                it = m_allocations.erase(it);
            }
            else
            {
                ++it;
            }
        }

        if (m_used < WorkingMark)
            m_workingMarkReported = false;

        if (released)
            LogStatsLocked("OWNER_RELEASE");

        return released;
    }

    CCleoMemoryManager::Stats CCleoMemoryManager::GetStats() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        Stats result;
        result.reserved = m_reserved;
        result.committed = m_committed;
        result.used = m_used;
        result.free = AbsoluteLimit - m_used;
        result.allocationCount = m_allocations.size();
        result.peakUsed = m_peakUsed;
        return result;
    }

    bool CCleoMemoryManager::Owns(void* address) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_allocations.find(address) != m_allocations.end();
    }

    void CCleoMemoryManager::Maintenance()
    {
        // Stage 1 intentionally does not perform speculative compaction or
        // background freeing. Every block is explicitly owned and released.
        // The maintenance entry point remains the boundary for future
        // frame-based cleanup/rebalance without touching the legacy executor.
    }

    CSmartMemoryEngine& GetSmartMemoryEngine()
    {
        static CSmartMemoryEngine engine;
        return engine;
    }

    void CSmartMemoryEngine::Tick()
    {
        ++m_tick;
        m_memory.Maintenance();
    }

    CCleoMemoryManager::Stats CSmartMemoryEngine::GetStats() const
    {
        return m_memory.GetStats();
    }
}
