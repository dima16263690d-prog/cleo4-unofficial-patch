#include "DebugUtils.h"
#include "resource.h"
#include <windows.h>
#include <psapi.h>
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
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#include <CTimer.h>

// plugin-sdk declares this GTA SA 1.0 US static reference but does not
// provide a definition in the CLEO/DebugUtils link. Resolve it directly to
// the verified game global used by CTimer::m_CodePause.
bool& CTimer::m_CodePause = *(bool*)0xB7CB48;

DebugUtils* DebugUtils::s_instance = nullptr;

namespace
{
    // DebugUtils uses the public SDK CScriptThread definition. Do not include
    // source/CCustomScript.h here because it declares a second incompatible
    // SCRIPT_VAR type. This local mirror is read-only and exists only so the
    // diagnostics can measure CCustomScript storage without changing the class.
    struct DebugCustomScriptLayout
    {
        CScriptThread base;
        DWORD dwChecksum;
        BYTE* ownedBuffer;
        bool bSaveEnabled;
        bool bOK;
        DWORD LastSearchPed;
        DWORD LastSearchCar;
        DWORD LastSearchObj;
        uint32_t CompatVer;
        size_t CodeSize;
        std::string ScriptFileDir;
        std::string ScriptFileName;
        DebugCustomScriptLayout* parentThread;
        int childLabel;
        DWORD savedNodeId;
        BYTE UseTextCommands;
        int NumDraws;
        int NumTexts;
        std::list<DebugCustomScriptLayout*> childThreads;
        std::list<void*> script_textures;
        std::vector<BYTE> script_draws;
        std::vector<BYTE> script_texts;
    };

    static_assert(sizeof(size_t) == 4, "CLEO4 DebugUtils is Win32; size_t must be 32-bit");
    static_assert(sizeof(CScriptThread) == 0xE0, "Unexpected CScriptThread layout");
    static_assert(offsetof(DebugCustomScriptLayout, CodeSize) == 0xFC, "Unexpected CCustomScript CodeSize offset");

    static const DebugCustomScriptLayout* GetDebugCustomScript(const CScriptThread* thread)
    {
        return reinterpret_cast<const DebugCustomScriptLayout*>(thread);
    }

    constexpr DWORD kGtaSa10ActiveScripts = 0x00A8B42C;

    static const char* ExceptionName(DWORD code)
    {
        switch (code)
        {
        case EXCEPTION_ACCESS_VIOLATION: return "ACCESS_VIOLATION";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR: return "IN_PAGE_ERROR";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_INT_OVERFLOW: return "INT_OVERFLOW";
        case EXCEPTION_PRIV_INSTRUCTION: return "PRIV_INSTRUCTION";
        case EXCEPTION_STACK_OVERFLOW: return "STACK_OVERFLOW";
        case 0xE06D7363: return "CXX_EXCEPTION";
        default: return "UNKNOWN";
        }
    }

    static size_t GetModuleImageSize(HMODULE module)
    {
        if (module == nullptr)
            return 0;

        MODULEINFO info{};
        if (!GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info)))
            return 0;

        return static_cast<size_t>(info.SizeOfImage);
    }

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
        case 0xE06D7363: // MSVC C++ exception
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

    void ExtractHexAddresses(const std::string& line, size_t start, std::vector<DWORD>& out)
    {
        size_t pos = start;
        while ((pos = line.find("0x", pos)) != std::string::npos)
        {
            const size_t valueStart = pos + 2;
            if (valueStart < line.size() && line[valueStart] == '*')
            {
                pos = valueStart + 1;
                continue;
            }

            char* end = nullptr;
            const unsigned long value = strtoul(line.c_str() + valueStart, &end, 16);
            if (end != line.c_str() + valueStart)
            {
                out.push_back(static_cast<DWORD>(value));
                pos = static_cast<size_t>(end - line.c_str());
            }
            else
            {
                pos = valueStart + 1;
            }
        }
    }

    void ExtractModuleNames(const std::string& line, std::vector<std::string>& out)
    {
        static const std::regex moduleRegex(
            R"(([A-Za-z0-9_~+.-]+\.(?:asi|cleo|dll)))",
            std::regex_constants::icase
        );

        for (std::sregex_iterator it(line.begin(), line.end(), moduleRegex), end; it != end; ++it)
            out.push_back((*it)[1].str());
    }

    bool ContainsAddress(const std::vector<DWORD>& values, DWORD address)
    {
        return std::find(values.begin(), values.end(), address) != values.end();
    }

    bool ContainsModule(const std::vector<std::string>& values, const std::string& module)
    {
        for (const auto& value : values)
        {
            if (value.size() == module.size())
            {
                bool same = true;
                for (size_t i = 0; i < value.size(); ++i)
                {
                    if (tolower(static_cast<unsigned char>(value[i])) !=
                        tolower(static_cast<unsigned char>(module[i])))
                    {
                        same = false;
                        break;
                    }
                }

                if (same)
                    return true;
            }
        }

        return false;
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
    EnsureCrashInfoDatabase();
    UpdateCrashInfoDatabaseIfNeeded();
    LoadCrashInfoList();
    WriteCoreHeader();
    WriteCoreThreadLayout();

    RegisterCallbacks();

    SetUnhandledExceptionFilter(&DebugUtils::UnhandledExceptionFilter);

    if (m_scriptLogEnabled)
    {
        m_scriptWriterStop.store(false, std::memory_order_release);
        m_scriptWriterThread = std::thread(&DebugUtils::ScriptWriterLoop, this);
        WriteScript("//////////////////////// scripts ////////////////////////");
        if (m_functionTrace)
            WriteScript("//////////////////////// function call check (0AB1 / 0AB2) ////////////////////////");
        if (m_scriptOpcodeTrace)
            WriteScript("//////////////////////// opcode check ////////////////////////");
    }

    if (m_memoryLogEnabled)
        WriteMemory("//////////////////////// memory ////////////////////////");

    if (m_diagnosticLogEnabled)
        WriteDiagnostic("//////////////////////// diagnostic ////////////////////////");

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
        "[debugutils] initialized version=0x%08X game=%d callbacks=active script_log=%d "
        "opcode_trace=%d function_trace=%d deduplicate=%d command_limit=%u time_limit=%u "
        "memory_log=%d memory_trace=%d diagnostic_log=%d legacy_debug=%d",
        CLEO_GetVersion(),
        CLEO_GetGameVersion(),
        m_scriptLogEnabled ? 1 : 0,
        m_scriptOpcodeTrace ? 1 : 0,
        m_functionTrace ? 1 : 0,
        m_scriptDeduplicate ? 1 : 0,
        static_cast<unsigned>(m_commandLimit),
        static_cast<unsigned>(m_timeLimitSeconds),
        m_memoryLogEnabled ? 1 : 0,
        m_memoryTrace ? 1 : 0,
        m_diagnosticLogEnabled ? 1 : 0,
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

    WriteCore("[debugutils] shutting down");
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

    if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        std::ofstream config(path, std::ios::out | std::ios::trunc);

        if (config.is_open())
        {
            config << "; DebugUtils configuration\r\n";
            config << "; Changes are loaded when GTA starts.\r\n";
            config << "; 1 = enabled, 0 = disabled.\r\n\r\n";

            config << "[DebugUtils.General]\r\n";
            config << "LegacyDebugOpcodes=0\r\n\r\n";

            config << "[DebugUtils.Limits]\r\n";
            config << "Command=2000000\r\n";
            config << "Time=5\r\n\r\n";

            config << "[DebugUtils.ScriptLog]\r\n";
            config << "Enabled=1\r\n";
            config << "OpcodeTrace=0\r\n";
            config << "FunctionTrace=0\r\n";
            config << "Deduplicate=1\r\n\r\n";

            config << "[DebugUtils.Logs]\r\n";
            config << "Memory=1\r\n";
            config << "MemoryTrace=0\r\n";
            config << "Diagnostic=0\r\n";
        }
    }

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

    m_functionTrace =
        GetPrivateProfileIntA(
            "DebugUtils.ScriptLog", "FunctionTrace",
            0,
            path.c_str()
        ) != 0;

    m_scriptDeduplicate =
        GetPrivateProfileIntA(
            "DebugUtils.ScriptLog", "Deduplicate",
            1,
            path.c_str()
        ) != 0;

    m_memoryLogEnabled =
        GetPrivateProfileIntA(
            "DebugUtils.Logs", "Memory",
            1,
            path.c_str()
        ) != 0;

    m_memoryTrace =
        GetPrivateProfileIntA(
            "DebugUtils.Logs", "MemoryTrace",
            0,
            path.c_str()
        ) != 0;

    m_diagnosticLogEnabled =
        GetPrivateProfileIntA(
            "DebugUtils.Logs", "diagnostic",
            0,
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

std::string DebugUtils::DiagnosticLogPath() const
{
    return DebugDir() + "cleo_diagnostic.log";
}

std::string DebugUtils::MemoryLogPath() const
{
    return DebugDir() + "cleo_memory.log";
}

std::string DebugUtils::CrashLogPath() const
{
    return DebugDir() + "gta_crashinfo.log";
}

std::string DebugUtils::CrashInfoPath() const
{
    return "cleo\\cleo_plugins\\CrashInfo\\CLEO-CrashList.txt";
}

void DebugUtils::EnsureCrashInfoDatabase()
{
    const std::string pluginCrashInfoDir = "cleo\\cleo_plugins\\CrashInfo\\";
    const std::string pluginCrashInfoPath = pluginCrashInfoDir + "CLEO-CrashList.txt";
    const std::string debugCrashInfoDir = DebugDir() + "CrashInfo\\";
    const std::string debugCrashInfoPath = debugCrashInfoDir + "CLEO-CrashList.txt";

    CreateDirectoryA("cleo", nullptr);
    CreateDirectoryA("cleo\\cleo_plugins", nullptr);
    CreateDirectoryA(pluginCrashInfoDir.c_str(), nullptr);
    CreateDirectoryA(debugCrashInfoDir.c_str(), nullptr);

    auto getFileSize = [](const std::string& path) -> DWORD
    {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data))
            return 0;

        if (data.nFileSizeHigh != 0)
            return MAXDWORD;

        return data.nFileSizeLow;
    };

    std::string source = "existing";
    DWORD size = getFileSize(pluginCrashInfoPath);

    // First use the bundled file beside DebugUtils.cleo. This keeps the
    // development/output layout working without requiring network access.
    if (size == 0)
    {
        HMODULE module = nullptr;
        if (GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(&DebugUtils::s_instance),
                &module))
        {
            char modulePath[MAX_PATH] = {};
            if (GetModuleFileNameA(module, modulePath, sizeof(modulePath)))
            {
                std::string path = modulePath;
                const size_t slash = path.find_last_of("\\\/");
                const std::string moduleDir =
                    slash == std::string::npos ? std::string() : path.substr(0, slash);
                const std::string bundledPath =
                    moduleDir + "\\CrashInfo\\CLEO-CrashList.txt";

                if (getFileSize(bundledPath) != 0 &&
                    CopyFileA(bundledPath.c_str(), pluginCrashInfoPath.c_str(), FALSE))
                {
                    size = getFileSize(pluginCrashInfoPath);
                    source = "module_bundle";
                }
            }
        }
    }

    // The database is embedded into DebugUtils.cleo, so installation of only
    // the .cleo file is sufficient to recreate the local CrashInfo database.
    if (size == 0)
    {
        HMODULE module = nullptr;
        if (GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(&DebugUtils::s_instance),
                &module))
        {
            HRSRC resource = FindResourceA(module, MAKEINTRESOURCEA(IDR_CRASHINFO), RT_RCDATA);
            if (resource != nullptr)
            {
                HGLOBAL loaded = LoadResource(module, resource);
                const DWORD resourceSize = SizeofResource(module, resource);
                const void* resourceData = loaded ? LockResource(loaded) : nullptr;

                if (resourceData != nullptr && resourceSize != 0)
                {
                    HANDLE file = CreateFileA(
                        pluginCrashInfoPath.c_str(),
                        GENERIC_WRITE,
                        FILE_SHARE_READ,
                        nullptr,
                        CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL,
                        nullptr
                    );

                    if (file != INVALID_HANDLE_VALUE)
                    {
                        DWORD written = 0;
                        const BOOL ok = WriteFile(
                            file,
                            resourceData,
                            resourceSize,
                            &written,
                            nullptr
                        );
                        CloseHandle(file);

                        if (ok && written == resourceSize)
                        {
                            size = getFileSize(pluginCrashInfoPath);
                            source = "embedded_resource";
                        }
                    }
                }
            }
        }
    }

    // Keep the documented debug path populated too, but never overwrite a
    // user's existing database.
    if (size != 0 && getFileSize(debugCrashInfoPath) == 0)
        CopyFileA(pluginCrashInfoPath.c_str(), debugCrashInfoPath.c_str(), FALSE);

    if (size != 0)
    {
        WriteCore(
            "[crashinfo] own database ready path=%s size=%u source=%s",
            pluginCrashInfoPath.c_str(),
            static_cast<unsigned>(size),
            source.c_str()
        );
    }
    else
    {
        WriteCore(
            "[crashinfo] database creation failed path=%s source=embedded_resource/module_bundle",
            pluginCrashInfoPath.c_str()
        );
    }
}

void DebugUtils::OpenLogs()
{
    const std::string crashInfoDir = DebugDir() + "CrashInfo\\";
    CreateDirectoryA(crashInfoDir.c_str(), nullptr);

    m_coreLog.open(CoreLogPath(), std::ios::out | std::ios::trunc);
    m_coreBytes = 0;
    m_coreLimitNoticeWritten = false;
    m_scriptLog.open(ScriptLogPath(), std::ios::out | std::ios::trunc);
    if (m_memoryLogEnabled)
        m_memoryLog.open(MemoryLogPath(), std::ios::out | std::ios::trunc);
    if (m_diagnosticLogEnabled)
        m_diagnosticLog.open(DiagnosticLogPath(), std::ios::out | std::ios::trunc);

    if (!m_coreLog.is_open())
        OutputDebugStringA("[debugutils] failed to open cleo_core.log\n");
    if (!m_scriptLog.is_open())
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

    char line[4096];
    sprintf_s(
        line, sizeof(line),
        "%04u-%02u-%02u %02u:%02u:%02u.%03u %s\n",
        t.wYear, t.wMonth, t.wDay,
        t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
        message
    );

    const size_t bytes = strlen(line);

    if (m_coreBytes + bytes <= kCoreLogMaxBytes)
    {
        m_coreLog.write(line, static_cast<std::streamsize>(bytes));
        m_coreBytes += bytes;
        m_lastCoreMessage = message;
        m_lastCoreRepeatCount = 1;

        if (++m_corePendingWrites >= 32)
        {
            m_coreLog.flush();
            m_corePendingWrites = 0;
        }
        return;
    }

    WriteCoreLimitNoticeLocked();
    m_lastCoreMessage.clear();
    m_lastCoreRepeatCount = 0;
}
void DebugUtils::WriteCoreLimitNoticeLocked()
{
    if (m_coreLimitNoticeWritten || !m_coreLog.is_open() || m_coreBytes >= kCoreLogMaxBytes)
    {
        m_coreLimitNoticeWritten = true;
        return;
    }

    const char* notice = " [core] log_limit=8192_bytes";
    SYSTEMTIME t{};
    GetLocalTime(&t);

    char line[128];
    sprintf_s(
        line, sizeof(line),
        "%04u-%02u-%02u %02u:%02u:%02u.%03u%s\n",
        t.wYear, t.wMonth, t.wDay,
        t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
        notice
    );

    const size_t bytes = strlen(line);
    if (m_coreBytes + bytes <= kCoreLogMaxBytes)
    {
        m_coreLog.write(line, static_cast<std::streamsize>(bytes));
        m_coreBytes += bytes;
    }

    m_coreLimitNoticeWritten = true;
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

        if (thread->baseIp != nullptr)
        {
            ++customCount;
            const auto* custom = GetDebugCustomScript(thread);
            customObjectBytes += sizeof(DebugCustomScriptLayout);

            if (!thread->missionFlag && countedCodeBases.insert(thread->baseIp).second)
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

        // Known CLEO footprint only: loaded images plus active custom-script
        // object storage and unique custom code buffers. GTA-owned allocations
        // outside these measured categories are intentionally not claimed here.
        const size_t cleoKnownBytes =
            cleoImage +
            debugUtilsImage +
            customObjectBytes +
            customCodeBytes;

        WriteMemory(
            "[memory] process_private=%I64u delta=%I64d working_set=%I64u peak_working_set=%I64u pagefile=%I64u | queue=%u native=%u custom=%u custom_delta=%I64d | cleo_image=%u debugutils_image=%u custom_objects=%u custom_code=%u cleo_known=%u",
            static_cast<unsigned __int64>(pmc.PrivateUsage),
            privateDelta,
            static_cast<unsigned __int64>(pmc.WorkingSetSize),
            static_cast<unsigned __int64>(pmc.PeakWorkingSetSize),
            static_cast<unsigned __int64>(pmc.PagefileUsage),
            static_cast<unsigned>(queueCount),
            static_cast<unsigned>(nativeCount),
            static_cast<unsigned>(customCount),
            m_memoryBaselineReady
                ? static_cast<long long>(customCount) - static_cast<long long>(m_lastMemoryCustomCount)
                : 0,
            static_cast<unsigned>(cleoImage),
            static_cast<unsigned>(debugUtilsImage),
            static_cast<unsigned>(customObjectBytes),
            static_cast<unsigned>(customCodeBytes),
            static_cast<unsigned>(cleoKnownBytes)
        );

        m_lastPrivateUsage = static_cast<uint64_t>(pmc.PrivateUsage);
    }

    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory))
    {
        WriteMemory(
            "[memory] system_load=%u%% physical=%llu/%llu virtual=%llu/%llu",
            memory.dwMemoryLoad,
            static_cast<unsigned long long>(memory.ullAvailPhys),
            static_cast<unsigned long long>(memory.ullTotalPhys),
            static_cast<unsigned long long>(memory.ullAvailVirtual),
            static_cast<unsigned long long>(memory.ullTotalVirtual)
        );
    }

    m_lastMemoryQueueCount = queueCount;
    m_lastMemoryCustomCount = customCount;
    m_memoryBaselineReady = true;
}

void DebugUtils::LoadCrashInfoList()
{
    m_crashInfo.clear();

    std::string loadedPath = CrashInfoPath();
    std::ifstream file(loadedPath);

    if (!file.is_open())
    {
        loadedPath = DebugDir() + "CrashInfo\\CLEO-CrashList.txt";
        file.open(loadedPath);
    }
    if (!file.is_open())
    {
        WriteCore(
            "[crashinfo] own database not found at %s",
            loadedPath.c_str()
        );
        return;
    }

    std::string line;
    CrashInfoEntry* current = nullptr;

    while (std::getline(file, line))
    {
        if (line.rfind("Error: ", 0) == 0)
        {
            if (m_crashInfo.size() >= 4096)
                break;

            m_crashInfo.push_back({});
            current = &m_crashInfo.back();

            const size_t matcherStart = 7;
            const size_t backtracePos = line.find("Backtrace", matcherStart);
            const size_t matcherEnd = backtracePos == std::string::npos
                ? line.size()
                : backtracePos;

            const std::string matcherText = line.substr(matcherStart, matcherEnd - matcherStart);

            ExtractHexAddresses(matcherText, 0, current->errorAddresses);
            ExtractModuleNames(matcherText, current->errorModules);

            if (matcherText.find("0x*") != std::string::npos)
                current->wildcardError = true;

            if (backtracePos != std::string::npos)
            {
                ExtractHexAddresses(line, backtracePos, current->backtraceAddresses);
                ExtractModuleNames(line.substr(backtracePos), current->backtraceModules);
            }

            current->hasMatcher =
                !current->errorAddresses.empty() ||
                !current->errorModules.empty() ||
                current->wildcardError ||
                !current->backtraceAddresses.empty() ||
                !current->backtraceModules.empty();

            continue;
        }

        if (current == nullptr)
            continue;

        if (line.rfind("Backtrace:", 0) == 0)
        {
            ExtractHexAddresses(line, 10, current->backtraceAddresses);
            ExtractModuleNames(line.substr(10), current->backtraceModules);
            current->hasMatcher =
                !current->errorAddresses.empty() ||
                !current->errorModules.empty() ||
                current->wildcardError ||
                !current->backtraceAddresses.empty() ||
                !current->backtraceModules.empty();
            continue;
        }

        if (!line.empty())
        {
            if (!current->description.empty())
                current->description += " | ";

            current->description += line;

            if (current->description.size() > 4000)
                current->description.resize(4000);
        }
    }

    WriteCore("[crashinfo] loaded entries=%u path=%s",
        static_cast<unsigned>(m_crashInfo.size()),
        loadedPath.c_str());
}

const DebugUtils::CrashInfoEntry* DebugUtils::FindCrashInfo(
    DWORD address,
    const std::string& faultModule,
    const std::vector<DWORD>& backtrace
) const
{
    const CrashInfoEntry* best = nullptr;
    int bestScore = -1;

    for (const auto& entry : m_crashInfo)
    {
        const bool exactError = ContainsAddress(entry.errorAddresses, address);
        const bool moduleError = ContainsModule(entry.errorModules, faultModule);

        if (entry.wildcardError && entry.backtraceAddresses.empty() && entry.backtraceModules.empty())
        {
            // A bare Error: 0x* entry is a true catch-all, just like the
            // generic wildcard entry in the source database.
        }
        else if (!exactError && !moduleError && !entry.wildcardError &&
                 entry.backtraceAddresses.empty() && entry.backtraceModules.empty())
        {
            continue;
        }

        if (entry.wildcardError == false &&
            !exactError &&
            !moduleError &&
            entry.errorAddresses.size() + entry.errorModules.size() > 0)
        {
            continue;
        }

        bool backtraceOk = true;
        int backtraceMatches = 0;

        for (DWORD expected : entry.backtraceAddresses)
        {
            if (!ContainsAddress(backtrace, expected))
            {
                backtraceOk = false;
                break;
            }
            ++backtraceMatches;
        }

        if (!backtraceOk)
            continue;

        std::vector<std::string> backtraceModules;
        backtraceModules.reserve(backtrace.size());
        for (DWORD bt : backtrace)
            backtraceModules.push_back(ModuleNameForAddress(bt));

        for (const auto& expectedModule : entry.backtraceModules)
        {
            if (!ContainsModule(backtraceModules, expectedModule))
            {
                backtraceOk = false;
                break;
            }
            ++backtraceMatches;
        }

        if (!backtraceOk)
            continue;

        int score = 0;
        if (exactError) score += 200;
        if (moduleError) score += 180;
        if (entry.wildcardError) score += 20;
        score += backtraceMatches * 50;

        if (score > bestScore)
        {
            bestScore = score;
            best = &entry;
        }
    }

    return best;
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

    WriteOpcodeHistory("crash");

    char line[4096];

    SYSTEMTIME t{};
    GetLocalTime(&t);

    sprintf_s(
        line, sizeof(line),
        "============================================================"
    );
    WinAppendLine(CrashLogPath(), line);

    const DWORD faultAddress = reinterpret_cast<DWORD>(info->ExceptionRecord->ExceptionAddress);
    const std::string faultModule = ModuleNameForAddress(faultAddress);

    sprintf_s(
        line, sizeof(line),
        "[crash] %04u-%02u-%02u %02u:%02u:%02u.%03u code=0x%08X type=%s address=0x%08X module=%s pid=%u tid=%u",
        t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
        info->ExceptionRecord->ExceptionCode,
        ExceptionName(info->ExceptionRecord->ExceptionCode),
        faultAddress,
        faultModule.c_str(),
        GetCurrentProcessId(),
        GetCurrentThreadId()
    );
    WinAppendLine(CrashLogPath(), line);

    CONTEXT* c = info->ContextRecord;

    sprintf_s(
        line, sizeof(line),
        "[regs] EAX=%08X EBX=%08X ECX=%08X EDX=%08X ESI=%08X EDI=%08X EBP=%08X ESP=%08X EIP=%08X EFLAGS=%08X",
        c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp, c->Eip, c->EFlags
    );
    WinAppendLine(CrashLogPath(), line);

    sprintf_s(
        line, sizeof(line),
        "[context] CS=%04X DS=%04X ES=%04X FS=%04X GS=%04X SS=%04X",
        c->SegCs, c->SegDs, c->SegEs, c->SegFs, c->SegGs, c->SegSs
    );
    WinAppendLine(CrashLogPath(), line);

    sprintf_s(
        line, sizeof(line),
        "[last_script] ptr=%p name='%.8s' opcode=0x%04X offset=0x%08X result=%d",
        reinterpret_cast<void*>(static_cast<uintptr_t>(m_lastScriptPtr)),
        m_lastScriptName,
        m_lastOpcode == 0xFFFFFFFF ? 0xFFFF : (m_lastOpcode & 0x7FFF),
        m_lastOpcodeOffset,
        m_lastOpcodeResult == 0xFFFFFFFF ? -1 : static_cast<int>(m_lastOpcodeResult)
    );
    WinAppendLine(CrashLogPath(), line);

    HMODULE faultModuleHandle = nullptr;
    MODULEINFO faultModuleInfo{};
    DWORD faultModuleBase = 0;
    DWORD faultModuleRva = 0;

    if (GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(faultAddress),
        &faultModuleHandle) &&
        GetModuleInformation(
            GetCurrentProcess(),
            faultModuleHandle,
            &faultModuleInfo,
            sizeof(faultModuleInfo)))
    {
        faultModuleBase = reinterpret_cast<DWORD>(faultModuleInfo.lpBaseOfDll);
        if (faultAddress >= faultModuleBase)
            faultModuleRva = faultAddress - faultModuleBase;

        sprintf_s(
            line, sizeof(line),
            "[module_at_fault] name=%s base=0x%08X rva=0x%08X image_size=0x%08X",
            faultModule.c_str(),
            faultModuleBase,
            faultModuleRva,
            static_cast<unsigned>(faultModuleInfo.SizeOfImage)
        );
        WinAppendLine(CrashLogPath(), line);
    }

    sprintf_s(
        line, sizeof(line),
        "[exception] flags=0x%08X parameters=%u",
        info->ExceptionRecord->ExceptionFlags,
        info->ExceptionRecord->NumberParameters
    );
    WinAppendLine(CrashLogPath(), line);

    if (info->ExceptionRecord->NumberParameters >= 2)
    {
        sprintf_s(
            line, sizeof(line),
            "[exception] access_type=%llu address=0x%08llX",
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
            "[memory_region] base=%p allocation_base=%p size=0x%08X state=0x%08X protect=0x%08X type=0x%08X",
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
            "[process_memory] working_set=%I64u peak=%I64u private=%I64u pagefile=%I64u",
            static_cast<unsigned __int64>(pmc.WorkingSetSize),
            static_cast<unsigned __int64>(pmc.PeakWorkingSetSize),
            static_cast<unsigned __int64>(pmc.PrivateUsage),
            static_cast<unsigned __int64>(pmc.PagefileUsage)
        );
        WinAppendLine(CrashLogPath(), line);
    }

    std::vector<DWORD> backtraceAddresses;
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

        backtraceAddresses.push_back(ret);

        sprintf_s(
            line, sizeof(line),
            "[backtrace] #%u frame=0x%08X return=0x%08X module=%s",
            i,
            frame,
            ret,
            ModuleNameForAddress(ret).c_str()
        );
        WinAppendLine(CrashLogPath(), line);

        frame = next;
    }

    const CrashInfoEntry* match = FindCrashInfo(
        faultAddress,
        faultModule,
        backtraceAddresses
    );
    if (match != nullptr)
    {
        const DWORD matchedAddress = match->errorAddresses.empty() ? 0 : match->errorAddresses.front();
        sprintf_s(
            line, sizeof(line),
            "[crashinfo_match] mode=%s address=0x%08X backtrace_rules=%u %s",
            match->wildcardError ? "wildcard" : "exact",
            matchedAddress,
            static_cast<unsigned>(match->backtraceAddresses.size()),
            match->description.c_str()
        );
        WinAppendLine(CrashLogPath(), line);
    }
    else
    {
        WinAppendLine(
            CrashLogPath(),
            "[crashinfo_match] no matching entry in local CrashInfo database (entries=%u)",
            static_cast<unsigned>(m_crashInfo.size())
        );
    }

    auto head = (CLEO_GetGameVersion() == GV_US10)
        ? *reinterpret_cast<CScriptThread**>(kGtaSa10ActiveScripts)
        : nullptr;

    if (head != nullptr)
    {
        WinAppendLine(CrashLogPath(), "[queue_at_crash] active script queue:");

        unsigned index = 0;
        for (auto thread = head; thread != nullptr && index < 256; thread = thread->next)
        {
            sprintf_s(
                line, sizeof(line),
                "[queue_script] #%u ptr=%p name='%.8s' ip=%p base=%p off=0x%zX active=%d external=%d",
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

    sprintf_s(line, sizeof(line), "[end_crash] exception=0x%08X",
        info->ExceptionRecord->ExceptionCode);
    WinAppendLine(CrashLogPath(), line);
}

void DebugUtils::RecordOpcode(CScriptThread* thread, DWORD opcode, DWORD result)
{
    if (thread == nullptr)
        return;

    OpcodeHistoryEntry entry{};
    entry.opcode = opcode & 0x7FFF;
    entry.result = result;
    entry.offset = static_cast<DWORD>(ScriptOffset(thread) >= 2 ? ScriptOffset(thread) - 2 : 0);
    entry.scriptPtr = reinterpret_cast<uintptr_t>(thread);
    strncpy_s(entry.scriptName, sizeof(entry.scriptName), thread->threadName, _TRUNCATE);

    std::lock_guard<std::mutex> lock(m_opcodeHistoryMutex);

    // The Before callback creates a pending entry. The After callback fills
    // that same entry with the opcode result. If an exception interrupts the
    // opcode, the pending entry remains as the last executed opcode.
    if (result != 0xFFFFFFFF &&
        m_opcodeHistoryCount > 0)
    {
        const size_t lastIndex =
            (m_opcodeHistoryNext + kOpcodeHistorySize - 1) % kOpcodeHistorySize;
        OpcodeHistoryEntry& last = m_opcodeHistory[lastIndex];

        if (last.result == 0xFFFFFFFF &&
            last.opcode == entry.opcode &&
            last.scriptPtr == entry.scriptPtr)
        {
            last.result = result;
            last.offset = entry.offset;
            strncpy_s(last.scriptName, sizeof(last.scriptName), entry.scriptName, _TRUNCATE);
            return;
        }
    }

    m_opcodeHistory[m_opcodeHistoryNext] = entry;
    m_opcodeHistoryNext = (m_opcodeHistoryNext + 1) % kOpcodeHistorySize;
    if (m_opcodeHistoryCount < kOpcodeHistorySize)
        ++m_opcodeHistoryCount;
}

void DebugUtils::WriteOpcodeHistory(const char* reason)
{
    std::array<OpcodeHistoryEntry, kOpcodeHistorySize> snapshot{};
    size_t count = 0;
    size_t next = 0;

    {
        std::lock_guard<std::mutex> lock(m_opcodeHistoryMutex);
        count = m_opcodeHistoryCount;
        next = m_opcodeHistoryNext;
        snapshot = m_opcodeHistory;
    }

    WriteCore("[opcode_history] reason='%s' count=%u", reason ? reason : "unknown",
        static_cast<unsigned>(count));

    if (count == 0)
        return;

    const size_t start = (count == kOpcodeHistorySize) ? next : 0;
    for (size_t i = 0; i < count; ++i)
    {
        const size_t index = (start + i) % kOpcodeHistorySize;
        const OpcodeHistoryEntry& e = snapshot[index];

        WriteCore(
            "[opcode_history] #%03u script='%.8s' ptr=%p opcode=0x%04X result=%u off=0x%08X",
            static_cast<unsigned>(i + 1),
            e.scriptName,
            reinterpret_cast<void*>(e.scriptPtr),
            e.opcode,
            e.result,
            e.offset
        );
    }
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
                s_instance->WriteCore("[callback] register failed: %s", CallbackName(callback.id));
        }
        else
        {
            if (s_instance != nullptr)
                s_instance->WriteCore("[callback] registered: %s", CallbackName(callback.id));
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
    s_instance->m_lastPrivateUsage = 0;
    s_instance->m_lastMemoryQueueCount = 0;
    s_instance->m_lastMemoryCustomCount = 0;
    s_instance->m_memoryBaselineReady = false;
    s_instance->WriteCore("//////////////////////// game begin ////////////////////////");
    s_instance->WriteCoreQueueSnapshot("GameBegin");
    s_instance->m_seenScripts.clear();
    s_instance->m_lastScriptMessage.clear();
    s_instance->m_lastScriptRepeatCount = 0;
    s_instance->WriteScript("//////////////////////// script execution ////////////////////////");
    if (s_instance->m_functionTrace)
        s_instance->WriteScript("//////////////////////// function call check (0AB1 / 0AB2) ////////////////////////");
    if (s_instance->m_scriptOpcodeTrace)
        s_instance->WriteScript("//////////////////////// opcode check ////////////////////////");
    s_instance->WriteCoreMemorySummary();
}

void __stdcall DebugUtils::OnGameEnd()
{
    if (!s_instance) return;
    s_instance->WriteCore("//////////////////////// game end ////////////////////////");
    s_instance->WriteCoreMemorySummary();
    s_instance->FlushScriptRepeat();
    s_instance->WriteScript("[game_end] runtime stopped");
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
                "[breakpoint] continued script='%.8s' key=F%d",
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
            "[script_begin] ptr=%p name='%.8s' ip=%p base=%p off=0x%zX active=%d external=%d mission=%d",
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
            "[hang_guard] script='%.8s' command_limit=%u",
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
                "[hang_guard] script='%.8s' elapsed_ms=%u time_limit_seconds=%u",
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
    s_instance->RecordOpcode(thread, opcode, 0xFFFFFFFF);

    const bool notFlag = thread->notFlag != 0;

    if (s_instance->m_scriptOpcodeTrace ||
        s_instance->m_debugScripts.find(reinterpret_cast<uintptr_t>(thread)) != s_instance->m_debugScripts.end())
    {
        s_instance->WriteScript(
            "[opcode_before] script='%.8s' ptr=%p opcode=0x%04X group=%u not=%d ip=%p off=0x%zX",
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
    s_instance->WriteCore("[debug] enabled script='%.8s' ptr=%p", thread->threadName, thread);
    return OR_CONTINUE;
}

int __stdcall DebugUtils::Opcode_DebugOff(CScriptThread* thread)
{
    if (!s_instance || !thread)
        return OR_CONTINUE;

    const uintptr_t scriptPtr = reinterpret_cast<uintptr_t>(thread);
    s_instance->m_debugScripts.erase(scriptPtr);
    s_instance->m_seenScripts.erase(scriptPtr);
    s_instance->WriteCore("[debug] disabled script='%.8s' ptr=%p", thread->threadName, thread);
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
        "[breakpoint] script='%.8s' ptr=%p blocking=%d message='%s' off=0x%zX",
        thread->threadName,
        thread,
        blocking ? 1 : 0,
        message.c_str(),
        ScriptOffset(thread)
    );

    if (blocking)
    {
        CTimer::m_CodePause = true;
        s_instance->WriteCore("[breakpoint] game paused");
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
        "[trace] script='%.8s' ptr=%p message='%s'",
        thread->threadName,
        thread,
        message
    );

    s_instance->WriteScript(
        "[trace] script='%.8s' ptr=%p %s",
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
            "[log_to_file] formatting failed script='%.8s' file='%s'",
            thread->threadName,
            filename
        );
        return OR_CONTINUE;
    }

    s_instance->WriteExternal(filename, timestamp != 0, message);

    s_instance->WriteCore(
        "[log_to_file] script='%.8s' file='%s' timestamp=%d",
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
        "[legacy_debug] script='%.8s' printstring='%s'",
        thread->threadName,
        label
    );
    s_instance->WriteScript(
        "[legacy_debug] script='%.8s' printstring='%s'",
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
        "[legacy_debug] script='%.8s' printint='%s: %lu'",
        thread->threadName,
        label,
        static_cast<unsigned long>(value)
    );
    s_instance->WriteScript(
        "[legacy_debug] script='%.8s' printint='%s: %lu'",
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
        "[legacy_debug] script='%.8s' printfloat='%s: %.6f'",
        thread->threadName,
        label,
        static_cast<double>(value)
    );
    s_instance->WriteScript(
        "[legacy_debug] script='%.8s' printfloat='%s: %.6f'",
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
    s_instance->RecordOpcode(thread, opcode, static_cast<DWORD>(result));

    if (s_instance->m_scriptOpcodeTrace ||
        s_instance->m_debugScripts.find(reinterpret_cast<uintptr_t>(thread)) != s_instance->m_debugScripts.end())
    {
        s_instance->WriteScript(
            "[opcode_after] script='%.8s' ptr=%p opcode=0x%04X result=%d cond=%d ip=%p off=0x%zX",
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
        "[thread_delete] ptr=%p name='%.8s' base=%p ip=%p off=0x%zX",
        thread,
        thread->threadName,
        thread->baseIp,
        thread->ip,
        ScriptOffset(thread)
    );
}

static DebugUtils g_debugUtils;
