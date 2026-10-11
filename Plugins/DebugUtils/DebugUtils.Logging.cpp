#include "DebugUtils.h"
#include "DebugUtils.Internal.h"
#include "resource.h"
#include <windows.h>
#include <psapi.h>
#include <shellapi.h>
#include <dbghelp.h>
#pragma comment(lib, "Shell32.lib")
#include <TlHelp32.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <fstream>
#include <sstream>
#include <regex>
#include <CTimer.h>

void DebugUtils::OpenLogs()
{
    m_coreLog.open(CoreLogPath(), std::ios::out | std::ios::trunc);
    m_coreBytes = 0;
    m_coreLimitNoticeWritten = false;

    if (m_scriptLogEnabled)
        m_scriptLog.open(ScriptLogPath(), std::ios::out | std::ios::trunc);
    if (m_memoryLogEnabled)
        m_memoryLog.open(MemoryLogPath(), std::ios::out | std::ios::trunc);
    if (m_diagnosticLogEnabled)
        m_diagnosticLog.open(DiagnosticLogPath(), std::ios::out | std::ios::trunc);

    if (!m_coreLog.is_open())
        OutputDebugStringA("[debugutils] failed to open cleo_core.log\n");
    if (m_scriptLogEnabled && !m_scriptLog.is_open())
        OutputDebugStringA("[debugutils] failed to open cleo_script.log\n");
    if (m_memoryLogEnabled && !m_memoryLog.is_open())
        OutputDebugStringA("[debugutils] failed to open cleo_memory.log\n");
    if (m_diagnosticLogEnabled && !m_diagnosticLog.is_open())
        OutputDebugStringA("[debugutils] failed to open cleo_diagnostic.log\n");
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

    char line[4096];
    sprintf_s(
        line, sizeof(line),
        "%04u-%02u-%02u %02u:%02u:%02u.%s [repeat] count=%u message=%s\n",
        t.wYear, t.wMonth, t.wDay,
        t.wHour, t.wMinute, t.wSecond,
        ms,
        static_cast<unsigned>(m_lastCoreRepeatCount),
        m_lastCoreMessage.c_str()
    );

    const size_t bytes = strlen(line);
    if (m_coreBytes + bytes <= kCoreLogMaxBytes)
    {
        m_coreLog.write(line, static_cast<std::streamsize>(bytes));
        m_coreBytes += bytes;
    }
    else
    {
        WriteCoreLimitNoticeLocked();
    }

    m_lastCoreMessage.clear();
    m_lastCoreRepeatCount = 0;
}

void DebugUtils::CloseLogs()
{
    {
        std::lock_guard<std::mutex> lock(m_coreMutex);
        FlushCoreRepeatLocked();
    }
    {
        std::lock_guard<std::mutex> lock(m_diagnosticMutex);
        FlushDiagnosticRepeatLocked();
    }
    {
        std::lock_guard<std::mutex> lock(m_memoryMutex);
        FlushMemoryRepeatLocked();
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

    if (m_diagnosticLog.is_open())
    {
        m_diagnosticLog.flush();
        m_diagnosticLog.close();
    }

    if (m_memoryLog.is_open())
    {
        m_memoryLog.flush();
        m_memoryLog.close();
    }
}

void DebugUtils::RotateCoreLogIfNeeded(size_t incomingBytes)
{
    if (!m_coreLog.is_open() || incomingBytes == 0)
        return;

    if (m_coreBytes + incomingBytes <= kCoreLogMaxBytes)
        return;

    m_coreLog.flush();
    m_coreLog.close();

    const std::string oldPath = CoreLogPath() + ".1";
    DeleteFileA(oldPath.c_str());
    MoveFileA(CoreLogPath().c_str(), oldPath.c_str());

    m_coreLog.open(CoreLogPath(), std::ios::out | std::ios::trunc);
    m_coreBytes = 0;
    m_corePendingWrites = 0;
    m_coreLimitNoticeWritten = false;

    if (m_coreLog.is_open())
    {
        const char* marker = "//////////////////////// rotated core log ////////////////////////\n";
        m_coreLog.write(marker, static_cast<std::streamsize>(strlen(marker)));
        m_coreBytes += strlen(marker);
    }
}

void DebugUtils::WriteCore(const char* format, ...)
{
    char message[4096] = {};
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

    char line[4096] = {};
    sprintf_s(
        line, sizeof(line),
        "%04u-%02u-%02u %02u:%02u:%02u.%03u %s\n",
        t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
        t.wMilliseconds, message
    );

    const size_t bytes = strlen(line);
    RotateCoreLogIfNeeded(bytes);

    if (!m_coreLog.is_open())
        return;

    m_coreLog.write(line, static_cast<std::streamsize>(bytes));
    m_coreBytes += bytes;
    m_lastCoreMessage = message;
    m_lastCoreRepeatCount = 1;

    m_coreLog.flush();
    m_corePendingWrites = 0;
}

void DebugUtils::WriteCoreLimitNoticeLocked()
{
    // Retained for source compatibility. Core logging now rotates at 1 MiB
    // instead of applying the old 8 KiB hard stop.
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
        const char* marker = "[debugutils] script log rotated at 128 MiB\n";
        m_scriptLog.write(marker, static_cast<std::streamsize>(strlen(marker)));
        m_scriptBytes += strlen(marker);
    }
}

void DebugUtils::QueueScriptLine(const char* line)
{
    if (line == nullptr || line[0] == '\0')
        return;

    if (!m_scriptWriterStarted.load(std::memory_order_acquire))
    {
        bool expected = false;
        if (m_scriptWriterStarted.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel))
        {
            m_scriptWriterStop.store(false, std::memory_order_release);
            m_scriptWriterThread = std::thread(&DebugUtils::ScriptWriterLoop, this);
        }
    }

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

                    // The writer thread is already off the opcode path.
                    // Flush when a queue batch is drained rather than for each line.
                    if (readIndex + 1 == writeIndex)
                        m_scriptLog.flush();
                }
            }

            ++readIndex;
            m_scriptReadIndex.store(readIndex, std::memory_order_release);
        }

        const uint32_t dropped = m_droppedScriptEvents.exchange(0, std::memory_order_acq_rel);
        if (dropped != 0)
            WriteCore("[script_queue] dropped_events=%u", static_cast<unsigned>(dropped));
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
        "[repeat] count=%u message=%s",
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

void DebugUtils::FlushDiagnosticRepeatLocked()
{
    if (m_lastDiagnosticRepeatCount <= 1 || m_lastDiagnosticMessage.empty() || !m_diagnosticLog.is_open())
    {
        m_lastDiagnosticMessage.clear();
        m_lastDiagnosticRepeatCount = 0;
        return;
    }

    SYSTEMTIME t{};
    GetLocalTime(&t);
    char ms[4];
    sprintf_s(ms, sizeof(ms), "%03u", t.wMilliseconds);

    m_diagnosticLog
        << t.wYear << '-'
        << (t.wMonth < 10 ? "0" : "") << t.wMonth << '-'
        << (t.wDay < 10 ? "0" : "") << t.wDay << ' '
        << (t.wHour < 10 ? "0" : "") << t.wHour << ':'
        << (t.wMinute < 10 ? "0" : "") << t.wMinute << ':'
        << (t.wSecond < 10 ? "0" : "") << t.wSecond << '.'
        << ms << " [repeat] count=" << m_lastDiagnosticRepeatCount
        << " message=" << m_lastDiagnosticMessage << '\n';

    m_lastDiagnosticMessage.clear();
    m_lastDiagnosticRepeatCount = 0;
}

void DebugUtils::WriteDiagnostic(const char* format, ...)
{
    if (!m_diagnosticLogEnabled)
        return;

    char message[4096];
    va_list args;
    va_start(args, format);
    SafeFormat(message, sizeof(message), format, args);
    va_end(args);

    std::lock_guard<std::mutex> lock(m_diagnosticMutex);

    if (!m_diagnosticLog.is_open())
        return;

    if (m_lastDiagnosticMessage == message && m_lastDiagnosticRepeatCount > 0)
    {
        ++m_lastDiagnosticRepeatCount;
        return;
    }

    FlushDiagnosticRepeatLocked();

    SYSTEMTIME t{};
    GetLocalTime(&t);
    char ms[4];
    sprintf_s(ms, sizeof(ms), "%03u", t.wMilliseconds);

    m_diagnosticLog
        << t.wYear << '-'
        << (t.wMonth < 10 ? "0" : "") << t.wMonth << '-'
        << (t.wDay < 10 ? "0" : "") << t.wDay << ' '
        << (t.wHour < 10 ? "0" : "") << t.wHour << ':'
        << (t.wMinute < 10 ? "0" : "") << t.wMinute << ':'
        << (t.wSecond < 10 ? "0" : "") << t.wSecond << '.'
        << ms << ' ' << message << '\n';

    m_lastDiagnosticMessage = message;
    m_lastDiagnosticRepeatCount = 1;
    m_diagnosticLog.flush();
}

void DebugUtils::FlushMemoryRepeatLocked()
{
    if (m_lastMemoryRepeatCount <= 1 || m_lastMemoryMessage.empty() || !m_memoryLog.is_open())
    {
        m_lastMemoryMessage.clear();
        m_lastMemoryRepeatCount = 0;
        return;
    }

    SYSTEMTIME t{};
    GetLocalTime(&t);
    char ms[4];
    sprintf_s(ms, sizeof(ms), "%03u", t.wMilliseconds);

    m_memoryLog
        << t.wYear << '-'
        << (t.wMonth < 10 ? "0" : "") << t.wMonth << '-'
        << (t.wDay < 10 ? "0" : "") << t.wDay << ' '
        << (t.wHour < 10 ? "0" : "") << t.wHour << ':'
        << (t.wMinute < 10 ? "0" : "") << t.wMinute << ':'
        << (t.wSecond < 10 ? "0" : "") << t.wSecond << '.'
        << ms << " [repeat] count=" << m_lastMemoryRepeatCount
        << " message=" << m_lastMemoryMessage << '\n';

    m_lastMemoryMessage.clear();
    m_lastMemoryRepeatCount = 0;
}

void DebugUtils::WriteMemory(const char* format, ...)
{
    if (!m_memoryLogEnabled)
        return;

    char message[4096];
    va_list args;
    va_start(args, format);
    SafeFormat(message, sizeof(message), format, args);
    va_end(args);

    std::lock_guard<std::mutex> lock(m_memoryMutex);

    if (!m_memoryLog.is_open())
        return;

    if (m_lastMemoryMessage == message && m_lastMemoryRepeatCount > 0)
    {
        ++m_lastMemoryRepeatCount;
        return;
    }

    FlushMemoryRepeatLocked();

    SYSTEMTIME t{};
    GetLocalTime(&t);
    char ms[4];
    sprintf_s(ms, sizeof(ms), "%03u", t.wMilliseconds);

    m_memoryLog
        << t.wYear << '-'
        << (t.wMonth < 10 ? "0" : "") << t.wMonth << '-'
        << (t.wDay < 10 ? "0" : "") << t.wDay << ' '
        << (t.wHour < 10 ? "0" : "") << t.wHour << ':'
        << (t.wMinute < 10 ? "0" : "") << t.wMinute << ':'
        << (t.wSecond < 10 ? "0" : "") << t.wSecond << '.'
        << ms << ' ' << message << '\n';
    m_memoryLog.flush();

    m_lastMemoryMessage = message;
    m_lastMemoryRepeatCount = 1;
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

    if (strncmp(message, "[SCRIPT] ", 9) == 0)
    {
        const char* payload = message + 9;
        if (strncmp(payload, "[function]", 10) == 0 && !s_instance->m_functionTrace)
            return;

        s_instance->WriteScript("%s", payload);
        return;
    }

    if (strncmp(message, "[MEMORY] ", 9) == 0)
    {
        if (!s_instance->m_memoryLogEnabled || !s_instance->m_memoryTrace)
            return;

        s_instance->WriteMemory("%s", message + 9);
        return;
    }

    // Script lifecycle belongs to the script log, not the 8 KiB core log.
    if (level == CLEO_DEBUG_INFO)
    {
        if (strstr(message, "Loading custom script ") != nullptr ||
            strstr(message, "Registering custom script") != nullptr ||
            strstr(message, "Unregistering custom script") != nullptr ||
            strstr(message, "Deleting inactive script") != nullptr ||
            strstr(message, "Starting new custom script") != nullptr ||
            strstr(message, "[0A92] ") != nullptr)
        {
            s_instance->WriteScript("[lifecycle] %s", message);
            return;
        }

        // High-volume initialization details are not core diagnostics.
        if (strncmp(message, "[PluginSystem] ", 15) == 0 ||
            strncmp(message, "[HookSystem] ", 13) == 0 ||
            strncmp(message, "Injecting ", 10) == 0 ||
            strncmp(message, "Replacing call: ", 16) == 0 ||
            strncmp(message, "Found sound device ", 19) == 0 ||
            strncmp(message, "On system found ", 16) == 0 ||
            strncmp(message, "Creating main window", 20) == 0 ||
            strncmp(message, "Floating-point audio supported!", 32) == 0 ||
            strncmp(message, "Audio hardware acceleration", 27) == 0)
        {
            return;
        }
    }

    const char* levelName = "info";
    switch (level)
    {
    case CLEO_DEBUG_WARNING: levelName = "warning"; break;
    case CLEO_DEBUG_ERROR: levelName = "error"; break;
    case CLEO_DEBUG_DIAGNOSTIC: levelName = "diagnostic"; break;
    default: break;
    }

    if (level == CLEO_DEBUG_DIAGNOSTIC)
    {
        if (!s_instance->m_diagnosticLogEnabled)
            return;

        s_instance->WriteDiagnostic("[%s] %s", levelName, message);
        return;
    }

    s_instance->WriteCore("[%s] %s", levelName, message);
}

void DebugUtils::WriteCoreHeader()
{
    WriteCore("//////////////////////// module ////////////////////////");

    HMODULE cleo = GetModuleHandleA("CLEO.asi");
    MODULEINFO info{};
    if (cleo && GetModuleInformation(GetCurrentProcess(), cleo, &info, sizeof(info)))
    {
        WriteCore(
            "[module] CLEO.asi base=%p size=0x%08X",
            info.lpBaseOfDll,
            static_cast<unsigned>(info.SizeOfImage)
        );
    }

    WriteCore("//////////////////////// game / api ////////////////////////");
    WriteCore("[game] GTA SA version enum=%d", CLEO_GetGameVersion());
    WriteCore("[api] CLEO_GetVersion=%p", reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(&CLEO_GetVersion)));
    WriteCore("[api] CLEO_RegisterOpcode=%p", reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(&CLEO_RegisterOpcode)));
    WriteCore("[api] CLEO_CreateCustomScript=%p", reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(&CLEO_CreateCustomScript)));
    WriteCore("[api] CLEO_GetLastCreatedCustomScript=%p", reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(&CLEO_GetLastCreatedCustomScript)));
    WriteCore("[api] CLEO_RegisterCallback=%p", reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(&CLEO_RegisterCallback)));
    WriteCore("[api] CLEO_UnregisterCallback=%p", reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(&CLEO_UnregisterCallback)));
    WriteCore("[gta] pActiveScripts address=0x%08X", kGtaSa10ActiveScripts);
}

void DebugUtils::WriteCoreThreadLayout()
{
    WriteCore("//////////////////////// script thread ////////////////////////");
    WriteCore(
        "[thread_layout] sizeof(CScriptThread)=%u next=0x00 prev=0x04 name=0x08 base=0x10 ip=0x14 "
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
        WriteCore("[queue] snapshot skipped: supported diagnostic layout is SA 1.0 US");
        return;
    }

    auto head = *reinterpret_cast<CScriptThread**>(kGtaSa10ActiveScripts);
    size_t count = 0;

    WriteCore("[queue] snapshot reason=%s head=%p", reason ? reason : "unknown", head);

    for (auto thread = head; thread != nullptr && count < 2048; thread = thread->next)
    {
        WriteCore(
            "[thread] #%u ptr=%p name='%.8s' prev=%p next=%p base=%p ip=%p off=0x%zX active=%d cond=%d "
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
        WriteCore("[queue] snapshot truncated at 2048 threads");

    WriteCore("[queue] count=%u", static_cast<unsigned>(count));
}

void DebugUtils::WriteCoreMemorySummary()
{
    if (!m_memoryLogEnabled || CLEO_GetGameVersion() != GV_US10)
        return;

    auto head = *reinterpret_cast<CScriptThread**>(kGtaSa10ActiveScripts);
    size_t queueCount = 0;
    size_t nativeCount = 0;
    size_t customCount = 0;
    size_t customObjectBytes = 0;
    size_t customCodeBytes = 0;
    std::set<BYTE*> countedCodeBases;

    for (auto thread = head; thread != nullptr && queueCount < 4096; thread = thread->next)
    {
        ++queueCount;

        // Only genuine CCustomScript threads own the extended layout that the
        // CodeSize field belongs to. Native SCM threads also carry a non-null
        // baseIp, so classify by the custom-flag byte instead.
        if (IsCustomScriptThread(thread))
        {
            ++customCount;
            const auto* custom = GetDebugCustomScript(thread);
            customObjectBytes += sizeof(DebugCustomScriptLayout);

            if (!thread->missionFlag && thread->baseIp != nullptr &&
                countedCodeBases.insert(thread->baseIp).second)
                customCodeBytes += custom->CodeSize;
        }
        else
        {
            ++nativeCount;
        }
    }

    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);

    if (GetProcessMemoryInfo(
        GetCurrentProcess(),
        reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
        sizeof(pmc)))
    {
        const long long privateDelta = m_memoryBaselineReady
            ? static_cast<long long>(pmc.PrivateUsage) - static_cast<long long>(m_lastPrivateUsage)
            : 0;

        const size_t cleoImage = GetModuleImageSize(GetModuleHandleA("CLEO.asi"));

        size_t debugUtilsImage = 0;
        debugUtilsImage = GetModuleImageSize(GetModuleHandleA("DebugUtils.cleo"));
        if (debugUtilsImage == 0)
            debugUtilsImage = GetModuleImageSize(GetModuleHandleA("DebugUtils.dll"));

        // Script allocations made through allocate_memory (0AC8). CLEO tracks
        // and reclaims these per script, so surface them separately from the
        // process-wide private usage.
        CLEO_MemoryStats scriptStats{};
        const bool haveScriptStats = CLEO_DebugGetMemoryStats(&scriptStats) != FALSE;

        // Known CLEO footprint only: loaded images plus active custom-script
        // object storage, unique custom code buffers, and tracked script
        // allocations. GTA-owned allocations outside these categories are
        // intentionally not claimed here.
        const size_t cleoKnownBytes =
            cleoImage +
            debugUtilsImage +
            customObjectBytes +
            customCodeBytes +
            (haveScriptStats ? static_cast<size_t>(scriptStats.totalBytes) : 0);

        m_lastMemoryScriptBlocks = haveScriptStats ? scriptStats.totalBlocks : 0;
        m_lastMemoryScriptBytes = haveScriptStats ? scriptStats.totalBytes : 0;

        if (static_cast<uint64_t>(pmc.PrivateUsage) > m_peakPrivateUsage)
            m_peakPrivateUsage = static_cast<uint64_t>(pmc.PrivateUsage);

        std::string privateDeltaText;
        if (m_memoryBaselineReady)
        {
            privateDeltaText =
                (privateDelta < 0 ? "-" : "+") +
                MemoryBytes(
                    static_cast<uint64_t>(privateDelta < 0 ? -privateDelta : privateDelta));
        }
        else
        {
            privateDeltaText = "-";
        }

        if (!m_memorySummaryHeaderWritten)
        {
            WriteMemory(
                "========================= memory summary ========================="
            );
            WriteMemory(
                "%-8s %10s %10s | %6s %7s %7s | %9s %9s %13s",
                "uptime", "private", "delta", "queue", "native", "custom",
                "cleo_img", "known", "private_bytes"
            );
            m_memorySummaryHeaderWritten = true;
        }

        // "uptime" is seconds since the session started, not GetTickCount()
        // (which is OS uptime in seconds and is huge and meaningless here).
        const DWORD uptimeSeconds =
            (GetTickCount() - m_memorySessionStartTick) / 1000;

        WriteMemory(
            "%-8u %10s %10s | %6u %7u %7u | %9s %9s %13llu",
            static_cast<unsigned>(uptimeSeconds),
            MemoryBytes(static_cast<uint64_t>(pmc.PrivateUsage)).c_str(),
            privateDeltaText.c_str(),
            static_cast<unsigned>(queueCount),
            static_cast<unsigned>(nativeCount),
            static_cast<unsigned>(customCount),
            MemoryBytes(cleoImage + debugUtilsImage).c_str(),
            MemoryBytes(cleoKnownBytes).c_str(),
            static_cast<unsigned long long>(pmc.PrivateUsage)
        );

        // PagefileUsage equals PrivateUsage on Windows, so printing both would
        // be a duplicate; keep private once above and show the working set here.
        WriteMemory(
            "         private=%s working_set=%s peak_working_set=%s "
            "custom_objects=%s custom_code=%s",
            MemoryBytes(static_cast<uint64_t>(pmc.PrivateUsage)).c_str(),
            MemoryBytes(static_cast<uint64_t>(pmc.WorkingSetSize)).c_str(),
            MemoryBytes(static_cast<uint64_t>(pmc.PeakWorkingSetSize)).c_str(),
            MemoryBytes(customObjectBytes).c_str(),
            MemoryBytes(customCodeBytes).c_str()
        );

        // Warn once when private bytes first cross the configured threshold and
        // once again if they fall back below it, so a warning is never spammed.
        if (m_memoryWarnMegabytes != 0)
        {
            const uint64_t warnBytes =
                static_cast<uint64_t>(m_memoryWarnMegabytes) * 1024ull * 1024ull;

            if (static_cast<uint64_t>(pmc.PrivateUsage) >= warnBytes)
            {
                if (!m_memoryWarnIssued)
                {
                    m_memoryWarnIssued = true;
                    WriteMemory(
                        "[memory][warn] private usage %s reached the %u MB threshold",
                        MemoryBytes(static_cast<uint64_t>(pmc.PrivateUsage)).c_str(),
                        m_memoryWarnMegabytes
                    );
                }
            }
            else
            {
                m_memoryWarnIssued = false;
            }
        }

        // 32-bit GTA with the standard 2 GB user address space: near the limit
        // fragmentation is expected, so flag the transition into the last 20%.
        const uint64_t kAddressSpaceBytes = 2ull * 1024ull * 1024ull * 1024ull;
        const uint64_t privateUsage = static_cast<uint64_t>(pmc.PrivateUsage);
        const uint64_t headroomBytes =
            privateUsage < kAddressSpaceBytes ? kAddressSpaceBytes - privateUsage : 0;
        const bool rotateActive = privateUsage >= (kAddressSpaceBytes * 80ull / 100ull);

        // Absolute headroom is easier to reason about than a percentage when a
        // leak is suspected, so print it every interval.
        WriteMemory(
            "         limit=%s used=%.1f%% headroom=%s",
            MemoryBytes(kAddressSpaceBytes).c_str(),
            static_cast<double>(privateUsage) * 100.0 /
                static_cast<double>(kAddressSpaceBytes),
            MemoryBytes(headroomBytes).c_str()
        );

        // Tracked script allocations (allocate_memory / free_allocated_memory),
        // with the per-script budget that CLEO now enforces.
        if (haveScriptStats)
        {
            WriteMemory(
                "         script_blocks=%llu script_mem=%s per_script_limit=%s/%d blocks",
                m_lastMemoryScriptBlocks,
                MemoryBytes(static_cast<uint64_t>(m_lastMemoryScriptBytes)).c_str(),
                MemoryBytes(static_cast<uint64_t>(scriptStats.perScriptSizeLimit)).c_str(),
                scriptStats.perScriptBlockLimit
            );
        }

        if (rotateActive && !m_memoryRotateActive)
        {
            WriteMemory(
                "[memory][warn] private usage %s is above 80%% of the 2 GB address space",
                MemoryBytes(static_cast<uint64_t>(pmc.PrivateUsage)).c_str()
            );
        }
        m_memoryRotateActive = rotateActive;

        m_lastPrivateUsage = static_cast<uint64_t>(pmc.PrivateUsage);
        m_lastMemoryNativeCount = nativeCount;
        m_lastMemoryCustomObjectBytes = customObjectBytes;
        m_lastMemoryCustomCodeBytes = customCodeBytes;
        m_lastMemoryCleoImage = cleoImage;
        m_lastMemoryDebugImage = debugUtilsImage;
        m_lastMemoryKnownBytes = cleoKnownBytes;
    }

    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory))
    {
        WriteMemory(
            "           system_load=%u%% physical=%s/%s virtual=%s/%s",
            memory.dwMemoryLoad,
            MemoryBytes(memory.ullAvailPhys).c_str(),
            MemoryBytes(memory.ullTotalPhys).c_str(),
            MemoryBytes(memory.ullAvailVirtual).c_str(),
            MemoryBytes(memory.ullTotalVirtual).c_str()
        );
    }

    m_lastMemoryQueueCount = queueCount;
    m_lastMemoryCustomCount = customCount;
    m_memoryBaselineReady = true;
}
