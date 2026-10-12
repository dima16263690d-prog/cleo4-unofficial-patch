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
    // Everything here runs on GTA's single script thread. No lock is taken on
    // the hot path by design.
    class CCleoMemoryManager
    {
    public:
        struct Stats
        {
            size_t totalBlocks = 0;
            size_t totalBytes = 0;
            size_t peakBytes = 0;
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

        // Called once when GTA/CLEO ends a game session: recycle everything and
        // release the fast-memory chunks back to the OS.
        void OnGameEnd();

        Stats GetStats() const;

    private:
        // Sizes up to kSmallMax use exact 16-byte classes; bigger requests use
        // 64-byte-granular single-block classes. Both are recycled.
        static constexpr size_t kSmallStep = 16;
        static constexpr size_t kSmallMax = 4096;
        static constexpr size_t kClassCount = kSmallMax / kSmallStep; // 256
        static constexpr size_t kChunkSize = 16 * 1024 * 1024;        // 16 MB
        static constexpr size_t kCommitGranularity = 64 * 1024;       // 64 KB
        static constexpr size_t kAnyGroup = static_cast<size_t>(-1);

        // One size class: a free list of equally-sized blocks and the class
        // size. While a block is free its first sizeof(void*) bytes hold the
        // next-free pointer (blocks are >= 16 bytes and 8-byte aligned).
        struct SizeClass
        {
            void* freeList = nullptr;
            size_t blockSize = 0;
        };

        struct Block
        {
            CRunningScript* owner = nullptr;
            uint32_t blockSize = 0; // arena bytes (freed for reuse)
            uint32_t claimed = 0;   // bytes the script requested (for accounting)
            bool large = false;
        };

        struct OwnerInfo
        {
            size_t blockCount = 0;
            size_t size = 0;
            std::unordered_map<void*, Block> blocks; // lets ReleaseOwner walk only this script
        };

        struct Chunk
        {
            BYTE* base = nullptr;
            size_t reserved = 0;
            size_t committed = 0;
            size_t cursor = 0;
            size_t group = 0; // size class this chunk back-fills, or kAnyGroup
        };

        // Live blocks (payload pointer -> bookkeeping).
        std::unordered_map<void*, Block> m_blocks;

        SizeClass m_classes[kClassCount];
        std::unordered_map<size_t, SizeClass> m_largeClasses;

        std::vector<Chunk*> m_chunks;

        // Per-owner index. Values hold a copy of each block so a release walk
        // stays local; the authoritative owner/size lives in m_blocks.
        std::unordered_map<CRunningScript*, OwnerInfo> m_owners;

        size_t m_totalBytes = 0;
        size_t m_peakBytes = 0;
        size_t m_reservedBytes = 0;
        size_t m_committedBytes = 0;
        size_t m_recyclableBytes = 0;

        SizeClass& ClassFor(size_t blockSize, bool large);
        void* TakeFree(size_t blockSize, bool large);
        void* Bump(size_t blockSize, bool large);
        void Claim(Chunk* chunk, size_t needed);
        void Recycle(void* payload, size_t blockSize, bool large);
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
