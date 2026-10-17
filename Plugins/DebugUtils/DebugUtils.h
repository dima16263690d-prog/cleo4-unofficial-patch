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
        bool releasedSince = false;          // it ever dropped back to zero
    };

    std::map<uintptr_t, LeakScriptState> m_leakScripts; // keyed by script pointer
    std::map<std::string, LeakScriptState> m_leakRetired; // by name, after unload
    std::set<uintptr_t> m_leakSeenThisSample;

    void SampleScriptMemoryLeaks();
    void WriteLeakReport(const char* reason);
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
    // below it; LowFpsWarn warns when the 1%-low FPS falls below it (both 0 = off).
    DWORD m_frameTimeMinFps = 0;
    DWORD m_frameTimeLowFpsWarn = 0;
    DWORD m_lastFrameTimeReportTick = 0;
    LARGE_INTEGER m_frameTimeFreq{};
    bool m_frameTimeFreqReady = false;
    LARGE_INTEGER m_frameTimeLastBefore{};  // previous frame's submit time
    bool m_frameTimeHaveLast = false;
    LARGE_INTEGER m_frameTimeLogicStart{};  // this frame's submit time
    double m_frameTimeLastPeriodMs = 0.0;   // period measured for the current frame
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
    double m_frameTimeWorstLogicMs = 0.0;       // slowest logic tick in the interval
    double m_frameTimeWorstLogicPeriodMs = 0.0; // its frame period, to split cause
    double m_frameTimeLastIntervalAvgPeriodMs = 0.0; // previous interval's average
    bool m_frameTimeHaveLastInterval = false;

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
    bool m_errorDiagEnabled = true;
    bool m_rwErrorWatchEnabled = true;
    bool m_errorModuleScanEnabled = true;

    struct ErrorRecord
    {
        unsigned int key = 0;          // unique id for dedup (rw: plugin<<12 ^ code)
        DWORD count = 0;               // occurrences since game begin
        DWORD firstTick = 0;           // GetTickCount of the first occurrence
        DWORD lastTick = 0;            // GetTickCount of the most recent
        DWORD lastUptimeSec = 0;
        unsigned int pluginId = 0;     // RenderWare pluginID (rw errors)
        unsigned int errorCode = 0;    // raw RenderWare errorCode (rw errors)
        std::string name;              // resolved name when known
        std::string module;            // owning module when resolvable
        std::string lastScript;        // script active when last seen
        DWORD lastOpcode = 0xFFFFFFFFu;
        std::vector<std::string> modules; // modules seen in the stack for this error
    };

    std::map<unsigned int, ErrorRecord> m_errorRecords; // keyed by record.key
    std::vector<unsigned int> m_errorOrder;             // first-seen order
    std::mutex m_errorMutex;
    unsigned int m_rwLastPluginId = 0xFFFFFFFFu;        // last polled RwError pair
    unsigned int m_rwLastErrorCode = 0xFFFFFFFFu;
    bool m_rwErrorPrimed = false;
    // Counters may be bumped from any thread that logs through CLEO (the core
    // debug callback is not game-thread bound), so they are interlocked.
    volatile LONG m_cleoErrorCount = 0;                 // CLEO error-level messages
    volatile LONG m_cleoWarningCount = 0;               // CLEO warning-level messages
    std::set<std::string> m_errorModulesSeen;           // distinct modules that raised errors

    void PollRenderWareError();
    void RecordError(
        unsigned int key,
        const std::string& name,
        unsigned int pluginId,
        unsigned int errorCode,
        const std::vector<std::string>& modules
    );
    void WriteErrorReport(const char* reason);
    static std::string CurrentScriptName();
    static DWORD CurrentScriptOpcode();
    static std::vector<std::string> ScanErrorModules(size_t maxModules);

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

