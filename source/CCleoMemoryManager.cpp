#include "stdafx.h"
#include "CCleoMemoryManager.h"
#include "CDebugBridge.h"
#include "CTheScripts.h"

#include <cstdlib>
#include <malloc.h>
#include <cstring>
#include <vector>

namespace CLEO
{
    CCleoMemoryManager::CCleoMemoryManager()
    {
        MEMORY_TRACE("INIT fast memory enabled (VirtualAlloc arenas, intrusive tracking)");
    }

    CCleoMemoryManager::~CCleoMemoryManager()
    {
        ReleaseAllChunks();
    }

    size_t CCleoMemoryManager::RoundUp(size_t value, size_t alignment)
    {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    CCleoMemoryManager::SizeClass& CCleoMemoryManager::ClassFor(size_t payloadSize, bool large)
    {
        if (!large)
            return m_classes[payloadSize / kSmallStep - 1];
        return m_largeClasses[payloadSize];
    }

    void CCleoMemoryManager::Claim(Chunk* chunk, size_t needed)
    {
        if (needed > chunk->reserved)
            needed = chunk->reserved;

        // Commit in 64 KB granules so the first request maps whole granules and
        // later growth in the same chunk is extended in place.
        const size_t end = RoundUp(chunk->cursor + needed, kCommitGranularity);
        const size_t target = end < chunk->reserved ? end : chunk->reserved;

        if (target <= chunk->committed)
            return;

        const size_t toCommit = target - chunk->committed;
        if (VirtualAlloc(
                chunk->base + chunk->committed,
                toCommit,
                MEM_COMMIT,
                PAGE_READWRITE) != nullptr)
        {
            chunk->committed = target;
            m_committedBytes += toCommit;
        }
    }

    void* CCleoMemoryManager::TakeFree(size_t payloadSize, bool large)
    {
        SizeClass& cls = ClassFor(payloadSize, large);
        void* link = cls.freeList;
        if (link == nullptr)
            return nullptr;

        // The free block's first bytes are the next-free link.
        cls.freeList = *reinterpret_cast<void**>(link);
        m_recyclableBytes -= payloadSize;
        return link;
    }

    void* CCleoMemoryManager::Bump(size_t payloadSize, bool large)
    {
        const size_t payloadOffset = sizeof(BlockHeader);
        const size_t total = payloadOffset + payloadSize;

        // Try any chunk back-filling this class that still has room. A chunk
        // that cannot fit (or whose next commit fails) is skipped, not the whole
        // search, so a class does not accumulate extra chunks needlessly.
        for (auto it = m_chunks.rbegin(); it != m_chunks.rend(); ++it)
        {
            Chunk* chunk = *it;
            if (chunk->group != (large ? kAnyGroup : payloadSize))
                continue;
            if (chunk->cursor + total > chunk->reserved)
                continue;

            Claim(chunk, total);
            if (chunk->cursor + total > chunk->committed)
                continue; // no commit room here; try another chunk

            void* payload = chunk->base + chunk->cursor + payloadOffset;
            chunk->cursor += total;
            return payload;
        }

        // Fresh chunk, sized to a batch of same-size blocks (capped at the chunk
        // size; a block larger than the chunk reserves at least itself).
        const size_t expectedBlocks = large ? 48 : 256;
        size_t reserve = RoundUp(total * expectedBlocks, kCommitGranularity);
        if (reserve > kChunkSize)
            reserve = kChunkSize;
        if (reserve < total)
            reserve = RoundUp(total, kCommitGranularity);
        if (reserve < total)
            return nullptr; // reservation overflow (request is absurdly large)

        BYTE* base = reinterpret_cast<BYTE*>(VirtualAlloc(
            nullptr, reserve, MEM_RESERVE, PAGE_READWRITE));
        if (base == nullptr)
            return nullptr;

        Chunk* chunk = new Chunk();
        chunk->base = base;
        chunk->reserved = reserve;
        chunk->committed = 0;
        chunk->cursor = 0;
        chunk->group = large ? kAnyGroup : payloadSize;

        m_chunks.push_back(chunk);
        m_reservedBytes += reserve;

        Claim(chunk, total);
        if (chunk->committed < total)
        {
            // Could not commit even the first block: drop the chunk.
            VirtualFree(base, 0, MEM_RELEASE);
            m_reservedBytes -= reserve;
            m_chunks.pop_back();
            delete chunk;
            return nullptr;
        }

        void* payload = base + payloadOffset;
        chunk->cursor = total;
        return payload;
    }

    bool CCleoMemoryManager::OwnsPointer(const void* payload) const
    {
        // Validate a freed pointer by chunk range first (nothing points into a
        // CLEO arena except our own blocks), then confirm the header magic.
        const BYTE* p = static_cast<const BYTE*>(payload);
        for (const Chunk* chunk : m_chunks)
        {
            if (p >= chunk->base && p < chunk->base + chunk->reserved)
                return true;
        }
        return false;
    }

    void CCleoMemoryManager::LinkOwner(BlockHeader* header)
    {
        OwnerInfo& info = m_owners[header->owner];
        header->prev = nullptr;
        header->next = info.head;
        if (info.head != nullptr)
            info.head->prev = header;
        info.head = header;
        ++info.blockCount;
        info.size += header->claimed;
    }

    void CCleoMemoryManager::UnlinkOwner(BlockHeader* header)
    {
        auto it = m_owners.find(header->owner);
        if (it == m_owners.end())
            return;

        OwnerInfo& info = it->second;
        if (header->prev != nullptr)
            header->prev->next = header->next;
        else
            info.head = header->next;
        if (header->next != nullptr)
            header->next->prev = header->prev;

        if (info.blockCount > 0)
            --info.blockCount;
        info.size = info.size >= header->claimed ? info.size - header->claimed : 0;
        if (info.blockCount == 0)
            m_owners.erase(it);
    }

    void* CCleoMemoryManager::Allocate(CRunningScript* owner, size_t size)
    {
        if (size == 0)
        {
            // CLEO5 explicitly removed the prohibition on zero-sized blocks.
            // Keep the tracker consistent and return nullptr.
            MEMORY_TRACE("ALLOC size=0 -> null");
            return nullptr;
        }

        const bool large = size > kSmallMax;
        const size_t payloadSize = large ? RoundUp(size, 64) : RoundUp(size, kSmallStep);

        // Reuse a freed block of the same size if available, otherwise carve one
        // from a chunk. Reuse keeps the arenas bounded under churn.
        void* payload = TakeFree(payloadSize, large);
        const bool reused = payload != nullptr;
        if (payload == nullptr)
            payload = Bump(payloadSize, large);

        if (payload == nullptr)
        {
            MEMORY_TRACE(
                "ALLOC_FAIL script='%.8s' size=%llu bytes",
                owner ? owner->GetName() : "unknown",
                static_cast<unsigned long long>(size)
            );
            return nullptr;
        }

        BlockHeader* header = reinterpret_cast<BlockHeader*>(
            static_cast<BYTE*>(payload) - sizeof(BlockHeader));
        const size_t total = sizeof(BlockHeader) + payloadSize;

        // Script-visible bytes are always zeroed (calloc semantics). A recycled
        // block additionally zeroes the whole arena block so a later same-class
        // reuse starts clean even if the previous request was larger than this
        // one (payloads are 16-byte multiples, so the total covers any header
        // bytes that could otherwise survive).
        memset(payload, 0, reused ? total : size);

        header->magic = kBlockMagic;
        header->payloadSize = static_cast<uint32_t>(payloadSize);
        header->claimed = static_cast<uint32_t>(size);
        header->large = large;
        header->owner = owner;
        header->prev = nullptr;
        header->next = nullptr;

        LinkOwner(header);

        ++m_totalBlocks;
        m_totalBytes += size;
        if (m_totalBytes > m_peakBytes)
            m_peakBytes = m_totalBytes;

        return payload;
    }

    void CCleoMemoryManager::Recycle(BlockHeader* header)
    {
        void* payload = reinterpret_cast<BYTE*>(header) + sizeof(BlockHeader);
        SizeClass& cls = ClassFor(header->payloadSize, header->large);
        *reinterpret_cast<void**>(payload) = cls.freeList;
        cls.freeList = payload;
        m_recyclableBytes += header->payloadSize;
    }

    bool CCleoMemoryManager::Free(CRunningScript* owner, void* address)
    {
        if (address == nullptr)
            return true;

        if (!OwnsPointer(address))
        {
            MEMORY_TRACE(
                "FREE_REJECT script='%.8s' address=0x%08X reason=outside_arena",
                owner ? owner->GetName() : "unknown",
                reinterpret_cast<unsigned>(address)
            );
            return false;
        }

        BlockHeader* header = reinterpret_cast<BlockHeader*>(
            static_cast<BYTE*>(address) - sizeof(BlockHeader));
        if (header->magic != kBlockMagic)
        {
            MEMORY_TRACE(
                "FREE_REJECT script='%.8s' address=0x%08X reason=already_freed_or_corrupt",
                owner ? owner->GetName() : "unknown",
                reinterpret_cast<unsigned>(address)
            );
            return false;
        }

        const size_t claimed = header->claimed;

        // Invalidate first so a double free is caught by the magic check even
        // though the payload now carries a free-list link.
        header->magic = 0;
        UnlinkOwner(header);
        Recycle(header);

        if (m_totalBlocks > 0)
            --m_totalBlocks;
        if (m_totalBytes >= claimed)
            m_totalBytes -= claimed;
        else
            m_totalBytes = 0;

        return true;
    }

    size_t CCleoMemoryManager::ReleaseOwner(CRunningScript* owner)
    {
        if (owner == nullptr)
            return 0;

        auto ownerIt = m_owners.find(owner);
        if (ownerIt == m_owners.end())
            return 0;

        size_t released = 0;

        // Walk this script's intrusive list; each block returns to its size-class
        // free list for reuse. No VirtualFree here, so resetting scripts on a
        // mode change stays cheap.
        BlockHeader* header = ownerIt->second.head;
        while (header != nullptr)
        {
            BlockHeader* next = header->next;
            const size_t claimed = header->claimed;

            if (m_totalBytes >= claimed)
                m_totalBytes -= claimed;
            else
                m_totalBytes = 0;
            if (m_totalBlocks > 0)
                --m_totalBlocks;

            Recycle(header);
            ++released;
            header = next;
        }

        m_owners.erase(ownerIt);

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

    void CCleoMemoryManager::ReleaseAllChunks()
    {
        // Blocks live inside the chunks, so releasing the chunks frees them all;
        // the owner map only needs to be dropped.
        m_owners.clear();

        for (Chunk* chunk : m_chunks)
        {
            if (chunk == nullptr)
                continue;
            if (chunk->base != nullptr)
                VirtualFree(chunk->base, 0, MEM_RELEASE);
            delete chunk;
        }
        m_chunks.clear();

        for (size_t i = 0; i < kClassCount; ++i)
            m_classes[i].freeList = nullptr;
        m_largeClasses.clear();

        m_reservedBytes = 0;
        m_committedBytes = 0;
        m_recyclableBytes = 0;
        m_totalBlocks = 0;
        m_totalBytes = 0;
        m_peakBytes = 0;
    }

    void CCleoMemoryManager::OnGameEnd()
    {
        MEMORY_TRACE(
            "GAME_END reset live_blocks=%llu bytes=%llu KB reserved=%llu KB",
            static_cast<unsigned long long>(m_totalBlocks),
            static_cast<unsigned long long>(m_totalBytes / 1024),
            static_cast<unsigned long long>(m_reservedBytes / 1024)
        );

        // Per-script cleanup already ran for every destroyed script, so a full
        // arena teardown here both reclaims anything left and returns the whole
        // fast-memory reservation to Windows for the next session. The peak is
        // reset too: it reports the peak of the session that just ended.
        ReleaseAllChunks();

        MEMORY_TRACE("GAME_END cleanup_complete");
    }

    CCleoMemoryManager::Stats CCleoMemoryManager::GetStats() const
    {
        Stats result;
        result.totalBlocks = m_totalBlocks;
        result.totalBytes = m_totalBytes;
        result.peakBytes = m_peakBytes;
        result.reservedBytes = m_reservedBytes;
        result.committedBytes = m_committedBytes;
        result.recyclableBytes = m_recyclableBytes;
        return result;
    }

    CSmartMemoryEngine& GetSmartMemoryEngine()
    {
        static CSmartMemoryEngine engine;
        return engine;
    }

    bool IsGameLargeAddressAware()
    {
        // Large Address Aware is a flag in gta_sa.exe's PE header; it is read
        // when the process is created and cannot be enabled from an ASI.
        auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(GetModuleHandle(nullptr));
        auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
            reinterpret_cast<const BYTE*>(dos) + dos->e_lfanew);
        return (nt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) != 0;
    }

    void ConfigureProcessMemory()
    {
        const bool laa = IsGameLargeAddressAware();
        MEMORY_TRACE("PROCESS large_address_aware=%d", laa ? 1 : 0);

        // Soft working-set hints. Windows still may trim under real RAM pressure,
        // but a higher minimum reduces page-in stutter after alt-tab / low memory.
        // LAA builds can hold more resident data safely in a 4 GB address space.
        const SIZE_T desiredMin = (laa ? 384u : 256u) * 1024 * 1024;
        const SIZE_T desiredMax = (laa ? 1536u : 1024u) * 1024 * 1024;

        HANDLE process = GetCurrentProcess();
        SIZE_T currentMin = 0, currentMax = 0;
        DWORD flags = 0;
        if (!GetProcessWorkingSetSizeEx(process, &currentMin, &currentMax, &flags))
            return;

        const SIZE_T newMin = currentMin > desiredMin ? currentMin : desiredMin;
        const SIZE_T newMax = currentMax > desiredMax ? currentMax : desiredMax;

        if (newMin != currentMin || newMax != currentMax)
        {
            const BOOL ok = SetProcessWorkingSetSizeEx(
                process,
                newMin,
                newMax,
                QUOTA_LIMITS_HARDWS_MIN_DISABLE | QUOTA_LIMITS_HARDWS_MAX_DISABLE);

            MEMORY_TRACE(
                "PROCESS working_set min=%u->%u KB max=%u->%u KB result=%d error=%u",
                static_cast<unsigned>(currentMin / 1024),
                static_cast<unsigned>(newMin / 1024),
                static_cast<unsigned>(currentMax / 1024),
                static_cast<unsigned>(newMax / 1024),
                ok ? 1 : 0,
                ok ? 0u : static_cast<unsigned>(GetLastError()));
        }

        // Prefer keeping game pages over background processes when the system
        // trims the working set (Win8+). Failure is non-fatal on older OS.
        using SetProcessInformationFn = BOOL(WINAPI*)(HANDLE, PROCESS_INFORMATION_CLASS, PVOID, DWORD);
        static const auto pSetProcessInformation = reinterpret_cast<SetProcessInformationFn>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetProcessInformation"));
        if (pSetProcessInformation)
        {
            // MEMORY_PRIORITY_NORMAL = 5
            struct { ULONG MemoryPriority; } priorityInfo{};
            priorityInfo.MemoryPriority = 5;
            // ProcessMemoryPriority == 40 on Windows 8+
            if (pSetProcessInformation(
                    process,
                    static_cast<PROCESS_INFORMATION_CLASS>(40),
                    &priorityInfo,
                    sizeof(priorityInfo)))
            {
                MEMORY_TRACE("PROCESS memory_priority=NORMAL");
            }
        }
    }

    void CompactProcessHeaps()
    {
        // After a full session teardown, return free pages to the OS so the
        // next load starts from a cleaner heap (less fragmentation / paging).
        HANDLE processHeap = GetProcessHeap();
        HANDLE crtHeap = reinterpret_cast<HANDLE>(_get_heap_handle());

        const SIZE_T largestFree = HeapCompact(processHeap, 0);
        if (crtHeap && crtHeap != processHeap)
            HeapCompact(crtHeap, 0);

        // CRT malloc arena: release empty pages when possible.
        _heapmin();

        MEMORY_TRACE("HEAP_COMPACT largest_free_block=%u KB",
            static_cast<unsigned>(largestFree / 1024));
    }
}
