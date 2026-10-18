#pragma once

#include "CLEO.h"
#include "CLEO_Debug.h"
#include <windows.h>
#include <string>
#include <vector>
#include <deque>
#include <map>
#include <set>
#include <fstream>
#include <cstdint>
#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

class DebugUtils
{
public:
    DebugUtils();
    ~DebugUtils();

private:
    struct CrashInfoEntry
    {
        std::vector<DWORD> errorAddresses;
        std::vector<std::string> errorModules;
        std::vector<DWORD> backtraceAddresses;
        std::vector<std::string> backtraceModules;
        std::vector<DWORD> lastCommands;
        std::string scriptName;
        DWORD lastOpcode = 0xFFFFFFFFu;
        DWORD exceptionCode = 0;
        int accessType = -1;
        bool wildcardError = false;
        bool hasMatcher = false;
        bool autoDiscovered = false;
        std::string name;
        std::string issue;
        std::string about;
        std::string solution;
        std::string description;
        std::string type;
        std::vector<std::string> notes;
    };

    static DebugUtils* s_instance;

    std::ofstream m_coreLog;
    std::ofstream m_scriptLog;
    std::ofstream m_diagnosticLog;
    std::ofstream m_memoryLog;
    std::mutex m_coreMutex;
    std::mutex m_diagnosticMutex;
    std::mutex m_memoryMutex;
    std::string m_lastCoreMessage;
    size_t m_lastCoreRepeatCount = 0;
    size_t m_corePendingWrites = 0;
    size_t m_coreBytes = 0;
    bool m_coreLimitNoticeWritten = false;
    static constexpr size_t kCoreLogMaxBytes = 1024u * 1024u;

    struct ScriptLogEvent
    {
        char line[768] = {};
    };

    static constexpr uint32_t kScriptQueueSize = 8192;
    static constexpr uint32_t kScriptQueueMask = kScriptQueueSize - 1;

    std::array<ScriptLogEvent, kScriptQueueSize> m_scriptQueue;
    std::atomic<uint32_t> m_scriptWriteIndex{ 0 };
    std::atomic<uint32_t> m_scriptReadIndex{ 0 };
    std::atomic<uint32_t> m_droppedScriptEvents{ 0 };
    std::mutex m_scriptWakeMutex;
    std::condition_variable m_scriptWake;
    std::atomic<bool> m_scriptWriterStop{ false };
    std::thread m_scriptWriterThread;
    std::vector<CrashInfoEntry> m_crashInfo;
    std::string m_lastScriptMessage;
    size_t m_lastScriptRepeatCount = 0;
    std::string m_lastDiagnosticMessage;
    size_t m_lastDiagnosticRepeatCount = 0;
    std::string m_lastMemoryMessage;
    size_t m_lastMemoryRepeatCount = 0;
    std::set<uintptr_t> m_seenScripts;

    DWORD m_lastMemoryLogTick = 0;
    DWORD m_memorySessionStartTick = 0;
    uint64_t m_lastPrivateUsage = 0;
    size_t m_lastMemoryQueueCount = 0;
    size_t m_lastMemoryCustomCount = 0;
    bool m_memoryBaselineReady = false;
    DWORD m_memoryWarnMegabytes = 0;
    uint64_t m_peakPrivateUsage = 0;
    bool m_memoryWarnIssued = false;
    bool m_memoryRotateActive = false;
    size_t m_lastMemoryNativeCount = 0;
    size_t m_lastMemoryCustomObjectBytes = 0;
    size_t m_lastMemoryCustomCodeBytes = 0;
    size_t m_lastMemoryCleoImage = 0;
    size_t m_lastMemoryDebugImage = 0;
    size_t m_lastMemoryKnownBytes = 0;
    unsigned long long m_lastMemoryScriptBlocks = 0;
    unsigned long long m_lastMemoryScriptBytes = 0;
    bool m_memorySummaryHeaderWritten = false;
    size_t m_scriptBytes = 0;
    size_t m_scriptCommands = 0;
    bool m_crashHandlerInstalled = false;
    PVOID m_vectoredHandler = nullptr;
    volatile LONG m_crashInProgress = 0;
    std::atomic<bool> m_scriptWriterStarted{ false };

    bool m_crashEnabled = true;
    bool m_crashWindowEnabled = true;
    bool m_crashBacktraceEnabled = true;
    bool m_crashOpcodeHistory = false;
    DWORD m_crashMaxFrames = 32;

    struct OpcodeHistoryEntry
    {
        DWORD opcode = 0xFFFFFFFF;
        DWORD result = 0xFFFFFFFF;
        DWORD offset = 0;
        uintptr_t scriptPtr = 0;
        char scriptName[9] = "none";
    };

    static constexpr size_t kOpcodeHistorySize = 256;
    std::array<OpcodeHistoryEntry, kOpcodeHistorySize> m_opcodeHistory{};
    size_t m_opcodeHistoryNext = 0;
    size_t m_opcodeHistoryCount = 0;
    std::mutex m_opcodeHistoryMutex;

    uintptr_t m_currentScriptPtr = 0;
    DWORD m_currentScriptStartTick = 0;
    size_t m_currentScriptCommands = 0;

    size_t m_commandLimit = 2000000;
    DWORD m_timeLimitSeconds = 5;
    bool m_scriptLogEnabled = true;
    bool m_scriptOpcodeTrace = false;
    bool m_functionTrace = false;
    bool m_scriptDeduplicate = true;
    bool m_memoryLogEnabled = true;
    bool m_memoryTrace = false;
    bool m_diagnosticLogEnabled = false;
    bool m_legacyDebugOpcodes = false;
    bool m_languageRussian = false;

    // Per-script profiler measurement (core CScriptProfiler via CLEO_SetProfilingEnabled).
    // Results are appended to the diagnostic log at a fixed interval.
    bool m_profilerEnabled = false;
    DWORD m_profilerIntervalSeconds = 60;
    DWORD m_profilerTop = 20;
    DWORD m_lastProfilerTick = 0;
    // Measure the profiler's own overhead (two QPC reads per opcode) and report
    // the instrumentation-free time as a separate column.
    bool m_profilerOverheadMode = false;
    // How many hot opcodes to list per report (0 = no opcode histogram).
    DWORD m_profilerTopOpcodes = 15;

    // Memory attribution: when on, periodically dump resident memory per loaded
    // module (and how much each grew since the previous dump) so growth that is
    // not CLEO's own can be traced to a module.
    bool m_memoryModuleMap = false;
    DWORD m_memoryModuleMapIntervalSeconds = 30;
    DWORD m_lastModuleMapTick = 0;
    std::map<std::string, unsigned long long> m_lastModuleResident;

    // Per-script memory-leak tracker. Samples the CLEO script-memory arena per
    // script (CLEO_DebugGetScriptMemoryOwners) and keeps a moving baseline, so a
    // script whose live allocation only ever grows can be named and its share of
    // the arena / of the process quantified. Off by default: it allocates.
    bool m_leakTrackerEnabled = false;
    DWORD m_leakIntervalSeconds = 30;
    DWORD m_leakMinGrowthBytes = 64u * 1024u;  // ignore growth below this
    DWORD m_lastLeakTick = 0;

    // Process-private baseline for the leak report, so it can state how much the
    // *whole process* grew (and the %/min) even when the growth is not in the
    // CLEO script arena. Captured at the first sample.
    uint64_t m_leakFirstPrivateBytes = 0;
    DWORD m_leakFirstPrivateUptimeSec = 0;

    // Previous report's process private bytes / uptime, so the report can show an
    // interval-over-interval trend: a steady climb is a real leak, a decelerating
    // one is a pool/streaming fill that is plateauing.
    uint64_t m_leakLastPrivateBytes = 0;
    DWORD m_leakLastUptimeSec = 0;

    // Streaming-pool bytes at the first sample, so the report can subtract the
    // world-loading fill from the process growth. A large part of the private
    // growth during the first minutes is the streaming pool filling, not a leak;
    // separating it keeps the verdict honest.
    uint64_t m_leakFirstStreamUsedBytes = 0;
    bool m_leakHaveStreamBaseline = false;

    struct LeakScriptState
    {
        std::string name;
        unsigned long long firstBytes = 0;   // live bytes at the first sample
        unsigned long long lastBytes = 0;    // live bytes at the most recent sample
        unsigned long long peakBytes = 0;
        unsigned long long firstBlocks = 0;
        unsigned long long lastBlocks = 0;
        DWORD firstUptimeSec = 0;
        DWORD lastUptimeSec = 0;
        DWORD samples = 0;                   // how many samples it was present in
        DWORD growthStreak = 0;              // consecutive samples of net growth
        DWORD flatStreak = 0;                // consecutive samples with no growth
        DWORD shrinkCount = 0;               // times the live bytes fell (churn proof)
        DWORD breakStreak = 0;               // consecutive growth runs broken by a drop
        bool releasedSince = false;          // it ever dropped back to zero
    };

    std::map<uintptr_t, LeakScriptState> m_leakScripts; // keyed by script pointer
    std::map<std::string, LeakScriptState> m_leakRetired; // by name, after unload
    std::set<uintptr_t> m_leakSeenThisSample;

    void SampleScriptMemoryLeaks();
    void WriteLeakReport(const char* reason);
    // Confidence that a tracked script is really leaking, 0-100%. Combines the
    // strength of the evidence (sample count, monotonic growth streak, growth
    // magnitude) and subtracts for churn (times memory was released). Answers the
    // "how many percent sure is this a leak?" question directly.
    static double LeakConfidencePercent(const LeakScriptState& state,
                                        unsigned long long growthBytes,
                                        unsigned long long arenaBytes);
    void UpdateLeakEntry(
        uintptr_t scriptPtr,
        const std::string& name,
        unsigned long long blocks,
        unsigned long long bytes
    );

    // Frame-time / stutter monitor. Samples the frame period and the logic-tick
    // duration with QueryPerformanceCounter and writes a dedicated log, so frame
    // hitches can be measured and attributed without cluttering the other logs.
    // Off by default; when disabled no QPC is read at all (zero overhead).
    bool m_frameTimeEnabled = false;
    DWORD m_frameTimeIntervalSeconds = 10;
    DWORD m_frameTimeWarnMs = 50;         // frame period at/above this = stutter event
    DWORD m_frameTimeTopStutter = 15;     // worst frames listed per report
    bool m_frameTimeHistogramEnabled = true; // emit a frame-period histogram
    bool m_frameTimeStreamingEnabled = true; // snapshot ms_memoryUsed per frame (SA 1.0)
    int  m_frameTimeStreamLimitMb = 0;       // request CStreaming budget: 0=off,-1=auto,>0=MB
    DWORD m_frameTimeMaxLogMegabytes = 16;
    // Frame-quality thresholds. MinFps warns when the interval's average FPS falls
    // below it; LowFpsWarn warns when the 1%-low FPS falls below it; LowFpsWarn01
    // warns when the 0.1%-low FPS falls below it (all 0 = off).
    DWORD m_frameTimeMinFps = 0;
    DWORD m_frameTimeLowFpsWarn = 0;
    DWORD m_frameTimeLowFpsWarn01 = 0;
    // Sustained-low detection: warn when the average FPS is below MinFps for this
    // many consecutive intervals. One slow interval is noise; a run of them is a
    // real regression. 0 = off. Needs MinFps > 0.
    DWORD m_frameTimeSustainedLowIntervals = 0;
    DWORD m_frameTimeConsecutiveLowIntervals = 0; // running count since the last good interval
    bool m_frameTimeSustainedLowReported = false;
    // A frame budget in ms (0 = off). The report states what share of the
    // interval's frames met it, so "does this build hold 60 FPS?" has one number.
    double m_frameTimeBudgetMs = 0.0;
    DWORD m_lastFrameTimeReportTick = 0;
    LARGE_INTEGER m_frameTimeFreq{};
    bool m_frameTimeFreqReady = false;
    LARGE_INTEGER m_frameTimeLastBefore{};  // previous frame's submit time
    bool m_frameTimeHaveLast = false;
    LARGE_INTEGER m_frameTimeLogicStart{};  // this frame's submit time
    double m_frameTimeLastPeriodMs = 0.0;   // period measured for the current frame
    double m_frameTimeLastLogicMs = 0.0;    // previous frame's logic-tick duration
    unsigned long long m_frameTimeFrames = 0;
    double m_frameTimeSumPeriodMs = 0.0;
    double m_frameTimeSumLogicMs = 0.0;
    double m_frameTimeMinPeriodMs = 0.0;
    double m_frameTimeMaxPeriodMs = 0.0;
    double m_frameTimeMaxLogicMs = 0.0;
    unsigned long long m_frameTimeStutterCount = 0;
    double m_frameTimeStutterLostMs = 0.0;  // time above the warning threshold
    double m_frameTimeIntervalMs = 0.0;     // wall-clock span of the interval
    size_t m_frameTimeScriptsLast = 0;

    // Frame-quality accumulators. The period of every frame is kept in a bounded
    // ring so the report can compute true percentiles / 1%-low FPS instead of
    // only min/avg/max, and so jitter (standard deviation) is available.
    static constexpr size_t kFrameTimePeriodSamples = 4096;
    std::array<float, kFrameTimePeriodSamples> m_frameTimePeriodRing{};
    size_t m_frameTimePeriodCount = 0;     // valid entries in the ring
    size_t m_frameTimePeriodNext = 0;      // next write slot
    double m_frameTimeSumPeriodSqMs = 0.0; // sum of period^2, for the stddev
    unsigned long long m_frameTimeSpike33 = 0;  // period >= 33.3 ms (< 30 FPS)
    unsigned long long m_frameTimeSpike50 = 0;  // period >= 50 ms   (< 20 FPS)
    unsigned long long m_frameTimeSpike100 = 0; // period >= 100 ms  (< 10 FPS)
    unsigned long long m_frameTimeSpike250 = 0; // period >= 250 ms  (< 4 FPS)
    unsigned long long m_frameTimeBudgetMet = 0;    // frames within m_frameTimeBudgetMs
    unsigned long long m_frameTimeBudgetMissed = 0; // frames over the budget
    bool m_frameTimeBudgetNoteLogged = false;       // the "impossible target" note, once
    double m_frameTimeWorstLogicMs = 0.0;       // slowest logic tick in the interval
    double m_frameTimeWorstLogicPeriodMs = 0.0; // its frame period, to split cause
    // Per-frame cause split: a frame is "logic-bound" when its logic tick used at
    // least kFrameTimeLogicShare of the frame period, otherwise "render-bound"
    // (GPU / driver / streaming). Counted for every frame and, separately, for the
    // stutter frames, so the report can say which side owns the hitches instead of
    // only labelling the single worst frame.
    unsigned long long m_frameTimeLogicBoundFrames = 0;
    unsigned long long m_frameTimeStutterLogicBound = 0;
    double m_frameTimeLastIntervalAvgPeriodMs = 0.0; // previous interval's average
    bool m_frameTimeHaveLastInterval = false;
    // Logic-bound share of the frame period (0.8 = logic used 80% of the frame).
    static constexpr double kFrameTimeLogicShare = 0.8;

    // Streaming memory snapshot (GTA SA 1.0 only). Read alongside each frame to
    // answer "do stutters coincide with streaming memory pressure / eviction
    // churn?". The engine updates ms_memoryUsed on the same logic thread these
    // hooks run on, so the read is race-free.
    unsigned long long m_frameTimeStreamUsedSum = 0;
    unsigned long long m_frameTimeStreamUsedMax = 0;
    unsigned long long m_frameTimeStreamAvail = 0;
    unsigned long long m_frameTimeStreamSamples = 0;
    unsigned long long m_frameTimeStreamReqSum = 0;   // ms_numModelsRequested sum
    unsigned long long m_frameTimeStreamReqMax = 0;
    unsigned long long m_frameTimeStreamReqBusy = 0;  // frames with a pending request

    // Frame-period histogram. Fixed bucket edges in milliseconds so the same
    // bins are comparable across intervals; the last bucket is the overflow.
    static constexpr size_t kFrameTimeHistogramBuckets = 8;
    static constexpr double kFrameTimeHistogramEdgesMs[kFrameTimeHistogramBuckets] =
    {
        16.6, 20.0, 25.0, 33.3, 50.0, 100.0, 250.0
    };
    unsigned long long m_frameTimeHistogram[kFrameTimeHistogramBuckets] = {};

    struct FrameTimeSample
    {
        double periodMs = 0.0;
        double logicMs = 0.0;
        size_t scripts = 0;
        DWORD uptimeSec = 0;
        unsigned long long streamUsed = 0;  // ms_memoryUsed at the frame (bytes)
        unsigned long long streamReq = 0;   // ms_numModelsRequested at the frame
    };
    static constexpr size_t kFrameTimeMaxWorst = 32;
    std::array<FrameTimeSample, kFrameTimeMaxWorst> m_frameTimeWorst{};
    size_t m_frameTimeWorstCount = 0;

    std::ofstream m_frameTimeLog;
    std::mutex m_frameTimeMutex;
    size_t m_frameTimeBytes = 0;
    bool m_frameTimeLimitNoticeWritten = false;

    // ---------------------------------------------------------------------
    // RenderWare / CLEO error diagnostics (reported inside the frame-time log).
    //
    // RenderWare stores the last error as a small RwError {pluginID, errorCode}
    // pair in a global; the plugin-sdk resolves RwErrorGet at a fixed address.
    // Polling it once per frame is read-only and cheap, and it is the only way
    // to see a RenderWare error that did not (yet) crash the process. Nothing is
    // patched: the monitor observes, it never modifies game code.
    //
    // CLEO-side errors arrive synchronously through OnCoreLog; the stack at that
    // moment is scanned for the first return address outside the core, DebugUtils
    // and the game itself, which is the module (mod/plugin/script loader) that
    // raised it.
    // ---------------------------------------------------------------------
    // Error diagnostics (RenderWare + CLEO). Off by default: the RenderWare poll
    // runs every frame and the module scan walks the stack, so the feature costs
    // a little even when nothing is wrong. Turn it on when hunting errors.
    bool m_errorDiagEnabled = false;
    bool m_rwErrorWatchEnabled = false;
    bool m_errorModuleScanEnabled = false;
    // Capture the exact filename of every RenderWare file open by wrapping the
    // rwfopen/rwfexist pointers in the RwFileFunctions table (data-only patch,
    // no code is modified). This is what turns E_RW_BADOPEN / E_RW_READTEXMASK
    // from "some texture failed" into "models\foo.txd failed". Opt-in.
    bool m_fileWatchEnabled = false;
    // Report failed opens (E_RW_BADOPEN/E_RW_READTEXMASK sources). Separate from
    // FileWatchNames: an empty watchlist still lets failures through, so this is
    // the switch that actually turns the failed-open report off.
    bool m_fileWatchFailuresEnabled = true;

    struct ErrorRecord
    {
        unsigned int key = 0;          // unique id for dedup (rw: plugin<<12 ^ code)
        DWORD count = 0;               // occurrences since game begin
        DWORD firstTick = 0;           // GetTickCount of the first occurrence
        DWORD lastTick = 0;            // GetTickCount of the most recent
        DWORD firstUptimeSec = 0;      // uptime of the first occurrence
        DWORD lastUptimeSec = 0;       // uptime of the most recent occurrence
        DWORD lastReportedCount = 0;   // count already printed by WriteErrorReport
        unsigned int pluginId = 0;     // RenderWare pluginID (rw errors)
        unsigned int errorCode = 0;    // raw RenderWare errorCode (rw errors)
        std::string name;              // resolved name when known
        std::string module;            // owning module when resolvable
        std::string lastScript;        // script active when last seen
        DWORD lastOpcode = 0xFFFFFFFFu;
        std::vector<std::string> modules; // modules seen in the stack for this error
        bool renderWare = false;       // came from the RW poll (no raising stack)
        std::string text;              // human-readable meaning when known
    };

    std::map<unsigned int, ErrorRecord> m_errorRecords; // keyed by record.key
    std::vector<unsigned int> m_errorOrder;             // first-seen order
    std::mutex m_errorMutex;

    // Failed RenderWare file opens captured by the open watch (see below).
    struct FileOpenFailure
    {
        DWORD count = 0;
        DWORD firstUptimeSec = 0;
        DWORD lastUptimeSec = 0;
        DWORD lastReportedCount = 0;
        std::string lastScript;
        std::string resolvedPath;      // absolute path the game looked in
        // Modules on the stack at the first failed open (the plugin/mod that
        // asked RenderWare to load the file), when ModuleScan is on.
        std::vector<std::string> modules;
        std::string chainVia; // "unwind" or "scan": how the chain was captured
        bool modulesScanned = false;
    };
    std::map<std::string, FileOpenFailure> m_fileOpenFailures; // keyed by path
    std::vector<std::string> m_fileOpenOrder;                  // first-seen order
    std::mutex m_fileOpenMutex;
    bool m_fileWatchInstalled = false;
    void* m_fileWatchTable = nullptr;         // RwFileFunctions*
    void* m_fileWatchOriginalFopen = nullptr; // the rwfopen we replaced

    // Optional comma-separated watchlist (FileWatchNames). A file whose path
    // contains one of these substrings is recorded on EVERY open - success or
    // failure - so "who loads particle.txd" can be answered even when the file
    // exists. Matched case-insensitively against the requested path.
    std::vector<std::string> m_fileWatchNames;

    struct FileOpenRecord
    {
        DWORD count = 0;
        DWORD firstUptimeSec = 0;
        DWORD lastUptimeSec = 0;
        DWORD lastReportedCount = 0;
        bool failed = false;
        std::string lastScript;
        std::string resolvedPath;
        std::vector<std::string> modules;
        std::string chainVia; // "unwind" or "scan": how the chain was captured
        bool modulesScanned = false;
    };
    std::map<std::string, FileOpenRecord> m_fileWatchRecords; // keyed by path
    std::vector<std::string> m_fileWatchRecordOrder;
    std::mutex m_fileWatchRecordMutex;

    bool IsWatchedFileName(const char* path) const;
    void RecordWatchedOpen(const char* path, bool failed);
    void WriteWatchedOpenReport(bool forceAll);

    void InstallFileOpenWatch();
    void RemoveFileOpenWatch();
    void RecordFileOpenFailure(const char* path);
    void WriteFileOpenReport(bool forceAll);
    static void* __cdecl FileOpenWatchFopen(const char* name, const char* mode);

    // Texture-read watch. E_RW_READTEXMASK ("Unable to read Texture %s / Mask %s")
    // is raised by the *name-based* reader RwTextureRead (SA 0x4C7510) when its
    // read callback returns NULL - the only raiser that matches GTA SA's texture
    // path. The error poll runs one frame later, when the raising stack is gone,
    // so it cannot name the mod. We wrap that read callback (via the public
    // RwTextureGetReadCallBack/RwTextureSetReadCallBack, SA 0x7F3520/0x7F3500) and
    // the stream reader RwGlobals.stdFunc[26] (SA 0x4CD820, the TXD-internal path);
    // both run while the game thread is inside the mod, so the requester chain is
    // captured at the right moment. Data-only swaps (no code is touched). Opt-in.
    bool m_textureWatchEnabled = false;
    bool m_textureReadCbInstalled = false;   // RwTextureRead read callback swapped
    void* m_textureReadCbOriginal = nullptr;
    bool m_textureWatchInstalled = false;    // RwGlobals.stdFunc[26] swapped
    void* m_textureWatchSlot = nullptr;      // &RwGlobals.stdFunc[26]
    void* m_textureWatchOriginalRead = nullptr;
    // Exact-caller entry hook on RwTextureRead itself. A 5-byte JMP detour
    // (no return address is pushed) so the wrapper's _ReturnAddress() is the
    // caller of RwTextureRead - the mod that asked for the texture, exactly.
    // Opt-in (TextureExactCaller=1) because it is the only code patch in this
    // file; everything else is a data-only pointer swap.
    bool m_textureEntryHookEnabled = false;
    DWORD m_textureEntryAddress = 0;         // 0 = use the built-in default
    bool m_textureEntryHookInstalled = false;
    void* m_textureEntryTarget = nullptr;    // RwTextureRead entry (game image)
    BYTE m_textureEntrySaved[5] = {};        // original prologue bytes
    void* m_textureEntryTrampoline = nullptr; // executable stub: saved bytes + JMP back
    std::string m_lastTextureExactCaller;    // captured by the wrapper, read by Record
    // The direct caller of RwTextureRead labelled even when it is the game or
    // CLEO. The exact-caller field stays empty for engine-initiated reads (the
    // engine loads the .txd, not the mod), so this says "GTA_SA.exe+0x..." and
    // the report can explain why the answer is only the scanned mod frame.
    std::string m_lastTextureDirectCallerRaw;
    // True while RwTextureRead's entry hook is running its trampoline. The read
    // callback fires inside that window; it must then NOT record the failure
    // itself, or every failed read would be counted twice (once by the callback,
    // once by the entry hook on return).
    bool m_textureInEntryHook = false;
    // How many times the entry hook was invoked. Lets the report say whether the
    // hook is actually on the call path (a mod may read textures through a path
    // that never reaches 0x4C7510), instead of leaving it ambiguous.
    unsigned long long m_textureEntryHookCalls = 0;
    struct TextureReadFailure
    {
        DWORD count = 0;
        DWORD firstUptimeSec = 0;
        DWORD lastUptimeSec = 0;
        DWORD lastReportedCount = 0;
        std::string lastScript;
        std::vector<std::string> modules;
        std::string chainVia;
        // The exact caller captured by the RwTextureRead entry hook, when that
        // hook is on and the caller is a mod/plugin (not the game). "mod.asi+0x1a2b".
        std::string exactCaller;
        // True when exactCaller is the DIRECT caller of RwTextureRead (from the
        // entry hook). False when it is only the nearest mod frame from the stack
        // scan, which is a strong hint but not the proven call site.
        bool exactCallerDirect = false;
        // The raw direct caller (any module, incl. the game/CLEO) when the direct
        // caller is NOT a mod - explains why exactCaller had to fall back to the
        // scan ("the engine read this .txd, not a mod").
        std::string directCallerRaw;
        bool modulesScanned = false;
    };
    std::map<std::string, TextureReadFailure> m_textureReadFailures; // keyed by texture name
    std::vector<std::string> m_textureReadOrder;
    std::mutex m_textureReadMutex;
    void InstallTextureReadWatch();
    void RemoveTextureReadWatch();
    // `exactCaller`, when non-null, is the return address of the mod that called
    // RwTextureRead directly (from the entry hook); it is resolved to a
    // "mod.asi+0x1a2b" label and stored as the row's exact caller.
    void RecordTextureReadFailure(const char* textureName, const void* exactCaller = nullptr);
    void WriteTextureReadReport(bool forceAll);
    static void* __cdecl TextureReadCallbackWatch(const char* name, const char* maskName);
    static int __cdecl TextureReadWatchRead(void* stream, void** texture, int length);
    static void* __cdecl TextureReadEntryWatch(const char* name, const char* maskName);
    void InstallTextureEntryHook();
    void RemoveTextureEntryHook();
    // Resolve a caller address to "mod.asi+0x1a2b", or an empty string when it is
    // the game, CLEO, DebugUtils or a system DLL (i.e. not an interesting answer).
    static std::string LabelForModCaller(const void* address);
    // Like LabelForModCaller but without the game/CLEO/system filters: returns
    // "module+0xoffset" for ANY mapped module, including the game. Used to show
    // where an engine-initiated RwTextureRead actually came from.
    static std::string LabelAnyModule(const void* address);
    unsigned int m_rwLastPluginId = 0xFFFFFFFFu;        // last polled RwError pair
    unsigned int m_rwLastErrorCode = 0xFFFFFFFFu;
    bool m_rwErrorPrimed = false;
    // Counters may be bumped from any thread that logs through CLEO (the core
    // debug callback is not game-thread bound), so they are interlocked.
    volatile LONG m_cleoErrorCount = 0;                 // CLEO error-level messages
    volatile LONG m_cleoWarningCount = 0;               // CLEO warning-level messages
    std::set<std::string> m_errorModulesSeen;           // distinct modules that raised errors
    bool m_errorNotePrinted = false;                    // the "note:" line printed once/session
    unsigned long long m_lastReportedCleoErrors = 0;    // CLEO error count at the last report
    unsigned long long m_lastReportedCleoWarnings = 0;  // CLEO warning count at the last report

    void PollRenderWareError();
    void RecordError(
        unsigned int key,
        const std::string& name,
        unsigned int pluginId,
        unsigned int errorCode,
        const std::vector<std::string>& modules,
        bool renderWare = false,
        const char* text = nullptr
    );
    void WriteErrorReport(const char* reason);
    static std::string CurrentScriptName();
    static DWORD CurrentScriptOpcode();
    // Scans the live stack for return addresses in third-party modules. When
    // `strictCount`/`totalCount` are non-null they receive how many accepted
    // frames were verified by a near CALL (E8) and how many were accepted in
    // total, so the caller can report a chain's reliability as a ratio.
    static std::vector<std::string> ScanErrorModules(
        size_t maxModules, size_t* strictCount = nullptr, size_t* totalCount = nullptr);

    // Module attribution shared by the error report and the file watch.
    static bool IsIgnoredModuleHandle(HMODULE module);
    // True for modules loaded from the Windows directory (user32, kernel32,
    // system d3d9, ...). Such a frame is OS noise in a requester chain; a mod
    // that ships its own d3d9.dll lives in the game folder and is kept.
    static bool IsSystemModuleHandle(HMODULE module);
    static std::string FormatFrameLabel(DWORD address);
    static void PushUniqueFrame(std::vector<std::string>& modules, DWORD address);
    // Turns a raw frame list (from any walker) into module labels, dropping the
    // game/CLEO/DebugUtils frames, system DLLs and implausible stale words.
    static std::vector<std::string> ModulesFromFrames(
        const DWORD* frames, size_t count, size_t maxFrames);
    // `via`, when non-null, receives the mechanism that produced the chain
    // ("unwind", "scan-e8" or "scan") so the log can say how trustworthy it is.
    static std::vector<std::string> CaptureRequesterChain(
        size_t maxFrames, const char** via = nullptr);

    std::set<uintptr_t> m_debugScripts;

    struct BreakpointInfo
    {
        uintptr_t scriptPtr = 0;
        std::string name;
        std::string message;
        bool blocking = true;
    };

    std::deque<BreakpointInfo> m_breakpoints;
    std::map<std::string, std::ofstream> m_externalLogs;
    bool m_keysReleased = true;

    std::string DebugDir() const;
    std::string CoreLogPath() const;
    std::string ScriptLogPath() const;
    std::string DiagnosticLogPath() const;
    std::string MemoryLogPath() const;
    std::string FrameTimeLogPath() const;
    std::string CrashLogPath() const;
    std::string CrashInfoPath() const;
    std::string CrashInfoRuPath() const;
    std::string CrashInfoAutoPath() const;
    std::string ConfigPath() const;

    // Writes the ini with comments in `ru` (Russian) or English, keeping any
    // values already present in `raw`. The file is fully regenerated on language
    // change or when keys were added since it was written.
    std::string BuildConfigText(bool ru, const std::map<std::string, std::string>& raw) const;
    void WriteConfigFile(const std::string& path, bool ru,
        const std::map<std::string, std::string>& raw) const;

    void LoadConfig();
    // Logs machine, display and GPU/driver info once at startup (from D3D9).
    void WriteSystemInfo();
    void OpenLogs();
    void CloseLogs();
    void WriteCore(const char* format, ...);
    void WriteScript(const char* format, ...);
    void WriteDiagnostic(const char* format, ...);
    void WriteMemory(const char* format, ...);
    void FlushCoreRepeatLocked();
    void FlushScriptRepeat();
    void FlushDiagnosticRepeatLocked();
    void FlushMemoryRepeatLocked();
    void QueueScriptLine(const char* line);
    void ScriptWriterLoop();
    void WriteExternal(const std::string& filename, bool timestamp, const char* message);
    void RotateScriptLogIfNeeded(size_t incomingBytes);
    void RotateCoreLogIfNeeded(size_t incomingBytes);
    void WriteCoreLimitNoticeLocked();

    void EnsureCrashInfoDatabase();
    void AppendAutomaticCrashInfo(
        const std::string& fingerprint,
        DWORD faultAddress,
        DWORD exceptionCode,
        const char* exceptionType,
        const std::string& faultModule,
        DWORD faultRva,
        int accessType,
        uintptr_t targetAddress,
        const std::string& lastScript,
        DWORD lastOpcode,
        const std::vector<DWORD>& backtrace
    );
    void LoadCrashInfoList();
    void LoadCrashInfoLocalization();
    const CrashInfoEntry* FindCrashInfo(
        DWORD address,
        const std::string& faultModule,
        const std::vector<DWORD>& backtrace,
        DWORD exceptionCode,
        int accessType,
        const std::string& lastScript,
        DWORD lastOpcode
    ) const;

    void WriteCoreHeader();
    void WriteCoreThreadLayout();
    void WriteCoreQueueSnapshot(const char* reason);
    void WriteCoreMemorySummary();

    // Per-module resident-memory dump with deltas since the previous dump. Used
    // to attribute private-bytes growth that is not CLEO's own.
    void WriteModuleMemoryMap();
    void WriteProfilerReport(const char* reason);

    // Frame-time monitor: sample once per frame, flush a report each interval.
    void BeginFrameTimeFrame();
    void EndFrameTimeFrame();
    void WriteFrameTimeReport(const char* reason);
    void WriteFrameTime(const char* format, ...);
    void ResetFrameTimeInterval();
    void InsertWorstTimeSample(const FrameTimeSample& sample);
    double FrameTimeMs(LARGE_INTEGER from, LARGE_INTEGER to) const;
    // Percentile of the frame-period ring, p in [0,1] (0.5 = median). Returns 0
    // when the ring is empty. Sorts a copy, so it is only called at report time.
    double FrameTimePeriodPercentile(double p) const;
    // 1%-low / 0.1%-low average FPS, computed from the slowest tail of the ring.
    double FrameTimeLowFps(double fraction) const;

    static void __cdecl OnCoreLog(int level, const char* format, va_list args);

    static void __stdcall OnGameBegin();
    static void __stdcall OnGameEnd();
    static void __stdcall OnGameProcessBefore();
    static void __stdcall OnGameProcessAfter();
    static BOOL __stdcall OnScriptProcessBefore(CScriptThread* thread);
    static void __stdcall OnScriptProcessAfter(CScriptThread* thread);
    static int __stdcall OnScriptOpcodeBefore(CScriptThread* thread, DWORD opcode);
    static int __stdcall Opcode_DebugOn(CScriptThread* thread);
    static int __stdcall Opcode_DebugOff(CScriptThread* thread);
    static int __stdcall Opcode_Breakpoint(CScriptThread* thread);
    static int __stdcall Opcode_Trace(CScriptThread* thread);
    static int __stdcall Opcode_LogToFile(CScriptThread* thread);
    static int __stdcall Opcode_PrintString(CScriptThread* thread);
    static int __stdcall Opcode_PrintInt(CScriptThread* thread);
    static int __stdcall Opcode_PrintFloat(CScriptThread* thread);
    static int __stdcall OnScriptOpcodeAfter(CScriptThread* thread, DWORD opcode, int result);
    static void __stdcall OnScriptDeleted(CScriptThread* thread);

    static LONG WINAPI VectoredExceptionHandler(PEXCEPTION_POINTERS info);
    static LONG WINAPI UnhandledExceptionFilter(PEXCEPTION_POINTERS info);

    LONG HandleException(PEXCEPTION_POINTERS info);
    void WriteCrashReport(PEXCEPTION_POINTERS info);
    void ShowCrashDialog(
        const char* crashName,
        DWORD exceptionCode,
        const char* exceptionType,
        DWORD faultAddress,
        const std::string& faultModule,
        DWORD faultRva,
        const char* confidence,
        const std::string& issue,
        const std::string& about,
        const std::string& solution,
        const std::vector<std::string>& notes,
        const std::string& lastScript,
        DWORD lastOpcode,
        const std::vector<DWORD>& backtrace
    );
    void RecordOpcode(CScriptThread* thread, DWORD opcode, DWORD result);
    void WriteOpcodeHistory(const char* reason);

    static std::string Basename(const std::string& path);
    static std::string ModuleNameForAddress(DWORD address);
    static std::vector<DWORD> BuildStackWalk(
        PEXCEPTION_POINTERS info,
        DWORD maxFrames,
        std::string* diagnostics,
        bool* heuristicBacktrace
    );
    static std::vector<DWORD> BuildRawStackCandidates(
        DWORD stackPointer,
        size_t scanBytes,
        size_t maxCandidates
    );
    static std::vector<DWORD> BuildHeuristicStackFrames(
        DWORD stackPointer,
        size_t scanBytes,
        size_t maxFrames
    );
    static std::string AccessTypeName(int accessType);
    static std::string MemoryStateName(DWORD state);
    static std::string MemoryProtectName(DWORD protect);
    static std::string BuildCrashFingerprint(
        DWORD exceptionCode,
        DWORD faultAddress,
        DWORD faultRva,
        const std::string& faultModule,
        int accessType,
        uintptr_t targetAddress,
        const std::string& lastScript,
        DWORD lastOpcode,
        const std::vector<DWORD>& backtrace
    );
    static bool SafeReadDword(const DWORD* address, DWORD& value);
    static bool SafeReadBytes(const void* address, void* buffer, size_t size);
    static size_t ScriptOffset(const CScriptThread* thread);
    static void RegisterCallbacks();
    static void UnregisterCallbacks();
};

