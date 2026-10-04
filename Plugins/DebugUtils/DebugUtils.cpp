#include "DebugUtils.h"
#include <windows.h>
#include <psapi.h>
#include <TlHelp32.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <sstream>
#include <CTimer.h>

// plugin-sdk declares this GTA SA 1.0 US static reference but does not
// provide a definition in the CLEO/DebugUtils link. Resolve it directly to
// the verified game global used by CTimer::m_CodePause.
bool& CTimer::m_CodePause = *(bool*)0xB7CB48;

DebugUtils* DebugUtils::s_instance = nullptr;

namespace
{
    constexpr DWORD kGtaSa10ActiveScripts = 0x00A8B42C;
    constexpr size_t kScriptLogMaxBytes = 128u * 1024u * 1024u;

    bool IsFatalException(DWORD code)
    {
        switch (code)
        {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        case EXCEPTION_DATATYPE_MISALIGNMENT:
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_IN_PAGE_ERROR:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_INT_OVERFLOW:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_STACK_OVERFLOW:
            return true;
        default:
            return false;
        }
    }

    void SafeFormat(char* buffer, size_t size, const char* format, va_list args)
    {
        if (size == 0) return;
        _vsnprintf_s(buffer, size, _TRUNCATE, format, args);
    }

    void WinAppendLine(const std::string& path, const char* text)
    {
        HANDLE file = CreateFileA(
            path.c_str(),
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );

        if (file == INVALID_HANDLE_VALUE)
            return;

        DWORD written = 0;
        WriteFile(file, text, static_cast<DWORD>(strlen(text)), &written, nullptr);
        WriteFile(file, "\r\n", 2, &written, nullptr);
        CloseHandle(file);
    }

    void SkipUnusedVarArgs(CScriptThread* thread)
    {
        if (!thread)
            return;

        while (*thread->ip != 0)
            CLEO_SkipOpcodeParams(thread, 1);

        thread->ip++;
    }

    const char* CallbackName(int id)
    {
        switch (id)
        {
        case CLEO_CB_GAME_BEGIN: return "GameBegin";
        case CLEO_CB_GAME_END: return "GameEnd";
        case CLEO_CB_GAME_PROCESS_BEFORE: return "GameProcessBefore";
        case CLEO_CB_GAME_PROCESS_AFTER: return "GameProcessAfter";
        case CLEO_CB_SCRIPT_PROCESS_BEFORE: return "ScriptProcessBefore";
        case CLEO_CB_SCRIPT_PROCESS_AFTER: return "ScriptProcessAfter";
        case CLEO_CB_SCRIPT_OPCODE_PROCESS_BEFORE: return "ScriptOpcodeProcessBefore";
        case CLEO_CB_SCRIPT_OPCODE_PROCESS_AFTER: return "ScriptOpcodeProcessAfter";
        case CLEO_CB_SCRIPT_DELETED: return "ScriptDeleted";
        default: return "Unknown";
        }
    }
}

DebugUtils::DebugUtils()
{
    if (s_instance != nullptr)
        return;

    if (CLEO_GetVersion() < CLEO_VERSION)
        return;

    s_instance = this;

    LoadConfig();
    OpenLogs();
    LoadCrashInfoList();
    WriteCoreHeader();
    WriteCoreThreadLayout();

    RegisterCallbacks();

    SetUnhandledExceptionFilter(&DebugUtils::UnhandledExceptionFilter);

    if (m_scriptLogEnabled)
    {
        m_scriptWriterStop.store(false, std::memory_order_release);
        m_scriptWriterThread = std::thread(&DebugUtils::ScriptWriterLoop, this);
    }

    CLEO_DebugSetLogCallback(&DebugUtils::OnCoreLog);

    CLEO_RegisterOpcode(0x00C3, &DebugUtils::Opcode_DebugOn);
    CLEO_RegisterOpcode(0x00C4, &DebugUtils::Opcode_DebugOff);
    CLEO_RegisterOpcode(0x2100, &DebugUtils::Opcode_Breakpoint);
    CLEO_RegisterOpcode(0x2101, &DebugUtils::Opcode_Trace);
    CLEO_RegisterOpcode(0x2102, &DebugUtils::Opcode_LogToFile);

    if (m_legacyDebugOpcodes)
    {
        CLEO_RegisterOpcode(0x0662, &DebugUtils::Opcode_PrintString);
        CLEO_RegisterOpcode(0x0663, &DebugUtils::Opcode_PrintInt);
        CLEO_RegisterOpcode(0x0664, &DebugUtils::Opcode_PrintFloat);
    }

    WriteCore(
        "[DEBUGUTILS] initialized version=0x%08X game=%d callbacks=active script_log=%d "
        "opcode_trace=%d deduplicate=%d command_limit=%u time_limit=%u legacy_debug=%d",
        CLEO_GetVersion(),
        CLEO_GetGameVersion(),
        m_scriptLogEnabled ? 1 : 0,
        m_scriptOpcodeTrace ? 1 : 0,
        m_scriptDeduplicate ? 1 : 0,
        static_cast<unsigned>(m_commandLimit),
        static_cast<unsigned>(m_timeLimitSeconds),
        m_legacyDebugOpcodes ? 1 : 0
    );
}

DebugUtils::~DebugUtils()
{
    if (s_instance != this)
        return;

    UnregisterCallbacks();
    CLEO_DebugSetLogCallback(nullptr);

    m_scriptWriterStop.store(true, std::memory_order_release);
    m_scriptWake.notify_one();
    if (m_scriptWriterThread.joinable())
        m_scriptWriterThread.join();

    WriteCore("[DEBUGUTILS] shutting down");
    CloseLogs();

    for (auto& entry : m_externalLogs)
    {
        if (entry.second.is_open())
            entry.second.close();
    }
    s_instance = nullptr;
}

std::string DebugUtils::ConfigPath() const
{
    CreateDirectoryA("cleo", nullptr);
    CreateDirectoryA("cleo\\cleo_plugins", nullptr);
    return "cleo\\cleo_plugins\\DebugUtils.ini";
}

void DebugUtils::LoadConfig()
{
    const std::string path = ConfigPath();

    m_commandLimit = static_cast<size_t>(
        GetPrivateProfileIntA(
            "DebugUtils.Limits", "Command",
            2000000,
            path.c_str()
        )
    );

    m_timeLimitSeconds = static_cast<DWORD>(
        GetPrivateProfileIntA(
            "DebugUtils.Limits", "Time",
            5,
            path.c_str()
        )
    );

    m_scriptLogEnabled =
        GetPrivateProfileIntA(
            "DebugUtils.ScriptLog", "Enabled",
            1,
            path.c_str()
        ) != 0;

    m_scriptOpcodeTrace =
        GetPrivateProfileIntA(
            "DebugUtils.ScriptLog", "OpcodeTrace",
            0,
            path.c_str()
        ) != 0;

    m_scriptDeduplicate =
        GetPrivateProfileIntA(
            "DebugUtils.ScriptLog", "Deduplicate",
            1,
            path.c_str()
        ) != 0;

    m_legacyDebugOpcodes =
        GetPrivateProfileIntA(
            "DebugUtils.General", "LegacyDebugOpcodes",
            0,
            path.c_str()
        ) != 0;
}

std::string DebugUtils::DebugDir() const
{
    CreateDirectoryA("cleo", nullptr);
    CreateDirectoryA("cleo\\debug", nullptr);
    return "cleo\\debug\\";
}

std::string DebugUtils::CoreLogPath() const
{
    return DebugDir() + "cleo_core.log";
}

std::string DebugUtils::ScriptLogPath() const
{
    return DebugDir() + "cleo_script.log";
}

std::string DebugUtils::CrashLogPath() const
{
    return DebugDir() + "gta_crashinfo.log";
}

std::string DebugUtils::CrashInfoPath() const
{
    return DebugDir() + "CrashInfo\\EN-CrashList.txt";
}

void DebugUtils::OpenLogs()
{
    const std::string crashInfoDir = DebugDir() + "CrashInfo\\";
    CreateDirectoryA(crashInfoDir.c_str(), nullptr);

    m_coreLog.open(CoreLogPath(), std::ios::out | std::ios::trunc);
    m_scriptLog.open(ScriptLogPath(), std::ios::out | std::ios::trunc);

    {
        std::ofstream crashLog(CrashLogPath(), std::ios::out | std::ios::app);
        if (crashLog.is_open())
        {
            SYSTEMTIME t{};
            GetLocalTime(&t);
            crashLog << "[DebugUtils] crash log ready "
                     << t.wYear << '-' << t.wMonth << '-' << t.wDay << ' '
                     << t.wHour << ':' << t.wMinute << ':' << t.wSecond
                     << std::endl;
        }
    }

    if (!m_coreLog.is_open())
        OutputDebugStringA("[DebugUtils] Failed to open cleo_core.log\n");
    if (!m_scriptLog.is_open())
        OutputDebugStringA("[DebugUtils] Failed to open cleo_script.log\n");
}

void DebugUtils::FlushCoreRepeatLocked()
{
    if (m_lastCoreRepeatCount <= 1 || m_lastCoreMessage.empty() || !m_coreLog.is_open())
    {
        m_lastCoreMessage.clear();
        m_lastCoreRepeatCount = 0;
        return;
    }

    SYSTEMTIME t{};
    GetLocalTime(&t);

    char ms[4];
    sprintf_s(ms, sizeof(ms), "%03u", t.wMilliseconds);

    m_coreLog
        << t.wYear << '-'
        << (t.wMonth < 10 ? "0" : "") << t.wMonth << '-'
        << (t.wDay < 10 ? "0" : "") << t.wDay << ' '
        << (t.wHour < 10 ? "0" : "") << t.wHour << ':'
        << (t.wMinute < 10 ? "0" : "") << t.wMinute << ':'
        << (t.wSecond < 10 ? "0" : "") << t.wSecond << '.'
        << ms << " [REPEAT] count=" << m_lastCoreRepeatCount
        << " message=" << m_lastCoreMessage << '\n';

    m_lastCoreMessage.clear();
    m_lastCoreRepeatCount = 0;
}

void DebugUtils::CloseLogs()
{
    {
        std::lock_guard<std::mutex> lock(m_coreMutex);
        FlushCoreRepeatLocked();
    }

    if (m_coreLog.is_open())
    {
        m_coreLog.flush();
        m_coreLog.close();
    }

    if (m_scriptLog.is_open())
    {
        m_scriptLog.flush();
        m_scriptLog.close();
    }
}

void DebugUtils::WriteCore(const char* format, ...)
{
    char message[4096];
    va_list args;
    va_start(args, format);
    SafeFormat(message, sizeof(message), format, args);
    va_end(args);

    std::lock_guard<std::mutex> lock(m_coreMutex);

    if (!m_coreLog.is_open())
        return;

    if (m_lastCoreMessage == message && m_lastCoreRepeatCount > 0)
    {
        ++m_lastCoreRepeatCount;
        return;
    }

    FlushCoreRepeatLocked();

    SYSTEMTIME t{};
    GetLocalTime(&t);

    m_coreLog
        << t.wYear << '-'
        << (t.wMonth < 10 ? "0" : "") << t.wMonth << '-'
        << (t.wDay < 10 ? "0" : "") << t.wDay << ' '
        << (t.wHour < 10 ? "0" : "") << t.wHour << ':'
        << (t.wMinute < 10 ? "0" : "") << t.wMinute << ':'
        << (t.wSecond < 10 ? "0" : "") << t.wSecond << '.';

    char ms[4];
    sprintf_s(ms, sizeof(ms), "%03u", t.wMilliseconds);
    m_coreLog << ms << " " << message << '\n';

    m_lastCoreMessage = message;
    m_lastCoreRepeatCount = 1;

    if (++m_corePendingWrites >= 32)
    {
        m_coreLog.flush();
        m_corePendingWrites = 0;
    }
}

void DebugUtils::RotateScriptLogIfNeeded(size_t incomingBytes)
{
    if (m_scriptBytes + incomingBytes <= kScriptLogMaxBytes)
        return;

    if (m_scriptLog.is_open())
    {
        m_scriptLog.flush();
        m_scriptLog.close();
    }

    const std::string oldPath = ScriptLogPath() + ".1";
    DeleteFileA(oldPath.c_str());
    MoveFileA(ScriptLogPath().c_str(), oldPath.c_str());

    m_scriptLog.open(ScriptLogPath(), std::ios::out | std::ios::trunc);
    m_scriptBytes = 0;

    if (m_scriptLog.is_open())
    {
        const char* marker = "[DebugUtils] script log rotated at 128 MiB\n";
        m_scriptLog.write(marker, static_cast<std::streamsize>(strlen(marker)));
        m_scriptBytes += strlen(marker);
    }
}

void DebugUtils::QueueScriptLine(const char* line)
{
    if (line == nullptr || line[0] == '\0')
        return;

    const uint32_t writeIndex = m_scriptWriteIndex.load(std::memory_order_relaxed);
    const uint32_t readIndex = m_scriptReadIndex.load(std::memory_order_acquire);

    if (writeIndex - readIndex >= kScriptQueueSize)
    {
        m_droppedScriptEvents.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    ScriptLogEvent& event = m_scriptQueue[writeIndex & kScriptQueueMask];
    strncpy_s(event.line, sizeof(event.line), line, _TRUNCATE);

    m_scriptWriteIndex.store(writeIndex + 1, std::memory_order_release);

    // Wake the writer periodically instead of taking a kernel transition for
    // every opcode callback.
    if ((writeIndex & 0xFFu) == 0)
        m_scriptWake.notify_one();
}

void DebugUtils::ScriptWriterLoop()
{
    m_scriptReadIndex.store(0, std::memory_order_release);

    for (;;)
    {
        uint32_t readIndex = m_scriptReadIndex.load(std::memory_order_relaxed);
        const uint32_t writeIndex = m_scriptWriteIndex.load(std::memory_order_acquire);

        if (readIndex == writeIndex)
        {
            if (m_scriptWriterStop.load(std::memory_order_acquire))
                break;

            std::unique_lock<std::mutex> lock(m_scriptWakeMutex);
            m_scriptWake.wait_for(
                lock,
                std::chrono::milliseconds(50),
                [this]
                {
                    return m_scriptWriterStop.load(std::memory_order_acquire) ||
                           m_scriptReadIndex.load(std::memory_order_acquire) !=
                           m_scriptWriteIndex.load(std::memory_order_acquire);
                }
            );
            continue;
        }

        while (readIndex != writeIndex)
        {
            const ScriptLogEvent& event = m_scriptQueue[readIndex & kScriptQueueMask];

            SYSTEMTIME t{};
            GetLocalTime(&t);

            char prefix[64];
            sprintf_s(
                prefix, sizeof(prefix),
                "%04u-%02u-%02u %02u:%02u:%02u.%03u ",
                t.wYear, t.wMonth, t.wDay,
                t.wHour, t.wMinute, t.wSecond, t.wMilliseconds
            );

            const size_t incoming = strlen(prefix) + strlen(event.line) + 1;

            if (m_scriptLog.is_open())
            {
                RotateScriptLogIfNeeded(incoming);

                if (m_scriptLog.is_open())
                {
                    m_scriptLog << prefix << event.line << '\n';
                    m_scriptBytes += incoming;

                    if ((m_scriptBytes & 0xFFFFu) < incoming)
                        m_scriptLog.flush();
                }
            }

            ++readIndex;
            m_scriptReadIndex.store(readIndex, std::memory_order_release);
        }

        const uint32_t dropped = m_droppedScriptEvents.exchange(0, std::memory_order_acq_rel);
        if (dropped != 0)
            WriteCore("[SCRIPT_QUEUE] dropped_events=%u", static_cast<unsigned>(dropped));
    }
}

void DebugUtils::FlushScriptRepeat()
{
    if (!m_scriptLogEnabled || m_lastScriptRepeatCount <= 1 || m_lastScriptMessage.empty())
    {
        m_lastScriptMessage.clear();
        m_lastScriptRepeatCount = 0;
        return;
    }

    char repeatLine[768] = {};
    sprintf_s(
        repeatLine,
        sizeof(repeatLine),
        "[REPEAT] count=%u message=%s",
        static_cast<unsigned>(m_lastScriptRepeatCount),
        m_lastScriptMessage.c_str()
    );
    QueueScriptLine(repeatLine);

    m_lastScriptMessage.clear();
    m_lastScriptRepeatCount = 0;
}

void DebugUtils::WriteScript(const char* format, ...)
{
    if (!m_scriptLogEnabled)
        return;

    char message[768];

    va_list args;
    va_start(args, format);
    SafeFormat(message, sizeof(message), format, args);
    va_end(args);

    if (m_scriptDeduplicate && m_lastScriptMessage == message && m_lastScriptRepeatCount > 0)
    {
        ++m_lastScriptRepeatCount;
        return;
    }

    if (m_scriptDeduplicate)
        FlushScriptRepeat();

    QueueScriptLine(message);

    if (m_scriptDeduplicate)
    {
        m_lastScriptMessage = message;
        m_lastScriptRepeatCount = 1;
    }
}

void DebugUtils::WriteExternal(const std::string& filename, bool timestamp, const char* message)
{
    if (filename.empty() || message == nullptr)
        return;

    auto& file = m_externalLogs.try_emplace(filename, filename, std::ios::app).first->second;
    if (!file.good())
        return;

    if (timestamp)
    {
        SYSTEMTIME t{};
        GetLocalTime(&t);
        file << (t.wDay < 10 ? "0" : "") << t.wDay << '/'
             << (t.wMonth < 10 ? "0" : "") << t.wMonth << '/'
             << t.wYear << ' '
             << (t.wHour < 10 ? "0" : "") << t.wHour << ':'
             << (t.wMinute < 10 ? "0" : "") << t.wMinute << ':'
             << (t.wSecond < 10 ? "0" : "") << t.wSecond << '.';
        char ms[4];
        sprintf_s(ms, sizeof(ms), "%03u", t.wMilliseconds);
        file << ms << ' ';
    }

    file << message << std::endl;
}

void __cdecl DebugUtils::OnCoreLog(int level, const char* format, va_list args)
{
    if (!s_instance || format == nullptr)
        return;

    char message[4096];
    SafeFormat(message, sizeof(message), format, args);

    const char* levelName = "Info";
    switch (level)
    {
    case CLEO_DEBUG_WARNING: levelName = "Warning"; break;
    case CLEO_DEBUG_ERROR: levelName = "Error"; break;
    case CLEO_DEBUG_DIAGNOSTIC: levelName = "Diagnostic"; break;
    default: break;
    }

    s_instance->WriteCore("[%s] %s", levelName, message);
}

void DebugUtils::WriteCoreHeader()
{
    HMODULE cleo = GetModuleHandleA("CLEO.asi");
    MODULEINFO info{};
    if (cleo && GetModuleInformation(GetCurrentProcess(), cleo, &info, sizeof(info)))
    {
        WriteCore(
            "[MODULE] CLEO.asi base=%p size=0x%08X",
            info.lpBaseOfDll,
            static_cast<unsigned>(info.SizeOfImage)
        );
    }

    WriteCore("[GAME] GTA SA version enum=%d", CLEO_GetGameVersion());
    WriteCore("[API] CLEO_GetVersion=%p", reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(&CLEO_GetVersion)));
    WriteCore("[API] CLEO_RegisterOpcode=%p", reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(&CLEO_RegisterOpcode)));
    WriteCore("[API] CLEO_CreateCustomScript=%p", reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(&CLEO_CreateCustomScript)));
    WriteCore("[API] CLEO_GetLastCreatedCustomScript=%p", reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(&CLEO_GetLastCreatedCustomScript)));
    WriteCore("[API] CLEO_RegisterCallback=%p", reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(&CLEO_RegisterCallback)));
    WriteCore("[API] CLEO_UnregisterCallback=%p", reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(&CLEO_UnregisterCallback)));
    WriteCore("[GTA] pActiveScripts address=0x%08X", kGtaSa10ActiveScripts);
}

void DebugUtils::WriteCoreThreadLayout()
{
    WriteCore(
        "[THREAD_LAYOUT] sizeof(CScriptThread)=%u next=0x00 prev=0x04 name=0x08 base=0x10 ip=0x14 "
        "stack=0x18 sp=0x38 tls=0x3C active=0xC4 cond=0xC5 external=0xC7 wake=0xCC logical=0xD0 "
        "not=0xD2 mission=0xDC size=0xE0",
        static_cast<unsigned>(sizeof(CScriptThread))
    );
}

size_t DebugUtils::ScriptOffset(const CScriptThread* thread)
{
    if (thread == nullptr || thread->baseIp == nullptr || thread->ip == nullptr)
        return 0;

    const uintptr_t base = reinterpret_cast<uintptr_t>(thread->baseIp);
    const uintptr_t ip = reinterpret_cast<uintptr_t>(thread->ip);
    if (ip < base)
        return 0;

    return static_cast<size_t>(ip - base);
}

void DebugUtils::WriteCoreQueueSnapshot(const char* reason)
{
    if (CLEO_GetGameVersion() != GV_US10)
    {
        WriteCore("[QUEUE] snapshot skipped: supported diagnostic layout is SA 1.0 US");
        return;
    }

    auto head = *reinterpret_cast<CScriptThread**>(kGtaSa10ActiveScripts);
    size_t count = 0;

    WriteCore("[QUEUE] snapshot reason=%s head=%p", reason ? reason : "unknown", head);

    for (auto thread = head; thread != nullptr && count < 2048; thread = thread->next)
    {
        WriteCore(
            "[THREAD] #%u ptr=%p name='%.8s' prev=%p next=%p base=%p ip=%p off=0x%zX active=%d cond=%d "
            "external=%d mission=%d wake=%u",
            static_cast<unsigned>(count),
            thread,
            thread->threadName,
            thread->prev,
            thread->next,
            thread->baseIp,
            thread->ip,
            ScriptOffset(thread),
            thread->isActive ? 1 : 0,
            thread->condResult ? 1 : 0,
            thread->external ? 1 : 0,
            thread->missionFlag ? 1 : 0,
            thread->wakeTime
        );
        ++count;
    }

    if (count == 2048)
        WriteCore("[QUEUE] snapshot truncated at 2048 threads");

    WriteCore("[QUEUE] count=%u", static_cast<unsigned>(count));
}

void DebugUtils::WriteCoreMemorySummary()
{
    if (CLEO_GetGameVersion() != GV_US10)
        return;

    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);

    if (GetProcessMemoryInfo(
        GetCurrentProcess(),
        reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
        sizeof(pmc)))
    {
        WriteCore(
            "[MEMORY] working_set=%I64u peak_working_set=%I64u private_usage=%I64u pagefile_usage=%I64u",
            static_cast<unsigned __int64>(pmc.WorkingSetSize),
            static_cast<unsigned __int64>(pmc.PeakWorkingSetSize),
            static_cast<unsigned __int64>(pmc.PrivateUsage),
            static_cast<unsigned __int64>(pmc.PagefileUsage)
        );
    }

    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory))
    {
        WriteCore(
            "[MEMORY_SYSTEM] load=%u%% physical=%llu/%llu virtual=%llu/%llu",
            memory.dwMemoryLoad,
            static_cast<unsigned long long>(memory.ullAvailPhys),
            static_cast<unsigned long long>(memory.ullTotalPhys),
            static_cast<unsigned long long>(memory.ullAvailVirtual),
            static_cast<unsigned long long>(memory.ullTotalVirtual)
        );
    }

    auto head = *reinterpret_cast<CScriptThread**>(kGtaSa10ActiveScripts);
    size_t count = 0;
    for (auto thread = head; thread != nullptr && count < 4096; thread = thread->next)
        ++count;

    WriteCore(
        "[RUNTIME] active_queue_count=%u callbacks=GameBegin/GameEnd/GameProcess/ScriptProcess/OpcodeProcess",
        static_cast<unsigned>(count)
    );
}

void DebugUtils::LoadCrashInfoList()
{
    m_crashInfo.clear();

    std::ifstream file(CrashInfoPath());
    if (!file.is_open())
    {
        WriteCore(
            "[CRASHINFO] database not found at %s; source=%s",
            CrashInfoPath().c_str(),
            "https://github.com/JuniorDjjr/CrashInfo/blob/main/Lists/GTA-SA-10US/EN-CrashList.txt"
        );
        return;
    }

    std::string line;
    CrashInfoEntry* current = nullptr;

    while (std::getline(file, line))
    {
        if (line.rfind("Error: 0x", 0) == 0 && line.size() >= 12)
        {
            const DWORD address = static_cast<DWORD>(strtoul(line.c_str() + 9, nullptr, 16));
            if (m_crashInfo.size() >= 4096)
                break;

            m_crashInfo.push_back({});
            current = &m_crashInfo.back();
            current->address = address;
            continue;
        }

        if (current == nullptr)
            continue;

        if (line.rfind("Problem:", 0) == 0 ||
            line.rfind("Issue:", 0) == 0 ||
            line.rfind("Solution:", 0) == 0 ||
            line.rfind("About:", 0) == 0 ||
            line.rfind("Type:", 0) == 0)
        {
            if (!current->description.empty())
                current->description += " | ";

            current->description += line;

            if (current->description.size() > 1000)
                current->description.resize(1000);
        }
    }

    WriteCore("[CRASHINFO] loaded entries=%u path=%s",
        static_cast<unsigned>(m_crashInfo.size()),
        CrashInfoPath().c_str());
}

const DebugUtils::CrashInfoEntry* DebugUtils::FindCrashInfo(DWORD address) const
{
    for (const auto& entry : m_crashInfo)
    {
        if (entry.address == address)
            return &entry;
    }

    return nullptr;
}

std::string DebugUtils::Basename(const std::string& path)
{
    const size_t p = path.find_last_of("\\/");
    return p == std::string::npos ? path : path.substr(p + 1);
}

std::string DebugUtils::ModuleNameForAddress(DWORD address)
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(address),
        &module))
    {
        return "<unknown>";
    }

    char path[MAX_PATH] = {};
    if (!GetModuleFileNameA(module, path, sizeof(path)))
        return "<unknown>";

    return Basename(path);
}

bool DebugUtils::SafeReadDword(const DWORD* address, DWORD& value)
{
    if (address == nullptr)
        return false;

    __try
    {
        value = *address;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void DebugUtils::WriteCrashReport(PEXCEPTION_POINTERS info)
{
    if (info == nullptr || info->ExceptionRecord == nullptr || info->ContextRecord == nullptr)
        return;

    char line[4096];

    SYSTEMTIME t{};
    GetLocalTime(&t);

    sprintf_s(
        line, sizeof(line),
        "============================================================"
    );
    WinAppendLine(CrashLogPath(), line);

    sprintf_s(
        line, sizeof(line),
        "[CRASH] %04u-%02u-%02u %02u:%02u:%02u.%03u code=0x%08X address=0x%08X module=%s",
        t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
        info->ExceptionRecord->ExceptionCode,
        reinterpret_cast<DWORD>(info->ExceptionRecord->ExceptionAddress),
        ModuleNameForAddress(reinterpret_cast<DWORD>(info->ExceptionRecord->ExceptionAddress)).c_str()
    );
    WinAppendLine(CrashLogPath(), line);

    const CrashInfoEntry* match =
        FindCrashInfo(reinterpret_cast<DWORD>(info->ExceptionRecord->ExceptionAddress));

    if (match != nullptr)
    {
        sprintf_s(
            line, sizeof(line),
            "[CRASHINFO_MATCH] address=0x%08X %s",
            match->address,
            match->description.c_str()
        );
        WinAppendLine(CrashLogPath(), line);
    }
    else
    {
        WinAppendLine(
            CrashLogPath(),
            "[CRASHINFO_MATCH] no exact address match in local CrashInfo database"
        );
    }

    CONTEXT* c = info->ContextRecord;

    sprintf_s(
        line, sizeof(line),
        "[REGS] EAX=%08X EBX=%08X ECX=%08X EDX=%08X ESI=%08X EDI=%08X EBP=%08X ESP=%08X EIP=%08X EFLAGS=%08X",
        c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp, c->Eip, c->EFlags
    );
    WinAppendLine(CrashLogPath(), line);

    sprintf_s(
        line, sizeof(line),
        "[LAST_SCRIPT] ptr=%p name='%.8s' opcode=0x%04X offset=0x%08X result=%d",
        reinterpret_cast<void*>(static_cast<uintptr_t>(m_lastScriptPtr)),
        m_lastScriptName,
        m_lastOpcode == 0xFFFFFFFF ? 0xFFFF : (m_lastOpcode & 0x7FFF),
        m_lastOpcodeOffset,
        m_lastOpcodeResult == 0xFFFFFFFF ? -1 : static_cast<int>(m_lastOpcodeResult)
    );
    WinAppendLine(CrashLogPath(), line);

    sprintf_s(
        line, sizeof(line),
        "[EXCEPTION] flags=0x%08X parameters=%u",
        info->ExceptionRecord->ExceptionFlags,
        info->ExceptionRecord->NumberParameters
    );
    WinAppendLine(CrashLogPath(), line);

    if (info->ExceptionRecord->NumberParameters >= 2)
    {
        sprintf_s(
            line, sizeof(line),
            "[EXCEPTION] access_type=%llu address=0x%08llX",
            static_cast<unsigned long long>(info->ExceptionRecord->ExceptionInformation[0]),
            static_cast<unsigned long long>(info->ExceptionRecord->ExceptionInformation[1])
        );
        WinAppendLine(CrashLogPath(), line);
    }

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(
        reinterpret_cast<LPCVOID>(info->ExceptionRecord->ExceptionAddress),
        &mbi, sizeof(mbi)) != 0)
    {
        sprintf_s(
            line, sizeof(line),
            "[MEMORY_REGION] base=%p allocation_base=%p size=0x%08X state=0x%08X protect=0x%08X type=0x%08X",
            mbi.BaseAddress,
            mbi.AllocationBase,
            static_cast<unsigned>(mbi.RegionSize),
            mbi.State,
            mbi.Protect,
            mbi.Type
        );
        WinAppendLine(CrashLogPath(), line);
    }

    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(
        GetCurrentProcess(),
        reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
        sizeof(pmc)))
    {
        sprintf_s(
            line, sizeof(line),
            "[PROCESS_MEMORY] working_set=%I64u peak=%I64u private=%I64u pagefile=%I64u",
            static_cast<unsigned __int64>(pmc.WorkingSetSize),
            static_cast<unsigned __int64>(pmc.PeakWorkingSetSize),
            static_cast<unsigned __int64>(pmc.PrivateUsage),
            static_cast<unsigned __int64>(pmc.PagefileUsage)
        );
        WinAppendLine(CrashLogPath(), line);
    }

    DWORD frame = c->Ebp;
    for (unsigned i = 0; i < 32 && frame != 0; ++i)
    {
        DWORD next = 0;
        DWORD ret = 0;

        if (!SafeReadDword(reinterpret_cast<const DWORD*>(frame), next) ||
            !SafeReadDword(reinterpret_cast<const DWORD*>(frame + 4), ret))
            break;

        if (next <= frame || next - frame > 0x10000)
            break;

        sprintf_s(
            line, sizeof(line),
            "[BACKTRACE] #%u frame=0x%08X return=0x%08X module=%s",
            i,
            frame,
            ret,
            ModuleNameForAddress(ret).c_str()
        );
        WinAppendLine(CrashLogPath(), line);

        frame = next;
    }

    auto head = (CLEO_GetGameVersion() == GV_US10)
        ? *reinterpret_cast<CScriptThread**>(kGtaSa10ActiveScripts)
        : nullptr;

    if (head != nullptr)
    {
        WinAppendLine(CrashLogPath(), "[QUEUE_AT_CRASH] active script queue:");

        unsigned index = 0;
        for (auto thread = head; thread != nullptr && index < 256; thread = thread->next)
        {
            sprintf_s(
                line, sizeof(line),
                "[QUEUE_SCRIPT] #%u ptr=%p name='%.8s' ip=%p base=%p off=0x%zX active=%d external=%d",
                index,
                thread,
                thread->threadName,
                thread->ip,
                thread->baseIp,
                ScriptOffset(thread),
                thread->isActive ? 1 : 0,
                thread->external ? 1 : 0
            );
            WinAppendLine(CrashLogPath(), line);
            ++index;
        }
    }

    sprintf_s(line, sizeof(line), "[END_CRASH] exception=0x%08X",
        info->ExceptionRecord->ExceptionCode);
    WinAppendLine(CrashLogPath(), line);
}

LONG DebugUtils::HandleException(PEXCEPTION_POINTERS info)
{
    if (!info || !info->ExceptionRecord || !IsFatalException(info->ExceptionRecord->ExceptionCode))
        return EXCEPTION_CONTINUE_SEARCH;

    if (InterlockedCompareExchange(&m_crashInProgress, 1, 0) != 0)
        return EXCEPTION_CONTINUE_SEARCH;

    WriteCrashReport(info);
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI DebugUtils::VectoredExceptionHandler(PEXCEPTION_POINTERS info)
{
    if (s_instance == nullptr)
        return EXCEPTION_CONTINUE_SEARCH;

    if (!info || !info->ExceptionRecord)
        return EXCEPTION_CONTINUE_SEARCH;

    if (!IsFatalException(info->ExceptionRecord->ExceptionCode))
        return EXCEPTION_CONTINUE_SEARCH;

    // Do not steal the exception from the game or other handlers.
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI DebugUtils::UnhandledExceptionFilter(PEXCEPTION_POINTERS info)
{
    if (s_instance == nullptr)
        return EXCEPTION_CONTINUE_SEARCH;

    return s_instance->HandleException(info);
}

void DebugUtils::RegisterCallbacks()
{
    struct Registration
    {
        CLEO_CallbackId id;
        uintptr_t fn;
    } callbacks[] =
    {
        { CLEO_CB_GAME_BEGIN, reinterpret_cast<uintptr_t>(&DebugUtils::OnGameBegin) },
        { CLEO_CB_GAME_END, reinterpret_cast<uintptr_t>(&DebugUtils::OnGameEnd) },
        { CLEO_CB_GAME_PROCESS_BEFORE, reinterpret_cast<uintptr_t>(&DebugUtils::OnGameProcessBefore) },
        { CLEO_CB_GAME_PROCESS_AFTER, reinterpret_cast<uintptr_t>(&DebugUtils::OnGameProcessAfter) },
        { CLEO_CB_SCRIPT_PROCESS_BEFORE, reinterpret_cast<uintptr_t>(&DebugUtils::OnScriptProcessBefore) },
        { CLEO_CB_SCRIPT_PROCESS_AFTER, reinterpret_cast<uintptr_t>(&DebugUtils::OnScriptProcessAfter) },
        { CLEO_CB_SCRIPT_OPCODE_PROCESS_BEFORE, reinterpret_cast<uintptr_t>(&DebugUtils::OnScriptOpcodeBefore) },
        { CLEO_CB_SCRIPT_OPCODE_PROCESS_AFTER, reinterpret_cast<uintptr_t>(&DebugUtils::OnScriptOpcodeAfter) },
        { CLEO_CB_SCRIPT_DELETED, reinterpret_cast<uintptr_t>(&DebugUtils::OnScriptDeleted) }
    };

    for (const auto& callback : callbacks)
    {
        if (!CLEO_RegisterCallback(callback.id, callback.fn))
        {
            if (s_instance != nullptr)
                s_instance->WriteCore("[CALLBACK] register failed: %s", CallbackName(callback.id));
        }
        else
        {
            if (s_instance != nullptr)
                s_instance->WriteCore("[CALLBACK] registered: %s", CallbackName(callback.id));
        }
    }
}

void DebugUtils::UnregisterCallbacks()
{
    struct Registration
    {
        CLEO_CallbackId id;
        uintptr_t fn;
    } callbacks[] =
    {
        { CLEO_CB_GAME_BEGIN, reinterpret_cast<uintptr_t>(&DebugUtils::OnGameBegin) },
        { CLEO_CB_GAME_END, reinterpret_cast<uintptr_t>(&DebugUtils::OnGameEnd) },
        { CLEO_CB_GAME_PROCESS_BEFORE, reinterpret_cast<uintptr_t>(&DebugUtils::OnGameProcessBefore) },
        { CLEO_CB_GAME_PROCESS_AFTER, reinterpret_cast<uintptr_t>(&DebugUtils::OnGameProcessAfter) },
        { CLEO_CB_SCRIPT_PROCESS_BEFORE, reinterpret_cast<uintptr_t>(&DebugUtils::OnScriptProcessBefore) },
        { CLEO_CB_SCRIPT_PROCESS_AFTER, reinterpret_cast<uintptr_t>(&DebugUtils::OnScriptProcessAfter) },
        { CLEO_CB_SCRIPT_OPCODE_PROCESS_BEFORE, reinterpret_cast<uintptr_t>(&DebugUtils::OnScriptOpcodeBefore) },
        { CLEO_CB_SCRIPT_OPCODE_PROCESS_AFTER, reinterpret_cast<uintptr_t>(&DebugUtils::OnScriptOpcodeAfter) },
        { CLEO_CB_SCRIPT_DELETED, reinterpret_cast<uintptr_t>(&DebugUtils::OnScriptDeleted) }
    };

    for (const auto& callback : callbacks)
        CLEO_UnregisterCallback(callback.id, callback.fn);
}

void __stdcall DebugUtils::OnGameBegin()
{
    if (!s_instance) return;
    s_instance->m_scriptCommands = 0;
    s_instance->m_currentScriptPtr = 0;
    s_instance->m_currentScriptCommands = 0;
    s_instance->m_debugScripts.clear();
    s_instance->m_breakpoints.clear();
    s_instance->m_keysReleased = true;
    s_instance->m_lastMemoryLogTick = GetTickCount();
    s_instance->WriteCore("//////////////////////// GAME BEGIN ////////////////////////");
    s_instance->WriteCoreQueueSnapshot("GameBegin");
    s_instance->m_seenScripts.clear();
    s_instance->m_lastScriptMessage.clear();
    s_instance->m_lastScriptRepeatCount = 0;
    s_instance->WriteScript("//////////////////////// SCRIPT EXECUTION ////////////////////////");
    s_instance->WriteCoreMemorySummary();
}

void __stdcall DebugUtils::OnGameEnd()
{
    if (!s_instance) return;
    s_instance->WriteCore("//////////////////////// GAME END ////////////////////////");
    s_instance->WriteCoreMemorySummary();
    s_instance->FlushScriptRepeat();
    s_instance->WriteScript("[GAME_END] runtime stopped");
}

void __stdcall DebugUtils::OnGameProcessBefore()
{
    if (!s_instance || s_instance->m_breakpoints.empty())
        return;

    bool anyKeyDown = false;
    for (int key = VK_F5; key <= VK_F12; ++key)
    {
        if (GetAsyncKeyState(key) & 0x8000)
        {
            anyKeyDown = true;
            break;
        }
    }

    if (anyKeyDown)
    {
        if (!s_instance->m_keysReleased)
            return;

        s_instance->m_keysReleased = false;

        const size_t count = std::min<size_t>(s_instance->m_breakpoints.size(), 8);
        for (size_t i = 0; i < count; ++i)
        {
            const int key = VK_F5 + static_cast<int>(i);
            if (!(GetAsyncKeyState(key) & 0x8000))
                continue;

            s_instance->WriteCore(
                "[BREAKPOINT] continued script='%.8s' key=F%d",
                s_instance->m_breakpoints[i].name.c_str(),
                5 + static_cast<int>(i)
            );

            if (CTimer::m_CodePause)
                CTimer::m_CodePause = false;

            s_instance->m_breakpoints.erase(
                s_instance->m_breakpoints.begin() + static_cast<ptrdiff_t>(i)
            );
            break;
        }
    }
    else
    {
        s_instance->m_keysReleased = true;
    }
}

void __stdcall DebugUtils::OnGameProcessAfter()
{
    if (!s_instance) return;

    const DWORD now = GetTickCount();
    if (now - s_instance->m_lastMemoryLogTick >= 10000)
    {
        s_instance->m_lastMemoryLogTick = now;
        s_instance->WriteCoreMemorySummary();
    }
}

BOOL __stdcall DebugUtils::OnScriptProcessBefore(CScriptThread* thread)
{
    if (!s_instance || !thread)
        return TRUE;

    s_instance->m_currentScriptPtr = reinterpret_cast<uintptr_t>(thread);
    s_instance->m_currentScriptStartTick = GetTickCount();
    s_instance->m_currentScriptCommands = 0;

    for (const auto& breakpoint : s_instance->m_breakpoints)
    {
        if (breakpoint.scriptPtr == reinterpret_cast<uintptr_t>(thread))
            return FALSE;
    }

    const uintptr_t scriptPtr = reinterpret_cast<uintptr_t>(thread);
    if (s_instance->m_seenScripts.insert(scriptPtr).second)
    {
        s_instance->WriteScript(
            "[SCRIPT_BEGIN] ptr=%p name='%.8s' ip=%p base=%p off=0x%zX active=%d external=%d mission=%d",
            thread,
            thread->threadName,
            thread->ip,
            thread->baseIp,
            ScriptOffset(thread),
            thread->isActive ? 1 : 0,
            thread->external ? 1 : 0,
            thread->missionFlag ? 1 : 0
        );
    }

    return TRUE;
}

void __stdcall DebugUtils::OnScriptProcessAfter(CScriptThread* thread)
{
    if (!s_instance || !thread)
        return;

    s_instance->m_currentScriptPtr = 0;
}

int __stdcall DebugUtils::OnScriptOpcodeBefore(CScriptThread* thread, DWORD opcode)
{
    if (!s_instance || !thread)
        return 0;

    ++s_instance->m_scriptCommands;
    ++s_instance->m_currentScriptCommands;

    const DWORD normalized = opcode & 0x7FFF;

    if (s_instance->m_commandLimit > 0 && s_instance->m_currentScriptCommands > s_instance->m_commandLimit)
    {
        s_instance->WriteCore(
            "[HANG_GUARD] script='%.8s' command_limit=%u",
            thread->threadName,
            static_cast<unsigned>(s_instance->m_commandLimit)
        );
        return CLEO_DEBUG_OPCODE_INTERRUPT;
    }

    if ((s_instance->m_currentScriptCommands % 1000u) == 0)
    {
        const DWORD elapsed = GetTickCount() - s_instance->m_currentScriptStartTick;
        if (s_instance->m_timeLimitSeconds > 0 && elapsed > s_instance->m_timeLimitSeconds * 1000u)
        {
            s_instance->WriteScript(
                "[HANG_GUARD] script='%.8s' elapsed_ms=%u time_limit_seconds=%u",
                thread->threadName,
                static_cast<unsigned>(elapsed),
                static_cast<unsigned>(s_instance->m_timeLimitSeconds)
            );
            s_instance->m_currentScriptStartTick = GetTickCount();
            return CLEO_DEBUG_OPCODE_INTERRUPT;
        }
    }

    s_instance->m_lastScriptPtr = reinterpret_cast<uintptr_t>(thread);
    strncpy_s(s_instance->m_lastScriptName, thread->threadName, _TRUNCATE);
    s_instance->m_lastOpcode = opcode;
    s_instance->m_lastOpcodeOffset =
        static_cast<DWORD>(ScriptOffset(thread) >= 2 ? ScriptOffset(thread) - 2 : 0);
    s_instance->m_lastOpcodeResult = 0xFFFFFFFF;

    const bool notFlag = thread->notFlag != 0;

    if (s_instance->m_scriptOpcodeTrace ||
        s_instance->m_debugScripts.find(reinterpret_cast<uintptr_t>(thread)) != s_instance->m_debugScripts.end())
    {
        s_instance->WriteScript(
            "[OPCODE_BEFORE] script='%.8s' ptr=%p opcode=0x%04X group=%u not=%d ip=%p off=0x%zX",
            thread->threadName,
            thread,
            normalized,
            static_cast<unsigned>(normalized / 100),
            notFlag ? 1 : 0,
            thread->ip,
            ScriptOffset(thread) >= 2 ? ScriptOffset(thread) - 2 : 0
        );
    }

    return 0;
}

int __stdcall DebugUtils::Opcode_DebugOn(CScriptThread* thread)
{
    if (!s_instance || !thread)
        return OR_CONTINUE;

    s_instance->m_debugScripts.insert(reinterpret_cast<uintptr_t>(thread));
    s_instance->WriteCore("[DEBUG] enabled script='%.8s' ptr=%p", thread->threadName, thread);
    return OR_CONTINUE;
}

int __stdcall DebugUtils::Opcode_DebugOff(CScriptThread* thread)
{
    if (!s_instance || !thread)
        return OR_CONTINUE;

    const uintptr_t scriptPtr = reinterpret_cast<uintptr_t>(thread);
    s_instance->m_debugScripts.erase(scriptPtr);
    s_instance->m_seenScripts.erase(scriptPtr);
    s_instance->WriteCore("[DEBUG] disabled script='%.8s' ptr=%p", thread->threadName, thread);
    return OR_CONTINUE;
}

int __stdcall DebugUtils::Opcode_Breakpoint(CScriptThread* thread)
{
    if (!s_instance || !thread)
        return OR_CONTINUE;

    const uintptr_t scriptPtr = reinterpret_cast<uintptr_t>(thread);
    if (s_instance->m_debugScripts.find(scriptPtr) == s_instance->m_debugScripts.end())
    {
        SkipUnusedVarArgs(thread);
        return OR_CONTINUE;
    }

    bool blocking = true;
    if (CLEO_GetOperandType(thread) == imm8)
        blocking = CLEO_GetIntOpcodeParam(thread) != 0;

    std::string message;
    char buffer[1024] = {};

    if (CLEO_GetOperandType(thread) == 0)
    {
        thread->ip++;
    }
    else
    {
        if (CLEO_FormatOpcodeString(thread, buffer, sizeof(buffer)) >= 0)
            message = buffer;
    }

    s_instance->m_breakpoints.emplace_back(
        BreakpointInfo{ scriptPtr, thread->threadName, message, blocking }
    );

    s_instance->WriteCore(
        "[BREAKPOINT] script='%.8s' ptr=%p blocking=%d message='%s' off=0x%zX",
        thread->threadName,
        thread,
        blocking ? 1 : 0,
        message.c_str(),
        ScriptOffset(thread)
    );

    if (blocking)
    {
        CTimer::m_CodePause = true;
        s_instance->WriteCore("[BREAKPOINT] game paused");
    }

    return OR_INTERRUPT;
}

int __stdcall DebugUtils::Opcode_Trace(CScriptThread* thread)
{
    if (!s_instance || !thread)
        return OR_CONTINUE;

    if (s_instance->m_debugScripts.find(reinterpret_cast<uintptr_t>(thread)) ==
        s_instance->m_debugScripts.end())
    {
        SkipUnusedVarArgs(thread);
        return OR_CONTINUE;
    }

    char message[1024] = {};
    if (CLEO_FormatOpcodeString(thread, message, sizeof(message)) < 0)
        return OR_CONTINUE;

    s_instance->WriteCore(
        "[TRACE] script='%.8s' ptr=%p message='%s'",
        thread->threadName,
        thread,
        message
    );

    s_instance->WriteScript(
        "[TRACE] script='%.8s' ptr=%p %s",
        thread->threadName,
        thread,
        message
    );

    return OR_CONTINUE;
}

int __stdcall DebugUtils::Opcode_LogToFile(CScriptThread* thread)
{
    if (!s_instance || !thread)
        return OR_CONTINUE;

    char filename[MAX_PATH] = {};
    CLEO_ReadStringOpcodeParam(thread, filename, sizeof(filename));

    const int timestamp = CLEO_GetIntOpcodeParam(thread) != 0;

    char message[1024] = {};
    if (CLEO_FormatOpcodeString(thread, message, sizeof(message)) < 0)
    {
        s_instance->WriteCore(
            "[LOG_TO_FILE] formatting failed script='%.8s' file='%s'",
            thread->threadName,
            filename
        );
        return OR_CONTINUE;
    }

    s_instance->WriteExternal(filename, timestamp != 0, message);

    s_instance->WriteCore(
        "[LOG_TO_FILE] script='%.8s' file='%s' timestamp=%d",
        thread->threadName,
        filename,
        timestamp
    );

    return OR_CONTINUE;
}

int __stdcall DebugUtils::Opcode_PrintString(CScriptThread* thread)
{
    if (!s_instance || !thread)
        return OR_CONTINUE;

    if (s_instance->m_debugScripts.find(reinterpret_cast<uintptr_t>(thread)) ==
        s_instance->m_debugScripts.end())
    {
        CLEO_SkipOpcodeParams(thread, 1);
        return OR_CONTINUE;
    }

    char label[1024] = {};
    CLEO_ReadStringOpcodeParam(thread, label, sizeof(label));

    s_instance->WriteCore(
        "[LEGACY_DEBUG] script='%.8s' printstring='%s'",
        thread->threadName,
        label
    );
    s_instance->WriteScript(
        "[LEGACY_DEBUG] script='%.8s' printstring='%s'",
        thread->threadName,
        label
    );

    return OR_CONTINUE;
}

int __stdcall DebugUtils::Opcode_PrintInt(CScriptThread* thread)
{
    if (!s_instance || !thread)
        return OR_CONTINUE;

    if (s_instance->m_debugScripts.find(reinterpret_cast<uintptr_t>(thread)) ==
        s_instance->m_debugScripts.end())
    {
        CLEO_SkipOpcodeParams(thread, 2);
        return OR_CONTINUE;
    }

    char label[1024] = {};
    CLEO_ReadStringOpcodeParam(thread, label, sizeof(label));
    const DWORD value = CLEO_GetIntOpcodeParam(thread);

    s_instance->WriteCore(
        "[LEGACY_DEBUG] script='%.8s' printint='%s: %lu'",
        thread->threadName,
        label,
        static_cast<unsigned long>(value)
    );
    s_instance->WriteScript(
        "[LEGACY_DEBUG] script='%.8s' printint='%s: %lu'",
        thread->threadName,
        label,
        static_cast<unsigned long>(value)
    );

    return OR_CONTINUE;
}

int __stdcall DebugUtils::Opcode_PrintFloat(CScriptThread* thread)
{
    if (!s_instance || !thread)
        return OR_CONTINUE;

    if (s_instance->m_debugScripts.find(reinterpret_cast<uintptr_t>(thread)) ==
        s_instance->m_debugScripts.end())
    {
        CLEO_SkipOpcodeParams(thread, 2);
        return OR_CONTINUE;
    }

    char label[1024] = {};
    CLEO_ReadStringOpcodeParam(thread, label, sizeof(label));
    const float value = CLEO_GetFloatOpcodeParam(thread);

    s_instance->WriteCore(
        "[LEGACY_DEBUG] script='%.8s' printfloat='%s: %.6f'",
        thread->threadName,
        label,
        static_cast<double>(value)
    );
    s_instance->WriteScript(
        "[LEGACY_DEBUG] script='%.8s' printfloat='%s: %.6f'",
        thread->threadName,
        label,
        static_cast<double>(value)
    );

    return OR_CONTINUE;
}

int __stdcall DebugUtils::OnScriptOpcodeAfter(CScriptThread* thread, DWORD opcode, int result)
{
    if (!s_instance || !thread)
        return 0;

    s_instance->m_lastOpcodeResult = static_cast<DWORD>(result);

    if (s_instance->m_scriptOpcodeTrace ||
        s_instance->m_debugScripts.find(reinterpret_cast<uintptr_t>(thread)) != s_instance->m_debugScripts.end())
    {
        s_instance->WriteScript(
            "[OPCODE_AFTER] script='%.8s' ptr=%p opcode=0x%04X result=%d cond=%d ip=%p off=0x%zX",
            thread->threadName,
            thread,
            opcode & 0x7FFF,
            result,
            thread->condResult ? 1 : 0,
            thread->ip,
            ScriptOffset(thread)
        );
    }

    return 0;
}

void __stdcall DebugUtils::OnScriptDeleted(CScriptThread* thread)
{
    if (!s_instance || !thread)
        return;

    s_instance->m_debugScripts.erase(reinterpret_cast<uintptr_t>(thread));

    s_instance->m_breakpoints.erase(
        std::remove_if(
            s_instance->m_breakpoints.begin(),
            s_instance->m_breakpoints.end(),
            [thread](const BreakpointInfo& bp)
            {
                return bp.scriptPtr == reinterpret_cast<uintptr_t>(thread);
            }
        ),
        s_instance->m_breakpoints.end()
    );

    s_instance->WriteCore(
        "[THREAD_DELETE] ptr=%p name='%.8s' base=%p ip=%p off=0x%zX",
        thread,
        thread->threadName,
        thread->baseIp,
        thread->ip,
        ScriptOffset(thread)
    );
}

static DebugUtils g_debugUtils;
