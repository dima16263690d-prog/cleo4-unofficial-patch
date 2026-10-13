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

    // Streaming budget request is forwarded to the core, which applies it at SCM
    // init. Independent of the frame-time monitor, so it works even with the
    // monitor off.
    if (m_frameTimeStreamLimitMb != 0)
    {
        const int previous = CLEO_SetStreamingMemoryLimit(m_frameTimeStreamLimitMb);
        WriteCore(
            "[debugutils] streaming limit requested=%d MB previous=%d MB (applied by core at SCM init)",
            m_frameTimeStreamLimitMb,
            previous
        );
    }

    if (m_frameTimeEnabled)
    {
        WriteFrameTime("//////////////////////// frame time ////////////////////////");
        WriteFrameTime(
            "[frametime] enabled interval=%us warn_ms=%u top_stutter=%u histogram=%u streaming=%u stream_limit_mb=%d target=cleo_frametime.log",
            static_cast<unsigned>(m_frameTimeIntervalSeconds),
            static_cast<unsigned>(m_frameTimeWarnMs),
            static_cast<unsigned>(m_frameTimeTopStutter),
            m_frameTimeHistogramEnabled ? 1u : 0u,
            m_frameTimeStreamingEnabled ? 1u : 0u,
            m_frameTimeStreamLimitMb
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

namespace
{
    std::string TrimAscii(const std::string& s)
    {
        size_t a = 0;
        size_t b = s.size();
        while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n'))
            ++a;
        while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n'))
            --b;
        return s.substr(a, b - a);
    }

    // Reads every key=value in the ini (no matter the comment language) so a
    // rewrite preserves the values the user set. Empty when the file is absent.
    std::map<std::string, std::string> ReadRawConfig(const std::string& path)
    {
        std::map<std::string, std::string> out;

        std::ifstream file(path);
        if (!file.is_open())
            return out;

        std::string line;
        std::string section;
        while (std::getline(file, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            const std::string trimmed = TrimAscii(line);
            if (trimmed.empty() || trimmed[0] == ';' || trimmed[0] == '#')
                continue;

            if (trimmed.front() == '[' && trimmed.back() == ']')
            {
                section = trimmed.substr(1, trimmed.size() - 2);
                continue;
            }

            const size_t eq = trimmed.find('=');
            if (eq == std::string::npos)
                continue;

            out[section + "." + TrimAscii(trimmed.substr(0, eq))] =
                TrimAscii(trimmed.substr(eq + 1));
        }

        return out;
    }

    std::string ValueOr(const std::map<std::string, std::string>& raw,
        const char* section, const char* key, const char* fallback)
    {
        const auto it = raw.find(std::string(section) + "." + key);
        if (it != raw.end() && !it->second.empty())
            return it->second;
        return fallback;
    }

    // Comment language can be told apart by the marker line written at the top.
    bool IniUsesRussian(const std::string& path)
    {
        std::ifstream file(path);
        if (!file.is_open())
            return false;

        std::string line;
        while (std::getline(file, line))
        {
            if (line.find("; language=ru") != std::string::npos)
                return true;
            if (line.find("; language=en") != std::string::npos)
                return false;
        }
        return false;
    }
}

// Builds the ini text with the descriptions in the chosen language. Only the
// comments are localized; keys and values never change. Values already present in
// `raw` are kept, everything else falls back to the defaults.
std::string DebugUtils::BuildConfigText(bool ru, const std::map<std::string, std::string>& raw) const
{
    std::string out;
    auto line = [&out](const std::string& text) { out += text; out += '\n'; };
    auto kv = [&](const char* section, const char* key, const char* fallback)
    {
        line(std::string(key) + "=" + ValueOr(raw, section, key, fallback));
    };
    // Descriptions must be ';' comments, otherwise the engine reads them as keys.
    auto desc = [&line](const std::string& text) { line("; " + text); };

    line(ru ? "; language=ru" : "; language=en");
    desc(ru ? "Конфигурация DebugUtils. Значения читаются при запуске GTA."
            : "DebugUtils configuration. Values are read when GTA starts.");
    desc(ru ? "Измените Language и перезапустите игру — файл перепишется с описанием"
            : "Change Language and restart the game - the file is rewritten with the");
    desc(ru ? "на выбранном языке, а ваши значения сохранятся."
            : "descriptions in the chosen language, keeping your values.");
    desc(ru ? "1 = включено, 0 = выключено."
            : "1 = enabled, 0 = disabled.");
    line("");

    line("[DebugUtils.General]");
    desc(ru ? "Язык интерфейса и CrashInfo: en или ru."
            : "UI and CrashInfo language: en or ru.");
    kv("DebugUtils.General", "Language", "en");
    desc(ru ? "Включить устаревшие отладочные опкоды 0662/0663/0664."
            : "Enable the legacy debug opcodes 0662/0663/0664.");
    kv("DebugUtils.General", "LegacyDebugOpcodes", "0");
    line("");

    line("[DebugUtils.Profiler]");
    desc(ru ? "Замер времени выполнения скриптов и подсчёт опкодов."
            : "Per-script execution time and opcode measurement.");
    desc(ru ? "Включение также активирует наблюдатель опкодов в ядре."
            : "Enabling also activates the core opcode observer.");
    desc(ru ? "Enabled: 1 = включить профайлер, 0 = выключить."
            : "Enabled: 1 = enable the profiler, 0 = disable.");
    kv("DebugUtils.Profiler", "Enabled", "0");
    desc(ru ? "Interval: секунды между отчётами; счётчики сбрасываются каждый интервал."
            : "Interval: seconds between reports; counters reset each interval.");
    kv("DebugUtils.Profiler", "Interval", "60");
    desc(ru ? "Top: сколько самых тяжёлых скриптов выводить в отчёте."
            : "Top: how many heaviest scripts to list per report.");
    kv("DebugUtils.Profiler", "Top", "20");
    desc(ru ? "Overhead: 1 = замерять и сам профайлер, добавив колонку net_ms."
            : "Overhead: 1 = time the profiler itself and add a net_ms column.");
    kv("DebugUtils.Profiler", "Overhead", "0");
    desc(ru ? "TopOpcodes: сколько горячих опкодов показывать по частоте (0 = выкл)."
            : "TopOpcodes: hot opcodes by execution count per report (0 = off).");
    kv("DebugUtils.Profiler", "TopOpcodes", "15");
    line("");

    line("[DebugUtils.FrameTime]");
    desc(ru ? "Монитор времени кадра и подвисаний. Замеряет период кадра и время"
            : "Frame-time / stutter monitor. Samples the frame period and the logic");
    desc(ru ? "игровой логики, пишет cleo_frametime.log (отдельный файл). Помогает"
            : "tick and writes cleo_frametime.log (its own file). Use it to find what");
    desc(ru ? "найти, что оптимизировать для устранения фризов."
            : "to optimise for freezes.");
    desc(ru ? "Enabled: 1 = вести монитор, 0 = выключить (при 0 часы не читаются)."
            : "Enabled: 1 = run the monitor, 0 = off (when off no clock is read).");
    kv("DebugUtils.FrameTime", "Enabled", "0");
    desc(ru ? "Interval: секунды между отчётами; счётчики сбрасываются каждый интервал."
            : "Interval: seconds between reports; counters reset each interval.");
    kv("DebugUtils.FrameTime", "Interval", "10");
    desc(ru ? "WarnMs: период кадра не меньше этого числа мс считается подвисанием."
            : "WarnMs: a frame period at or above this many ms counts as a stutter.");
    kv("DebugUtils.FrameTime", "WarnMs", "50");
    desc(ru ? "TopStutter: сколько худших кадров выводить в отчёте (1..32)."
            : "TopStutter: how many worst frames to list per report (1..32).");
    kv("DebugUtils.FrameTime", "TopStutter", "15");
    desc(ru ? "Histogram: 1 = выводить гистограмму периодов кадров."
            : "Histogram: 1 = emit a frame-period histogram per report.");
    kv("DebugUtils.FrameTime", "Histogram", "1");
    desc(ru ? "Streaming: 1 = каждый кадр снимать память CStreaming"
            : "Streaming: 1 = snapshot CStreaming memory each frame");
    desc(ru ? "  (ms_memoryUsed/ms_memoryAvailable), чтобы связать фризы с нагрузкой"
            : "  (ms_memoryUsed/ms_memoryAvailable) to tie stutters to streaming load.");
    desc(ru ? "  стриминга. Только GTA SA 1.0, на других версиях игнорируется."
            : "  GTA SA 1.0 only; ignored on other versions.");
    kv("DebugUtils.FrameTime", "Streaming", "1");
    desc(ru ? "StreamLimitMB: запрашиваемый бюджет стриминга, МБ (только SA 1.0)."
            : "StreamLimitMB: requested streaming budget, in MB (SA 1.0 only).");
    desc(ru ? "   0 = не менять значение игры/мода (по умолчанию);"
            : "   0 = leave the value the game / another mod set (default);");
    desc(ru ? "  -1 = авто: 1024 МБ при Large Address Aware, иначе 512 МБ;"
            : "  -1 = auto: 1024 MB with Large Address Aware, else 512 MB;");
    desc(ru ? "  >0 = указанное число МБ. Значение только повышается, не понижается."
            : "  >0 = that many MB. Only ever raised, never lowered.");
    kv("DebugUtils.FrameTime", "StreamLimitMB", "0");
    desc(ru ? "MaxLogMB: ротация cleo_frametime.log после этого размера, МБ."
            : "MaxLogMB: rotate cleo_frametime.log when it grows past this many MB.");
    kv("DebugUtils.FrameTime", "MaxLogMB", "16");
    line("");

    line("[DebugUtils.Limits]");
    desc(ru ? "Command: предел команд на один запуск скрипта (0 = без предела)."
            : "Command: per-run command limit for a script (0 = no limit).");
    kv("DebugUtils.Limits", "Command", "0");
    desc(ru ? "Time: предел времени работы скрипта в секундах (0 = без предела)."
            : "Time: per-run time limit for a script in seconds (0 = no limit).");
    kv("DebugUtils.Limits", "Time", "0");
    line("");

    line("[DebugUtils.ScriptLog]");
    desc(ru ? "Enabled: 1 = писать лог скриптов, 0 = выключить."
            : "Enabled: 1 = write the script log, 0 = disable.");
    kv("DebugUtils.ScriptLog", "Enabled", "1");
    desc(ru ? "OpcodeTrace: 1 = трассировать каждый опкод (очень подробно)."
            : "OpcodeTrace: 1 = trace every opcode (very verbose).");
    kv("DebugUtils.ScriptLog", "OpcodeTrace", "0");
    desc(ru ? "FunctionTrace: 1 = проверять вызовы функций 0AB1/0AB2."
            : "FunctionTrace: 1 = check the function calls 0AB1/0AB2.");
    kv("DebugUtils.ScriptLog", "FunctionTrace", "0");
    desc(ru ? "Deduplicate: 1 = схлопывать повторяющиеся строки лога."
            : "Deduplicate: 1 = collapse repeated log lines.");
    kv("DebugUtils.ScriptLog", "Deduplicate", "1");
    line("");

    line("[DebugUtils.Logs]");
    desc(ru ? "Memory: 1 = писать лог памяти, 0 = выключить."
            : "Memory: 1 = write the memory log, 0 = disable.");
    kv("DebugUtils.Logs", "Memory", "1");
    desc(ru ? "MemoryTrace: 1 = подробная трассировка памяти."
            : "MemoryTrace: 1 = verbose memory tracing.");
    kv("DebugUtils.Logs", "MemoryTrace", "0");
    desc(ru ? "MemoryWarnMB: предупреждать при private bytes выше этого числа МБ (0 = выкл)."
            : "MemoryWarnMB: warn when private bytes cross this many MB (0 = off).");
    kv("DebugUtils.Logs", "MemoryWarnMB", "1500");
    desc(ru ? "ModuleMemoryMap: 1 = дамп резидентной памяти по модулям."
            : "ModuleMemoryMap: 1 = per-module resident-memory dump.");
    kv("DebugUtils.Logs", "ModuleMemoryMap", "0");
    desc(ru ? "ModuleMemoryMapInterval: секунды между дампами памяти по модулям."
            : "ModuleMemoryMapInterval: seconds between module memory dumps.");
    kv("DebugUtils.Logs", "ModuleMemoryMapInterval", "30");
    desc(ru ? "Diagnostic: 1 = писать диагностический лог, 0 = выключить."
            : "Diagnostic: 1 = write the diagnostic log, 0 = disable.");
    kv("DebugUtils.Logs", "Diagnostic", "0");
    line("");

    line("[DebugUtils.Crash]");
    desc(ru ? "Enabled: 1 = перехватывать падения и писать CrashInfo, 0 = выключить."
            : "Enabled: 1 = capture crashes and write CrashInfo, 0 = disable.");
    kv("DebugUtils.Crash", "Enabled", "1");
    desc(ru ? "Window: 1 = показывать окно CrashInfo при падении."
            : "Window: 1 = show the CrashInfo window on a crash.");
    kv("DebugUtils.Crash", "Window", "1");
    desc(ru ? "Backtrace: 1 = собирать стек вызовов при падении."
            : "Backtrace: 1 = collect a backtrace on a crash.");
    kv("DebugUtils.Crash", "Backtrace", "1");
    desc(ru ? "OpcodeHistory: 1 = хранить историю последних опкодов."
            : "OpcodeHistory: 1 = keep a history of recent opcodes.");
    kv("DebugUtils.Crash", "OpcodeHistory", "0");
    desc(ru ? "MaxFrames: максимальная глубина стека вызовов."
            : "MaxFrames: maximum backtrace frame depth.");
    kv("DebugUtils.Crash", "MaxFrames", "32");

    return out;
}

void DebugUtils::WriteConfigFile(const std::string& path, bool ru,
    const std::map<std::string, std::string>& raw) const
{
    std::string text = BuildConfigText(ru, raw);

    // The ini format expects CRLF; the builder uses LF for readability.
    std::string crlf;
    crlf.reserve(text.size() + text.size() / 16);
    for (char c : text)
    {
        if (c == '\n')
            crlf += "\r\n";
        else
            crlf += c;
    }

    std::ofstream config(path, std::ios::out | std::ios::trunc | std::ios::binary);
    if (config.is_open())
        config.write(crlf.data(), static_cast<std::streamsize>(crlf.size()));
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

    const std::map<std::string, std::string> raw = ReadRawConfig(path);

    const bool fileMissing = GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES;
    const bool languageMismatch = !fileMissing && (IniUsesRussian(path) != m_languageRussian);
    // A config from an older build lacks keys added since; rewrite it so the new
    // options are documented without dropping the values already set.
    const bool missingNewKeys =
        !fileMissing && raw.find("DebugUtils.Logs.ModuleMemoryMap") == raw.end();

    if (fileMissing || languageMismatch || missingNewKeys)
    {
        // Keep a one-shot backup of the previous file before rewriting it.
        if (!fileMissing && GetFileAttributesA((path + ".bak").c_str()) == INVALID_FILE_ATTRIBUTES)
            CopyFileA(path.c_str(), (path + ".bak").c_str(), TRUE);

        WriteConfigFile(path, m_languageRussian, raw);
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

    m_frameTimeStreamingEnabled =
        GetPrivateProfileIntA(
            "DebugUtils.FrameTime", "Streaming",
            1,
            path.c_str()
        ) != 0;

    m_frameTimeStreamLimitMb = static_cast<int>(
        GetPrivateProfileIntA(
            "DebugUtils.FrameTime", "StreamLimitMB",
            0,
            path.c_str()
        )
    );

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
