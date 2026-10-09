#include "stdafx.h"
#include "CCleoMemoryManager.h"
#include "CDebugBridge.h"
#include "CTheScripts.h"

#include <cstdlib>
#include <malloc.h>

namespace CLEO
{
    CCleoMemoryManager::CCleoMemoryManager()
    {
        MEMORY_TRACE(
            "INIT limits blocks=%d size=%d MB",
            m_configLimitAllocationCount,
            m_configLimitAllocationSize / (1024 * 1024)
        );
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
        info.blocks.insert(address);

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
            scriptIt->second.blocks.erase(address);
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

        void* memory = std::calloc(size ? size : 1, 1);
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

        // Walk only this script's blocks instead of every tracked allocation,
        // so tearing down many scripts on reset stays linear.
        auto ownerIt = m_scriptAllocations.find(owner);
        if (ownerIt == m_scriptAllocations.end())
            return 0;

        for (void* address : ownerIt->second.blocks)
        {
            auto it = m_allocations.find(address);
            if (it == m_allocations.end())
                continue;

            std::free(address);

            if (m_totalBytes >= it->second.size)
                m_totalBytes -= it->second.size;
            else
                m_totalBytes = 0;

            ++released;
            m_allocations.erase(it);
        }

        m_scriptAllocations.erase(ownerIt);

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

        // swap() also releases the hash tables' bucket arrays, clear() keeps them.
        std::unordered_map<void*, Allocation>().swap(m_allocations);
        std::unordered_map<CRunningScript*, AllocationInfo>().swap(m_scriptAllocations);
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

    void ConfigureProcessMemory()
    {
        // Large Address Aware is a flag in gta_sa.exe's PE header; it is read
        // when the process is created and cannot be enabled from an ASI.
        auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(GetModuleHandle(nullptr));
        auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
            reinterpret_cast<const BYTE*>(dos) + dos->e_lfanew);
        const bool largeAddressAware =
            (nt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) != 0;
        MEMORY_TRACE("PROCESS large_address_aware=%d", largeAddressAware ? 1 : 0);

        // Windows starts every process with a ~200 KB minimum working set and
        // trims game pages under memory pressure, which shows up as stutter
        // when they are paged back in. Raise the soft limits; the *_DISABLE
        // flags keep them as hints, so Windows can still trim if RAM runs out.
        const SIZE_T desiredMin = 256u * 1024 * 1024;
        const SIZE_T desiredMax = 1024u * 1024 * 1024;

        HANDLE process = GetCurrentProcess();
        SIZE_T currentMin = 0, currentMax = 0;
        DWORD flags = 0;
        if (!GetProcessWorkingSetSizeEx(process, &currentMin, &currentMax, &flags))
            return;

        if (currentMin >= desiredMin)
            return;

        const SIZE_T newMax = currentMax > desiredMax ? currentMax : desiredMax;
        const BOOL ok = SetProcessWorkingSetSizeEx(
            process,
            desiredMin,
            newMax,
            QUOTA_LIMITS_HARDWS_MIN_DISABLE | QUOTA_LIMITS_HARDWS_MAX_DISABLE);

        MEMORY_TRACE(
            "PROCESS working_set min=%u->%u KB max=%u->%u KB result=%d error=%u",
            static_cast<unsigned>(currentMin / 1024),
            static_cast<unsigned>(desiredMin / 1024),
            static_cast<unsigned>(currentMax / 1024),
            static_cast<unsigned>(newMax / 1024),
            ok ? 1 : 0,
            ok ? 0u : static_cast<unsigned>(GetLastError()));
    }

    void CompactProcessHeaps()
    {
        HANDLE processHeap = GetProcessHeap();
        HANDLE crtHeap = reinterpret_cast<HANDLE>(_get_heap_handle());

        const SIZE_T largestFree = HeapCompact(processHeap, 0);
        if (crtHeap && crtHeap != processHeap)
            HeapCompact(crtHeap, 0);

        MEMORY_TRACE("HEAP_COMPACT largest_free_block=%u KB",
            static_cast<unsigned>(largestFree / 1024));
    }
}
