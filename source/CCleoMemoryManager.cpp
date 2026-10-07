#include "stdafx.h"
#include "CCleoMemoryManager.h"
#include "CDebugBridge.h"
#include "CDebugCallbackSystem.h"
#include "CTheScripts.h"

#include <cstdlib>

namespace CLEO
{
    CCleoMemoryManager::CCleoMemoryManager()
    {
        RegisterCallback(CLEO_CB_GAME_END, reinterpret_cast<uintptr_t>(&GameEndCallback));

        MEMORY_TRACE(
            "INIT limits blocks=%d size=%d MB",
            m_configLimitAllocationCount,
            m_configLimitAllocationSize / (1024 * 1024)
        );
    }

    void __stdcall CCleoMemoryManager::GameEndCallback()
    {
        GetSmartMemoryEngine().Memory().OnGameEnd();
    }

    void CCleoMemoryManager::RegisterMemoryAllocationLocked(
        CRunningScript* owner,
        void* address,
        size_t size
    )
    {
        m_allocations[address] = Allocation{ size, owner };

        auto& info = m_scriptAllocations[owner];
        ++info.count;
        info.size += size;

        m_totalBytes += size;
        if (m_totalBytes > m_peakBytes)
            m_peakBytes = m_totalBytes;
    }

    bool CCleoMemoryManager::UnregisterMemoryAllocationLocked(void* address, bool freeMemory)
    {
        auto it = m_allocations.find(address);
        if (it == m_allocations.end())
            return false;

        const Allocation allocation = it->second;

        if (freeMemory)
        {
            std::free(address);
        }

        auto scriptIt = m_scriptAllocations.find(allocation.owner);
        if (scriptIt != m_scriptAllocations.end())
        {
            scriptIt->second.count--;
            if (scriptIt->second.size >= allocation.size)
                scriptIt->second.size -= allocation.size;
            else
                scriptIt->second.size = 0;

            if (scriptIt->second.count <= 0)
                m_scriptAllocations.erase(scriptIt);
        }

        if (m_totalBytes >= allocation.size)
            m_totalBytes -= allocation.size;
        else
            m_totalBytes = 0;

        m_allocations.erase(it);
        return true;
    }

    void CCleoMemoryManager::LogScriptWarningLocked(
        CRunningScript* owner,
        const char* reason
    ) const
    {
        const auto it = m_scriptAllocations.find(owner);
        if (it == m_scriptAllocations.end())
            return;

        const char* scriptName = owner ? owner->GetName() : "unknown";

        MEMORY_TRACE(
            "WARNING %s script='%.8s' blocks=%d size=%llu KB",
            reason,
            scriptName,
            it->second.count,
            static_cast<unsigned long long>(it->second.size / 1024)
        );
    }

    void CCleoMemoryManager::LogRemainingLocked() const
    {
        MEMORY_TRACE(
            "GAME_END remaining_blocks=%llu remaining_bytes=%llu KB",
            static_cast<unsigned long long>(m_allocations.size()),
            static_cast<unsigned long long>(m_totalBytes / 1024)
        );

        for (const auto& entry : m_scriptAllocations)
        {
            CRunningScript* owner = entry.first;
            const AllocationInfo& info = entry.second;

            if (info.count <= 0)
                continue;

            MEMORY_TRACE(
                "LEAK script='%.8s' blocks=%d size=%llu KB",
                owner ? owner->GetName() : "unknown",
                info.count,
                static_cast<unsigned long long>(info.size / 1024)
            );
        }
    }

    void* CCleoMemoryManager::Allocate(CRunningScript* owner, size_t size)
    {
        if (size == 0)
        {
            // CLEO5 explicitly removed the prohibition on zero-sized blocks.
            // calloc(0, 1) is implementation-defined, so return nullptr here
            // while keeping the tracker consistent.
            MEMORY_TRACE("ALLOC size=0 -> null");
            return nullptr;
        }

        void* memory = std::calloc(1, size);
        if (!memory)
        {
            MEMORY_TRACE(
                "ALLOC_FAIL script='%.8s' size=%llu bytes",
                owner ? owner->GetName() : "unknown",
                static_cast<unsigned long long>(size)
            );
            return nullptr;
        }

        std::lock_guard<std::mutex> lock(m_mutex);

        RegisterMemoryAllocationLocked(owner, memory, size);

        auto it = m_scriptAllocations.find(owner);
        if (it != m_scriptAllocations.end())
        {
            if (m_configLimitAllocationSize > 0 &&
                it->second.size > static_cast<size_t>(m_configLimitAllocationSize))
            {
                LogScriptWarningLocked(owner, "size_limit_exceeded");
            }
            else if (
                m_configLimitAllocationCount > 0 &&
                it->second.count > m_configLimitAllocationCount)
            {
                LogScriptWarningLocked(owner, "block_limit_exceeded");
            }
        }

        return memory;
    }

    bool CCleoMemoryManager::Free(CRunningScript* owner, void* address)
    {
        if (!address)
            return true;

        std::lock_guard<std::mutex> lock(m_mutex);

        const auto it = m_allocations.find(address);
        if (it == m_allocations.end())
        {
            MEMORY_TRACE(
                "FREE_REJECT script='%.8s' address=0x%08X reason=unknown_or_already_freed",
                owner ? owner->GetName() : "unknown",
                static_cast<unsigned int>(reinterpret_cast<uintptr_t>(address))
            );
            return false;
        }

        return UnregisterMemoryAllocationLocked(address, true);
    }

    bool CCleoMemoryManager::Forget(CRunningScript* owner, void* address)
    {
        if (!address)
            return true;

        std::lock_guard<std::mutex> lock(m_mutex);

        const auto it = m_allocations.find(address);
        if (it == m_allocations.end())
        {
            MEMORY_TRACE(
                "FORGET_REJECT script='%.8s' address=0x%08X reason=unknown_or_already_freed",
                owner ? owner->GetName() : "unknown",
                static_cast<unsigned int>(reinterpret_cast<uintptr_t>(address))
            );
            return false;
        }

        // Deliberately do not free() the memory. This is the CLEO5
        // distinction between forget_memory and free_memory.
        return UnregisterMemoryAllocationLocked(address, false);
    }

    size_t CCleoMemoryManager::ReleaseOwner(CRunningScript* owner)
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

            std::free(address);

            if (m_totalBytes >= allocation.size)
                m_totalBytes -= allocation.size;
            else
                m_totalBytes = 0;

            ++released;
            it = m_allocations.erase(it);
        }

        m_scriptAllocations.erase(owner);

        if (released)
        {
            MEMORY_TRACE(
                "OWNER_RELEASE script='%.8s' blocks=%llu",
                owner->GetName(),
                static_cast<unsigned long long>(released)
            );
        }

        return released;
    }

    void CCleoMemoryManager::OnGameEnd()
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        LogRemainingLocked();

        for (const auto& entry : m_allocations)
            std::free(entry.first);

        m_allocations.clear();
        m_scriptAllocations.clear();
        m_totalBytes = 0;

        MEMORY_TRACE("GAME_END cleanup_complete peak=%llu KB",
            static_cast<unsigned long long>(m_peakBytes / 1024));
    }

    CCleoMemoryManager::Stats CCleoMemoryManager::GetStats() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        Stats result;
        result.totalBlocks = m_allocations.size();
        result.totalBytes = m_totalBytes;
        result.peakBytes = m_peakBytes;
        result.configuredBlockLimit = m_configLimitAllocationCount;
        result.configuredSizeLimit = m_configLimitAllocationSize;
        return result;
    }

    CSmartMemoryEngine& GetSmartMemoryEngine()
    {
        static CSmartMemoryEngine engine;
        return engine;
    }
}
