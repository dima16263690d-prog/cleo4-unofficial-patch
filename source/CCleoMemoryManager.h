#pragma once

#include <Windows.h>
#include "CTheScripts.h"
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace CLEO
{
    // Per-script script memory owned by CLEO instead of by GTA.
    //
    // allocate_memory (0AC8) hands out blocks from arena chunks that CLEO
    // obtains straight from VirtualAlloc. Because those chunks are not part of
    // the game's or the CRT heap, they do not fragment or pressure the 32-bit
    // address space of GTA, and Windows can page individual chunks out
    // independently of the game. Freed blocks return to per-size-class free
    // lists and are reused, so a script that churns memory every frame does not
    // keep calling the kernel.
    //
    // Bookkeeping is fully intrusive: every block carries its own header and the
    // owner's live blocks are a doubly-linked list threaded through those
    // headers. The hot path therefore performs NO heap/map insertion per
    // allocate - only the owner list head is touched, and only when the owner
    // changes. The single remaining map (owner -> list) has one entry per script,
    // not one per block.
    //
    // THREAD AFFINITY: everything here runs on GTA's single script thread
    // (allocate_memory / free_allocated_memory opcodes, the script-delete reclaim
    // tick, game begin/end). No lock is taken by design. The exported
    // CLEO_DebugGetMemoryStats() must only be called from that same thread.
    class CCleoMemoryManager
    {
    public:
        struct Stats
        {
            size_t totalBlocks = 0;
            size_t totalBytes = 0;
            size_t peakBytes = 0;       // peak of the CURRENT session (reset at GameEnd)
            size_t reservedBytes = 0;   // fast memory currently reserved
            size_t committedBytes = 0;  // fast memory currently committed
            size_t recyclableBytes = 0; // free blocks held for reuse
        };

        CCleoMemoryManager();
        ~CCleoMemoryManager();
        CCleoMemoryManager(const CCleoMemoryManager&) = delete;
        CCleoMemoryManager& operator=(const CCleoMemoryManager&) = delete;

        // Every request is honored at its full size. The block comes from
        // CLEO-owned fast memory, tracked per script so a script that stops
        // without freeing is reclaimed automatically.
        void* Allocate(CRunningScript* owner, size_t size);
        bool Free(CRunningScript* owner, void* address);

        // Release every tracked allocation belonging to one script. The blocks
        // return to the fast-memory free lists (not the OS), so resetting many
        // scripts is cheap and the reserved memory is reused by the next script.
        // Called when a script stops and again from its destructor as a net.
        size_t ReleaseOwner(CRunningScript* owner);

        // Called once when GTA/CLEO ends a game session: recycle everything,
        // release the fast-memory chunks back to the OS, and reset the peak.
        void OnGameEnd();

        // Game-thread only (see THREAD AFFINITY above).
        Stats GetStats() const;

        // One live allocation owner: the script pointer and how much it holds.
        struct OwnerStats
        {
            CRunningScript* owner;
            size_t blocks;
            size_t bytes;
        };

        // Game-thread only. Fills `out` with one entry per script that currently
        // owns at least one live block, largest first. Used to attribute a
        // growing script-memory total to the script responsible for it.
        void GetOwnerStats(std::vector<OwnerStats>& out) const;

    private:
        // Sizes up to kSmallMax use exact 16-byte classes; bigger requests use
        // 64-byte-granular single-block classes. Both are recycled.
        static constexpr size_t kSmallStep = 16;
        static constexpr size_t kSmallMax = 4096;
        static constexpr size_t kClassCount = kSmallMax / kSmallStep; // 256
        static constexpr size_t kChunkSize = 16 * 1024 * 1024;        // 16 MB
        static constexpr size_t kCommitGranularity = 64 * 1024;       // 64 KB
        static constexpr uint32_t kBlockMagic = 0xC1E0B10Cu;

        // Block header sits immediately before the payload the script sees. It is
        // not visible to scripts and not counted as payload, so scripts that
        // treat the pointer as opaque (the normal case) cannot notice it. While
        // the block is free, its first bytes hold the free-list link instead.
        // 32 bytes on Win32 (48 on 64-bit hosts): a multiple of 16 either way,
        // so every payload stays 16-byte aligned.
        struct BlockHeader
        {
            uint32_t magic = kBlockMagic;   // validates a freed pointer
            uint32_t payloadSize = 0;       // arena size (freed for reuse)
            uint32_t claimed = 0;           // bytes the script requested (accounting)
            uint32_t reserved = 0;          // pads the payload to 16-byte alignment
            CRunningScript* owner = nullptr;
            BlockHeader* prev = nullptr;    // owner's live list (intrusive)
            BlockHeader* next = nullptr;
            bool large = false;
            uint8_t tailPadding[3] = {};
        };

        static_assert(sizeof(BlockHeader) % 16 == 0,
            "BlockHeader must be a multiple of 16 bytes to keep payloads aligned");

        // One size class: a free list of equally-sized blocks and the class
        // size. While a block is free its first sizeof(void*) bytes hold the
        // next-free pointer (payloads are >= 16 bytes and 8-byte aligned).
        struct SizeClass
        {
            void* freeList = nullptr;
            size_t payloadSize = 0;
        };

        struct OwnerInfo
        {
            size_t blockCount = 0;
            size_t size = 0;
            BlockHeader* head = nullptr; // intrusive list of this owner's live blocks
        };

        struct Chunk
        {
            BYTE* base = nullptr;
            size_t reserved = 0;
            size_t committed = 0;
            size_t cursor = 0;
            size_t group = 0; // size class this chunk back-fills, or kAnyGroup
        };

        static constexpr size_t kAnyGroup = static_cast<size_t>(-1);

        SizeClass m_classes[kClassCount];
        std::unordered_map<size_t, SizeClass> m_largeClasses;

        std::vector<Chunk*> m_chunks;

        // One entry per script (not per block). Only touched when an owner gains
        // its first live block or loses its last one.
        std::unordered_map<CRunningScript*, OwnerInfo> m_owners;

        size_t m_totalBlocks = 0;
        size_t m_totalBytes = 0;
        size_t m_peakBytes = 0;
        size_t m_reservedBytes = 0;
        size_t m_committedBytes = 0;
        size_t m_recyclableBytes = 0;

        SizeClass& ClassFor(size_t payloadSize, bool large);
        void* TakeFree(size_t payloadSize, bool large);
        void* Bump(size_t payloadSize, bool large);
        void Claim(Chunk* chunk, size_t needed);
        void Recycle(BlockHeader* header);
        bool OwnsPointer(const void* payload) const;
        void LinkOwner(BlockHeader* header);
        void UnlinkOwner(BlockHeader* header);
        void ReleaseAllChunks();

        static size_t RoundUp(size_t value, size_t alignment);
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
