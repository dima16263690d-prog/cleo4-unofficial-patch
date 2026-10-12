#include "DebugUtils.h"
#include "DebugUtils.Internal.h"
#include "resource.h"
#include <windows.h>
#include <psapi.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cmath>

// Frame-time / stutter monitor.
//
// GTA SA logic runs on a single thread inside OnUpdateGameLogics(), which CLEO
// already wraps (see source/cleo.cpp). This monitor brackets that call with
// QueryPerformanceCounter reads: the *period* is measured between consecutive
// frame starts, and the *logic time* covers just the game-logic tick. That split
// separates "the whole frame took long" (streaming / driver / render) from "the
// logic tick itself was slow" (scripts), which is what is needed to decide what
// to optimise.
//
// Everything is off unless [DebugUtils.FrameTime] Enabled=1: when disabled this
// file reads no clock and returns immediately, so normal players pay nothing.
// Output goes to its own file (cleo_frametime.log) and never touches other logs.

double DebugUtils::FrameTimeMs(LARGE_INTEGER from, LARGE_INTEGER to) const
{
    if (!m_frameTimeFreqReady || m_frameTimeFreq.QuadPart == 0)
        return 0.0;

    const LONGLONG delta = to.QuadPart - from.QuadPart;
    return static_cast<double>(delta) * 1000.0 /
        static_cast<double>(m_frameTimeFreq.QuadPart);
}

void DebugUtils::ResetFrameTimeInterval()
{
    m_frameTimeFrames = 0;
    m_frameTimeSumPeriodMs = 0.0;
    m_frameTimeSumLogicMs = 0.0;
    m_frameTimeMinPeriodMs = 0.0;
    m_frameTimeMaxPeriodMs = 0.0;
    m_frameTimeMaxLogicMs = 0.0;
    m_frameTimeStutterCount = 0;
    m_frameTimeStutterLostMs = 0.0;
    m_frameTimeIntervalMs = 0.0;
    m_frameTimeWorstCount = 0;
    for (size_t i = 0; i < kFrameTimeHistogramBuckets; ++i)
        m_frameTimeHistogram[i] = 0;
}

void DebugUtils::InsertWorstTimeSample(const FrameTimeSample& sample)
{
    const size_t capacity = std::min<size_t>(m_frameTimeTopStutter, kFrameTimeMaxWorst);
    if (capacity == 0)
        return;

    FrameTimeSample* table = m_frameTimeWorst.data();

    // Table stays sorted descending by period. If it is full and this sample is
    // not worse than the current smallest, it cannot make the list.
    if (m_frameTimeWorstCount == capacity && sample.periodMs <= table[capacity - 1].periodMs)
        return;

    const size_t limit = m_frameTimeWorstCount < capacity
        ? m_frameTimeWorstCount + 1
        : capacity;

    size_t pos = 0;
    while (pos < limit - 1 && table[pos].periodMs >= sample.periodMs)
        ++pos;

    const size_t last = m_frameTimeWorstCount < capacity
        ? m_frameTimeWorstCount
        : capacity - 1;
    for (size_t i = last; i > pos; --i)
        table[i] = table[i - 1];

    table[pos] = sample;
    if (m_frameTimeWorstCount < capacity)
        ++m_frameTimeWorstCount;
}

void DebugUtils::BeginFrameTimeFrame()
{
    if (!m_frameTimeEnabled)
        return;

    if (!m_frameTimeFreqReady)
    {
        if (!QueryPerformanceFrequency(&m_frameTimeFreq) || m_frameTimeFreq.QuadPart == 0)
            return;
        m_frameTimeFreqReady = true;
    }

    QueryPerformanceCounter(&m_frameTimeLogicStart);

    // Frame period = distance between consecutive frame starts.
    if (m_frameTimeHaveLast)
    {
        const double periodMs = FrameTimeMs(m_frameTimeLastBefore, m_frameTimeLogicStart);
        m_frameTimeLastPeriodMs = periodMs;

        // The first period arrives when the second frame begins (frames == 1);
        // seed the minimum there, otherwise it would stay at its 0.0 initial.
        if (m_frameTimeFrames <= 1 || periodMs < m_frameTimeMinPeriodMs)
            m_frameTimeMinPeriodMs = periodMs;
        if (periodMs > m_frameTimeMaxPeriodMs)
            m_frameTimeMaxPeriodMs = periodMs;

        m_frameTimeSumPeriodMs += periodMs;
        m_frameTimeIntervalMs += periodMs;

        if (periodMs >= static_cast<double>(m_frameTimeWarnMs))
        {
            ++m_frameTimeStutterCount;
            m_frameTimeStutterLostMs += periodMs - static_cast<double>(m_frameTimeWarnMs);
        }

        // Histogram bucket: first edge the period is below, else overflow.
        size_t bucket = kFrameTimeHistogramBuckets - 1;
        for (size_t i = 0; i < kFrameTimeHistogramBuckets - 1; ++i)
        {
            if (periodMs < kFrameTimeHistogramEdgesMs[i])
            {
                bucket = i;
                break;
            }
        }
        ++m_frameTimeHistogram[bucket];
    }
    else
    {
        m_frameTimeLastPeriodMs = 0.0;
    }

    m_frameTimeLastBefore = m_frameTimeLogicStart;
    m_frameTimeHaveLast = true;
}

void DebugUtils::EndFrameTimeFrame()
{
    if (!m_frameTimeEnabled || !m_frameTimeFreqReady)
        return;

    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);

    const double logicMs = FrameTimeMs(m_frameTimeLogicStart, now);
    if (logicMs > m_frameTimeMaxLogicMs)
        m_frameTimeMaxLogicMs = logicMs;
    m_frameTimeSumLogicMs += logicMs;

    // Active-script count for this frame, so a stutter can be correlated with a
    // script burst. Same walk as the memory summary; the queue is delimited by
    // nullptr.
    size_t scripts = 0;
    if (CLEO_GetGameVersion() == GV_US10)
    {
        auto head = *reinterpret_cast<CScriptThread**>(kGtaSa10ActiveScripts);
        for (auto thread = head; thread != nullptr && scripts < 4096; thread = thread->next)
            ++scripts;
    }

    FrameTimeSample sample;
    sample.periodMs = m_frameTimeLastPeriodMs;
    sample.logicMs = logicMs;
    sample.scripts = scripts;
    sample.uptimeSec = static_cast<DWORD>(
        (GetTickCount() - m_memorySessionStartTick) / 1000u);
    InsertWorstTimeSample(sample);

    m_frameTimeScriptsLast = scripts;
    ++m_frameTimeFrames;

    const DWORD nowTick = GetTickCount();
    if (nowTick - m_lastFrameTimeReportTick >= m_frameTimeIntervalSeconds * 1000u)
    {
        m_lastFrameTimeReportTick = nowTick;
        WriteFrameTimeReport("interval");
    }
}

void DebugUtils::WriteFrameTime(const char* format, ...)
{
    if (!m_frameTimeEnabled)
        return;

    char message[2048];
    va_list args;
    va_start(args, format);
    SafeFormat(message, sizeof(message), format, args);
    va_end(args);

    std::lock_guard<std::mutex> lock(m_frameTimeMutex);

    if (!m_frameTimeLog.is_open())
        return;

    const size_t limit =
        static_cast<size_t>(m_frameTimeMaxLogMegabytes) * 1024u * 1024u;
    const size_t incoming = strlen(message) + 1;
    if (m_frameTimeBytes + incoming > limit)
    {
        if (!m_frameTimeLimitNoticeWritten)
        {
            m_frameTimeLog << "[frametime] log limit reached ("
                << m_frameTimeMaxLogMegabytes << " MB); further samples skipped\n";
            m_frameTimeLog.flush();
            m_frameTimeLimitNoticeWritten = true;
        }
        return;
    }

    SYSTEMTIME t{};
    GetLocalTime(&t);
    char ms[4];
    sprintf_s(ms, sizeof(ms), "%03u", t.wMilliseconds);

    m_frameTimeLog
        << t.wYear << '-'
        << (t.wMonth < 10 ? "0" : "") << t.wMonth << '-'
        << (t.wDay < 10 ? "0" : "") << t.wDay << ' '
        << (t.wHour < 10 ? "0" : "") << t.wHour << ':'
        << (t.wMinute < 10 ? "0" : "") << t.wMinute << ':'
        << (t.wSecond < 10 ? "0" : "") << t.wSecond << '.'
        << ms << ' ' << message << '\n';
    m_frameTimeLog.flush();

    m_frameTimeBytes += incoming;
}

void DebugUtils::WriteFrameTimeReport(const char* reason)
{
    if (!m_frameTimeEnabled)
        return;

    const char* tag = reason ? reason : "interval";

    if (m_frameTimeFrames == 0)
    {
        ResetFrameTimeInterval();
        return;
    }

    const double frames = static_cast<double>(m_frameTimeFrames);
    const double avgPeriod = m_frameTimeSumPeriodMs / frames;
    const double avgLogic = m_frameTimeSumLogicMs / frames;
    const double avgFps = avgPeriod > 0.0 ? 1000.0 / avgPeriod : 0.0;

    WriteFrameTime("================= frame time (%s) =================", tag);
    WriteFrameTime(
        "frames=%llu span_s=%.1f fps_avg=%.1f period_ms min=%.2f avg=%.2f max=%.2f "
        "logic_ms avg=%.2f max=%.2f scripts=%zu",
        m_frameTimeFrames,
        m_frameTimeIntervalMs / 1000.0,
        avgFps,
        m_frameTimeMinPeriodMs,
        avgPeriod,
        m_frameTimeMaxPeriodMs,
        avgLogic,
        m_frameTimeMaxLogicMs,
        m_frameTimeScriptsLast
    );
    WriteFrameTime(
        "stutter warn_ms=%u count=%llu (%.2f%% of frames) lost_ms=%.1f",
        static_cast<unsigned>(m_frameTimeWarnMs),
        m_frameTimeStutterCount,
        frames > 0.0
            ? 100.0 * static_cast<double>(m_frameTimeStutterCount) / frames
            : 0.0,
        m_frameTimeStutterLostMs
    );

    if (m_frameTimeHistogram)
    {
        WriteFrameTime("%-24s %10s %8s", "period bucket", "frames", "share%");
        for (size_t i = 0; i < kFrameTimeHistogramBuckets; ++i)
        {
            const bool overflow = i + 1 == kFrameTimeHistogramBuckets;
            char label[32];
            if (overflow)
                sprintf_s(label, sizeof(label), ">= %.1f ms",
                    kFrameTimeHistogramEdgesMs[kFrameTimeHistogramBuckets - 2]);
            else if (i == 0)
                sprintf_s(label, sizeof(label), "< %.1f ms", kFrameTimeHistogramEdgesMs[0]);
            else
                sprintf_s(label, sizeof(label), "%.1f - %.1f ms",
                    kFrameTimeHistogramEdgesMs[i - 1], kFrameTimeHistogramEdgesMs[i]);

            const double share = frames > 0.0
                ? 100.0 * static_cast<double>(m_frameTimeHistogram[i]) / frames
                : 0.0;
            WriteFrameTime("%-24s %10llu %7.2f%%", label, m_frameTimeHistogram[i], share);
        }
    }

    // Worst frames of the interval, so a stutter can be tied to a moment/script count.
    const size_t listed = std::min<size_t>(m_frameTimeTopStutter, m_frameTimeWorstCount);
    if (listed > 0)
    {
        WriteFrameTime("---- worst frames ----");
        WriteFrameTime("%-3s %12s %12s %8s %9s",
            "#", "period_ms", "logic_ms", "scripts", "uptime_s");
        for (size_t i = 0; i < listed; ++i)
        {
            const auto& s = m_frameTimeWorst[i];
            WriteFrameTime(
                "%-3zu %12.2f %12.2f %8zu %9u",
                i + 1,
                s.periodMs,
                s.logicMs,
                s.scripts,
                static_cast<unsigned>(s.uptimeSec)
            );
        }
    }

    ResetFrameTimeInterval();
}
