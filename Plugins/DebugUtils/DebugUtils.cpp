#include "DebugUtils.h"
#include "resource.h"
#include "DebugUtils.Internal.h"
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

// plugin-sdk declares this GTA SA 1.0 US static reference but does not
// provide a definition in the CLEO/DebugUtils link. Resolve it directly to
// the verified game global used by CTimer::m_CodePause.
bool& CTimer::m_CodePause = *(bool*)0xB7CB48;

DebugUtils* DebugUtils::s_instance = nullptr;



DebugUtils::DebugUtils()
{
    if (s_instance != nullptr)
        return;

    if (CLEO_GetVersion() < CLEO_VERSION)
        return;

    s_instance = this;

    LoadConfig();

    // Crash collection is optional. The database and all heavy forensic
    // components are lazy-loaded only after a real unhandled exception.
    if (m_crashEnabled)
    {
        m_vectoredHandler = AddVectoredExceptionHandler(
            1,
            &DebugUtils::VectoredExceptionHandler
        );
        m_crashHandlerInstalled = m_vectoredHandler != nullptr;
    }

    CLEO_DebugSetCrashSnapshotEnabled(
        m_crashEnabled ? TRUE : FALSE
    );

    OpenLogs();
    WriteCore("[debugutils] configuration=%s", ConfigPath().c_str());
    WriteCore(
        "[debugutils] build_id=%s language=%s",
        kDebugUtilsBuildId,
        m_languageRussian ? "ru" : "en"
    );
    WriteCore("[debugutils] log files initialized");
    WriteCoreHeader();
    WriteCoreThreadLayout();

    RegisterCallbacks();

    if (m_crashEnabled)
        SetUnhandledExceptionFilter(&DebugUtils::UnhandledExceptionFilter);

    WriteCore(
        "[crash] enabled=%d window=%d backtrace=%d opcode_history=%d max_frames=%u "
        "VEH=%d bridge_snapshot=%d",
        m_crashEnabled ? 1 : 0,
        m_crashWindowEnabled ? 1 : 0,
        m_crashBacktraceEnabled ? 1 : 0,
        m_crashOpcodeHistory ? 1 : 0,
        static_cast<unsigned>(m_crashMaxFrames),
        m_crashHandlerInstalled ? 1 : 0,
        m_crashEnabled ? 1 : 0
    );

    if (m_scriptLogEnabled)
    {
        m_scriptWriterStop.store(false, std::memory_order_release);
        m_scriptWriterStarted.store(false, std::memory_order_release);
        WriteScript("//////////////////////// scripts ////////////////////////");
        if (m_functionTrace)
            WriteScript("//////////////////////// function call check (0AB1 / 0AB2) ////////////////////////");
        if (m_scriptOpcodeTrace)
            WriteScript("//////////////////////// opcode check ////////////////////////");
    }

    if (m_memoryLogEnabled)
        WriteMemory("//////////////////////// memory ////////////////////////");

    if (m_frameTimeEnabled)
    {
        WriteFrameTime("//////////////////////// frame time ////////////////////////");
        WriteFrameTime(
            "[frametime] enabled interval=%us warn_ms=%u top_stutter=%u histogram=%u target=cleo_frametime.log",
            static_cast<unsigned>(m_frameTimeIntervalSeconds),
            static_cast<unsigned>(m_frameTimeWarnMs),
            static_cast<unsigned>(m_frameTimeTopStutter),
            m_frameTimeHistogramEnabled ? 1u : 0u
        );
    }

    if (m_diagnosticLogEnabled)
        WriteDiagnostic("//////////////////////// diagnostic ////////////////////////");

    // Per-script profiler. Enabling it also activates the core opcode observer
    // wrapper (via CLEO_SetProfilingEnabled), so opcode counts become live.
    if (m_profilerEnabled)
    {
        CLEO_SetProfilingEnabled(TRUE);
        if (m_profilerOverheadMode)
            CLEO_SetProfilerOverheadEnabled(TRUE);
        WriteCore(
            "[profiler] enabled interval=%us top=%u topopcodes=%u overhead=%u target=diagnostic_log",
            static_cast<unsigned>(m_profilerIntervalSeconds),
            static_cast<unsigned>(m_profilerTop),
            static_cast<unsigned>(m_profilerTopOpcodes),
            m_profilerOverheadMode ? 1u : 0u
        );
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

    CLEO_DebugSetCrashSnapshotEnabled(FALSE);

    UnregisterCallbacks();
    CLEO_DebugSetLogCallback(nullptr);

    m_scriptWriterStop.store(true, std::memory_order_release);
    m_scriptWake.notify_one();
    if (m_scriptWriterThread.joinable())
        m_scriptWriterThread.join();

    if (m_vectoredHandler != nullptr)
    {
        RemoveVectoredExceptionHandler(m_vectoredHandler);
        m_vectoredHandler = nullptr;
    }

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
    const std::string standardPath =
        "cleo\\cleo_plugins\\DebugUtils.ini";

    CreateDirectoryA("cleo", nullptr);
    CreateDirectoryA("cleo\\cleo_plugins", nullptr);

    HMODULE module = nullptr;
    if (GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&DebugUtils::s_instance),
            &module))
    {
        char modulePath[MAX_PATH] = {};
        const DWORD length =
            GetModuleFileNameA(module, modulePath, sizeof(modulePath));

        if (length != 0 && length < sizeof(modulePath))
        {
            std::string pluginPath(modulePath, length);
            const size_t slash = pluginPath.find_last_of("\\\\/");
            if (slash != std::string::npos)
            {
                const std::string pluginIni =
                    pluginPath.substr(0, slash + 1) + "DebugUtils.ini";

                if (GetFileAttributesA(pluginIni.c_str()) !=
                    INVALID_FILE_ATTRIBUTES)
                {
                    return pluginIni;
                }
            }
        }
    }

    return standardPath;
}

void DebugUtils::LoadConfig()
{
    const std::string path = ConfigPath();

    char language[16] = {};
    GetPrivateProfileStringA(
        "DebugUtils.General",
        "Language",
        "en",
        language,
        sizeof(language),
        path.c_str()
    );

    for (char* p = language; *p != '\0'; ++p)
        *p = static_cast<char>(tolower(static_cast<unsigned char>(*p)));

    m_languageRussian = _stricmp(language, "ru") == 0;

    if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        std::ofstream config(path, std::ios::out | std::ios::trunc);

        if (config.is_open())
        {
            config << "; DebugUtils configuration\r\n";
            config << "; Changes are loaded when GTA starts.\r\n";
            config << "; 1 = enabled, 0 = disabled.\r\n\r\n";

            config << "[DebugUtils.General]\r\n";
            config << "; UI and CrashInfo language: en or ru.\r\n";
            config << "Language=en\r\n";
            config << "LegacyDebugOpcodes=0\r\n\r\n";

            config << "[DebugUtils.Profiler]\r\n";
            config << "; Per-script execution time and opcode measurement.\r\n";
            config << "; Enabling also activates the core opcode observer.\r\n";
            config << "Enabled=0\r\n";
            config << "; Seconds between reports. Counters reset each interval.\r\n";
            config << "Interval=60\r\n";
            config << "; Top scripts by self time per report.\r\n";
            config << "Top=20\r\n";
            config << "; Time the profiler itself and add a net_ms column.\r\n";
            config << "Overhead=0\r\n";
            config << "; Top opcodes by execution count per report (0 = off).\r\n";
            config << "TopOpcodes=15\r\n\r\n";

            config << "[DebugUtils.FrameTime]\r\n";
            config << "; Frame-time / stutter monitor. Samples the frame period and the\r\n";
            config << "; logic-tick duration and writes cleo_frametime.log (own file, so the\r\n";
            config << "; other logs stay clean). Use it to find what to optimise for freezes.\r\n";
            config << "Enabled=0\r\n";
            config << "; Seconds between reports. Counters reset each interval.\r\n";
            config << "Interval=10\r\n";
            config << "; A frame period at or above this many ms counts as a stutter event.\r\n";
            config << "WarnMs=50\r\n";
            config << "; How many worst frames to list per report (1..32).\r\n";
            config << "TopStutter=15\r\n";
            config << "; Emit a frame-period histogram per report.\r\n";
            config << "Histogram=1\r\n";
            config << "; Rotate cleo_frametime.log when it grows past this many MB.\r\n";
            config << "MaxLogMB=16\r\n\r\n";

            config << "[DebugUtils.Limits]\r\n";
            config << "Command=0\r\n";
            config << "Time=0\r\n\r\n";

            config << "[DebugUtils.ScriptLog]\r\n";
            config << "Enabled=1\r\n";
            config << "OpcodeTrace=0\r\n";
            config << "FunctionTrace=0\r\n";
            config << "Deduplicate=1\r\n\r\n";

            config << "[DebugUtils.Logs]\r\n";
            config << "Memory=1\r\n";
            config << "MemoryTrace=0\r\n";
            config << "; Warn when private bytes cross this many MB (0 = off).\r\n";
            config << "MemoryWarnMB=1500\r\n";
            config << "; Per-module resident-memory dump to attribute non-CLEO growth.\r\n";
            config << "ModuleMemoryMap=0\r\n";
            config << "; Seconds between module memory dumps.\r\n";
            config << "ModuleMemoryMapInterval=30\r\n";
            config << "Diagnostic=0\r\n\r\n";

            config << "[DebugUtils.Crash]\r\n";
            config << "; Main crash capture/test switch.\r\n";
            config << "Enabled=1\r\n";
            config << "Window=1\r\n";
            config << "Backtrace=1\r\n";
            config << "OpcodeHistory=0\r\n";
            config << "MaxFrames=32\r\n";
        }
    }

    m_commandLimit = static_cast<size_t>(
        GetPrivateProfileIntA(
            "DebugUtils.Limits", "Command",
            0,
            path.c_str()
        )
    );

    m_timeLimitSeconds = static_cast<DWORD>(
        GetPrivateProfileIntA(
            "DebugUtils.Limits", "Time",
            0,
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

    m_memoryModuleMap =
        GetPrivateProfileIntA(
            "DebugUtils.Logs", "ModuleMemoryMap",
            0,
            path.c_str()
        ) != 0;

    m_memoryModuleMapIntervalSeconds = static_cast<DWORD>(
        GetPrivateProfileIntA(
            "DebugUtils.Logs", "ModuleMemoryMapInterval",
            30,
            path.c_str()
        )
    );
    if (m_memoryModuleMapIntervalSeconds < 5)
        m_memoryModuleMapIntervalSeconds = 5;

    // Warn when private bytes cross this many megabytes (0 disables the warning).
    m_memoryWarnMegabytes = static_cast<DWORD>(
        GetPrivateProfileIntA(
            "DebugUtils.Logs", "MemoryWarnMB",
            1500,
            path.c_str()
        )
    );

    m_diagnosticLogEnabled =
        GetPrivateProfileIntA(
            "DebugUtils.Logs", "Diagnostic",
            0,
            path.c_str()
        ) != 0;

    m_legacyDebugOpcodes =
        GetPrivateProfileIntA(
            "DebugUtils.General", "LegacyDebugOpcodes",
            0,
            path.c_str()
        ) != 0;

    m_profilerEnabled =
        GetPrivateProfileIntA(
            "DebugUtils.Profiler", "Enabled",
            0,
            path.c_str()
        ) != 0;

    m_profilerIntervalSeconds = static_cast<DWORD>(
        GetPrivateProfileIntA(
            "DebugUtils.Profiler", "Interval",
            60,
            path.c_str()
        )
    );

    m_profilerTop = static_cast<DWORD>(
        GetPrivateProfileIntA(
            "DebugUtils.Profiler", "Top",
            20,
            path.c_str()
        )
    );

    m_profilerOverheadMode =
        GetPrivateProfileIntA(
            "DebugUtils.Profiler", "Overhead",
            0,
            path.c_str()
        ) != 0;

    m_profilerTopOpcodes = static_cast<DWORD>(
        GetPrivateProfileIntA(
            "DebugUtils.Profiler", "TopOpcodes",
            15,
            path.c_str()
        )
    );

    if (m_profilerIntervalSeconds < 5)
        m_profilerIntervalSeconds = 5;
    if (m_profilerTop == 0)
        m_profilerTop = 1;
    if (m_profilerTop > 64)
        m_profilerTop = 64;
    if (m_profilerTopOpcodes > 64)
        m_profilerTopOpcodes = 64;

    m_frameTimeEnabled =
        GetPrivateProfileIntA(
            "DebugUtils.FrameTime", "Enabled",
            0,
            path.c_str()
        ) != 0;

    m_frameTimeIntervalSeconds = static_cast<DWORD>(
        GetPrivateProfileIntA(
            "DebugUtils.FrameTime", "Interval",
            10,
            path.c_str()
        )
    );

    m_frameTimeWarnMs = static_cast<DWORD>(
        GetPrivateProfileIntA(
            "DebugUtils.FrameTime", "WarnMs",
            50,
            path.c_str()
        )
    );

    m_frameTimeTopStutter = static_cast<DWORD>(
        GetPrivateProfileIntA(
            "DebugUtils.FrameTime", "TopStutter",
            15,
            path.c_str()
        )
    );

    m_frameTimeHistogramEnabled =
        GetPrivateProfileIntA(
            "DebugUtils.FrameTime", "Histogram",
            1,
            path.c_str()
        ) != 0;

    m_frameTimeMaxLogMegabytes = static_cast<DWORD>(
        GetPrivateProfileIntA(
            "DebugUtils.FrameTime", "MaxLogMB",
            16,
            path.c_str()
        )
    );

    if (m_frameTimeIntervalSeconds < 1)
        m_frameTimeIntervalSeconds = 1;
    if (m_frameTimeWarnMs < 1)
        m_frameTimeWarnMs = 1;
    if (m_frameTimeTopStutter == 0)
        m_frameTimeTopStutter = 1;
    if (m_frameTimeTopStutter > kFrameTimeMaxWorst)
        m_frameTimeTopStutter = static_cast<DWORD>(kFrameTimeMaxWorst);
    if (m_frameTimeMaxLogMegabytes < 1)
        m_frameTimeMaxLogMegabytes = 1;

    m_crashEnabled =
        GetPrivateProfileIntA("DebugUtils.Crash", "Enabled", 1, path.c_str()) != 0;

    m_crashWindowEnabled =
        GetPrivateProfileIntA("DebugUtils.Crash", "Window", 1, path.c_str()) != 0;

    m_crashBacktraceEnabled =
        GetPrivateProfileIntA("DebugUtils.Crash", "Backtrace", 1, path.c_str()) != 0;

    m_crashOpcodeHistory =
        GetPrivateProfileIntA("DebugUtils.Crash", "OpcodeHistory", 0, path.c_str()) != 0;

    m_crashMaxFrames = static_cast<DWORD>(
        GetPrivateProfileIntA("DebugUtils.Crash", "MaxFrames", 32, path.c_str())
    );

    if (m_crashMaxFrames == 0)
        m_crashMaxFrames = 1;
    if (m_crashMaxFrames > 32)
        m_crashMaxFrames = 32;
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

std::string DebugUtils::FrameTimeLogPath() const
{
    return DebugDir() + "cleo_frametime.log";
}

std::string DebugUtils::CrashLogPath() const
{
    return DebugDir() + "gta_crashinfo.log";
}

std::string DebugUtils::CrashInfoPath() const
{
    return "cleo\\cleo_plugins\\CrashInfo\\CLEO-CrashList.txt";
}

std::string DebugUtils::CrashInfoRuPath() const
{
    return "cleo\\cleo_plugins\\CrashInfo\\CLEO-CrashList-RU.txt";
}

std::string DebugUtils::CrashInfoAutoPath() const
{
    return "cleo\\cleo_plugins\\CrashInfo\\CLEO-CrashAuto.txt";
}

static DebugUtils g_debugUtils;
