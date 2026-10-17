#include "DebugUtils.h"
#include "DebugUtils.Internal.h"
#include "resource.h"
#include <windows.h>
#include <psapi.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

// Per-script memory-leak tracker.
//
// The CLEO script-memory arena (allocate_memory 0AC8 / free_allocated_memory
// 0AC9) is tracked per script, and CLEO_DebugGetScriptMemoryOwners exposes that
// breakdown. This tracker samples it on an interval and keeps a per-script
// history so a script whose live allocation only ever grows can be named.
//
// It answers the two questions that matter when a leak is suspected:
//   * which script (or script name) is holding the growth, and
//   * how much, in bytes and as a percentage of the arena and of the process.
//
// Verdicts are conservative and explained in the report:
//   LEAK       - monotonic growth over several samples, never released.
//   SUSPECTED  - net growth over several samples but not strictly monotonic.
//   STABLE     - it grows and shrinks (normal churn) or never moved.
//   RELEASED   - it held memory before and has since dropped back to zero.

namespace
{
    // Minimum number of samples with net growth before a script is called a leak.
    constexpr DWORD kLeakMinSamples = 3;
}

void DebugUtils::UpdateLeakEntry(
    uintptr_t scriptPtr,
    const std::string& name,
    unsigned long long blocks,
    unsigned long long bytes
)
{
    auto it = m_leakScripts.find(scriptPtr);

    if (it == m_leakScripts.end())
    {
        LeakScriptState state;
        state.name = name;
        state.firstBytes = bytes;
        state.lastBytes = bytes;
        state.peakBytes = bytes;
        state.firstBlocks = blocks;
        state.lastBlocks = blocks;
        state.firstUptimeSec =
            (GetTickCount() - m_memorySessionStartTick) / 1000u;
        state.lastUptimeSec = state.firstUptimeSec;
        state.samples = 1;
        m_leakScripts.emplace(scriptPtr, std::move(state));
        return;
    }

    LeakScriptState& state = it->second;
    if (!name.empty() && name != "none")
        state.name = name;

    state.lastUptimeSec = (GetTickCount() - m_memorySessionStartTick) / 1000u;
    ++state.samples;

    if (bytes > state.peakBytes)
        state.peakBytes = bytes;

    if (bytes > state.lastBytes)
    {
        ++state.growthStreak;
        state.flatStreak = 0;
    }
    else if (bytes == state.lastBytes)
    {
        ++state.flatStreak;
    }
    else
    {
        state.growthStreak = 0;
        state.flatStreak = 0;
    }

    state.lastBytes = bytes;
    state.lastBlocks = blocks;
}

void DebugUtils::SampleScriptMemoryLeaks()
{
    if (!m_leakTrackerEnabled || !m_memoryLogEnabled)
        return;

    // Owner enumeration has the same game-thread affinity as the memory stats.
    DWORD ownerCount = CLEO_DebugGetScriptMemoryOwners(nullptr, 0);
    if (ownerCount == 0)
    {
        // No live script allocations: retire any tracked script that is gone.
        for (auto it = m_leakScripts.begin(); it != m_leakScripts.end();)
        {
            if (!it->second.name.empty())
            {
                LeakScriptState retired = it->second;
                retired.releasedSince = true;
                m_leakRetired[it->second.name] = std::move(retired);
            }
            it = m_leakScripts.erase(it);
        }
        return;
    }

    std::vector<CLEO_ScriptMemoryOwner> owners(ownerCount);
    const DWORD written =
        CLEO_DebugGetScriptMemoryOwners(owners.data(), ownerCount);
    owners.resize(written);

    m_leakSeenThisSample.clear();
    for (const auto& owner : owners)
    {
        const uintptr_t ptr = static_cast<uintptr_t>(owner.scriptPtr);
        m_leakSeenThisSample.insert(ptr);

        char name[9];
        memcpy(name, owner.scriptName, 8);
        name[8] = '\0';

        UpdateLeakEntry(
            ptr,
            std::string(name),
            static_cast<unsigned long long>(owner.blocks),
            static_cast<unsigned long long>(owner.bytes)
        );
    }

    // A script that held memory but is no longer an owner released it (or was
    // unloaded). Record it as retired so the report can show the release.
    for (auto it = m_leakScripts.begin(); it != m_leakScripts.end();)
    {
        if (m_leakSeenThisSample.find(it->first) == m_leakSeenThisSample.end())
        {
            it->second.releasedSince = true;
            if (!it->second.name.empty())
                m_leakRetired[it->second.name] = it->second;
            it = m_leakScripts.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void DebugUtils::WriteLeakReport(const char* reason)
{
    if (!m_leakTrackerEnabled || !m_memoryLogEnabled)
        return;

    // Process private bytes, so a script leak can be expressed as a share of the
    // whole process, not only of the arena.
    unsigned long long privateBytes = 0;
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(
            GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
            sizeof(pmc)))
    {
        privateBytes = static_cast<unsigned long long>(pmc.PrivateUsage);
    }

    CLEO_MemoryStats arena{};
    const bool haveArena = CLEO_DebugGetMemoryStats(&arena) != FALSE;
    const unsigned long long arenaBytes =
        haveArena ? static_cast<unsigned long long>(arena.totalBytes) : 0;

    const DWORD uptimeSeconds =
        (GetTickCount() - m_memorySessionStartTick) / 1000u;

    WriteMemory("==================== script memory leak report (%s) ====================",
        reason ? reason : "interval");
    WriteMemory(
        "uptime=%us tracked=%zu retired=%zu arena=%s process_private=%s",
        static_cast<unsigned>(uptimeSeconds),
        m_leakScripts.size(),
        m_leakRetired.size(),
        MemoryBytes(arenaBytes).c_str(),
        MemoryBytes(privateBytes).c_str()
    );

    if (m_leakScripts.empty() && m_leakRetired.empty())
    {
        WriteMemory("[leak] no script currently holds allocate_memory blocks");
        return;
    }

    WriteMemory(
        "%-3s %-9s %12s %12s %8s %10s %8s %-10s",
        "#", "script", "first_bytes", "now_bytes", "growth", "growth_%arena",
        "samples", "verdict"
    );

    struct Row
    {
        const LeakScriptState* state;
        unsigned long long growth;
        double growthPctArena;
        double growthPctProcess;
        double growthPctSelf;
        double growthBytesPerSec;
        double growthPctSelfPerMin;
        const char* verdict;
    };

    std::vector<Row> rows;
    rows.reserve(m_leakScripts.size() + m_leakRetired.size());

    auto classify = [&](const LeakScriptState& state, unsigned long long growth,
                        const char*& verdict)
    {
        if (state.releasedSince && state.lastBytes == 0)
        {
            verdict = "RELEASED";
            return;
        }

        const unsigned long long growthBytes = growth;
        if (growthBytes < m_leakMinGrowthBytes)
        {
            verdict = "STABLE";
            return;
        }

        if (state.samples < kLeakMinSamples)
        {
            verdict = "SUSPECTED";
            return;
        }

        // Monotonic (never dropped) and grew on at least the required number of
        // consecutive samples: that is the strong signature.
        if (state.growthStreak >= kLeakMinSamples && state.flatStreak == 0)
            verdict = "LEAK";
        else if (growthBytes >= m_leakMinGrowthBytes)
            verdict = "SUSPECTED";
        else
            verdict = "STABLE";
    };

    auto addRow = [&](const LeakScriptState& state)
    {
        const unsigned long long growth =
            state.lastBytes >= state.firstBytes
                ? state.lastBytes - state.firstBytes
                : 0;

        const char* verdict = "STABLE";
        classify(state, growth, verdict);

        Row row;
        row.state = &state;
        row.growth = growth;
        row.growthPctArena = arenaBytes != 0
            ? 100.0 * static_cast<double>(growth) / static_cast<double>(arenaBytes)
            : 0.0;
        row.growthPctProcess = privateBytes != 0
            ? 100.0 * static_cast<double>(growth) / static_cast<double>(privateBytes)
            : 0.0;
        row.growthPctSelf = state.firstBytes != 0
            ? 100.0 * static_cast<double>(growth) / static_cast<double>(state.firstBytes)
            : 0.0;

        // Rate makes the leak exact regardless of when the interval happened to
        // be sampled: bytes/second over the observed window, and the equivalent
        // percent-per-minute growth of the script's own first sample.
        const unsigned long long windowSec =
            state.lastUptimeSec > state.firstUptimeSec
                ? state.lastUptimeSec - state.firstUptimeSec
                : 0;
        row.growthBytesPerSec = windowSec != 0
            ? static_cast<double>(growth) / static_cast<double>(windowSec)
            : 0.0;
        row.growthPctSelfPerMin = state.firstBytes != 0 && windowSec != 0
            ? (100.0 * static_cast<double>(growth) /
                   static_cast<double>(state.firstBytes)) *
                  (60.0 / static_cast<double>(windowSec))
            : 0.0;
        row.verdict = verdict;
        rows.push_back(row);
    };

    for (const auto& entry : m_leakScripts)
        addRow(entry.second);
    for (const auto& entry : m_leakRetired)
        addRow(entry.second);

    // Biggest growth first, so the report leads with what matters.
    std::sort(
        rows.begin(),
        rows.end(),
        [](const Row& a, const Row& b) { return a.growth > b.growth; }
    );

    size_t index = 0;
    for (const auto& row : rows)
    {
        ++index;
        const auto& state = *row.state;

        WriteMemory(
            "%-3zu %-9s %12s %12s %8s %9.2f%% %8u %-10s",
            index,
            state.name.empty() ? "?" : state.name.c_str(),
            MemoryBytes(state.firstBytes).c_str(),
            MemoryBytes(state.lastBytes).c_str(),
            MemoryBytes(row.growth).c_str(),
            row.growthPctArena,
            static_cast<unsigned>(state.samples),
            row.verdict
        );

        WriteMemory(
            "      growth %s = %.3f%% of process private (%s) | +%.1f%% vs first sample | "
            "rate %.1f B/s (+%.2f%%/min of own) | "
            "blocks %llu -> %llu | peak %s | first_seen=%us last_seen=%us",
            MemoryBytes(row.growth).c_str(),
            row.growthPctProcess,
            MemoryBytes(privateBytes).c_str(),
            row.growthPctSelf,
            row.growthBytesPerSec,
            row.growthPctSelfPerMin,
            state.firstBlocks,
            state.lastBlocks,
            MemoryBytes(state.peakBytes).c_str(),
            static_cast<unsigned>(state.firstUptimeSec),
            static_cast<unsigned>(state.lastUptimeSec)
        );
    }

    // The single "on how many percent exactly" answer for the whole arena.
    unsigned long long totalGrowth = 0;
    for (const auto& row : rows)
        totalGrowth += row.growth;

    const double totalPctArena = arenaBytes != 0
        ? 100.0 * static_cast<double>(totalGrowth) / static_cast<double>(arenaBytes)
        : 0.0;
    const double totalPctProcess = privateBytes != 0
        ? 100.0 * static_cast<double>(totalGrowth) / static_cast<double>(privateBytes)
        : 0.0;

    WriteMemory(
        "[leak] total growth since first sample = %s (%.2f%% of arena, %.3f%% of process private)",
        MemoryBytes(totalGrowth).c_str(),
        totalPctArena,
        totalPctProcess
    );

    WriteMemory(
        "[leak] verdict guide: LEAK = grew every sample and was never released; "
        "SUSPECTED = net growth but not monotonic; STABLE = normal churn; "
        "RELEASED = held memory earlier and has since freed it"
    );
}
