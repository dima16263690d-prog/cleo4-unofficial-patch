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

namespace
{
    // The vectored handler runs in first-chance territory, before stack
    // unwinding, where no STL, file I/O or GUI work is permitted. It records
    // only this fixed-size snapshot. The unhandled filter later reports it so
    // that a crash preceded by a handled first-chance fault still shows both.
    struct FirstChanceSnapshot
    {
        DWORD code;
        DWORD address;
        DWORD threadId;
    };

    static volatile FirstChanceSnapshot g_firstChance{};

    struct CrashDialogData
    {
        std::wstring title;
        std::wstring details;
        std::wstring logPath;
        DWORD exitCode = 1;
        bool languageRussian = false;
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
        {
            data = reinterpret_cast<CrashDialogData*>(lParam);
            SetWindowLongPtrW(hwnd, DWLP_USER, lParam);

            const bool russian =
                data != nullptr && data->languageRussian;

            SetWindowTextW(
                hwnd,
                russian
                    ? L"CLEO DebugUtils - \u041a\u0440\u0430\u0448 GTA SA"
                    : L"CLEO DebugUtils - GTA SA crash"
            );
            SetDlgItemTextW(hwnd, IDC_CRASH_DETAILS, data != nullptr ? data->details.c_str() : L"");
            PrepareCrashWindow(hwnd);

            SetDlgItemTextW(
                hwnd,
                IDC_CRASH_COPY,
                russian ? L"\u041a\u043e\u043f\u0438\u0440\u043e\u0432\u0430\u0442\u044c" : L"Copy"
            );
            SetDlgItemTextW(
                hwnd,
                IDC_CRASH_OPEN_LOG,
                russian ? L"\u041e\u0442\u043a\u0440\u044b\u0442\u044c \u043b\u043e\u0433" : L"Open log"
            );
            SetDlgItemTextW(
                hwnd,
                IDC_CRASH_EXIT,
                russian ? L"\u0417\u0430\u0432\u0435\u0440\u0448\u0438\u0442\u044c \u0438\u0433\u0440\u0443" : L"Exit game"
            );
            return TRUE;
        }

        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
            case IDC_CRASH_COPY:
                if (data != nullptr)
                {
                    const bool russian = data->languageRussian;

                    const std::wstring copied =
                        data->details +
                        (russian ? L"\r\n\u041b\u043e\u0433: " : L"\r\nLog: ") +
                        data->logPath;

                    if (CopyCrashTextToClipboard(hwnd, copied))
                    {
                        SetDlgItemTextW(
                            hwnd,
                            IDC_CRASH_COPY,
                            data->languageRussian
                                ? L"\u0421\u043a\u043e\u043f\u0438\u0440\u043e\u0432\u0430\u043d\u043e"
                                : L"Copied"
                        );
                    }
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
    const std::vector<std::string>& notes,
    const std::string& lastScript,
    DWORD lastOpcode,
    const std::vector<DWORD>& backtrace
)
{
    CrashDialogData data{};

    const bool russian = m_languageRussian;

    const char* intro =
        russian
            ? "\xD0\x9E\xD0\xB1\xD0\xBD\xD0\xB0\xD1\x80\xD1\x83\xD0\xB6\xD0\xB5\xD0\xBD\x20\xD0\xBA\xD1\x80\xD0\xB8\xD1\x82\xD0\xB8\xD1\x87\xD0\xB5\xD1\x81\xD0\xBA\xD0\xB8\xD0\xB9\x20\xD0\xBA\xD1\x80\xD0\xB0\xD1\x88\x20\x47\x54\x41\x20\x53\x41\x20\xD0\xBE\xD1\x82\x20\x44\x65\x62\x75\x67\x55\x74\x69\x6C\x73\x2E"
            : "Critical GTA SA crash detected by DebugUtils.";

    const char* labelCrash = russian ? "\xD0\x9A\xD1\x80\xD0\xB0\xD1\x88" : "Crash";
    const char* labelException = russian ? "\xD0\x98\xD1\x81\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB5" : "Exception";
    const char* labelAddress = russian ? "\xD0\x90\xD0\xB4\xD1\x80\xD0\xB5\xD1\x81" : "Address";
    const char* labelModule = russian ? "\xD0\x9C\xD0\xBE\xD0\xB4\xD1\x83\xD0\xBB\xD1\x8C" : "Module";
    const char* labelRva = "RVA";
    const char* labelCrashInfo = "CrashInfo";
    const char* labelLastScript = russian ? "\xD0\x9F\xD0\xBE\xD1\x81\xD0\xBB\xD0\xB5\xD0\xB4\xD0\xBD\xD0\xB8\xD0\xB9\x20\xD1\x81\xD0\xBA\xD1\x80\xD0\xB8\xD0\xBF\xD1\x82" : "Last script";
    const char* labelLastOpcode = russian ? "\xD0\x9F\xD0\xBE\xD1\x81\xD0\xBB\xD0\xB5\xD0\xB4\xD0\xBD\xD0\xB8\xD0\xB9\x20\x6F\x70\x63\x6F\x64\x65" : "Last opcode";

    char text[8192] = {};
    {
        const DWORD dialogOpcode =
            lastOpcode == 0xFFFFFFFF ? 0xFFFF : (lastOpcode & 0x7FFF);
        const char* dialogOpcodeName = OpcodeName(dialogOpcode);

        sprintf_s(
            text, sizeof(text),
            "%s\r\n\r\n"
            "%s: %s\r\n"
            "%s: 0x%08X (%s)\r\n"
            "%s: 0x%08X\r\n"
            "%s: %s\r\n"
            "%s: 0x%08X\r\n"
            "%s: %s\r\n\r\n"
            "%s: %s\r\n"
            "%s: 0x%04X (%s)\r\n",
            intro,
            labelCrash,
            crashName ? crashName : (russian ? "\xD0\x9D\xD0\xB5\xD0\xB8\xD0\xB7\xD0\xB2\xD0\xB5\xD1\x81\xD1\x82\xD0\xBD\xD1\x8B\xD0\xB9" : "Unknown"),
            labelException,
            exceptionCode,
            exceptionType ? exceptionType : "UNKNOWN",
            labelAddress,
            faultAddress,
            labelModule,
            faultModule.c_str(),
            labelRva,
            faultRva,
            labelCrashInfo,
            confidence ? confidence : "none",
            labelLastScript,
            lastScript.c_str(),
            labelLastOpcode,
            dialogOpcode,
            dialogOpcodeName ? dialogOpcodeName : "unknown"
        );
    }

    std::string details(text);
    if (!issue.empty())
        details += std::string("\r\n") +
            (russian ? "\xD0\x9F\xD1\x80\xD0\xBE\xD0\xB1\xD0\xBB\xD0\xB5\xD0\xBC\xD0\xB0\x3A\x20" : "Issue: ") + issue;
    if (!about.empty())
        details += std::string("\r\n") +
            (russian ? "\xD0\x9E\xD0\xBF\xD0\xB8\xD1\x81\xD0\xB0\xD0\xBD\xD0\xB8\xD0\xB5\x3A\x20" : "About: ") + about;
    if (!solution.empty())
        details += std::string("\r\n") +
            (russian ? "\xD0\xA0\xD0\xB5\xD1\x88\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB5\x3A\x20" : "Solution: ") + solution;
    for (const auto& note : notes)
        details += std::string("\r\n") +
            (russian ? "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xBC\xD0\xB5\xD1\x87\xD0\xB0\xD0\xBD\xD0\xB8\xD0\xB5\x3A\x20" : "Note: ") + note;

    details += std::string("\r\n\r\n") +
        (russian ? "\xD0\xA1\xD1\x82\xD0\xB5\xD0\xBA\x20\xD0\xB2\xD1\x8B\xD0\xB7\xD0\xBE\xD0\xB2\xD0\xBE\xD0\xB2\x3A" : "Backtrace:");
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
    data.languageRussian = m_languageRussian;

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

    std::string stackWalkDiagnostics =
        m_crashBacktraceEnabled
            ? "status=CALLING_BUILD_STACK_WALK"
            : "status=NOT_REQUESTED";

    bool heuristicBacktrace = false;
    const std::vector<DWORD> backtraceAddresses =
        m_crashBacktraceEnabled
            ? BuildStackWalk(
                info,
                m_crashMaxFrames,
                &stackWalkDiagnostics,
                &heuristicBacktrace)
            : std::vector<DWORD>();

    // Heuristic frames are useful for display/forensics, but must never
    // influence verified CrashInfo matching or the stable crash fingerprint.
    const std::vector<DWORD> matcherBacktrace =
        heuristicBacktrace ? std::vector<DWORD>() : backtraceAddresses;

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
            matcherBacktrace
        );

    const bool nullEip = context->Eip == 0;

    const CrashInfoEntry* match =
        FindCrashInfo(
            faultAddress,
            faultModule,
            matcherBacktrace,
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
    if (nullEip)
        confidence = "NULL-EIP";
    else if (match == nullptr)
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

    std::string crashDisplayName;
    if (nullEip)
    {
        crashDisplayName = "Null Instruction Execution";
    }
    else if (match != nullptr && !match->name.empty())
    {
        crashDisplayName = match->name;
    }
    else if (faultModule != "<unknown>" && faultRva != 0)
    {
        char nameBuffer[128] = {};
        sprintf_s(
            nameBuffer,
            sizeof(nameBuffer),
            "%s + 0x%08X",
            faultModule.c_str(),
            faultRva
        );
        crashDisplayName = nameBuffer;
    }
    else if (faultAddress != 0)
    {
        char nameBuffer[128] = {};
        sprintf_s(
            nameBuffer,
            sizeof(nameBuffer),
            "Unknown Crash @ 0x%08X",
            faultAddress
        );
        crashDisplayName = nameBuffer;
    }
    else
    {
        crashDisplayName = "Unknown / Unclassified Crash";
    }

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
        "debugutils_build=%s\r\n"
        "time=%04u-%02u-%02u %02u:%02u:%02u.%03u\r\n"
        "fingerprint=%s",
        kDebugUtilsBuildId,
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

    if (exceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        record->NumberParameters >= 2)
    {
        appendf(
            "access=%s\r\ntarget=0x%08X",
            accessName.c_str(),
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
                "working_set=%I64u (%s)\r\n"
                "peak_working_set=%I64u (%s)\r\n"
                "private=%I64u (%s)\r\n"
                "pagefile=%I64u (%s)",
                static_cast<unsigned __int64>(pmc.WorkingSetSize),
                MemoryBytes(static_cast<uint64_t>(pmc.WorkingSetSize)).c_str(),
                static_cast<unsigned __int64>(pmc.PeakWorkingSetSize),
                MemoryBytes(static_cast<uint64_t>(pmc.PeakWorkingSetSize)).c_str(),
                static_cast<unsigned __int64>(pmc.PrivateUsage),
                MemoryBytes(static_cast<uint64_t>(pmc.PrivateUsage)).c_str(),
                static_cast<unsigned __int64>(pmc.PagefileUsage),
                MemoryBytes(static_cast<uint64_t>(pmc.PagefileUsage)).c_str()
            );

            // 32-bit GTA: private usage near 2 GB means the address space is
            // nearly exhausted, which is itself a plausible crash cause.
            const uint64_t kAddressSpaceBytes = 2ull * 1024ull * 1024ull * 1024ull;
            appendf(
                "address_space_nearly_exhausted=%d",
                static_cast<uint64_t>(pmc.PrivateUsage) >=
                    (kAddressSpaceBytes * 80ull / 100ull) ? 1 : 0
            );

            MEMORYSTATUSEX systemMemory{};
            systemMemory.dwLength = sizeof(systemMemory);
            if (GlobalMemoryStatusEx(&systemMemory))
            {
                appendf(
                    "system_load=%u%%\r\n"
                    "system_physical_available=%s\r\n"
                    "system_virtual_available=%s",
                    systemMemory.dwMemoryLoad,
                    MemoryBytes(systemMemory.ullAvailPhys).c_str(),
                    MemoryBytes(systemMemory.ullAvailVirtual).c_str()
                );
            }
        }
        else
        {
            output += "state=<unavailable>\r\n";
        }
    }

    output += "\r\n[STACK WALK]\r\n";
    appendf("%s", stackWalkDiagnostics.empty()
        ? "status=NO_DIAGNOSTIC"
        : stackWalkDiagnostics.c_str());
    if (!backtraceAddresses.empty())
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

    const std::vector<DWORD> rawStackCandidates =
        BuildRawStackCandidates(context->Esp, 0x400u, 16u);

    output += "\r\n[STACK RAW CANDIDATES]\r\n";
    if (rawStackCandidates.empty())
    {
        output += "status=NONE\r\n";
    }
    else
    {
        for (size_t i = 0; i < rawStackCandidates.size(); ++i)
        {
            const DWORD address = rawStackCandidates[i];
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
                "candidate=%02u address=0x%08X module=%s rva=0x%08X",
                static_cast<unsigned>(i),
                address,
                module.c_str(),
                rva
            );
        }
    }

    output += "\r\n[CLEO CONTEXT]\r\n";
    {
        const DWORD normalizedOpcode =
            lastOpcode == 0xFFFFFFFFu ? 0xFFFFu : lastOpcode & 0x7FFFu;
        const char* opcodeName = OpcodeName(normalizedOpcode);

        appendf(
            "snapshot_valid=%d\r\n"
            "script=%.8s\r\n"
            "script_ptr=%p\r\n"
            "last_opcode=0x%04X\r\n"
            "last_opcode_name=%s\r\n"
            "opcode_offset=0x%08X\r\n"
            "opcode_result=%d\r\n"
            "game_tick=%u",
            crashSnapshotValid ? 1 : 0,
            lastScript.c_str(),
            reinterpret_cast<void*>(lastScriptPtr),
            normalizedOpcode,
            opcodeName ? opcodeName : "unknown",
            lastOpcodeOffset,
            static_cast<int>(lastOpcodeResult),
            crashSnapshotValid ? crashSnapshot.gameTick : 0
        );
    }

    output += "\r\n[FIRST CHANCE]\r\n";
    {
        MemoryBarrier();
        const DWORD firstChanceCode = g_firstChance.code;

        if (firstChanceCode == 0)
        {
            output += "captured=0\r\n";
        }
        else
        {
            const DWORD firstChanceAddress = g_firstChance.address;
            const DWORD firstChanceThread = g_firstChance.threadId;

            appendf(
                "captured=1\r\n"
                "code=0x%08X (%s)\r\n"
                "address=0x%08X\r\n"
                "module=%s\r\n"
                "thread_id=%u",
                firstChanceCode,
                ExceptionName(firstChanceCode),
                firstChanceAddress,
                ModuleNameForAddress(firstChanceAddress).c_str(),
                firstChanceThread
            );
        }
    }

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
            !match->name.empty()
                ? match->name.c_str()
                : crashDisplayName.c_str(),
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
        if (!match->type.empty())
            appendf("type=%s", match->type.c_str());
        if (!match->description.empty())
            appendf("description=%s", match->description.c_str());
        for (const auto& note : match->notes)
            appendf("note=%s", note.c_str());
    }
    else
    {
        appendf(
            "database=NONE\r\n"
            "name=%s\r\n"
            "confidence=%s",
            crashDisplayName.c_str(),
            confidence
        );
    }

    const bool faultInCleo =
        _stricmp(faultModule.c_str(), "CLEO.asi") == 0;
    const bool databaseMentionsCleo =
        match != nullptr &&
        (match->issue.find("CLEO") != std::string::npos ||
         match->about.find("CLEO") != std::string::npos ||
         match->solution.find("CLEO") != std::string::npos);

    output += "\r\n[DIAGNOSIS]\r\n";

    if (nullEip)
    {
        output +=
            "execution_control=INVALID\r\n"
            "execution_target=0x00000000\r\n"
            "probable_fault=NULL-EIP instruction execution\r\n";
    }

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

    // The opcode ring buffer is filled only when OpcodeHistory is enabled. It
    // is dumped here (not during opcode execution) so the crash path owns the
    // only formatting cost. try_lock keeps this safe inside the exception
    // handler: if another thread held the lock during the fault, we skip the
    // dump instead of deadlocking the crash report.
    if (m_crashOpcodeHistory &&
        m_opcodeHistoryMutex.try_lock())
    {
        output += "\r\n[OPCODE HISTORY]\r\n";

        const size_t count = m_opcodeHistoryCount;
        const size_t next = m_opcodeHistoryNext;

        if (count == 0)
        {
            output += "count=0\r\n";
        }
        else
        {
            appendf("count=%u", static_cast<unsigned>(count));

            const size_t start = (count == kOpcodeHistorySize) ? next : 0;
            for (size_t i = 0; i < count; ++i)
            {
                const OpcodeHistoryEntry& e =
                    m_opcodeHistory[(start + i) % kOpcodeHistorySize];

                const DWORD entryOpcode = e.opcode & 0x7FFFu;
                const char* entryName = OpcodeName(entryOpcode);

                appendf(
                    "#%03u script=%.8s opcode=0x%04X name=%s result=%u off=0x%08X",
                    static_cast<unsigned>(i + 1),
                    e.scriptName,
                    entryOpcode,
                    entryName ? entryName : "unknown",
                    e.result,
                    e.offset
                );
            }
        }

        m_opcodeHistoryMutex.unlock();
    }

    output += "\r\n[END]\r\n";
    output += "============================================================\r\n";

    WinWriteTextFile(CrashLogPath(), output);

    if (m_crashWindowEnabled)
    {
        ShowCrashDialog(
            crashDisplayName.c_str(),
            exceptionCode,
            ExceptionName(exceptionCode),
            faultAddress,
            faultModule,
            faultRva,
            confidence,
            match != nullptr ? match->issue : std::string(),
            match != nullptr ? match->about : std::string(),
            match != nullptr ? match->solution : std::string(),
            match != nullptr ? match->notes : std::vector<std::string>(),
            lastScript,
            lastOpcode,
            backtraceAddresses
        );
    }
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
    // The authoritative full report is generated only by the unhandled filter,
    // which also reports this snapshot so a handled first-chance fault that
    // preceded the fatal one is not silently lost.
    g_firstChance.code = info->ExceptionRecord->ExceptionCode;
    g_firstChance.address =
        static_cast<DWORD>(
            reinterpret_cast<uintptr_t>(
                info->ExceptionRecord->ExceptionAddress));
    g_firstChance.threadId = GetCurrentThreadId();
    MemoryBarrier();

    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI DebugUtils::UnhandledExceptionFilter(PEXCEPTION_POINTERS info)
{
    if (s_instance == nullptr)
        return EXCEPTION_CONTINUE_SEARCH;

    return s_instance->HandleException(info);
}
