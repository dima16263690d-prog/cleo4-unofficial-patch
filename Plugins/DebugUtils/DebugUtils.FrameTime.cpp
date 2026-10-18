#include "DebugUtils.h"
#include "DebugUtils.Internal.h"
#include "resource.h"
#include <windows.h>
#include <psapi.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>
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
    m_frameTimeStreamUsedSum = 0;
    m_frameTimeStreamUsedMax = 0;
    m_frameTimeStreamAvail = 0;
    m_frameTimeStreamSamples = 0;
    m_frameTimeStreamReqSum = 0;
    m_frameTimeStreamReqMax = 0;
    m_frameTimeStreamReqBusy = 0;
    for (size_t i = 0; i < kFrameTimeHistogramBuckets; ++i)
        m_frameTimeHistogram[i] = 0;

    // Frame-quality accumulators.
    m_frameTimePeriodCount = 0;
    m_frameTimePeriodNext = 0;
    m_frameTimeSumPeriodSqMs = 0.0;
    m_frameTimeSpike33 = 0;
    m_frameTimeSpike50 = 0;
    m_frameTimeSpike100 = 0;
    m_frameTimeSpike250 = 0;
    m_frameTimeBudgetMet = 0;
    m_frameTimeBudgetMissed = 0;
    m_frameTimeWorstLogicMs = 0.0;
    m_frameTimeWorstLogicPeriodMs = 0.0;
    m_frameTimeHavePending = false;
    m_frameTimePending = FrameTimeSample{};
    m_frameTimeLogicBoundFrames = 0;
    m_frameTimeSlowFrames = 0;
    m_frameTimeStutterLogicBound = 0;
}

double DebugUtils::FrameTimePeriodPercentile(double p) const
{
    if (m_frameTimePeriodCount == 0)
        return 0.0;

    if (p < 0.0) p = 0.0;
    if (p > 1.0) p = 1.0;

    // Copy only the valid entries, then index the sorted order. Called at report
    // time only, so the allocation and sort are not on the hot path.
    std::vector<float> periods(
        m_frameTimePeriodRing.begin(),
        m_frameTimePeriodRing.begin() + static_cast<ptrdiff_t>(m_frameTimePeriodCount)
    );
    std::sort(periods.begin(), periods.end());

    size_t index = static_cast<size_t>(p * static_cast<double>(periods.size() - 1) + 0.5);
    if (index >= periods.size())
        index = periods.size() - 1;

    return static_cast<double>(periods[index]);
}

double DebugUtils::FrameTimeLowFps(double fraction) const
{
    if (m_frameTimePeriodCount == 0 || fraction <= 0.0 || fraction >= 1.0)
        return 0.0;

    // Sort descending (slowest first) and average the slowest `fraction` of the
    // frames. This is the standard "1% low / 0.1% low" FPS measure.
    std::vector<float> periods(
        m_frameTimePeriodRing.begin(),
        m_frameTimePeriodRing.begin() + static_cast<ptrdiff_t>(m_frameTimePeriodCount)
    );
    std::sort(periods.begin(), periods.end(), std::greater<float>());

    size_t tail = static_cast<size_t>(
        static_cast<double>(periods.size()) * fraction + 0.5);
    if (tail == 0)
        tail = 1;
    if (tail > periods.size())
        tail = periods.size();

    double sum = 0.0;
    for (size_t i = 0; i < tail; ++i)
        sum += static_cast<double>(periods[i]);

    const double avgPeriod = sum / static_cast<double>(tail);
    return avgPeriod > 0.0 ? 1000.0 / avgPeriod : 0.0;
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
        m_frameTimeSumPeriodSqMs += periodMs * periodMs;

        // The frame that just finished (parked at EndFrameTimeFrame) now has its
        // real period, so pair the period and logic time of the SAME frame and
        // rank it. Doing this at End would use the previous frame's period.
        if (m_frameTimeHavePending)
        {
            m_frameTimePending.periodMs = periodMs;
            InsertWorstTimeSample(m_frameTimePending);

            if (m_frameTimePending.logicMs > m_frameTimeWorstLogicMs)
            {
                m_frameTimeWorstLogicMs = m_frameTimePending.logicMs;
                m_frameTimeWorstLogicPeriodMs = periodMs;
            }
            m_frameTimeHavePending = false;
        }

        // Bounded ring of every period, so the report can compute true
        // percentiles and 1%-low FPS instead of only min/avg/max.
        m_frameTimePeriodRing[m_frameTimePeriodNext] = static_cast<float>(periodMs);
        m_frameTimePeriodNext = (m_frameTimePeriodNext + 1) % kFrameTimePeriodSamples;
        if (m_frameTimePeriodCount < kFrameTimePeriodSamples)
            ++m_frameTimePeriodCount;

        // Spike bands: how many frames fell below each FPS floor.
        if (periodMs >= 33.3) ++m_frameTimeSpike33;
        if (periodMs >= 50.0) ++m_frameTimeSpike50;
        if (periodMs >= 100.0) ++m_frameTimeSpike100;
        if (periodMs >= 250.0) ++m_frameTimeSpike250;

        if (m_frameTimeBudgetMs > 0.0)
        {
            if (periodMs <= m_frameTimeBudgetMs)
                ++m_frameTimeBudgetMet;
            else
                ++m_frameTimeBudgetMissed;
        }

        // Per-frame cause split. Only SLOW frames count: a logic-heavy frame that
        // still met the frame target is not a problem, and counting it would
        // mislabel a perfectly smooth interval as "logic/script bound".
        if (periodMs >= kFrameTimeSlowMs &&
            m_frameTimeLastLogicMs >= kFrameTimeLogicShare * periodMs)
            ++m_frameTimeLogicBoundFrames;

        // Frames that missed the target (slow frames), regardless of cause, so the
        // report can say "smooth" when there were none instead of always blaming a
        // subsystem.
        if (periodMs >= kFrameTimeSlowMs)
            ++m_frameTimeSlowFrames;

        if (periodMs >= static_cast<double>(m_frameTimeWarnMs))
        {
            ++m_frameTimeStutterCount;
            m_frameTimeStutterLostMs += periodMs - static_cast<double>(m_frameTimeWarnMs);
            if (periodMs > 0.0 && m_frameTimeLastLogicMs >= kFrameTimeLogicShare * periodMs)
                ++m_frameTimeStutterLogicBound;
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
    m_frameTimeLastLogicMs = logicMs;
    if (logicMs > m_frameTimeMaxLogicMs)
        m_frameTimeMaxLogicMs = logicMs;

    // The period of this frame is only known at the NEXT frame start (it is
    // start-to-start), so the finished frame is parked and ranked in
    // BeginFrameTimeFrame, where the period of the SAME frame is available.
    // Pairing here would use the previous frame's period and print an impossible
    // ratio (a logic tick longer than the frame it supposedly sat in).
    m_frameTimePending.logicMs = logicMs;
    m_frameTimeHavePending = true;

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

    // Streaming memory snapshot. Both globals are updated by the same thread that
    // runs this hook, so no lock is needed. Only meaningful on 1.0 (US/EU share
    // the addresses; 1.01 is unsupported).
    unsigned long long streamUsed = 0;
    unsigned long long streamReq = 0;
    if (m_frameTimeStreamingEnabled &&
        (CLEO_GetGameVersion() == GV_US10 || CLEO_GetGameVersion() == GV_EU10))
    {
        streamUsed = *reinterpret_cast<unsigned int*>(kGtaSa10StreamingMemoryUsed);
        streamReq = *reinterpret_cast<unsigned int*>(kGtaSa10StreamingModelsRequested);
        const unsigned long long avail =
            *reinterpret_cast<unsigned int*>(kGtaSa10StreamingMemoryAvailable);
        if (m_frameTimeStreamSamples == 0)
            m_frameTimeStreamAvail = avail;
        m_frameTimeStreamUsedSum += streamUsed;
        if (streamUsed > m_frameTimeStreamUsedMax)
            m_frameTimeStreamUsedMax = streamUsed;
        m_frameTimeStreamReqSum += streamReq;
        if (streamReq > m_frameTimeStreamReqMax)
            m_frameTimeStreamReqMax = streamReq;
        if (streamReq > 0)
            ++m_frameTimeStreamReqBusy;
        ++m_frameTimeStreamSamples;
    }

    FrameTimeSample sample;
    sample.periodMs = m_frameTimeLastPeriodMs;
    sample.logicMs = logicMs;
    sample.scripts = scripts;
    sample.uptimeSec = static_cast<DWORD>(
        (GetTickCount() - m_memorySessionStartTick) / 1000u);
    sample.streamUsed = streamUsed;
    sample.streamReq = streamReq;
    // Park the finished frame: its period is only measured at the next frame
    // start, and the ranking must pair the period and the logic time of the SAME
    // frame (see m_frameTimePending).
    m_frameTimePending = sample;
    m_frameTimeHavePending = true;

    m_frameTimeScriptsLast = scripts;
    ++m_frameTimeFrames;

    // Observe RenderWare's last-error global once per frame (read-only). Any
    // change is recorded and reported with the interval.
    PollRenderWareError();

    const DWORD nowTick = GetTickCount();
    if (nowTick - m_lastFrameTimeReportTick >= m_frameTimeIntervalSeconds * 1000u)
    {
        m_lastFrameTimeReportTick = nowTick;
        WriteFrameTimeReport("interval");
        WriteErrorReport("interval");
        WriteFileOpenReport(false);
        WriteWatchedOpenReport(false);
        WriteTextureReadReport(false);
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

    // Frame-quality statistics from the period ring.
    const double p50 = FrameTimePeriodPercentile(0.50);
    const double p95 = FrameTimePeriodPercentile(0.95);
    const double p99 = FrameTimePeriodPercentile(0.99);
    const double p999 = FrameTimePeriodPercentile(0.999);
    const double low1Fps = FrameTimeLowFps(0.01);
    const double low01Fps = FrameTimeLowFps(0.001);

    // Jitter = standard deviation of the frame period (variance = E[x^2]-E[x]^2).
    const double meanSq = m_frameTimeSumPeriodSqMs / frames;
    const double variance = meanSq - avgPeriod * avgPeriod;
    const double jitterMs = variance > 0.0 ? std::sqrt(variance) : 0.0;

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

    // Percentiles and low-FPS: the numbers that describe smoothness rather than
    // raw average. low_1%_fps is the average of the slowest 1% of frames.
    WriteFrameTime(
        "period_ms p50=%.2f p95=%.2f p99=%.2f p99.9=%.2f | jitter_ms=%.2f (stddev) | "
        "fps_1%%low=%.1f fps_0.1%%low=%.1f",
        p50, p95, p99, p999, jitterMs, low1Fps, low01Fps
    );

    WriteFrameTime(
        "spikes (frames below): 30fps=%llu (%.2f%%) 20fps=%llu (%.2f%%) "
        "10fps=%llu (%.2f%%) 4fps=%llu (%.2f%%)",
        m_frameTimeSpike33,
        frames > 0.0 ? 100.0 * static_cast<double>(m_frameTimeSpike33) / frames : 0.0,
        m_frameTimeSpike50,
        frames > 0.0 ? 100.0 * static_cast<double>(m_frameTimeSpike50) / frames : 0.0,
        m_frameTimeSpike100,
        frames > 0.0 ? 100.0 * static_cast<double>(m_frameTimeSpike100) / frames : 0.0,
        m_frameTimeSpike250,
        frames > 0.0 ? 100.0 * static_cast<double>(m_frameTimeSpike250) / frames : 0.0
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

    // Cause split across ALL frames, not just the worst one: what share of frames
    // were logic/script bound (the logic tick used >= 80% of the frame) versus
    // render/streaming bound. The stutter subset is given too, so "what owns the
    // hitches?" has a direct answer.
    if (frames > 0.0)
    {
        const double logicSharePct =
            100.0 * static_cast<double>(m_frameTimeLogicBoundFrames) / frames;
        const double stutterLogicPct = m_frameTimeStutterCount > 0
            ? 100.0 * static_cast<double>(m_frameTimeStutterLogicBound) /
              static_cast<double>(m_frameTimeStutterCount)
            : 0.0;

        // The dominant cause is decided by the STUTTERS when there are any (they
        // are the frames that hurt), and only by the overall share when the
        // interval was smooth. Otherwise a mostly-smooth interval with all its
        // few hitches in logic would still be labelled "render bound" and send
        // the reader after the wrong subsystem.
        const char* dominant;
        if (m_frameTimeSlowFrames == 0)
            // No frame even missed the 50 FPS floor: nothing to blame. Saying
            // "render/streaming bound" here would be a false lead.
            dominant = "smooth (no slow frames)";
        else if (m_frameTimeStutterCount > 0)
        {
            if (stutterLogicPct >= 50.0)
                dominant = "logic/script bound (most stutters)";
            else if (stutterLogicPct >= 20.0)
                dominant = "mixed";
            else
                dominant = "render/streaming bound (most stutters)";
        }
        else
        {
            dominant = logicSharePct >= 50.0
                ? "logic/script bound"
                : (logicSharePct >= 20.0 ? "mixed" : "render/streaming bound");
        }

        WriteFrameTime(
            "[fps] cause: %llu/%llu frames logic-bound (%.1f%%), %llu/%llu stutters "
            "logic-bound (%.1f%%) | %llu slow frames (>=%.0fms) -> overall %s",
            m_frameTimeLogicBoundFrames,
            m_frameTimeFrames,
            logicSharePct,
            m_frameTimeStutterLogicBound,
            m_frameTimeStutterCount,
            stutterLogicPct,
            m_frameTimeSlowFrames,
            kFrameTimeSlowMs,
            dominant
        );
    }

    // Auto frame-pacing verdict: one greppable line that needs no configuration.
    // It flags the two independent problems separately - a low average (not
    // enough performance) and a low 1%-low relative to the average (uneven
    // pacing / hitches) - so "FPS is fine but it stutters" is visible even when
    // MinFps/LowFpsWarn are left at 0.
    if (avgFps > 0.0)
    {
        const double smoothness = low1Fps > 0.0 ? low1Fps / avgFps : 0.0;
        const bool lowAvg = avgFps < 55.0;            // below a 60 FPS target
        const bool uneven = low1Fps > 0.0 && smoothness < 0.75;

        const char* pacing = "smooth";
        if (lowAvg && uneven)
            pacing = "low FPS and uneven pacing";
        else if (lowAvg)
            pacing = "low FPS (even pacing)";
        else if (uneven)
            pacing = "uneven pacing (hitches despite a good average)";

        if (lowAvg || uneven)
        {
            WriteFrameTime(
                "[fps] VERDICT: %s - fps_avg=%.1f fps_1%%low=%.1f (%.0f%% of average)",
                pacing, avgFps, low1Fps, smoothness * 100.0
            );
        }
    }

    // Cause split: if the slowest logic tick consumed most of its frame, the
    // hitch is logic/script bound (a mod or script), otherwise it is render
    // bound (GPU/driver/streaming). Stated as a hint, not proof. The logic tick
    // can legitimately be longer than the frame period it was sampled in (a
    // stall inside logic that the period sampling did not span), so the ratio is
    // reported as a multiple past 100% instead of an impossible percentage.
    if (m_frameTimeWorstLogicPeriodMs > 0.0)
    {
        const double ratio = m_frameTimeWorstLogicMs / m_frameTimeWorstLogicPeriodMs;

        if (ratio > 1.0)
        {
            // The logic tick outlasted its own frame period: a stall that the
            // period sampling did not span (a one-off hitch inside logic), not a
            // steady "logic bound" load. Labelling this "logic/script bound" would
            // contradict the per-frame cause split, which reports such a frame as
            // render/streaming bound.
            WriteFrameTime(
                "worst_frame logic_ms=%.2f period_ms=%.2f cause=logic overrun/stall "
                "(logic tick ran %.1fx its own frame period - a one-off hitch inside "
                "logic, not steady script load)",
                m_frameTimeWorstLogicMs,
                m_frameTimeWorstLogicPeriodMs,
                ratio
            );
        }
        else
        {
            const char* cause = ratio >= 0.8
                ? "logic/script bound"
                : (ratio >= 0.4 ? "mixed" : "render/streaming bound");
            WriteFrameTime(
                "worst_frame logic_ms=%.2f period_ms=%.2f logic_share=%.1f%% cause=%s",
                m_frameTimeWorstLogicMs,
                m_frameTimeWorstLogicPeriodMs,
                100.0 * ratio,
                cause
            );
        }
    }

    // Interval-over-interval comparison: a falling average FPS across intervals
    // is the earliest sign of a slowdown that the in-interval stats cannot show.
    if (m_frameTimeHaveLastInterval && m_frameTimeLastIntervalAvgPeriodMs > 0.0)
    {
        const double lastFps = 1000.0 / m_frameTimeLastIntervalAvgPeriodMs;
        const double deltaPct = lastFps > 0.0
            ? 100.0 * (avgFps - lastFps) / lastFps
            : 0.0;
        WriteFrameTime(
            "trend fps_prev=%.1f fps_now=%.1f delta=%+.2f%% %s",
            lastFps,
            avgFps,
            deltaPct,
            deltaPct <= -10.0 ? "<-- FPS dropping" : ""
        );
    }

    // Threshold warnings. Kept explicit so the log is greppable for [fps].
    if (m_frameTimeMinFps > 0 && avgFps < static_cast<double>(m_frameTimeMinFps))
    {
        WriteFrameTime(
            "[fps] WARNING average FPS %.1f is below MinFps=%u",
            avgFps,
            static_cast<unsigned>(m_frameTimeMinFps)
        );
    }
    if (m_frameTimeLowFpsWarn > 0 && low1Fps < static_cast<double>(m_frameTimeLowFpsWarn))
    {
        WriteFrameTime(
            "[fps] WARNING 1%% low FPS %.1f is below LowFpsWarn=%u (stuttery)",
            low1Fps,
            static_cast<unsigned>(m_frameTimeLowFpsWarn)
        );
    }
    if (m_frameTimeLowFpsWarn01 > 0 && low01Fps < static_cast<double>(m_frameTimeLowFpsWarn01))
    {
        WriteFrameTime(
            "[fps] WARNING 0.1%% low FPS %.1f is below LowFpsWarn01=%u (long hitches)",
            low01Fps,
            static_cast<unsigned>(m_frameTimeLowFpsWarn01)
        );
    }

    // Frame-budget compliance: the single "does it hold the target FPS?" number.
    if (m_frameTimeBudgetMs > 0.0)
    {
        const unsigned long long judged = m_frameTimeBudgetMet + m_frameTimeBudgetMissed;
        const double metPct = judged != 0
            ? 100.0 * static_cast<double>(m_frameTimeBudgetMet) / static_cast<double>(judged)
            : 0.0;
        const double targetFps = 1000.0 / m_frameTimeBudgetMs;

        // A budget below the achieved median frame period cannot be met in any
        // meaningful way: on a vsync-locked 60 FPS build the median period is
        // ~16.67 ms, so a budget of 16.00 asks for something impossible. A few
        // frames may still slip under by luck, so test the budget against the
        // median rather than requiring zero met frames - otherwise "met by 1/600"
        // repeats every interval and looks like a real miss. Print the line once
        // with a note, then stay quiet: it is the setting talking, not an event.
        const bool impossibleTarget =
            m_frameTimeBudgetMs < p50 * 0.995;
        if (!(impossibleTarget && m_frameTimeBudgetNoteLogged))
        {
            WriteFrameTime(
                "[fps] budget <=%.2f ms (%.0f FPS) met by %llu/%llu frames (%.2f%%), missed %llu",
                m_frameTimeBudgetMs,
                targetFps,
                m_frameTimeBudgetMet,
                judged,
                metPct,
                m_frameTimeBudgetMissed
            );

            if (impossibleTarget)
            {
                m_frameTimeBudgetNoteLogged = true;
                WriteFrameTime(
                    "[fps] note: FrameBudgetMs=%.2f is below this build's median frame "
                    "period (%.2f ms), so it cannot be met - set it to your real "
                    "target (16.67 for 60 FPS, 8.33 for 120) or 0 to disable (this "
                    "line is printed once)",
                    m_frameTimeBudgetMs,
                    p50
                );
            }
        }
    }

    // Sustained-low detection: count consecutive intervals whose average FPS was
    // below MinFps. One slow interval is noise (a load hitch, an alt-tab); a run
    // of them is a real regression and worth a distinct, greppable warning.
    if (m_frameTimeMinFps > 0 && m_frameTimeSustainedLowIntervals > 0)
    {
        if (avgFps < static_cast<double>(m_frameTimeMinFps))
        {
            ++m_frameTimeConsecutiveLowIntervals;
            if (m_frameTimeConsecutiveLowIntervals >= m_frameTimeSustainedLowIntervals &&
                !m_frameTimeSustainedLowReported)
            {
                m_frameTimeSustainedLowReported = true;
                WriteFrameTime(
                    "[fps] SUSTAINED LOW: average FPS has been below MinFps=%u for "
                    "%u consecutive intervals (< %.1f FPS) - a steady regression, not "
                    "a one-off hitch",
                    static_cast<unsigned>(m_frameTimeMinFps),
                    static_cast<unsigned>(m_frameTimeConsecutiveLowIntervals),
                    static_cast<double>(m_frameTimeMinFps)
                );
            }
        }
        else
        {
            if (m_frameTimeSustainedLowReported)
            {
                WriteFrameTime(
                    "[fps] recovered: average FPS %.1f is back at/above MinFps=%u "
                    "after %u slow interval(s)",
                    avgFps,
                    static_cast<unsigned>(m_frameTimeMinFps),
                    static_cast<unsigned>(m_frameTimeConsecutiveLowIntervals)
                );
            }
            m_frameTimeConsecutiveLowIntervals = 0;
            m_frameTimeSustainedLowReported = false;
        }
    }

    if (m_frameTimeStreamSamples > 0)
    {
        // Bytes so the reader sees exact values; MB in parentheses for scale.
        const double avgUsedMb =
            static_cast<double>(m_frameTimeStreamUsedSum / m_frameTimeStreamSamples) / (1024.0 * 1024.0);
        const double maxUsedMb =
            static_cast<double>(m_frameTimeStreamUsedMax) / (1024.0 * 1024.0);
        const double availMb =
            static_cast<double>(m_frameTimeStreamAvail) / (1024.0 * 1024.0);
        const double pct = m_frameTimeStreamAvail > 0
            ? 100.0 * static_cast<double>(m_frameTimeStreamUsedMax) /
              static_cast<double>(m_frameTimeStreamAvail)
            : 0.0;
        const double avgReq = m_frameTimeStreamSamples > 0
            ? static_cast<double>(m_frameTimeStreamReqSum) /
              static_cast<double>(m_frameTimeStreamSamples)
            : 0.0;
        const double busyShare = m_frameTimeStreamSamples > 0
            ? 100.0 * static_cast<double>(m_frameTimeStreamReqBusy) /
              static_cast<double>(m_frameTimeStreamSamples)
            : 0.0;
        WriteFrameTime(
            "streaming avail=%.1f MB used avg=%.1f MB max=%.1f MB (max %.1f%% of avail) "
            "req avg=%.1f max=%llu busy=%.1f%%",
            availMb,
            avgUsedMb,
            maxUsedMb,
            pct,
            avgReq,
            m_frameTimeStreamReqMax,
            busyShare
        );
    }

    if (m_frameTimeHistogramEnabled)
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

    // Worst frames of the interval, so a stutter can be tied to a moment/script
    // count. Only worth listing when the interval actually stuttered (a frame at
    // or above WarnMs): on a smooth interval the top-15 are all ~16.7 ms and the
    // table is pure noise. m_frameTimeWorst is sorted descending, so [0] is worst.
    const size_t listed = std::min<size_t>(m_frameTimeTopStutter, m_frameTimeWorstCount);
    const bool hadStutter =
        listed > 0 && m_frameTimeWorst[0].periodMs >= static_cast<double>(m_frameTimeWarnMs);
    if (hadStutter)
    {
        // Split the listed hitches into world-load (early uptime) and steady state,
        // so a burst of big frames right after a load is not read as ongoing
        // stutter. In the log this is the difference between "the game stutters"
        // and "the game hitched once while it was loading the world".
        size_t startupCount = 0;
        for (size_t i = 0; i < listed; ++i)
        {
            if (m_frameTimeWorst[i].uptimeSec < kFrameTimeStartupSec)
                ++startupCount;
        }

        WriteFrameTime("---- worst frames ----");
        WriteFrameTime("%-3s %12s %12s %8s %9s %11s %8s %-9s",
            "#", "period_ms", "logic_ms", "scripts", "uptime_s", "stream_MB", "req", "phase");
        for (size_t i = 0; i < listed; ++i)
        {
            const auto& s = m_frameTimeWorst[i];
            WriteFrameTime(
                "%-3zu %12.2f %12.2f %8zu %9u %11.1f %8llu %-9s",
                i + 1,
                s.periodMs,
                s.logicMs,
                s.scripts,
                static_cast<unsigned>(s.uptimeSec),
                static_cast<double>(s.streamUsed) / (1024.0 * 1024.0),
                s.streamReq,
                s.uptimeSec < kFrameTimeStartupSec ? "load" : "steady"
            );
        }

        if (startupCount > 0 && startupCount < listed)
        {
            WriteFrameTime(
                "[fps] note: %zu of %zu listed hitches happened during world load "
                "(uptime < %us) - those are load-time hitches, not steady-state stutter",
                startupCount, listed, static_cast<unsigned>(kFrameTimeStartupSec));
        }
        else if (startupCount == listed)
        {
            WriteFrameTime(
                "[fps] note: all %zu listed hitches are load-time (uptime < %us); "
                "the steady-state frames were within the target",
                listed, static_cast<unsigned>(kFrameTimeStartupSec));
        }
    }

    // Remember this interval's average so the next report can show the trend.
    m_frameTimeLastIntervalAvgPeriodMs = avgPeriod;
    m_frameTimeHaveLastInterval = true;

    ResetFrameTimeInterval();
}
