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
    uint64_t m_lastPrivateUsage = 0;
    size_t m_lastMemoryQueueCount = 0;
    size_t m_lastMemoryCustomCount = 0;
    bool m_memoryBaselineReady = false;
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
    std::string CrashLogPath() const;
    std::string CrashInfoPath() const;
    std::string CrashInfoRuPath() const;
    std::string CrashInfoAutoPath() const;
    std::string ConfigPath() const;

    void LoadConfig();
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
        std::string* diagnostics
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

