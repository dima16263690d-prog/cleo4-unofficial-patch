#include "DebugUtils.h"
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

// plugin-sdk declares this GTA SA 1.0 US static reference but does not
// provide a definition in the CLEO/DebugUtils link. Resolve it directly to
// the verified game global used by CTimer::m_CodePause.
bool& CTimer::m_CodePause = *(bool*)0xB7CB48;

DebugUtils* DebugUtils::s_instance = nullptr;
namespace
{
    struct CrashDialogData
    {
        std::wstring title;
        std::wstring details;
        std::wstring logPath;
        DWORD exitCode = 1;
    };

    static std::wstring CrashToWide(const std::string& value)
    {
        if (value.empty())
            return L"";

        UINT codePage = CP_UTF8;
        int count = MultiByteToWideChar(codePage, 0, value.c_str(), -1, nullptr, 0);
        if (count <= 0)
        {
            codePage = CP_ACP;
            count = MultiByteToWideChar(codePage, 0, value.c_str(), -1, nullptr, 0);
        }

        if (count <= 0)
            return L"";

        std::wstring result(static_cast<size_t>(count), L'\0');
        if (MultiByteToWideChar(
            codePage, 0, value.c_str(), -1, &result[0], count) <= 0)
            return L"";

        result.resize(static_cast<size_t>(count - 1));
        return result;
    }

    static bool CopyCrashTextToClipboard(HWND owner, const std::wstring& text)
    {
        if (!OpenClipboard(owner))
            return false;

        if (!EmptyClipboard())
        {
            CloseClipboard();
            return false;
        }

        const SIZE_T bytes =
            static_cast<SIZE_T>((text.size() + 1) * sizeof(wchar_t));

        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
        if (memory == nullptr)
        {
            CloseClipboard();
            return false;
        }

        void* target = GlobalLock(memory);
        if (target == nullptr)
        {
            GlobalFree(memory);
            CloseClipboard();
            return false;
        }

        memcpy(target, text.c_str(), bytes);
        GlobalUnlock(memory);

        if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr)
        {
            GlobalFree(memory);
            CloseClipboard();
            return false;
        }

        CloseClipboard();
        return true;
    }

    struct MainWindowSearchContext
    {
        DWORD processId = 0;
        HWND window = nullptr;
    };

    static BOOL CALLBACK FindMainWindowProc(HWND hwnd, LPARAM lParam)
    {
        MainWindowSearchContext* context =
            reinterpret_cast<MainWindowSearchContext*>(lParam);

        if (context == nullptr)
            return FALSE;

        DWORD windowProcessId = 0;
        GetWindowThreadProcessId(hwnd, &windowProcessId);

        if (windowProcessId != context->processId)
            return TRUE;

        if (GetWindow(hwnd, GW_OWNER) != nullptr)
            return TRUE;

        if (!IsWindowVisible(hwnd))
            return TRUE;

        if (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)
            return TRUE;

        context->window = hwnd;
        return FALSE;
    }

    static HWND FindProcessMainWindow()
    {
        MainWindowSearchContext context{};
        context.processId = GetCurrentProcessId();

        EnumWindows(&FindMainWindowProc, reinterpret_cast<LPARAM>(&context));
        return context.window;
    }

    static void PrepareCrashWindow(HWND dialog)
    {
        HWND gameWindow = FindProcessMainWindow();

        // GTA can keep the mouse captured/clipped even after its window is
        // minimized (DirectInput / fullscreen input handling). Release all
        // process-level mouse capture before showing the crash UI.
        ReleaseCapture();
        ClipCursor(nullptr);

        // Restore the cursor visibility even if GTA hid it before the crash.
        CURSORINFO cursorInfo{};
        cursorInfo.cbSize = sizeof(cursorInfo);
        if (GetCursorInfo(&cursorInfo))
        {
            if ((cursorInfo.flags & CURSOR_SHOWING) == 0)
            {
                ShowCursor(TRUE);
            }
        }
        else
        {
            ShowCursor(TRUE);
        }

        if (gameWindow != nullptr && gameWindow != dialog)
        {
            // Disable the crashed game's window so Windows cannot activate it
            // again while the diagnostic dialog is being used.
            EnableWindow(gameWindow, FALSE);
            ShowWindow(gameWindow, SW_MINIMIZE);
            UpdateWindow(gameWindow);
        }

        if (dialog != nullptr)
        {
            // Make the diagnostic dialog a real interactive foreground window.
            // WS_EX_NOACTIVATE must never be set here.
            SetWindowLongPtrW(
                dialog,
                GWL_EXSTYLE,
                GetWindowLongPtrW(dialog, GWL_EXSTYLE) & ~WS_EX_NOACTIVATE
            );

            SetWindowPos(
                dialog,
                HWND_TOPMOST,
                0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW
            );

            ShowWindow(dialog, SW_SHOWNORMAL);
            BringWindowToTop(dialog);
            SetActiveWindow(dialog);
            SetForegroundWindow(dialog);
            SetFocus(dialog);

            // Move the cursor into the client area so the first mouse click
            // is guaranteed to belong to DebugUtils.
            POINT pt{};
            RECT rc{};
            if (GetClientRect(dialog, &rc) && GetWindowRect(dialog, &rc))
            {
                pt.x = (rc.left + rc.right) / 2;
                pt.y = (rc.top + rc.bottom) / 2;
                SetCursorPos(pt.x, pt.y);
            }

            // Explicitly remove any clip region that may have been restored
            // by the game between Minimize and foreground activation.
            ClipCursor(nullptr);
        }
    }

    static INT_PTR CALLBACK CrashDialogProc(
        HWND hwnd,
        UINT message,
        WPARAM wParam,
        LPARAM lParam)
    {
        CrashDialogData* data =
            reinterpret_cast<CrashDialogData*>(
                GetWindowLongPtrW(hwnd, DWLP_USER));

        switch (message)
        {
        case WM_INITDIALOG:
            data = reinterpret_cast<CrashDialogData*>(lParam);
            SetWindowLongPtrW(hwnd, DWLP_USER, lParam);

            SetWindowTextW(hwnd, data->title.c_str());
            SetDlgItemTextW(hwnd, IDC_CRASH_DETAILS, data->details.c_str());
            PrepareCrashWindow(hwnd);
            SetDlgItemTextW(hwnd, IDC_CRASH_COPY, L"\u0421\u043a\u043e\u043f\u0438\u0440\u043e\u0432\u0430\u0442\u044c");
            SetDlgItemTextW(hwnd, IDC_CRASH_OPEN_LOG, L"\u041e\u0442\u043a\u0440\u044b\u0442\u044c \u043b\u043e\u0433");
            SetDlgItemTextW(hwnd, IDC_CRASH_EXIT, L"\u0417\u0430\u0432\u0435\u0440\u0448\u0438\u0442\u044c \u0438\u0433\u0440\u0443");
            return TRUE;

        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
            case IDC_CRASH_COPY:
                if (data != nullptr)
                {
                    const std::wstring copied =
                        data->details + L"\r\nLog: " + data->logPath;

                    if (CopyCrashTextToClipboard(hwnd, copied))
                        SetDlgItemTextW(hwnd, IDC_CRASH_COPY, L"\u0421\u043a\u043e\u043f\u0438\u0440\u043e\u0432\u0430\u043d\u043e");
                }
                return TRUE;

            case IDC_CRASH_OPEN_LOG:
                if (data != nullptr)
                {
                    ShellExecuteW(
                        hwnd,
                        L"open",
                        data->logPath.c_str(),
                        nullptr,
                        nullptr,
                        SW_SHOWNORMAL
                    );
                }
                return TRUE;

            case IDC_CRASH_EXIT:
                EnableWindow(FindProcessMainWindow(), TRUE);
                ClipCursor(nullptr);
                ReleaseCapture();
                ShowCursor(TRUE);
                TerminateProcess(
                    GetCurrentProcess(),
                    data != nullptr && data->exitCode != 0 ? data->exitCode : 1
                );
                return TRUE;
            }
            break;

        case WM_CLOSE:
            EnableWindow(FindProcessMainWindow(), TRUE);
            ClipCursor(nullptr);
            ReleaseCapture();
            ShowCursor(TRUE);
            TerminateProcess(
                GetCurrentProcess(),
                data != nullptr && data->exitCode != 0 ? data->exitCode : 1
            );
            return TRUE;
        }

        return FALSE;
    }
}


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

    void WinWriteTextFile(const std::string& path, const std::string& text)
    {
        if (path.empty() || text.empty())
            return;

        CreateDirectoryA("cleo", nullptr);
        CreateDirectoryA("cleo\\debug", nullptr);

        HANDLE file = CreateFileA(
            path.c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );

        if (file == INVALID_HANDLE_VALUE)
            return;

        DWORD written = 0;
        const DWORD bytes = static_cast<DWORD>(
            std::min<size_t>(text.size(), 0x7FFFFFFFu)
        );

        const BOOL ok = WriteFile(
            file,
            text.data(),
            bytes,
            &written,
            nullptr
        );

        if (ok && written == bytes)
            FlushFileBuffers(file);

        CloseHandle(file);
    }

    void SkipUnusedVarArgs(CScriptThread* thread)
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

std::string DebugUtils::CrashLogPath() const
{
    return DebugDir() + "gta_crashinfo.log";
}

std::string DebugUtils::CrashInfoPath() const
{
    return "cleo\\cleo_plugins\\CrashInfo\\CLEO-CrashList.txt";
}

std::string DebugUtils::CrashInfoAutoPath() const
{
    return "cleo\\cleo_plugins\\CrashInfo\\CLEO-CrashAuto.txt";
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

    const char* requiredMarker = "# DebugUtils-Database-Version: 2";
    bool databaseReady = false;

    DWORD size = getFileSize(pluginCrashInfoPath);
    if (size != 0)
    {
        std::ifstream file(pluginCrashInfoPath);
        char header[1024] = {};
        file.read(header, sizeof(header) - 1);
        header[file.gcount()] = '\0';
        databaseReady = strstr(header, requiredMarker) != nullptr;
    }

    if (!databaseReady)
    {
        // Preserve an older installation before replacing it with the bundled
        // verified baseline. This avoids silently destroying old local data.
        if (size != 0)
        {
            const std::string legacyPath =
                pluginCrashInfoPath + ".legacy-v1.txt";
            DeleteFileA(legacyPath.c_str());
            CopyFileA(pluginCrashInfoPath.c_str(), legacyPath.c_str(), FALSE);
        }

        HMODULE module = nullptr;
        if (GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(&DebugUtils::s_instance),
                &module))
        {
            HRSRC resource = FindResourceA(
                module,
                MAKEINTRESOURCEA(IDR_CRASHINFO),
                RT_RCDATA
            );

            if (resource != nullptr)
            {
                HGLOBAL loaded = LoadResource(module, resource);
                const DWORD resourceSize = SizeofResource(module, resource);
                const void* resourceData =
                    loaded ? LockResource(loaded) : nullptr;

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
                        FlushFileBuffers(file);
                        CloseHandle(file);
                        databaseReady = ok && written == resourceSize;
                    }
                }
            }
        }
    }

    // Keep the debug copy for human inspection, but never use it as the
    // runtime source of truth.
    size = getFileSize(pluginCrashInfoPath);
    if (size != 0 && getFileSize(debugCrashInfoPath) == 0)
        CopyFileA(pluginCrashInfoPath.c_str(), debugCrashInfoPath.c_str(), FALSE);

    if (databaseReady)
    {
        WriteCore(
            "[crashinfo] verified database ready path=%s size=%u version=2",
            pluginCrashInfoPath.c_str(),
            static_cast<unsigned>(size)
        );
    }
    else
    {
        WriteCore(
            "[crashinfo] verified database unavailable path=%s",
            pluginCrashInfoPath.c_str()
        );
    }
}


void DebugUtils::AppendAutomaticCrashInfo(
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
)
{
    if (fingerprint.empty())
        return;

    const std::string path = CrashInfoAutoPath();
    const std::string dir = DebugDir() + "CrashInfo\\";
    CreateDirectoryA(dir.c_str(), nullptr);

    // AUTO entries are deliberately isolated from the verified database.
    // A crash observed once can never promote itself to VERIFIED.
    {
        std::ifstream existing(path);
        if (existing.is_open())
        {
            std::string line;
            const std::string marker = "Fingerprint: " + fingerprint;
            while (std::getline(existing, line))
            {
                if (line == marker)
                    return;
            }
        }
    }

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

    std::string record;
    LARGE_INTEGER fileSize{};
    if (GetFileSizeEx(file, &fileSize) && fileSize.QuadPart == 0)
    {
        record += "# CLEO DebugUtils Auto Crash Database\r\n";
        record += "# Status: UNVERIFIED observations only.\r\n";
        record += "# These entries are never used as VERIFIED matches.\r\n\r\n";
    }

    char line[1024] = {};
    sprintf_s(
        line, sizeof(line),
        "# --------------------------------------------------------------------\r\n"
        "Status: UNVERIFIED\r\n"
        "Fingerprint: %s\r\n"
        "Exception: 0x%08X (%s)\r\n"
        "Fault: %s + 0x%08X\r\n"
        "Access: %s\r\n"
        "Target: 0x%08X\r\n"
        "Last script: %.8s\r\n"
        "Last opcode: 0x%04X\r\n",
        fingerprint.c_str(),
        exceptionCode,
        exceptionType ? exceptionType : "UNKNOWN",
        faultModule.empty() ? "<unknown>" : faultModule.c_str(),
        faultRva,
        AccessTypeName(accessType).c_str(),
        static_cast<DWORD>(targetAddress),
        lastScript.empty() ? "none" : lastScript.c_str(),
        lastOpcode == 0xFFFFFFFF ? 0xFFFF : (lastOpcode & 0x7FFF)
    );
    record += line;

    if (!backtrace.empty())
    {
        record += "Backtrace:";
        const size_t count = std::min<size_t>(backtrace.size(), 12);
        for (size_t i = 0; i < count; ++i)
        {
            char bt[32] = {};
            sprintf_s(bt, sizeof(bt), " 0x%08X", backtrace[i]);
            record += bt;
        }
        record += "\r\n";
    }

    record += "Cause: NOT PROVEN\r\n";
    record += "Promotion: Reproduce and verify before adding to CLEO-CrashList.txt\r\n\r\n";

    DWORD written = 0;
    const BOOL ok = WriteFile(
        file,
        record.data(),
        static_cast<DWORD>(record.size()),
        &written,
        nullptr
    );
    FlushFileBuffers(file);
    CloseHandle(file);

    if (!ok || written != record.size())
        return;

    WriteCore(
        "[crashinfo] auto candidate added fingerprint=%s path=%s",
        fingerprint.c_str(),
        path.c_str()
    );
}

void DebugUtils::OpenLogs()void DebugUtils::OpenLogs()
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

    if (++m_corePendingWrites >= 32)
    {
        m_coreLog.flush();
        m_corePendingWrites = 0;
    }
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

    const std::string path = CrashInfoPath();
    std::ifstream file(path);
    if (!file.is_open())
    {
        WriteCore("[crashinfo] verified database unavailable path=%s", path.c_str());
        return;
    }

    size_t loadedEntries = 0;
    bool scriptSection = false;
    CrashInfoEntry* current = nullptr;

    auto beginEntry = [this, &current, &loadedEntries]()
    {
        if (m_crashInfo.size() >= 8192)
            return false;

        m_crashInfo.push_back({});
        current = &m_crashInfo.back();
        ++loadedEntries;
        return true;
    };

    std::string line;
    while (std::getline(file, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        if (line.rfind("By scripts:", 0) == 0)
        {
            scriptSection = true;
            current = nullptr;
            continue;
        }

        if (line.rfind("Others:", 0) == 0)
        {
            scriptSection = false;
            current = nullptr;
            continue;
        }

        if (line.rfind("Error: ", 0) == 0)
        {
            scriptSection = false;

            if (!beginEntry())
                break;

            const size_t matcherStart = 7;
            const size_t backtracePos = line.find("Backtrace", matcherStart);
            const size_t matcherEnd =
                backtracePos == std::string::npos ? line.size() : backtracePos;
            const std::string matcherText =
                line.substr(matcherStart, matcherEnd - matcherStart);

            ExtractHexAddresses(matcherText, 0, current->errorAddresses);
            ExtractModuleNames(matcherText, current->errorModules);

            if (matcherText.find("0x*") != std::string::npos)
                current->wildcardError = true;

            if (backtracePos != std::string::npos)
            {
                ExtractHexAddresses(line, backtracePos, current->backtraceAddresses);
                ExtractModuleNames(
                    line.substr(backtracePos),
                    current->backtraceModules
                );
            }

            current->hasMatcher =
                !current->errorAddresses.empty() ||
                !current->errorModules.empty() ||
                current->wildcardError ||
                !current->backtraceAddresses.empty() ||
                !current->backtraceModules.empty();

            continue;
        }

        if (line.rfind("Last command:", 0) == 0)
        {
            scriptSection = false;

            if (!beginEntry())
                break;

            current->lastCommands.clear();
            ExtractHexAddresses(line, 0, current->lastCommands);
            if (!current->lastCommands.empty())
            {
                current->lastOpcode = current->lastCommands.front();
                char name[64] = {};
                sprintf_s(
                    name, sizeof(name),
                    "Last command [0x%04X]",
                    current->lastOpcode & 0x7FFF
                );
                current->name = name;
                current->hasMatcher = true;
            }
            continue;
        }

        if (scriptSection &&
            !line.empty() &&
            line.find(':') == std::string::npos &&
            line != "Others")
        {
            if (!beginEntry())
                break;

            current->scriptName = line;
            current->name = "Script: " + line;
            current->hasMatcher = true;
            continue;
        }

        if (current == nullptr)
            continue;

        if (line.rfind("Status: ", 0) == 0)
        {
            current->autoDiscovered =
                line.substr(8).find("UNVERIFIED") != std::string::npos;
            continue;
        }

        if (line.rfind("Fingerprint: ", 0) == 0)
        {
            continue;
        }

        if (line.rfind("Exception: ", 0) == 0)
        {
            const size_t p = line.find("0x", 11);
            if (p != std::string::npos)
                current->exceptionCode =
                    static_cast<DWORD>(strtoul(line.c_str() + p + 2, nullptr, 16));
            continue;
        }

        if (line.rfind("Access: ", 0) == 0)
        {
            const std::string value = line.substr(8);
            if (_stricmp(value.c_str(), "READ") == 0)
                current->accessType = 0;
            else if (_stricmp(value.c_str(), "WRITE") == 0)
                current->accessType = 1;
            else if (_stricmp(value.c_str(), "EXECUTE") == 0)
                current->accessType = 8;
            continue;
        }

        if (line.rfind("Last script: ", 0) == 0)
        {
            current->scriptName = line.substr(12);
            current->hasMatcher = true;
            continue;
        }

        if (line.rfind("Last opcode: ", 0) == 0)
        {
            const size_t p = line.find("0x", 13);
            if (p != std::string::npos)
            {
                current->lastOpcode =
                    static_cast<DWORD>(strtoul(line.c_str() + p + 2, nullptr, 16));
                current->hasMatcher = true;
            }
            continue;
        }

        if (line.rfind("Name: ", 0) == 0)
        {
            current->name = line.substr(6);
            continue;
        }

        if (line.rfind("Problem: ", 0) == 0)
        {
            current->issue = line.substr(9);
            continue;
        }

        if (line.rfind("Issue: ", 0) == 0)
        {
            current->issue = line.substr(7);
            continue;
        }

        if (line.rfind("Problem ", 0) == 0)
        {
            if (!current->issue.empty())
                current->issue += " | ";
            current->issue += line;
            continue;
        }

        if (line.rfind("About: ", 0) == 0)
        {
            if (!current->about.empty())
                current->about += " | ";
            current->about += line.substr(7);
            continue;
        }

        if (line.rfind("Solution: ", 0) == 0)
        {
            if (!current->solution.empty())
                current->solution += " | ";
            current->solution += line.substr(10);
            continue;
        }

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

        if (!line.empty() && line[0] == '#')
            continue;

        if (!line.empty())
        {
            if (!current->description.empty())
                current->description += " | ";

            current->description += line;

            if (current->description.size() > 4000)
                current->description.resize(4000);
        }
    }

    WriteCore(
        "[crashinfo] loaded verified entries=%u path=%s",
        static_cast<unsigned>(loadedEntries),
        path.c_str()
    );
}


const DebugUtils::CrashInfoEntry* DebugUtils::FindCrashInfo(
    DWORD address,
    const std::string& faultModule,
    const std::vector<DWORD>& backtrace,
    DWORD exceptionCode,
    int accessType,
    const std::string& lastScript,
    DWORD lastOpcode
) const
{
    const CrashInfoEntry* best = nullptr;
    int bestScore = -1;

    std::vector<std::string> backtraceModules;

    auto sameText = [](const std::string& a, const std::string& b) -> bool
    {
        if (a.empty() || b.empty() || a.size() != b.size())
            return false;

        return _stricmp(a.c_str(), b.c_str()) == 0;
    };

    for (const auto& entry : m_crashInfo)
    {
        if (entry.autoDiscovered)
            continue;

        const bool exactError =
            ContainsAddress(entry.errorAddresses, address);
        const bool moduleError =
            ContainsModule(entry.errorModules, faultModule);
        const bool wildcard =
            entry.wildcardError;

        int score = 0;

        if (!entry.errorAddresses.empty() ||
            !entry.errorModules.empty() ||
            entry.wildcardError)
        {
            if (!exactError && !moduleError && !wildcard)
                continue;

            if (exactError)
                score += 500;
            if (moduleError)
                score += 320;
            if (wildcard)
                score += 20;
        }

        int backtraceMatches = 0;

        for (DWORD expected : entry.backtraceAddresses)
        {
            if (!ContainsAddress(backtrace, expected))
                continue;

            ++backtraceMatches;
        }

        if (!entry.backtraceAddresses.empty() &&
            backtraceMatches != static_cast<int>(entry.backtraceAddresses.size()))
        {
            continue;
        }

        score += backtraceMatches * 80;

        if (!entry.backtraceModules.empty())
        {
            if (backtraceModules.empty())
            {
                backtraceModules.reserve(backtrace.size());
                for (DWORD bt : backtrace)
                    backtraceModules.push_back(ModuleNameForAddress(bt));
            }

            for (const auto& expected : entry.backtraceModules)
            {
                if (!ContainsModule(backtraceModules, expected))
                    continue;

                ++backtraceMatches;
            }

            const int requiredModules =
                static_cast<int>(entry.backtraceModules.size());

            if (backtraceMatches < requiredModules)
                continue;

            score += requiredModules * 60;
        }

        if (entry.exceptionCode != 0)
        {
            if (entry.exceptionCode != exceptionCode)
                continue;
            score += 40;
        }

        if (entry.accessType >= 0)
        {
            if (entry.accessType != accessType)
                continue;
            score += 30;
        }

        bool contextMatched = false;

        const DWORD normalizedOpcode =
            lastOpcode == 0xFFFFFFFF ? 0xFFFFFFFF : lastOpcode & 0x7FFF;

        if (entry.lastOpcode != 0)
        {
            if (normalizedOpcode != 0xFFFFFFFF &&
                entry.lastOpcode == normalizedOpcode)
            {
                score += 120;
                contextMatched = true;
            }
            else if (!entry.errorAddresses.empty() ||
                     !entry.errorModules.empty() ||
                     entry.wildcardError)
            {
                continue;
            }
        }

        for (DWORD expectedOpcode : entry.lastCommands)
        {
            if (normalizedOpcode != 0xFFFFFFFF &&
                expectedOpcode == normalizedOpcode)
            {
                score += 120;
                contextMatched = true;
                break;
            }
        }

        if (!entry.scriptName.empty() &&
            sameText(entry.scriptName, lastScript))
        {
            score += 160;
            contextMatched = true;
        }
        else if (!entry.scriptName.empty() &&
                 entry.errorAddresses.empty() &&
                 entry.errorModules.empty() &&
                 !entry.wildcardError &&
                 entry.backtraceAddresses.empty() &&
                 entry.backtraceModules.empty())
        {
            continue;
        }

        const bool hasAnyMatcher =
            !entry.errorAddresses.empty() ||
            !entry.errorModules.empty() ||
            entry.wildcardError ||
            !entry.backtraceAddresses.empty() ||
            !entry.backtraceModules.empty() ||
            entry.lastOpcode != 0 ||
            !entry.lastCommands.empty() ||
            !entry.scriptName.empty() ||
            entry.exceptionCode != 0 ||
            entry.accessType >= 0;

        if (!hasAnyMatcher || score <= 0)
            continue;

        if (entry.errorAddresses.empty() &&
            entry.errorModules.empty() &&
            !entry.wildcardError &&
            entry.backtraceAddresses.empty() &&
            entry.backtraceModules.empty() &&
            !contextMatched)
        {
            continue;
        }

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

bool DebugUtils::SafeReadBytes(const void* address, void* buffer, size_t size)
{
    if (address == nullptr || buffer == nullptr || size == 0)
        return false;

    __try
    {
        memcpy(buffer, address, size);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
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

namespace
{
    static volatile LONG g_dbgHelpActive = 0;

    static BOOL CALLBACK DebugUtilsReadProcessMemory(
        HANDLE process,
        DWORD64 baseAddress,
        PVOID buffer,
        DWORD size,
        LPDWORD bytesRead
    )
    {
        SIZE_T read = 0;
        if (!ReadProcessMemory(
                process,
                reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(baseAddress)),
                buffer,
                size,
                &read))
        {
            if (bytesRead != nullptr)
                *bytesRead = 0;
            return FALSE;
        }

        if (bytesRead != nullptr)
            *bytesRead = static_cast<DWORD>(read);

        return TRUE;
    }

    static HMODULE GetDebugHelpModule()
    {
        return LoadLibraryA("DbgHelp.dll");
    }
}

std::vector<DWORD> DebugUtils::BuildStackWalk(
    PEXCEPTION_POINTERS info,
    DWORD maxFrames
)
{
    std::vector<DWORD> frames;

    if (info == nullptr || info->ContextRecord == nullptr || maxFrames == 0)
        return frames;

    if (InterlockedCompareExchange(&g_dbgHelpActive, 1, 0) != 0)
        return frames;

    HMODULE dbgHelp = GetDebugHelpModule();
    if (dbgHelp == nullptr)
    {
        InterlockedExchange(&g_dbgHelpActive, 0);
        return frames;
    }

    using StackWalk64Proc = decltype(&StackWalk64);
    using SymInitializeProc = decltype(&SymInitialize);
    using SymCleanupProc = decltype(&SymCleanup);
    using SymFunctionTableAccess64Proc = decltype(&SymFunctionTableAccess64);
    using SymGetModuleBase64Proc = decltype(&SymGetModuleBase64);

    const auto pStackWalk64 =
        reinterpret_cast<StackWalk64Proc>(GetProcAddress(dbgHelp, "StackWalk64"));
    const auto pSymInitialize =
        reinterpret_cast<SymInitializeProc>(GetProcAddress(dbgHelp, "SymInitialize"));
    const auto pSymCleanup =
        reinterpret_cast<SymCleanupProc>(GetProcAddress(dbgHelp, "SymCleanup"));
    const auto pSymFunctionTableAccess64 =
        reinterpret_cast<SymFunctionTableAccess64Proc>(
            GetProcAddress(dbgHelp, "SymFunctionTableAccess64"));
    const auto pSymGetModuleBase64 =
        reinterpret_cast<SymGetModuleBase64Proc>(
            GetProcAddress(dbgHelp, "SymGetModuleBase64"));

    if (pStackWalk64 == nullptr ||
        pSymFunctionTableAccess64 == nullptr ||
        pSymGetModuleBase64 == nullptr)
    {
        FreeLibrary(dbgHelp);
        InterlockedExchange(&g_dbgHelpActive, 0);
        return frames;
    }

    const HANDLE process = GetCurrentProcess();
    const HANDLE thread = GetCurrentThread();

    bool symbolsInitialized = false;

    __try
    {
        if (pSymInitialize != nullptr)
            symbolsInitialized = pSymInitialize(process, nullptr, TRUE) != FALSE;

        CONTEXT context = *info->ContextRecord;
        STACKFRAME64 frame{};

        frame.AddrPC.Offset = context.Eip;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = context.Ebp;
        frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = context.Esp;
        frame.AddrStack.Mode = AddrModeFlat;

        frames.push_back(context.Eip);

        while (frames.size() < maxFrames)
        {
            const BOOL ok = pStackWalk64(
                IMAGE_FILE_MACHINE_I386,
                process,
                thread,
                &frame,
                &context,
                &DebugUtilsReadProcessMemory,
                reinterpret_cast<PFUNCTION_TABLE_ACCESS_ROUTINE64>(
                    pSymFunctionTableAccess64),
                reinterpret_cast<PGET_MODULE_BASE_ROUTINE64>(
                    pSymGetModuleBase64),
                nullptr
            );

            if (!ok)
                break;

            const DWORD address =
                static_cast<DWORD>(frame.AddrPC.Offset);

            if (address == 0 ||
                address == frames.back())
                break;

            frames.push_back(address);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        // Never let a broken stack prevent the crash report from being written.
    }

    if (symbolsInitialized && pSymCleanup != nullptr)
        pSymCleanup(process);

    FreeLibrary(dbgHelp);
    InterlockedExchange(&g_dbgHelpActive, 0);

    return frames;
}

std::string DebugUtils::AccessTypeName(int accessType)
{
    switch (accessType)
    {
    case 0: return "READ";
    case 1: return "WRITE";
    case 8: return "EXECUTE";
    default: return "UNKNOWN";
    }
}

std::string DebugUtils::MemoryStateName(DWORD state)
{
    switch (state)
    {
    case MEM_COMMIT: return "COMMIT";
    case MEM_RESERVE: return "RESERVE";
    case MEM_FREE: return "FREE";
    default: return "UNKNOWN";
    }
}

std::string DebugUtils::MemoryProtectName(DWORD protect)
{
    const DWORD base = protect & 0xFFu;

    switch (base)
    {
    case PAGE_NOACCESS: return "NOACCESS";
    case PAGE_READONLY: return "READONLY";
    case PAGE_READWRITE: return "READWRITE";
    case PAGE_WRITECOPY: return "WRITECOPY";
    case PAGE_EXECUTE: return "EXECUTE";
    case PAGE_EXECUTE_READ: return "EXECUTE_READ";
    case PAGE_EXECUTE_READWRITE: return "EXECUTE_READWRITE";
    case PAGE_EXECUTE_WRITECOPY: return "EXECUTE_WRITECOPY";
    default: return "UNKNOWN";
    }
}

std::string DebugUtils::BuildCrashFingerprint(
    DWORD exceptionCode,
    DWORD faultAddress,
    DWORD faultRva,
    const std::string& faultModule,
    int accessType,
    uintptr_t targetAddress,
    const std::string& lastScript,
    DWORD lastOpcode,
    const std::vector<DWORD>& backtrace
)
{
    uint32_t hash = 2166136261u;

    auto mixByte = [&hash](BYTE value)
    {
        hash ^= value;
        hash *= 16777619u;
    };

    auto mixDword = [&mixByte](DWORD value)
    {
        mixByte(static_cast<BYTE>(value));
        mixByte(static_cast<BYTE>(value >> 8));
        mixByte(static_cast<BYTE>(value >> 16));
        mixByte(static_cast<BYTE>(value >> 24));
    };

    mixDword(exceptionCode);
    mixDword(faultAddress);
    mixDword(faultRva);
    mixDword(static_cast<DWORD>(accessType));
    mixDword(static_cast<DWORD>(targetAddress));

    for (unsigned char ch : faultModule)
        mixByte(ch);

    for (unsigned char ch : lastScript)
        mixByte(ch);

    mixDword(lastOpcode == 0xFFFFFFFF ? 0xFFFFFFFFu : lastOpcode & 0x7FFFu);

    const size_t count = std::min<size_t>(backtrace.size(), 8);
    for (size_t i = 0; i < count; ++i)
        mixDword(backtrace[i]);

    char result[16] = {};
    sprintf_s(result, sizeof(result), "%08X", hash);
    return result;
}

void DebugUtils::ShowCrashDialog(
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
)
{
    CrashDialogData data{};

    char text[8192] = {};
    sprintf_s(
        text, sizeof(text),
        "Critical GTA SA crash detected by DebugUtils.\r\n\r\n"
        "Crash: %s\r\n"
        "Exception: 0x%08X (%s)\r\n"
        "Address: 0x%08X\r\n"
        "Module: %s\r\n"
        "RVA: 0x%08X\r\n"
        "CrashInfo: %s\r\n\r\n"
        "Last script: %s\r\n"
        "Last opcode: 0x%04X\r\n",
        crashName ? crashName : "Unknown",
        exceptionCode,
        exceptionType ? exceptionType : "UNKNOWN",
        faultAddress,
        faultModule.c_str(),
        faultRva,
        confidence ? confidence : "none",
        lastScript.c_str(),
        lastOpcode == 0xFFFFFFFF ? 0xFFFF : (lastOpcode & 0x7FFF)
    );

    std::string details(text);
    if (!issue.empty())
        details += "\r\nIssue: " + issue;
    if (!about.empty())
        details += "\r\nAbout: " + about;
    if (!solution.empty())
        details += "\r\nSolution: " + solution;

    details += "\r\n\r\nBacktrace:";
    if (backtrace.empty())
    {
        details += "\r\n  <not available>";
    }
    else
    {
        for (size_t i = 0; i < std::min<size_t>(backtrace.size(), 8); ++i)
        {
            char frame[128] = {};
            sprintf_s(
                frame, sizeof(frame),
                "\r\n  #%02u 0x%08X %s",
                static_cast<unsigned>(i),
                backtrace[i],
                ModuleNameForAddress(backtrace[i]).c_str()
            );
            details += frame;
        }
    }

    data.title = L"CLEO DebugUtils - GTA SA crash";
    data.details = CrashToWide(details);
    data.logPath = CrashToWide(CrashLogPath());
    data.exitCode = exceptionCode;

    HMODULE dialogModule = nullptr;
    if (!GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&DebugUtils::s_instance),
        &dialogModule))
    {
        TerminateProcess(
            GetCurrentProcess(),
            exceptionCode != 0 ? exceptionCode : 1
        );
    }

    const INT_PTR result = DialogBoxParamW(
        dialogModule,
        MAKEINTRESOURCEW(IDD_CRASH_DIALOG),
        nullptr,
        &CrashDialogProc,
        reinterpret_cast<LPARAM>(&data)
    );

    // The normal result path is process termination from the dialog. If the
    // dialog cannot be created, never return to corrupted GTA state.
    if (result == -1)
        TerminateProcess(
            GetCurrentProcess(),
            exceptionCode != 0 ? exceptionCode : 1
        );
}

void DebugUtils::WriteCrashReport(PEXCEPTION_POINTERS info)
{
    if (info == nullptr ||
        info->ExceptionRecord == nullptr ||
        info->ContextRecord == nullptr)
        return;

    const EXCEPTION_RECORD* record = info->ExceptionRecord;
    const CONTEXT* context = info->ContextRecord;

    const DWORD exceptionCode = record->ExceptionCode;
    const DWORD faultAddress =
        static_cast<DWORD>(reinterpret_cast<uintptr_t>(record->ExceptionAddress));

    const std::string faultModule = ModuleNameForAddress(faultAddress);

    DWORD faultModuleBase = 0;
    DWORD faultRva = 0;
    HMODULE faultModuleHandle = nullptr;
    MODULEINFO faultModuleInfo{};

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
        faultModuleBase =
            static_cast<DWORD>(reinterpret_cast<uintptr_t>(faultModuleInfo.lpBaseOfDll));

        if (faultAddress >= faultModuleBase)
            faultRva = faultAddress - faultModuleBase;
    }

    int accessType = -1;
    uintptr_t targetAddress = 0;

    if (exceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        record->NumberParameters >= 2)
    {
        accessType =
            static_cast<int>(record->ExceptionInformation[0]);
        targetAddress =
            static_cast<uintptr_t>(record->ExceptionInformation[1]);
    }

    // The CLEO snapshot is TLS-backed. It therefore identifies the last
    // opcode on the thread that actually crashed, without a mutex or global
    // sequence protocol in the normal opcode path.
    CLEO_CrashSnapshot crashSnapshot{};
    const bool crashSnapshotValid =
        CLEO_DebugGetCrashSnapshot(&crashSnapshot) != FALSE;

    const std::string lastScript =
        crashSnapshotValid
            ? std::string(
                crashSnapshot.scriptName,
                strnlen_s(
                    crashSnapshot.scriptName,
                    sizeof(crashSnapshot.scriptName)))
            : std::string("none");

    const DWORD lastOpcode =
        crashSnapshotValid ? crashSnapshot.opcode : 0xFFFFFFFFu;
    const DWORD lastOpcodeOffset =
        crashSnapshotValid ? crashSnapshot.opcodeOffset : 0;
    const LONG lastOpcodeResult =
        crashSnapshotValid ? crashSnapshot.opcodeResult : -1;
    const uintptr_t lastScriptPtr =
        crashSnapshotValid ? crashSnapshot.scriptPtr : 0;

    // All heavy work starts here, after the exception has reached the
    // top-level unhandled filter.
    EnsureCrashInfoDatabase();
    LoadCrashInfoList();

    const std::vector<DWORD> backtraceAddresses =
        m_crashBacktraceEnabled
            ? BuildStackWalk(info, m_crashMaxFrames)
            : std::vector<DWORD>();

    const std::string fingerprint =
        BuildCrashFingerprint(
            exceptionCode,
            faultAddress,
            faultRva,
            faultModule,
            accessType,
            targetAddress,
            lastScript,
            lastOpcode,
            backtraceAddresses
        );

    const CrashInfoEntry* match =
        FindCrashInfo(
            faultAddress,
            faultModule,
            backtraceAddresses,
            exceptionCode,
            accessType,
            lastScript,
            lastOpcode
        );

    bool exactAddress = false;
    bool moduleMatch = false;
    bool wildcardMatch = false;
    bool contextMatch = false;

    if (match != nullptr)
    {
        exactAddress =
            ContainsAddress(match->errorAddresses, faultAddress);
        moduleMatch =
            ContainsModule(match->errorModules, faultModule);
        wildcardMatch =
            match->wildcardError;

        if (lastOpcode != 0xFFFFFFFFu)
        {
            const DWORD normalized =
                lastOpcode & 0x7FFFu;

            if (match->lastOpcode == normalized)
                contextMatch = true;

            for (DWORD expected : match->lastCommands)
            {
                if (expected == normalized)
                {
                    contextMatch = true;
                    break;
                }
            }
        }

        if (!match->scriptName.empty() &&
            !_stricmp(match->scriptName.c_str(), lastScript.c_str()))
        {
            contextMatch = true;
        }
    }

    const char* confidence = "NONE";
    if (match == nullptr)
        confidence = "UNKNOWN";
    else if (exactAddress)
        confidence = "EXACT-ADDRESS";
    else if (moduleMatch)
        confidence = "MODULE";
    else if (contextMatch)
        confidence = "SCRIPT/OPCODE";
    else if (wildcardMatch)
        confidence = "FALLBACK";
    else
        confidence = "BACKTRACE";

    // Unknown locations are added automatically, but never to the verified
    // database. The generated fingerprint is the stable duplicate key.
    if (match == nullptr || wildcardMatch)
    {
        AppendAutomaticCrashInfo(
            fingerprint,
            faultAddress,
            exceptionCode,
            ExceptionName(exceptionCode),
            faultModule,
            faultRva,
            accessType,
            targetAddress,
            lastScript,
            lastOpcode,
            backtraceAddresses
        );
    }

    const std::string accessName =
        AccessTypeName(accessType);

    char report[8192] = {};
    SYSTEMTIME t{};
    GetLocalTime(&t);

    std::string output;
    output.reserve(32768);

    auto appendf = [&output](const char* format, ...)
    {
        char line[4096] = {};
        va_list args;
        va_start(args, format);
        SafeFormat(line, sizeof(line), format, args);
        va_end(args);
        output += line;
        output += "\r\n";
    };

    output += "============================================================\r\n";
    output += "                 CLEO DEBUGUTILS CRASH REPORT\r\n";
    output += "============================================================\r\n\r\n";

    appendf(
        "[REPORT]\r\n"
        "time=%04u-%02u-%02u %02u:%02u:%02u.%03u\r\n"
        "fingerprint=%s",
        t.wYear, t.wMonth, t.wDay,
        t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
        fingerprint.c_str()
    );

    output += "\r\n[EXCEPTION]\r\n";
    appendf(
        "code=0x%08X\r\ntype=%s\r\nflags=0x%08X\r\nparameters=%u",
        exceptionCode,
        ExceptionName(exceptionCode),
        record->ExceptionFlags,
        record->NumberParameters
    );

    output += "\r\n[FAULT]\r\n";
    appendf(
        "address=0x%08X\r\nmodule=%s\r\nmodule_base=0x%08X\r\nrva=0x%08X",
        faultAddress,
        faultModule.c_str(),
        faultModuleBase,
        faultRva
    );

    if (record->NumberParameters >= 2)
    {
        appendf(
            "access=%s (%d)\r\ntarget=0x%08X",
            accessName.c_str(),
            accessType,
            static_cast<DWORD>(targetAddress)
        );
    }
    else
    {
        output += "access=NOT_AVAILABLE\r\ntarget=NOT_AVAILABLE\r\n";
    }

    output += "\r\n[CPU]\r\n";
    appendf(
        "EAX=%08X EBX=%08X ECX=%08X EDX=%08X\r\n"
        "EDI=%08X ESI=%08X EBP=%08X EIP=%08X\r\n"
        "ESP=%08X EFLAGS=%08X\r\n"
        "CS=%04X SS=%04X DS=%04X ES=%04X FS=%04X GS=%04X",
        context->Eax, context->Ebx, context->Ecx, context->Edx,
        context->Edi, context->Esi, context->Ebp, context->Eip,
        context->Esp, context->EFlags,
        context->SegCs, context->SegSs, context->SegDs,
        context->SegEs, context->SegFs, context->SegGs
    );

    output += "\r\n[INSTRUCTION]\r\n";
    {
        BYTE bytes[16] = {};
        if (SafeReadBytes(
                reinterpret_cast<const void*>(faultAddress),
                bytes,
                sizeof(bytes)))
        {
            char hex[16 * 3 + 1] = {};
            size_t pos = 0;

            for (unsigned i = 0; i < 16; ++i)
            {
                sprintf_s(
                    hex + pos,
                    sizeof(hex) - pos,
                    i == 0 ? "%02X" : " %02X",
                    static_cast<unsigned>(bytes[i])
                );
                pos = strlen(hex);
            }

            appendf(
                "address=0x%08X\r\nbytes=%s",
                faultAddress,
                hex
            );
        }
        else
        {
            output += "bytes=<not readable>\r\n";
        }
    }

    output += "\r\n[MEMORY: FAULT ADDRESS]\r\n";
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(
                reinterpret_cast<LPCVOID>(
                    static_cast<uintptr_t>(faultAddress)),
                &mbi,
                sizeof(mbi)) != 0)
        {
            appendf(
                "base=%p\r\nallocation_base=%p\r\nregion_size=0x%08X\r\n"
                "state=%s\r\nprotect=%s\r\ntype=0x%08X",
                mbi.BaseAddress,
                mbi.AllocationBase,
                static_cast<unsigned>(mbi.RegionSize),
                MemoryStateName(mbi.State).c_str(),
                MemoryProtectName(mbi.Protect).c_str(),
                mbi.Type
            );
        }
        else
        {
            output += "state=<unavailable>\r\n";
        }
    }

    if (targetAddress != 0)
    {
        output += "\r\n[MEMORY: ACCESS TARGET]\r\n";

        MEMORY_BASIC_INFORMATION targetMbi{};
        if (VirtualQuery(
                reinterpret_cast<LPCVOID>(targetAddress),
                &targetMbi,
                sizeof(targetMbi)) != 0)
        {
            appendf(
                "base=%p\r\nallocation_base=%p\r\nregion_size=0x%08X\r\n"
                "state=%s\r\nprotect=%s\r\ntype=0x%08X",
                targetMbi.BaseAddress,
                targetMbi.AllocationBase,
                static_cast<unsigned>(targetMbi.RegionSize),
                MemoryStateName(targetMbi.State).c_str(),
                MemoryProtectName(targetMbi.Protect).c_str(),
                targetMbi.Type
            );
        }
        else
        {
            output += "state=<unavailable>\r\n";
        }
    }

    output += "\r\n[PROCESS MEMORY]\r\n";
    {
        PROCESS_MEMORY_COUNTERS_EX pmc{};
        pmc.cb = sizeof(pmc);

        if (GetProcessMemoryInfo(
                GetCurrentProcess(),
                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                sizeof(pmc)))
        {
            appendf(
                "working_set=%I64u\r\n"
                "peak_working_set=%I64u\r\n"
                "private=%I64u\r\n"
                "pagefile=%I64u",
                static_cast<unsigned __int64>(pmc.WorkingSetSize),
                static_cast<unsigned __int64>(pmc.PeakWorkingSetSize),
                static_cast<unsigned __int64>(pmc.PrivateUsage),
                static_cast<unsigned __int64>(pmc.PagefileUsage)
            );
        }
        else
        {
            output += "state=<unavailable>\r\n";
        }
    }

    output += "\r\n[STACK WALK]\r\n";
    if (backtraceAddresses.empty())
    {
        output += "status=UNAVAILABLE\r\n";
    }
    else
    {
        for (size_t i = 0; i < backtraceAddresses.size(); ++i)
        {
            const DWORD address = backtraceAddresses[i];
            const std::string module = ModuleNameForAddress(address);

            DWORD base = 0;
            DWORD rva = 0;
            HMODULE handle = nullptr;
            MODULEINFO infoModule{};

            if (GetModuleHandleExA(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCSTR>(address),
                    &handle) &&
                GetModuleInformation(
                    GetCurrentProcess(),
                    handle,
                    &infoModule,
                    sizeof(infoModule)))
            {
                base =
                    static_cast<DWORD>(
                        reinterpret_cast<uintptr_t>(infoModule.lpBaseOfDll));
                if (address >= base)
                    rva = address - base;
            }

            appendf(
                "#%02u address=0x%08X module=%s rva=0x%08X",
                static_cast<unsigned>(i),
                address,
                module.c_str(),
                rva
            );
        }
    }

    output += "\r\n[CLEO CONTEXT]\r\n";
    appendf(
        "snapshot_valid=%d\r\n"
        "script=%.8s\r\n"
        "script_ptr=%p\r\n"
        "last_opcode=0x%04X\r\n"
        "opcode_offset=0x%08X\r\n"
        "opcode_result=%d\r\n"
        "game_tick=%u",
        crashSnapshotValid ? 1 : 0,
        lastScript.c_str(),
        reinterpret_cast<void*>(lastScriptPtr),
        lastOpcode == 0xFFFFFFFFu ? 0xFFFFu : lastOpcode & 0x7FFFu,
        lastOpcodeOffset,
        static_cast<int>(lastOpcodeResult),
        crashSnapshotValid ? crashSnapshot.gameTick : 0
    );

    output += "\r\n[CRASH MATCHER]\r\n";
    if (match != nullptr)
    {
        appendf(
            "database=VERIFIED\r\n"
            "name=%s\r\n"
            "confidence=%s\r\n"
            "exact_address=%d\r\n"
            "module_match=%d\r\n"
            "wildcard=%d\r\n"
            "context_match=%d",
            match->name.empty() ? "Unnamed signature" : match->name.c_str(),
            confidence,
            exactAddress ? 1 : 0,
            moduleMatch ? 1 : 0,
            wildcardMatch ? 1 : 0,
            contextMatch ? 1 : 0
        );

        if (!match->issue.empty())
            appendf("issue=%s", match->issue.c_str());
        if (!match->about.empty())
            appendf("about=%s", match->about.c_str());
        if (!match->solution.empty())
            appendf("solution=%s", match->solution.c_str());
    }
    else
    {
        output +=
            "database=NONE\r\n"
            "name=Unknown / Unclassified Crash\r\n"
            "confidence=UNKNOWN\r\n";
    }

    const bool faultInCleo =
        _stricmp(faultModule.c_str(), "CLEO.asi") == 0;
    const bool databaseMentionsCleo =
        match != nullptr &&
        (match->issue.find("CLEO") != std::string::npos ||
         match->about.find("CLEO") != std::string::npos ||
         match->solution.find("CLEO") != std::string::npos);

    output += "\r\n[DIAGNOSIS]\r\n";

    if (!output.empty())
    {
        appendf(
            "fault_location=%s + 0x%08X",
            faultModule.empty() ? "<unknown>" : faultModule.c_str(),
            faultRva
        );
    }

    if (faultInCleo)
        output += "cleo_involvement=NOT_PROVEN (fault address is inside CLEO.asi)\r\n";
    else if (databaseMentionsCleo)
        output += "cleo_involvement=NOT_PROVEN (database context mentions CLEO)\r\n";
    else
        output += "cleo_involvement=NOT_PROVEN\r\n";

    output +=
        "last_opcode_is_cause=NOT_PROVEN\r\n"
        "last_script_is_cause=NOT_PROVEN\r\n";

    if (match == nullptr || wildcardMatch)
        output +=
            "auto_database=UNVERIFIED candidate written to CLEO-CrashAuto.txt\r\n";

    output += "\r\n[END]\r\n";
    output += "============================================================\r\n";

    WinWriteTextFile(CrashLogPath(), output);

    if (m_crashWindowEnabled)
    {
        ShowCrashDialog(
            match != nullptr
                ? (match->name.empty()
                    ? "Unnamed Signature"
                    : match->name.c_str())
                : "Unknown / Unclassified Crash",
            exceptionCode,
            ExceptionName(exceptionCode),
            faultAddress,
            faultModule,
            faultRva,
            confidence,
            match != nullptr ? match->issue : std::string(),
            match != nullptr ? match->about : std::string(),
            match != nullptr ? match->solution : std::string(),
            lastScript,
            lastOpcode,
            backtraceAddresses
        );
    }
}

void DebugUtils::RecordOpcode(void DebugUtils::RecordOpcode(CScriptThread* thread, DWORD opcode, DWORD result)
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
    if (s_instance == nullptr ||
        info == nullptr ||
        info->ExceptionRecord == nullptr)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    if (!IsFatalException(info->ExceptionRecord->ExceptionCode))
        return EXCEPTION_CONTINUE_SEARCH;

    // VEH runs before stack unwinding and is therefore first-chance territory.
    // It records only a fixed-size exception snapshot. No STL, file I/O,
    // CrashInfo parsing, DbgHelp or GUI work is allowed here.
    // The authoritative full report is generated only by the unhandled filter.
    struct FirstChance
    {
        DWORD code;
        DWORD address;
        DWORD threadId;
    };

    static volatile FirstChance snapshot{};
    snapshot.code = info->ExceptionRecord->ExceptionCode;
    snapshot.address =
        static_cast<DWORD>(
            reinterpret_cast<uintptr_t>(
                info->ExceptionRecord->ExceptionAddress));
    snapshot.threadId = GetCurrentThreadId();
    MemoryBarrier();

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

    const bool needOpcodeCallbacks =
        s_instance != nullptr &&
        (s_instance->m_scriptOpcodeTrace ||
         s_instance->m_functionTrace ||
         s_instance->m_crashOpcodeHistory ||
         s_instance->m_commandLimit > 0 ||
         s_instance->m_timeLimitSeconds > 0);

    if (needOpcodeCallbacks)
    {
        const struct
        {
            CLEO_CallbackId id;
            uintptr_t fn;
        } opcodeCallbacks[] =
        {
            { CLEO_CB_SCRIPT_OPCODE_PROCESS_BEFORE, reinterpret_cast<uintptr_t>(&DebugUtils::OnScriptOpcodeBefore) },
            { CLEO_CB_SCRIPT_OPCODE_PROCESS_AFTER, reinterpret_cast<uintptr_t>(&DebugUtils::OnScriptOpcodeAfter) }
        };

        for (const auto& callback : opcodeCallbacks)
        {
            if (!CLEO_RegisterCallback(callback.id, callback.fn))
                s_instance->WriteCore("[callback] register failed: %s", CallbackName(callback.id));
            else
                s_instance->WriteCore("[callback] registered: %s", CallbackName(callback.id));
        }

        s_instance->WriteCore(
            "[callback] opcode observer=enabled trace=%d history=%d guard=%d",
            (s_instance->m_scriptOpcodeTrace || s_instance->m_functionTrace) ? 1 : 0,
            s_instance->m_crashOpcodeHistory ? 1 : 0,
            (s_instance->m_commandLimit > 0 || s_instance->m_timeLimitSeconds > 0) ? 1 : 0
        );
    }
    else if (s_instance != nullptr)
    {
        s_instance->WriteCore("[callback] opcode observer=crash-only (DebugUtils callback bypassed)");
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
    s_instance->m_seenScripts.clear();
    s_instance->m_lastScriptMessage.clear();
    s_instance->m_lastScriptRepeatCount = 0;
    s_instance->WriteScript("//////////////////////// script execution ////////////////////////");
    if (s_instance->m_functionTrace)
        s_instance->WriteScript("//////////////////////// function call check (0AB1 / 0AB2) ////////////////////////");
    if (s_instance->m_scriptOpcodeTrace)
        s_instance->WriteScript("//////////////////////// opcode check ////////////////////////");
    if (s_instance->m_memoryLogEnabled)
        s_instance->WriteCoreMemorySummary();
}

void __stdcall DebugUtils::OnGameEnd()
{
    if (!s_instance) return;
    s_instance->WriteCore("//////////////////////// game end ////////////////////////");
    if (s_instance->m_memoryLogEnabled)
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

    const bool timingGuardEnabled =
        s_instance->m_commandLimit > 0 ||
        s_instance->m_timeLimitSeconds > 0;

    if (timingGuardEnabled)
    {
        s_instance->m_currentScriptPtr = reinterpret_cast<uintptr_t>(thread);
        s_instance->m_currentScriptStartTick = GetTickCount();
        s_instance->m_currentScriptCommands = 0;
    }

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

    if (s_instance->m_scriptOpcodeTrace ||
        s_instance->m_functionTrace ||
        s_instance->m_crashOpcodeHistory)
    {
        s_instance->RecordOpcode(thread, opcode, 0xFFFFFFFF);
    }

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

    if (s_instance->m_scriptOpcodeTrace ||
        s_instance->m_functionTrace ||
        s_instance->m_crashOpcodeHistory)
    {
        s_instance->RecordOpcode(thread, opcode, static_cast<DWORD>(result));
    }

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
