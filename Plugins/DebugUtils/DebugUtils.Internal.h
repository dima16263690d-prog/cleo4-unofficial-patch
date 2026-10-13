#pragma once

#include "DebugUtils.h"
#include "DebugUtils.OpcodeNames.h"
#include <windows.h>
#include <psapi.h>
#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <list>
#include <regex>
#include <string>
#include <vector>

namespace
{
    // Visible in both the startup log and crash report so the installed
    // DebugUtils.cleo can be distinguished from stale copies.
    static constexpr char kDebugUtilsBuildId[] =
        "test-xx02-stackwalk-esp-scan-2026-10-06";

    // Resolve a 15-bit opcode to its canonical SCRLog/Sanny Builder name.
    // The table is sorted by opcode, so a binary search keeps this cheap even
    // when called for every entry of the crash opcode ring buffer.
    static const char* OpcodeName(DWORD opcode)
    {
        using namespace DebugUtilsOpcodeNames;

        const unsigned short key = static_cast<unsigned short>(opcode & 0x7FFFu);
        unsigned lo = 0;
        unsigned hi = kCount;

        while (lo < hi)
        {
            const unsigned mid = lo + (hi - lo) / 2;
            if (kNames[mid].opcode < key)
                lo = mid + 1;
            else
                hi = mid;
        }

        if (lo < kCount && kNames[lo].opcode == key)
            return kNames[lo].name;

        return nullptr;
    }

    // Render a byte count as a short human readable value (e.g. "23.4 MB").
    // Used by the periodic memory summary so the log stays readable.
    static std::string MemoryBytes(uint64_t bytes)
    {
        char buffer[32] = {};

        constexpr double kKiB = 1024.0;
        constexpr double kMiB = kKiB * 1024.0;
        constexpr double kGiB = kMiB * 1024.0;

        const double value = static_cast<double>(bytes);
        if (value >= kGiB)
            sprintf_s(buffer, sizeof(buffer), "%.2f GB", value / kGiB);
        else if (value >= kMiB)
            sprintf_s(buffer, sizeof(buffer), "%.1f MB", value / kMiB);
        else if (value >= kKiB)
            sprintf_s(buffer, sizeof(buffer), "%.1f KB", value / kKiB);
        else
            sprintf_s(buffer, sizeof(buffer), "%llu B", static_cast<unsigned long long>(bytes));

        return buffer;
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

        // CCleoScriptState
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
        DWORD savedNodeId;

        // CCustomScript relation state
        DebugCustomScriptLayout* parentThread;
        int childLabel;
        std::list<DebugCustomScriptLayout*> childThreads;

        // CScriptResources
        BYTE UseTextCommands;
        int NumDraws;
        int NumTexts;
        std::list<void*> script_textures;
        std::vector<BYTE> script_draws;
        std::vector<BYTE> script_texts;
    };

    static_assert(sizeof(size_t) == 4, "CLEO4 DebugUtils is Win32; size_t must be 32-bit");
    static_assert(sizeof(CScriptThread) == 0xE0, "Unexpected CScriptThread layout");
    static_assert(offsetof(DebugCustomScriptLayout, CodeSize) == 0xFC, "Unexpected CCustomScript CodeSize offset");
#if defined(_DEBUG)
    static_assert(offsetof(DebugCustomScriptLayout, savedNodeId) == 0x138, "Unexpected CCustomScript savedNodeId offset in Debug");
#else
    static_assert(offsetof(DebugCustomScriptLayout, savedNodeId) == 0x130, "Unexpected CCustomScript savedNodeId offset in Release");
#endif

    static const DebugCustomScriptLayout* GetDebugCustomScript(const CScriptThread* thread)
    {
        return reinterpret_cast<const DebugCustomScriptLayout*>(thread);
    }

    // A native GTA thread and a CLEO custom thread are the same size as the
    // CRunningScript base (0xE0), so the CCustomScript storage offsets
    // (CodeSize at 0xFC, relation fields, std::string members) are only valid
    // for threads that actually own that extended layout. CCustomScript marks
    // itself with IsCustom() at byte +0xDF. baseIp is not a discriminator:
    // native SCM threads keep a non-null main.scm pointer there too.
    static bool IsCustomScriptThread(const CScriptThread* thread)
    {
        if (thread == nullptr)
            return false;

        return *reinterpret_cast<const volatile BYTE*>(
            reinterpret_cast<const BYTE*>(thread) + 0xDF) != 0;
    }

    // Copies the 8-byte GTA thread name into a 9-byte, always NUL-terminated
    // buffer. threadName is not guaranteed to be NUL-terminated.
    static void SafeThreadName(const CScriptThread* thread, char (&out)[9])
    {
        out[8] = '\0';

        if (thread == nullptr)
        {
            memcpy(out, "none", 5);
            return;
        }

        memcpy(out, thread->threadName, 8);
    }

    constexpr DWORD kGtaSa10ActiveScripts = 0x00A8B42C;

    // CStreaming statics in GTA SA 1.0 US (addresses per plugin-sdk CStreaming.cpp).
    // ms_memoryAvailable is the budget the engine tries to stay under; ms_memoryUsed
    // is how much it currently holds. Both are plain unsigned int globals.
    constexpr DWORD kGtaSa10StreamingMemoryAvailable = 0x008A5A80;
    constexpr DWORD kGtaSa10StreamingMemoryUsed = 0x008E4CB4;
    constexpr DWORD kGtaSa10StreamingModelsRequested = 0x008E4CB8;

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

    // SCRLog writes opcodes as [0001], [0B10] or with stray spaces like
    // [ 0D0B]. The CrashInfo database uses that notation for "Last command"
    // groups, so scan it in addition to plain 0x addresses.
    void ExtractBracketedOpcodes(
        const std::string& line,
        size_t start,
        std::vector<DWORD>& out
    )
    {
        size_t pos = start;

        while ((pos = line.find('[', pos)) != std::string::npos)
        {
            size_t cursor = pos + 1;
            while (cursor < line.size() &&
                   (line[cursor] == ' ' || line[cursor] == '\t'))
            {
                ++cursor;
            }

            const size_t hexStart = cursor;
            while (cursor < line.size() &&
                   isxdigit(static_cast<unsigned char>(line[cursor])))
            {
                ++cursor;
            }

            if (cursor == hexStart)
            {
                pos = cursor;
                continue;
            }

            // The closing bracket may be separated from the digits by spaces.
            size_t after = cursor;
            while (after < line.size() &&
                   (line[after] == ' ' || line[after] == '\t'))
            {
                ++after;
            }

            if (after >= line.size() || line[after] != ']')
            {
                pos = cursor;
                continue;
            }

            const std::string token = line.substr(hexStart, cursor - hexStart);
            if (!token.empty() && token.size() <= 8)
            {
                const unsigned long value = strtoul(token.c_str(), nullptr, 16);
                out.push_back(static_cast<DWORD>(value & 0x7FFFu));
            }

            pos = after + 1;
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
