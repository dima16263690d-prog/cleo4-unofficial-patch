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
        // The live bytes fell: memory was released. This is the strongest single
        // piece of evidence AGAINST a leak, so count it and break the growth run.
        state.growthStreak = 0;
        state.flatStreak = 0;
        ++state.shrinkCount;
        ++state.breakStreak;
    }

    state.lastBytes = bytes;
    state.lastBlocks = blocks;
}

// How sure are we that this script is leaking, as a percentage?
//
// A leak is "memory taken and never given back", so the score is built from
// monotonicity first (never released), then how long it has been observed and how
// large the growth is, and it is cut hard for any release (churn). The output is
// a confidence, not a proof: 100% is only reached by a script that grew on many
// consecutive samples with zero releases and a large absolute growth.
double DebugUtils::LeakConfidencePercent(
    const LeakScriptState& state,
    unsigned long long growthBytes,
    unsigned long long arenaBytes)
{
    if (growthBytes == 0)
        return 0.0;

    double score = 0.0;

    // 1) Monotonicity - the dominant signal (up to 55 points). A release caps this
    //    at a low value: a script that frees memory is churning, not leaking.
    if (state.shrinkCount == 0)
    {
        const double streak = static_cast<double>(state.growthStreak);
        const double mono = streak / (streak + 2.0);   // 0.6 at 3, 0.83 at 10, 0.94 at 30
        score += 55.0 * mono;
    }
    else
    {
        // Some releases: keep a little credit, scaled down by how many.
        score += 15.0 / (1.0 + static_cast<double>(state.shrinkCount));
    }

    // 2) Observation length (up to 20 points). One or two samples prove nothing.
    const double obs = static_cast<double>(state.samples);
    score += 20.0 * (obs / (obs + 3.0));               // 0.4 at 2, 0.77 at 10, 0.91 at 30

    // 3) Growth magnitude (up to 25 points). Absolute, with a share-of-arena term
    //    so a large script that grows a lot scores highest.
    constexpr double kBigGrowthBytes = 1.0 * 1024.0 * 1024.0;
    const double mag = static_cast<double>(growthBytes) / kBigGrowthBytes;
    score += 20.0 * (mag / (mag + 1.0));               // 0.5 at 1 MB, 0.8 at 4 MB
    if (arenaBytes != 0)
    {
        const double share = 100.0 * static_cast<double>(growthBytes) /
            static_cast<double>(arenaBytes);
        score += 5.0 * (share / (share + 1.0));
    }

    // 4) Sample-count gate: below the minimum evidence, halve it. The verdict line
    //    already says SUSPECTED at this point; the number should agree.
    if (state.samples < kLeakMinSamples)
        score *= 0.5;

    // 5) A retired (released-to-zero) script is by definition not a leak.
    if (state.releasedSince && state.lastBytes == 0)
        score = 0.0;

    if (score < 0.0) score = 0.0;
    if (score > 100.0) score = 100.0;
    return score;
}

void DebugUtils::SampleScriptMemoryLeaks()
{
    if (!m_leakTrackerEnabled || !m_memoryLogEnabled)
        return;

    // Capture the process-private baseline once per session, so the report can
    // state process-level growth even when the script arena never moves.
    if (m_leakFirstPrivateBytes == 0)
    {
        PROCESS_MEMORY_COUNTERS_EX pmc{};
        pmc.cb = sizeof(pmc);
        if (GetProcessMemoryInfo(
                GetCurrentProcess(),
                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                sizeof(pmc)))
        {
            m_leakFirstPrivateBytes = static_cast<uint64_t>(pmc.PrivateUsage);
            m_leakFirstPrivateUptimeSec =
                (GetTickCount() - m_memorySessionStartTick) / 1000u;

            // Streaming-pool baseline: the world-loading fill shows up as private
            // growth but is not a leak, so remember where it started.
            if (CLEO_GetGameVersion() == GV_US10)
            {
                m_leakFirstStreamUsedBytes = static_cast<uint64_t>(
                    *reinterpret_cast<unsigned int*>(kGtaSa10StreamingMemoryUsed));
                m_leakHaveStreamBaseline = true;
            }
        }
    }

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

    // The script arena can be perfectly flat while the process still grows. That
    // is the common case for a streaming/pool/GPU leak, so state the process
    // growth explicitly and, when the arena did not move, say so: otherwise the
    // "0 B" arena total reads as "no leak" and hides a real one. Printed on every
    // report, including when no script holds memory.
    auto writeProcessGrowth = [&](unsigned long long totalArenaGrowth)
    {
        if (m_leakFirstPrivateBytes == 0 || privateBytes < m_leakFirstPrivateBytes)
        {
            // Still remember this report as the "previous" one for the next trend.
            m_leakLastPrivateBytes = privateBytes;
            m_leakLastUptimeSec = uptimeSeconds;
            return;
        }

        const unsigned long long privateGrowth = privateBytes - m_leakFirstPrivateBytes;
        const unsigned long long windowSec =
            uptimeSeconds > m_leakFirstPrivateUptimeSec
                ? uptimeSeconds - m_leakFirstPrivateUptimeSec
                : 0;

        const double pctOfStart = 100.0 * static_cast<double>(privateGrowth) /
            static_cast<double>(m_leakFirstPrivateBytes);
        const double bytesPerSec = windowSec != 0
            ? static_cast<double>(privateGrowth) / static_cast<double>(windowSec)
            : 0.0;
        const double pctPerMin = windowSec != 0
            ? pctOfStart * (60.0 / static_cast<double>(windowSec))
            : 0.0;

        WriteMemory(
            "[leak] process private %s -> %s = +%s in %us (+%.2f%%, +%.2f%%/min, "
            "%.2f MB/min, %.0f B/s)",
            MemoryBytes(m_leakFirstPrivateBytes).c_str(),
            MemoryBytes(privateBytes).c_str(),
            MemoryBytes(privateGrowth).c_str(),
            static_cast<unsigned>(windowSec),
            pctOfStart,
            pctPerMin,
            bytesPerSec * 60.0 / (1024.0 * 1024.0),
            bytesPerSec
        );

        // Interval-over-interval trend. Compare this interval's rate to the
        // average rate since the first sample: a rate that keeps up with the
        // average is a real leak, one that falls well below it is a pool or
        // streaming fill that is plateauing (not a leak). Comparing to the
        // average rather than to the previous interval avoids a false
        // "accelerating" when the previous interval happened to be flat.
        if (m_leakLastPrivateBytes != 0 && uptimeSeconds > m_leakLastUptimeSec)
        {
            const unsigned long long prevWindow =
                uptimeSeconds - m_leakLastUptimeSec;
            const long long deltaBytes =
                static_cast<long long>(privateBytes) -
                static_cast<long long>(m_leakLastPrivateBytes);
            const double recentRate = static_cast<double>(deltaBytes) /
                static_cast<double>(prevWindow);
            const double avgRate = windowSec != 0
                ? static_cast<double>(privateGrowth) / static_cast<double>(windowSec)
                : 0.0;

            const char* trend = "steady (leak-like)";
            if (recentRate < 0.0)
                trend = "shrinking (memory was freed)";
            else if (avgRate > 0.0)
            {
                if (recentRate < avgRate * 0.5)
                    trend = "decelerating (likely a pool/streaming fill, not a leak)";
                else if (recentRate > avgRate * 1.5)
                    trend = "accelerating (worsening)";
            }

            WriteMemory(
                "[leak] this interval %+.2f MB (%.0f B/s); average since first "
                "%.0f B/s -> %s",
                static_cast<double>(deltaBytes) / (1024.0 * 1024.0),
                recentRate,
                avgRate,
                trend
            );
        }

        if (privateGrowth >= m_leakMinGrowthBytes && totalArenaGrowth == 0)
        {
            WriteMemory(
                "[leak] NOTE: the CLEO script arena did not grow, so this %s is NOT "
                "a script allocate_memory leak; look at streaming pools / textures / "
                "mods (see the memory summary and the per-module map: set "
                "[DebugUtils.Logs] ModuleMemoryMap=1 to name the DLL that grows).",
                MemoryBytes(privateGrowth).c_str()
            );
        }

        m_leakLastPrivateBytes = privateBytes;
        m_leakLastUptimeSec = uptimeSeconds;
    };

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
        writeProcessGrowth(0);
        return;
    }

    WriteMemory(
        "%-3s %-9s %12s %12s %8s %10s %8s %6s %-10s",
        "#", "script", "first_bytes", "now_bytes", "growth", "growth_%arena",
        "samples", "conf%", "verdict"
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
        double growthPctArenaPerMin;   // leak rate as a share of the whole arena
        double confidencePct;          // how sure this is a leak, 0-100
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
        // Leak rate as a share of the whole arena per minute: the "how fast is
        // this eating the arena" figure, independent of the script's own size.
        row.growthPctArenaPerMin = arenaBytes != 0 && windowSec != 0
            ? (100.0 * static_cast<double>(growth) /
                   static_cast<double>(arenaBytes)) *
                  (60.0 / static_cast<double>(windowSec))
            : 0.0;
        row.confidencePct = LeakConfidencePercent(state, growth, arenaBytes);
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
            "%-3zu %-9s %12s %12s %8s %9.2f%% %8u %5.0f%% %-10s",
            index,
            state.name.empty() ? "?" : state.name.c_str(),
            MemoryBytes(state.firstBytes).c_str(),
            MemoryBytes(state.lastBytes).c_str(),
            MemoryBytes(row.growth).c_str(),
            row.growthPctArena,
            static_cast<unsigned>(state.samples),
            row.confidencePct,
            row.verdict
        );

        WriteMemory(
            "      growth %s = %.3f%% of process private (%s) | +%.1f%% vs first sample | "
            "rate %.1f B/s (+%.2f%%/min of own, %.3f%%/min of arena) | "
            "blocks %llu -> %llu | peak %s | first_seen=%us last_seen=%us | "
            "confidence=%.0f%% (releases=%u)",
            MemoryBytes(row.growth).c_str(),
            row.growthPctProcess,
            MemoryBytes(privateBytes).c_str(),
            row.growthPctSelf,
            row.growthBytesPerSec,
            row.growthPctSelfPerMin,
            row.growthPctArenaPerMin,
            state.firstBlocks,
            state.lastBlocks,
            MemoryBytes(state.peakBytes).c_str(),
            static_cast<unsigned>(state.firstUptimeSec),
            static_cast<unsigned>(state.lastUptimeSec),
            row.confidencePct,
            static_cast<unsigned>(state.shrinkCount)
        );
    }

    // Headline suspect: the single script to look at first. Rows are already
    // sorted by growth, so the top non-trivial verdict is the answer.
    const Row* suspect = nullptr;
    for (const auto& row : rows)
    {
        if (row.growth < m_leakMinGrowthBytes)
            continue;
        if (std::strcmp(row.verdict, "LEAK") == 0 ||
            std::strcmp(row.verdict, "SUSPECTED") == 0)
        {
            suspect = &row;
            break;
        }
    }

    if (suspect != nullptr)
    {
        WriteMemory(
            "[leak] TOP SUSPECT: script '%s' grew %s (+%.1f%% of its own, "
            "%.3f%%/min of arena) over %u samples - verdict %s, confidence %.0f%%",
            suspect->state->name.empty() ? "?" : suspect->state->name.c_str(),
            MemoryBytes(suspect->growth).c_str(),
            suspect->growthPctSelf,
            suspect->growthPctArenaPerMin,
            static_cast<unsigned>(suspect->state->samples),
            suspect->verdict,
            suspect->confidencePct
        );
    }
    else
    {
        // No script is leaking, but the process may still be. Do not answer
        // "none" while private bytes climb: name the process-level growth and
        // how long until the 2 GB address-space limit, which is the number that
        // actually matters for a 32-bit game.
        unsigned long long privateGrowth = 0;
        double privateRatePerMin = 0.0;
        if (m_leakFirstPrivateBytes != 0 && privateBytes >= m_leakFirstPrivateBytes &&
            uptimeSeconds > m_leakFirstPrivateUptimeSec)
        {
            privateGrowth = privateBytes - m_leakFirstPrivateBytes;
            const unsigned long long windowSec =
                uptimeSeconds - m_leakFirstPrivateUptimeSec;
            const double bytesPerSec =
                static_cast<double>(privateGrowth) / static_cast<double>(windowSec);
            privateRatePerMin = bytesPerSec * 60.0 / (1024.0 * 1024.0);
        }

        if (privateGrowth >= m_leakMinGrowthBytes)
        {
            const double pctOfPrivate = privateBytes != 0
                ? 100.0 * static_cast<double>(privateGrowth) /
                  static_cast<double>(privateBytes)
                : 0.0;
            WriteMemory(
                "[leak] TOP SUSPECT: process private (no script is leaking) - grew %s "
                "(+%.2f%% of private) at %.2f MB/min",
                MemoryBytes(privateGrowth).c_str(),
                pctOfPrivate,
                privateRatePerMin
            );

            // Attribute part of the growth to the streaming pool: during the first
            // minutes the world load fills it, which shows up as private growth but
            // is not a leak. Report what is left once the pool fill is removed.
            if (m_leakHaveStreamBaseline && CLEO_GetGameVersion() == GV_US10)
            {
                const unsigned long long streamNow = static_cast<unsigned long long>(
                    *reinterpret_cast<unsigned int*>(kGtaSa10StreamingMemoryUsed));
                const unsigned long long streamGrowth =
                    streamNow > m_leakFirstStreamUsedBytes
                        ? streamNow - m_leakFirstStreamUsedBytes
                        : 0;

                if (streamGrowth >= m_leakMinGrowthBytes)
                {
                    const unsigned long long unattributed =
                        privateGrowth > streamGrowth ? privateGrowth - streamGrowth : 0;
                    WriteMemory(
                        "[leak] of that, %s is the streaming pool filling (world load, "
                        "not a leak); %s is unattributed - that is the part to chase",
                        MemoryBytes(streamGrowth).c_str(),
                        MemoryBytes(unattributed).c_str()
                    );
                }
            }

            constexpr unsigned long long kAddressSpaceBytes =
                2ull * 1024ull * 1024ull * 1024ull;
            if (privateRatePerMin > 0.0 && privateBytes < kAddressSpaceBytes)
            {
                const double headroomMb =
                    static_cast<double>(kAddressSpaceBytes - privateBytes) /
                    (1024.0 * 1024.0);
                const double minutesToLimit = headroomMb / privateRatePerMin;
                if (minutesToLimit < 24.0 * 60.0)
                {
                    WriteMemory(
                        "[leak] at this rate the 2.00 GB limit is reached in ~%.0f "
                        "min (%.1f h) - %s headroom left",
                        minutesToLimit,
                        minutesToLimit / 60.0,
                        MemoryBytes(kAddressSpaceBytes - privateBytes).c_str()
                    );
                }
            }
        }
        else
        {
            WriteMemory(
                "[leak] TOP SUSPECT: none - no script grew by at least %s with a "
                "LEAK/SUSPECTED verdict, and process private is not climbing",
                MemoryBytes(m_leakMinGrowthBytes).c_str()
            );
        }
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

    // The script arena can be perfectly flat while the process still grows. That
    // is the common case for a streaming/pool/GPU leak, so state the process
    // growth explicitly and, when the arena did not move, say so: otherwise
    // "0 B" above reads as "no leak" and hides a real one.
    writeProcessGrowth(totalGrowth);

    WriteMemory(
        "[leak] verdict guide: LEAK = grew every sample and was never released; "
        "SUSPECTED = net growth but not monotonic; STABLE = normal churn; "
        "RELEASED = held memory earlier and has since freed it"
    );
    WriteMemory(
        "[leak] confidence%% = how sure this is a real leak: 100%% needs many "
        "consecutive growing samples, zero releases and a large growth; every "
        "release (releases=N) cuts it hard, so a churning script scores low"
    );
}
