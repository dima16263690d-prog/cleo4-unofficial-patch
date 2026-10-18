#include "DebugUtils.h"
#include "DebugUtils.Internal.h"
#include <windows.h>
#include <psapi.h>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <intrin.h>
#include <map>
#include <string>
#include <vector>

// RenderWare file-open and texture-read watches. Both are *data-only* patches -
// a function pointer inside RenderWare's own tables is swapped, no instruction is
// overwritten - so they carry none of the risk of a code detour. Each fires while
// the game thread is inside the mod that asked for the resource, so the requester
// chain captured there names the culprit. Opt-in via [DebugUtils.Errors].

namespace
{
    // Case-sensitive glob for already-lowercased input: '*' matches any run of
    // characters (including none), '?' matches exactly one. Used for the
    // FileWatchNames masks such as "*.txd" or "lunar.*".
    bool GlobMatchImpl(const char* text, const char* pattern)
    {
        while (*pattern != '\0')
        {
            if (*pattern == '*')
            {
                // Collapse consecutive stars, then try to match the rest of the
                // pattern at every remaining position of the text.
                while (*pattern == '*')
                    ++pattern;
                if (*pattern == '\0')
                    return true;
                while (*text != '\0')
                {
                    if (GlobMatchImpl(text, pattern))
                        return true;
                    ++text;
                }
                return GlobMatchImpl(text, pattern);
            }

            if (*text == '\0')
                return false;

            if (*pattern != '?' && *pattern != *text)
                return false;

            ++text;
            ++pattern;
        }

        return *text == '\0';
    }

    bool MatchGlob(const std::string& text, const std::string& pattern)
    {
        return GlobMatchImpl(text.c_str(), pattern.c_str());
    }

    // Absolute path the game actually looks in for a (possibly relative) name.
    // A relative name is resolved against the process working directory, which
    // for GTA SA is the folder holding gta_sa.exe - so this shows exactly where
    // a missing file was expected.
    std::string ResolveReportPath(const char* path)
    {
        if (path == nullptr)
            return {};

        char full[MAX_PATH] = {};
        const DWORD length = GetFullPathNameA(path, MAX_PATH, full, nullptr);
        if (length == 0 || length >= MAX_PATH)
            return std::string(path);

        return std::string(full);
    }

}

// ---------------------------------------------------------------------------
// RenderWare file-open watch.
//
// E_RW_BADOPEN is raised inside RenderWare's stream-open-by-name path, which
// reaches the OS through the file-function table returned by
// RwOsGetFileInterface() (a RwFileFunctions* living in RenderWare's globals).
// Replacing that table's rwfopen pointer with our own wrapper is a *data-only*
// patch: no instruction is overwritten and no trampoline is needed, so it
// carries none of the risk of a code detour. The wrapper calls the original and
// records the name whenever it returns null, which is exactly the condition that
// makes RenderWare raise E_RW_BADOPEN. This turns "some texture failed" into the
// exact path, e.g. "models\cutscene\foo.txd".
// ---------------------------------------------------------------------------
namespace
{
    // plugin-sdk: RwFileFunctions* RwOsGetFileInterface(void) at 0x00804130.
    constexpr DWORD kGtaSa10RwOsGetFileInterface = 0x00804130;

    // Prefix of RwFileFunctions that we need. The real struct is
    // { rwfexist, rwfopen, rwfclose, rwfread, ... }; only the first two matter.
    struct RwFileFunctionsHead
    {
        void* rwfexist; // RwBool (*)(const RwChar* name)
        void* rwfopen;  // void*  (*)(const RwChar* name, const RwChar* mode)
    };

    typedef RwFileFunctionsHead* (__cdecl *RwOsGetFileInterfaceFn)(void);
    typedef void* (__cdecl *RwFopenFn)(const char* name, const char* mode);

    // Assigned during InstallFileOpenWatch, read on every intercepted open.
    RwFopenFn g_rwFopenOriginal = nullptr;
}
void* __cdecl DebugUtils::FileOpenWatchFopen(const char* name, const char* mode)
{
    RwFopenFn original = g_rwFopenOriginal;
    if (original == nullptr)
        return nullptr;

    void* handle = original(name, mode);

    if (name != nullptr && name[0] != '\0' && s_instance != nullptr)
    {
        // A null handle is a failed open, which is what RenderWare turns into
        // E_RW_BADOPEN. Anything else opened fine and is not interesting.
        if (handle == nullptr)
            s_instance->RecordFileOpenFailure(name);

        // Files named on the watchlist are recorded whether or not the open
        // succeeded, so "who loads particle.txd" can be answered even when the
        // file exists.
        s_instance->RecordWatchedOpen(name, handle == nullptr);
    }

    return handle;
}

bool DebugUtils::IsWatchedFileName(const char* path) const
{
    if (path == nullptr || m_fileWatchNames.empty())
        return false;

    std::string lower(path);
    for (char& c : lower)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));

    // Each entry may be a plain substring ("particle.txd"), a glob mask
    // ("*.txd", "lunar.*"), or a bare name ("lunar"). A match on any entry
    // watches the file.
    for (const auto& needle : m_fileWatchNames)
    {
        if (needle.empty())
            continue;

        // Glob: '*' matches any run of characters, '?' matches one. A pattern
        // without a wildcard behaves exactly like the old substring test.
        const bool hasWildcard =
            needle.find('*') != std::string::npos || needle.find('?') != std::string::npos;

        if (!hasWildcard)
        {
            if (lower.find(needle) != std::string::npos)
                return true;
            continue;
        }

        // Anchor the glob to the file name (after the last path separator), so
        // "*.txd" means "any .txd file", not "any path ending in .txd".
        std::string fileName = lower;
        const size_t slash = fileName.find_last_of("\\/");
        if (slash != std::string::npos)
            fileName = fileName.substr(slash + 1);

        if (MatchGlob(fileName, needle) || MatchGlob(lower, needle))
            return true;
    }

    return false;
}

void DebugUtils::RecordWatchedOpen(const char* path, bool failed)
{
    if (!IsWatchedFileName(path))
        return;

    const std::string key(path);
    const DWORD now = GetTickCount();
    const DWORD uptimeSec = (now - m_memorySessionStartTick) / 1000u;
    const std::string script = CurrentScriptName();
    const std::string resolved = ResolveReportPath(path);

    std::lock_guard<std::mutex> lock(m_fileWatchRecordMutex);

    auto it = m_fileWatchRecords.find(key);
    if (it == m_fileWatchRecords.end())
    {
        FileOpenRecord record;
        record.count = 1;
        record.firstUptimeSec = uptimeSec;
        record.lastUptimeSec = uptimeSec;
        record.failed = failed;
        record.lastScript = script;
        record.resolvedPath = resolved;

        m_fileWatchRecordOrder.push_back(key);
        it = m_fileWatchRecords.emplace(key, std::move(record)).first;
    }
    else
    {
        FileOpenRecord& record = it->second;
        ++record.count;
        record.lastUptimeSec = uptimeSec;
        record.failed = failed;
        record.lastScript = script;
        record.resolvedPath = resolved;
    }

    FileOpenRecord& stored = it->second;
    if (!stored.modulesScanned)
    {
        stored.modulesScanned = true;
        if (m_errorModuleScanEnabled)
        {
            const char* via = nullptr;
            stored.modules = CaptureRequesterChain(4, &via);
            stored.chainVia = via ? via : "";
        }
    }
}

void DebugUtils::WriteWatchedOpenReport(bool forceAll)
{
    std::map<std::string, FileOpenRecord> records;
    std::vector<std::string> order;
    {
        std::lock_guard<std::mutex> lock(m_fileWatchRecordMutex);
        records = m_fileWatchRecords;
        order = m_fileWatchRecordOrder;
    }

    if (records.empty())
        return;

    // Dedup like the failed-open report: print only what changed since last time.
    std::vector<std::string> changedKeys;
    for (const auto& key : order)
    {
        auto it = records.find(key);
        if (it == records.end())
            continue;
        if (forceAll || it->second.count != it->second.lastReportedCount)
            changedKeys.push_back(key);
    }

    if (changedKeys.empty())
        return;

    WriteFrameTime("================= watched file opens =================");
    WriteFrameTime("%-3s %-46s %6s %5s %5s %-6s %-14s", "#", "path", "count", "first", "last", "result", "script");

    size_t index = 0;
    for (const auto& key : changedKeys)
    {
        auto it = records.find(key);
        if (it == records.end())
            continue;

        const FileOpenRecord& record = it->second;
        ++index;

        std::string path = key;
        if (path.size() > 46)
            path = std::string("...") + path.substr(path.size() - 43);

        WriteFrameTime(
            "%-3zu %-46s %6u %5u %5u %-6s %-14s",
            index,
            path.c_str(),
            static_cast<unsigned>(record.count),
            static_cast<unsigned>(record.firstUptimeSec),
            static_cast<unsigned>(record.lastUptimeSec),
            record.failed ? "FAILED" : "ok",
            record.lastScript.empty() ? "?" : record.lastScript.c_str()
        );

        if (!record.modules.empty())
        {
            std::string chain;
            for (const auto& module : record.modules)
            {
                if (!chain.empty())
                    chain += " <- ";
                chain += module;
            }
            WriteFrameTime("      requested by: %s", chain.c_str());
            if (!record.chainVia.empty())
                WriteFrameTime("      chain via: %s", record.chainVia.c_str());
        }

        if (!record.resolvedPath.empty())
            WriteFrameTime("      looked in: %s", record.resolvedPath.c_str());
    }

    WriteFrameTime(
        "[filewatch] watchlist: %zu path(s) matched (FileWatchNames)",
        records.size()
    );

    {
        std::lock_guard<std::mutex> lock(m_fileWatchRecordMutex);
        for (auto& entry : m_fileWatchRecords)
            entry.second.lastReportedCount = entry.second.count;
    }
}

void DebugUtils::InstallFileOpenWatch()
{
    if (m_fileWatchInstalled || !m_fileWatchEnabled)
        return;

    // The table address is for the SA 1.0 US RenderWare build; refuse anything
    // else rather than poke a wrong pointer.
    if (CLEO_GetGameVersion() != GV_US10)
    {
        WriteFrameTime(
            "[filewatch] disabled: RenderWare file interface address is SA 1.0 US only"
        );
        return;
    }

    const auto getFileInterface =
        reinterpret_cast<RwOsGetFileInterfaceFn>(kGtaSa10RwOsGetFileInterface);

    RwFileFunctionsHead* table = getFileInterface();
    if (table == nullptr || table->rwfopen == nullptr)
    {
        WriteFrameTime("[filewatch] disabled: RenderWare file interface unavailable");
        return;
    }

    // Guard against a wrong table layout: the pointer we are about to replace
    // must be executable code inside the game image. Wrapping a data pointer and
    // then calling it would crash, so refuse anything that is not executable.
    {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(table->rwfopen, &info, sizeof(info)) == 0 ||
            (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                             PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) == 0)
        {
            WriteFrameTime(
                "[filewatch] disabled: RenderWare rwfopen is not executable code"
            );
            return;
        }
    }

    // Keep the original so our wrapper (and, on unload, the game) still opens
    // files exactly as before.
    g_rwFopenOriginal = reinterpret_cast<RwFopenFn>(table->rwfopen);
    m_fileWatchOriginalFopen = table->rwfopen;
    m_fileWatchTable = table;

    DWORD oldProtect = 0;
    if (!VirtualProtect(&table->rwfopen, sizeof(void*), PAGE_READWRITE, &oldProtect))
    {
        g_rwFopenOriginal = nullptr;
        WriteFrameTime("[filewatch] disabled: could not make the table writable");
        return;
    }

    table->rwfopen = reinterpret_cast<void*>(&DebugUtils::FileOpenWatchFopen);
    VirtualProtect(&table->rwfopen, sizeof(void*), oldProtect, &oldProtect);

    m_fileWatchInstalled = true;
    WriteFrameTime(
        "[filewatch] enabled: RenderWare file opens are captured; failures are "
        "reported with the exact path"
    );
}

void DebugUtils::RemoveFileOpenWatch()
{
    if (!m_fileWatchInstalled || m_fileWatchTable == nullptr)
        return;

    auto* table = reinterpret_cast<RwFileFunctionsHead*>(m_fileWatchTable);

    DWORD oldProtect = 0;
    if (VirtualProtect(&table->rwfopen, sizeof(void*), PAGE_READWRITE, &oldProtect))
    {
        table->rwfopen = m_fileWatchOriginalFopen;
        VirtualProtect(&table->rwfopen, sizeof(void*), oldProtect, &oldProtect);
    }

    m_fileWatchInstalled = false;
    m_fileWatchTable = nullptr;
    m_fileWatchOriginalFopen = nullptr;
    g_rwFopenOriginal = nullptr;
}

void DebugUtils::RecordFileOpenFailure(const char* path)
{
    if (!m_fileWatchFailuresEnabled)
        return;

    if (path == nullptr || path[0] == '\0')
        return;

    const std::string key(path);
    const DWORD now = GetTickCount();
    const DWORD uptimeSec = (now - m_memorySessionStartTick) / 1000u;
    const std::string script = CurrentScriptName();
    const std::string resolved = ResolveReportPath(path);

    std::lock_guard<std::mutex> lock(m_fileOpenMutex);

    auto it = m_fileOpenFailures.find(key);
    if (it == m_fileOpenFailures.end())
    {
        FileOpenFailure failure;
        failure.count = 1;
        failure.firstUptimeSec = uptimeSec;
        failure.lastUptimeSec = uptimeSec;
        failure.lastScript = script;
        failure.resolvedPath = resolved;

        m_fileOpenOrder.push_back(key);
        it = m_fileOpenFailures.emplace(key, std::move(failure)).first;
    }
    else
    {
        FileOpenFailure& failure = it->second;
        ++failure.count;
        failure.lastUptimeSec = uptimeSec;
        failure.lastScript = script;
        failure.resolvedPath = resolved;
    }

    // Walk the stack once per distinct path, at the moment of the failed open:
    // the caller chain is the mod/plugin that asked RenderWare for the file. Done
    // under the lock only on first sight so the scan cost is paid once per path.
    FileOpenFailure& stored = it->second;
    if (!stored.modulesScanned)
    {
        stored.modulesScanned = true;
        if (m_errorModuleScanEnabled)
        {
            const char* via = nullptr;
            stored.modules = CaptureRequesterChain(4, &via);
            stored.chainVia = via ? via : "";
        }
    }
}

void DebugUtils::WriteFileOpenReport(bool forceAll)
{
    std::map<std::string, FileOpenFailure> failures;
    std::vector<std::string> order;
    {
        std::lock_guard<std::mutex> lock(m_fileOpenMutex);
        failures = m_fileOpenFailures;
        order = m_fileOpenOrder;
    }

    if (failures.empty())
        return;

    // Dedup like the error report: print only what changed since last time (a new
    // path, or one whose count grew), never the whole table again.
    std::vector<std::string> changedKeys;
    for (const auto& key : order)
    {
        auto it = failures.find(key);
        if (it == failures.end())
            continue;
        if (forceAll || it->second.count != it->second.lastReportedCount)
            changedKeys.push_back(key);
    }

    if (changedKeys.empty())
        return;

    WriteFrameTime("================= failed file opens =================");
    WriteFrameTime("%-3s %-52s %8s %5s %5s %-16s", "#", "path", "count", "first", "last", "script");

    size_t index = 0;
    for (const auto& key : changedKeys)
    {
        auto it = failures.find(key);
        if (it == failures.end())
            continue;

        const FileOpenFailure& failure = it->second;
        ++index;

        std::string path = key;
        // Keep the row readable when a path is very long.
        if (path.size() > 52)
            path = std::string("...") + path.substr(path.size() - 49);
        WriteFrameTime(
            "%-3zu %-52s %8u %5u %5u %-16s",
            index,
            path.c_str(),
            static_cast<unsigned>(failure.count),
            static_cast<unsigned>(failure.firstUptimeSec),
            static_cast<unsigned>(failure.lastUptimeSec),
            failure.lastScript.empty() ? "?" : failure.lastScript.c_str()
        );

        // Name the module(s) that requested the file: this is the concrete
        // "which mod" answer (e.g. a .asi plugin), more specific than the
        // script bridge in the script column.
        if (!failure.modules.empty())
        {
            std::string chain;
            for (const auto& module : failure.modules)
            {
                if (!chain.empty())
                    chain += " <- ";
                chain += module;
            }
            WriteFrameTime("      requested by: %s", chain.c_str());
            if (!failure.chainVia.empty())
                WriteFrameTime("      chain via: %s", failure.chainVia.c_str());
        }

        // The absolute path the game looked in, so the exact folder to put the
        // file into is unambiguous.
        if (!failure.resolvedPath.empty())
            WriteFrameTime("      looked in: %s", failure.resolvedPath.c_str());

        // A bare name (no directory) is resolved against the game's working
        // directory (the GTA SA folder), which is the usual reason a file a mod
        // ships "goes missing": it was put in the wrong place.
        if (key.find('\\') == std::string::npos && key.find('/') == std::string::npos)
        {
            WriteFrameTime(
                "      hint: bare name - it is looked up in the game folder "
                "(where gta_sa.exe is); put the file there or fix the mod's path"
            );
        }
    }

    WriteFrameTime(
        "[filewatch] %zu distinct path(s) failed to open; these are the files "
        "behind E_RW_BADOPEN / E_RW_READTEXMASK",
        failures.size()
    );

    {
        std::lock_guard<std::mutex> lock(m_fileOpenMutex);
        for (auto& entry : m_fileOpenFailures)
            entry.second.lastReportedCount = entry.second.count;
    }
}

// ---------------------------------------------------------------------------
// RenderWare texture-read watch.
//
// E_RW_READTEXMASK ("Unable to read Texture %s / Mask %s") is raised by the
// *name-based* reader RwTextureRead (SA 1.0 US 0x4C7510) when its read callback
// returns NULL. The error poll in PollRenderWareError runs one frame later, when
// the raising stack is already gone, so it cannot name the mod. We wrap that read
// callback (through the public RwTextureGetReadCallBack/RwTextureSetReadCallBack,
// SA 0x7F3520/0x7F3500) and, as a secondary, the stream reader
// RwGlobals.stdFunc[26] (SA 0x4CD820, the TXD-internal path). Both run while the
// game thread is inside the mod that asked for the texture, so the requester
// chain is captured at the right moment. Both are data-only swaps (no code is
// modified), like the rwfopen watch, so they carry none of the risk of a detour.
// ---------------------------------------------------------------------------
namespace
{
    // RwEngineInstance holds a pointer to RwGlobals; stdFunc[26] sits at +0xB0.
    constexpr DWORD kGtaSa10RwEngineInstance = 0xC97B24;
    constexpr DWORD kRwStdFuncNativeTextureReadOffset = 0xB0;

    // RwTextureRead's read callback, swapped through the public API so we never
    // touch code: RwTextureCallBackRead is RwTexture* cdecl (name, maskName).
    constexpr DWORD kGtaSa10RwTextureGetReadCallBack = 0x7F3520;
    constexpr DWORD kGtaSa10RwTextureSetReadCallBack = 0x7F3500;

    // Entry of RwTextureRead (SA 1.0 US). Verified against plugin-sdk
    // (plugin_sa/game_sa/RenderWare.cpp): RwTextureRead() itself calls 0x4C7510,
    // and CVehicleModelInfo::FindTextureCB calls the same address, so this is what
    // mods and the game both hit. (An earlier guess of 0x7F3AC0 was wrong: that is
    // RwTextureGenerateMipmapName's neighbourhood, which is why the entry hook
    // refused it as "not relocatable".)
    constexpr DWORD kGtaSa10RwTextureRead = 0x4C7510;

    typedef void* (__cdecl* RwTextureCallBackReadFn)(const char* name, const char* maskName);
    typedef RwTextureCallBackReadFn(__cdecl* RwTextureGetReadCallBackFn)(void);
    typedef int(__cdecl* RwTextureSetReadCallBackFn)(RwTextureCallBackReadFn callBack);

    typedef int(__cdecl* RwNativeTextureReadFn)(void* stream, void** texture, int length);

    // Assigned during InstallTextureReadWatch, read on every intercepted call.
    RwNativeTextureReadFn g_rwTextureReadOriginal = nullptr;
    RwTextureCallBackReadFn g_rwTextureReadCbOriginal = nullptr;

    // -----------------------------------------------------------------------
    // Minimal x86 prologue length decoder, for the exact-caller entry hook.
    //
    // A 5-byte JMP detour must copy WHOLE instructions out of the target's
    // prologue into a trampoline, so we need the byte length of the first
    // instructions until we have >= 5 bytes. It only accepts instructions that
    // are safe to relocate (no relative branch, which would change meaning when
    // moved). Anything it does not fully understand returns 0, and the caller
    // then refuses to hook - falling back to the labelled stack scan. Being
    // conservative here is the whole safety story: a wrong length corrupts the
    // game's code, a refused decode just disables a feature.
    // -----------------------------------------------------------------------

    // x86 ModRM: returns the number of bytes the modrm+disp consumes for the
    // given modrm byte (0 when the byte is not a valid modrm).
    int ModRmLength(BYTE modrm)
    {
        const BYTE mod = (modrm >> 6) & 3;
        const BYTE rm = modrm & 7;

        if (mod == 3)
            return 1; // register operand, no disp

        if (rm == 4)
        {
            // SIB follows; disp depends on mod and the SIB base.
            if (mod == 0)
                return 2; // modrm + sib (base!=5 -> no disp)
            if (mod == 1)
                return 3; // modrm + sib + disp8
            return 6;     // mod==2: modrm + sib + disp32
        }

        if (rm == 5 && mod == 0)
            return 5; // [disp32]
        if (mod == 1)
            return 2; // modrm + disp8
        return 5;     // mod==2: modrm + disp32
    }

    // Length of the instruction at `code`, or 0 when it is not one we are willing
    // to relocate. Covers the one-byte opcode map (the forms MSVC prologues use),
    // including modrm/immediate forms. Relative branches are handled by the caller
    // (rel32 via *relWithin) or refused (rel8, far). Two-byte (0x0F) opcodes and
    // operand/address-size prefixes are refused: a miss only disables the hook,
    // while a wrong length would corrupt the game's code.
    //
    // *relWithin, when non-null, is set to the offset (from the start of this
    // instruction, prefixes included) of a rel32 operand to fix up after the copy,
    // or -1 when there is none.
    int InstructionLengthEx(const BYTE* code, int* relWithin)
    {
        if (relWithin != nullptr)
            *relWithin = -1;

        int i = 0;
        for (;;) // legacy prefixes (no operand/address-size override: refuse those)
        {
            const BYTE b = code[i];
            if (b == 0x66 || b == 0x67)
                return 0;
            if (b == 0xF0 || b == 0xF2 || b == 0xF3 ||
                b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26 ||
                b == 0x64 || b == 0x65)
            {
                ++i;
                continue;
            }
            break;
        }

        const BYTE op = code[i];
        int n = 0; // bytes after the prefixes

        if (op == 0x0F)
            return 0; // two-byte opcode map: refused

        // 0x00..0x3F ALU family (add/or/adc/sbb/and/sub/xor/cmp).
        {
            const int d = op & 0x07;
            const int hi = op >> 3;
            if (hi <= 7 && (op & 0xC0) == 0)
            {
                // op is one of 0x00..0x3D with the segment-prefix bytes (0x26,
                // 0x2E, 0x36, 0x3E) already consumed above.
                if (d == 6 || d == 7)
                    return 0; // 0x06/0x0E/... push/pop segment - not a prologue shape
                if (d == 4)
                    n = 2;      // AL, imm8
                else if (d == 5)
                    n = 5;      // EAX, imm32
                else
                {
                    const int m = ModRmLength(code[i + 1]);
                    if (m == 0)
                        return 0;
                    n = 1 + m;
                }
                return i + n;
            }
        }

        // 0x40-0x4F inc/dec r32.
        if (op >= 0x40 && op <= 0x4F) return i + 1;
        // 0x50-0x5F push/pop r32.
        if (op >= 0x50 && op <= 0x5F) return i + 1;
        if (op == 0x60 || op == 0x61) return i + 1; // pushad/popad
        if (op == 0x63) // arpl r/m16, r16
        {
            const int m = ModRmLength(code[i + 1]);
            return m == 0 ? 0 : i + 1 + m;
        }
        if (op == 0x68) return i + 5; // push imm32
        if (op == 0x69)               // imul r32, r/m32, imm32
        {
            const int m = ModRmLength(code[i + 1]);
            return m == 0 ? 0 : i + 1 + m + 4;
        }
        if (op == 0x6A) return i + 2; // push imm8
        if (op == 0x6B)               // imul r32, r/m32, imm8
        {
            const int m = ModRmLength(code[i + 1]);
            return m == 0 ? 0 : i + 1 + m + 1;
        }
        if (op >= 0x6C && op <= 0x6F) return i + 1; // ins/outs
        if (op >= 0x70 && op <= 0x7F) return 0;     // jcc rel8: refuse

        if (op == 0x80 || op == 0x83) // group1 r/m, imm8
        {
            const int m = ModRmLength(code[i + 1]);
            return m == 0 ? 0 : i + 1 + m + 1;
        }
        if (op == 0x81) // group1 r/m32, imm32
        {
            const int m = ModRmLength(code[i + 1]);
            return m == 0 ? 0 : i + 1 + m + 4;
        }
        if (op == 0x84 || op == 0x85 || op == 0x86 || op == 0x87) // test/xchg
        {
            const int m = ModRmLength(code[i + 1]);
            return m == 0 ? 0 : i + 1 + m;
        }
        if (op >= 0x88 && op <= 0x8E) // mov family (r/m <-> r, sreg)
        {
            const int m = ModRmLength(code[i + 1]);
            return m == 0 ? 0 : i + 1 + m;
        }
        if (op == 0x8F) // pop r/m32
        {
            const int m = ModRmLength(code[i + 1]);
            return m == 0 ? 0 : i + 1 + m;
        }
        if (op == 0x90) return i + 1; // nop
        if (op >= 0x91 && op <= 0x97) return i + 1; // xchg eax, r32
        if (op == 0x98 || op == 0x99 || op == 0x9B || op == 0x9C ||
            op == 0x9D || op == 0x9E || op == 0x9F)
            return i + 1;
        if (op == 0x9A) return 0; // far call: refuse

        if (op >= 0xA0 && op <= 0xA3) return i + 5; // mov moffs32
        if (op >= 0xA4 && op <= 0xA7) return i + 1; // movs/cmps
        if (op == 0xA8) return i + 2;               // test al, imm8
        if (op == 0xA9) return i + 5;               // test eax, imm32
        if (op >= 0xAA && op <= 0xAF) return i + 1; // stos/lods/scas

        if (op >= 0xB0 && op <= 0xB7) return i + 2; // mov r8, imm8
        if (op >= 0xB8 && op <= 0xBF) return i + 5; // mov r32, imm32

        if (op == 0xC0 || op == 0xC1) // shift r/m, imm8
        {
            const int m = ModRmLength(code[i + 1]);
            return m == 0 ? 0 : i + 1 + m + 1;
        }
        if (op == 0xC2) return i + 3; // ret imm16
        if (op == 0xC3) return i + 1; // ret
        if (op == 0xC4 || op == 0xC5) return 0; // les/lds (or VEX): refuse
        if (op == 0xC6) // mov r/m8, imm8
        {
            const int m = ModRmLength(code[i + 1]);
            return m == 0 ? 0 : i + 1 + m + 1;
        }
        if (op == 0xC7) // mov r/m32, imm32
        {
            const int m = ModRmLength(code[i + 1]);
            return m == 0 ? 0 : i + 1 + m + 4;
        }
        if (op == 0xC8) return i + 4; // enter imm16, imm8
        if (op == 0xC9) return i + 1; // leave
        if (op == 0xCA) return i + 3; // retf imm16
        if (op == 0xCB || op == 0xCC || op == 0xCE || op == 0xCF) return i + 1;
        if (op == 0xCD) return i + 2; // int imm8

        if (op == 0xD0 || op == 0xD1 || op == 0xD2 || op == 0xD3) // shift
        {
            const int m = ModRmLength(code[i + 1]);
            return m == 0 ? 0 : i + 1 + m;
        }
        if (op == 0xD4 || op == 0xD5) return i + 2; // aam/aad
        if (op == 0xD6 || op == 0xD7) return i + 1;
        if (op >= 0xD8 && op <= 0xDF) // x87 FPU
        {
            const int m = ModRmLength(code[i + 1]);
            return m == 0 ? 0 : i + 1 + m;
        }

        if (op >= 0xE0 && op <= 0xE3) return 0; // loop/jcxz rel8: refuse
        if (op == 0xE4 || op == 0xE5) return i + 2; // in imm8
        if (op == 0xE6 || op == 0xE7) return i + 2; // out imm8
        if (op == 0xE8 || op == 0xE9) // call/jmp rel32: relocatable with a fixup
        {
            if (relWithin != nullptr)
                *relWithin = i + 1;
            return i + 5;
        }
        if (op == 0xEA) return 0; // far jmp: refuse
        if (op == 0xEB) return 0; // jmp rel8: refuse
        if (op == 0xEC || op == 0xED || op == 0xEE || op == 0xEF) return i + 1;

        if (op == 0xF1 || op == 0xF4 || op == 0xF5) return i + 1;
        if (op == 0xF6) // group3 r/m8 (test/not/neg/...)
        {
            const int m = ModRmLength(code[i + 1]);
            if (m == 0)
                return 0;
            const int reg = (code[i + 1] >> 3) & 7;
            return i + 1 + m + ((reg == 0 || reg == 1) ? 1 : 0);
        }
        if (op == 0xF7) // group3 r/m32
        {
            const int m = ModRmLength(code[i + 1]);
            if (m == 0)
                return 0;
            const int reg = (code[i + 1] >> 3) & 7;
            return i + 1 + m + ((reg == 0 || reg == 1) ? 4 : 0);
        }
        if (op >= 0xF8 && op <= 0xFD) return i + 1;
        if (op == 0xFE) // group4 (inc/dec r/m8)
        {
            const int m = ModRmLength(code[i + 1]);
            return m == 0 ? 0 : i + 1 + m;
        }
        if (op == 0xFF) // group5 (indirect call/jmp/push are position-independent)
        {
            const int m = ModRmLength(code[i + 1]);
            return m == 0 ? 0 : i + 1 + m;
        }

        return 0; // unknown opcode: refuse
    }

    // Number of whole bytes to copy so that at least `need` bytes are covered, or
    // 0 when the prologue contains something we cannot relocate. Rel32 operands
    // found on the way are appended to `relOffsets` (offsets from `code`), so the
    // caller can fix them up after moving the bytes into the trampoline.
    int PrologueLengthForJump(const BYTE* code, int need,
                              int* relOffsets, int* relCount, int maxRel)
    {
        int total = 0;
        if (relCount != nullptr)
            *relCount = 0;
        while (total < need)
        {
            int relWithin = -1;
            const int len = InstructionLengthEx(code + total, &relWithin);
            if (len <= 0)
                return 0;
            if (relWithin >= 0 && relCount != nullptr && *relCount < maxRel)
                relOffsets[(*relCount)++] = total + relWithin;
            total += len;
            if (total > 32) // sanity bound
                return 0;
        }
        return total;
    }
}

// Wraps RwTextureRead's read callback (the name-based path). A NULL return is
// exactly what makes RwTextureRead raise E_RW_READTEXMASK, and it happens while
// the requesting mod is still on the stack.
void* __cdecl DebugUtils::TextureReadCallbackWatch(const char* name, const char* maskName)
{
    RwTextureCallBackReadFn original = g_rwTextureReadCbOriginal;
    if (original == nullptr)
        return nullptr;

    void* texture = original(name, maskName);

    if (texture == nullptr && s_instance != nullptr)
        s_instance->RecordTextureReadFailure(name, _ReturnAddress());

    return texture;
}

// Exact-caller entry hook. RwTextureRead's entry JMPs here (see
// InstallTextureEntryHook), and because a JMP pushes no return address,
// _ReturnAddress() is the *direct caller* of RwTextureRead - the mod that asked
// for the texture. The original prologue was copied into a trampoline, so we run
// it, then jump into the rest of RwTextureRead; the callback watch still fires
// for a NULL result and reports the exact caller recorded here.
void* __cdecl DebugUtils::TextureReadEntryWatch(const char* name, const char* maskName)
{
    // Remember the exact caller for the duration of the call, so the callback
    // wrapper (which runs deeper, inside the game) can attribute the failure to
    // this caller rather than to the game frame it sees.
    if (s_instance != nullptr)
        s_instance->m_lastTextureExactCaller = LabelForModCaller(_ReturnAddress());

    typedef void* (__cdecl* RwTextureReadFn)(const char*, const char*);
    auto trampoline = reinterpret_cast<RwTextureReadFn>(s_instance->m_textureEntryTrampoline);
    if (trampoline == nullptr)
        return nullptr;

    void* texture = trampoline(name, maskName);

    if (texture == nullptr && s_instance != nullptr)
        s_instance->RecordTextureReadFailure(name, _ReturnAddress());

    return texture;
}

// Resolve a caller address to "mod.asi+0x1a2b", or an empty string when it is
// the game, CLEO, DebugUtils itself or a system DLL - i.e. not an interesting
// answer. Reuses the same module filters as the stack scan so the label means the
// same thing everywhere.
std::string DebugUtils::LabelForModCaller(const void* address)
{
    const DWORD addr = reinterpret_cast<DWORD>(address);
    if (addr < 0x10000)
        return {};

    HMODULE module = nullptr;
    if (!GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(addr),
            &module) || module == nullptr)
    {
        return {};
    }

    if (IsIgnoredModuleHandle(module) || IsSystemModuleHandle(module))
        return {};

    return FormatFrameLabel(addr);
}

int __cdecl DebugUtils::TextureReadWatchRead(void* stream, void** texture, int length)
{
    RwNativeTextureReadFn original = g_rwTextureReadOriginal;
    if (original == nullptr)
        return 0;

    const int result = original(stream, texture, length);

    // The stream (TXD-internal) path does not raise E_RW_READTEXMASK, but a false
    // return still means a texture failed to read, so it is recorded too.
    if (result == 0 && s_instance != nullptr)
        s_instance->RecordTextureReadFailure(nullptr);

    return result;
}

void DebugUtils::InstallTextureReadWatch()
{
    if (!m_textureWatchEnabled)
        return;
    if (m_textureReadCbInstalled || m_textureWatchInstalled)
        return;

    // The addresses are SA 1.0 US specific; refuse anything else rather than call
    // a wrong pointer and crash the game.
    if (CLEO_GetGameVersion() != GV_US10)
    {
        WriteFrameTime(
            "[texturewatch] disabled: RenderWare texture-read addresses are SA 1.0 US only"
        );
        return;
    }

    // 1) The name-based reader's callback - the raiser of E_RW_READTEXMASK. This
    //    is a public RW API, so swapping it is a data-only change.
    auto getReadCallback = reinterpret_cast<RwTextureGetReadCallBackFn>(
        kGtaSa10RwTextureGetReadCallBack);
    auto setReadCallback = reinterpret_cast<RwTextureSetReadCallBackFn>(
        kGtaSa10RwTextureSetReadCallBack);

    RwTextureCallBackReadFn current = getReadCallback();
    {
        MEMORY_BASIC_INFORMATION info{};
        if (current == nullptr ||
            VirtualQuery(reinterpret_cast<void*>(current), &info, sizeof(info)) == 0 ||
            (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                             PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) == 0)
        {
            WriteFrameTime(
                "[texturewatch] disabled: RwTextureRead callback is not executable code"
            );
            return;
        }
    }

    g_rwTextureReadCbOriginal = current;
    m_textureReadCbOriginal = reinterpret_cast<void*>(current);
    setReadCallback(&DebugUtils::TextureReadCallbackWatch);
    m_textureReadCbInstalled = true;

    // 2) The stream reader RwGlobals.stdFunc[26] (the TXD-internal path). Optional:
    //    if the layout guard fails we still keep the callback watch above.
    void* rwGlobals = *reinterpret_cast<void**>(kGtaSa10RwEngineInstance);
    if (rwGlobals != nullptr)
    {
        void** slot = reinterpret_cast<void**>(
            reinterpret_cast<uintptr_t>(rwGlobals) + kRwStdFuncNativeTextureReadOffset);

        MEMORY_BASIC_INFORMATION info{};
        if (*slot != nullptr &&
            VirtualQuery(*slot, &info, sizeof(info)) != 0 &&
            (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                             PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0)
        {
            g_rwTextureReadOriginal = reinterpret_cast<RwNativeTextureReadFn>(*slot);
            m_textureWatchOriginalRead = *slot;
            m_textureWatchSlot = slot;

            DWORD oldProtect = 0;
            if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect))
            {
                *slot = reinterpret_cast<void*>(&DebugUtils::TextureReadWatchRead);
                VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
                m_textureWatchInstalled = true;
            }
            else
            {
                g_rwTextureReadOriginal = nullptr;
                m_textureWatchSlot = nullptr;
            }
        }
    }

    WriteFrameTime(
        "[texturewatch] enabled: RenderWare texture reads are captured "
        "(read callback=%u, stream slot=%u); a failed read is reported with the "
        "mod that requested it",
        m_textureReadCbInstalled ? 1u : 0u,
        m_textureWatchInstalled ? 1u : 0u
    );

    // Optional exact-caller entry hook (opt-in). Runs after the data-only swaps so
    // a refusal here still leaves the callback/stream watch working.
    InstallTextureEntryHook();
}

void DebugUtils::RemoveTextureReadWatch()
{
    if (m_textureReadCbInstalled)
    {
        auto setReadCallback = reinterpret_cast<RwTextureSetReadCallBackFn>(
            kGtaSa10RwTextureSetReadCallBack);
        setReadCallback(reinterpret_cast<RwTextureCallBackReadFn>(m_textureReadCbOriginal));
        m_textureReadCbInstalled = false;
        m_textureReadCbOriginal = nullptr;
        g_rwTextureReadCbOriginal = nullptr;
    }

    if (m_textureWatchInstalled && m_textureWatchSlot != nullptr)
    {
        auto* slot = reinterpret_cast<void**>(m_textureWatchSlot);

        DWORD oldProtect = 0;
        if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect))
        {
            *slot = m_textureWatchOriginalRead;
            VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
        }

        m_textureWatchInstalled = false;
        m_textureWatchSlot = nullptr;
        m_textureWatchOriginalRead = nullptr;
        g_rwTextureReadOriginal = nullptr;
    }

    RemoveTextureEntryHook();
}

// Install the exact-caller entry hook on RwTextureRead. This is the only code
// patch in this file: a 5-byte JMP at the target's entry to TextureReadEntryWatch,
// with the displaced prologue copied into an executable trampoline that jumps
// back. A JMP pushes no return address, so inside the wrapper _ReturnAddress() is
// the mod that called RwTextureRead - an exact answer, unlike the stack scan.
//
// Every step is guarded: the target must be inside the game image and executable,
// the prologue must decode to relocatable instructions (else we refuse), and the
// trampoline must allocate. Any failure just disables this hook and leaves the
// data-only callback watch (and the labelled scan) in charge.
void DebugUtils::InstallTextureEntryHook()
{
    if (!m_textureEntryHookEnabled || m_textureEntryHookInstalled)
        return;

    if (CLEO_GetGameVersion() != GV_US10)
    {
        WriteFrameTime(
            "[texturewatch] exact caller disabled: RenderWare addresses are SA 1.0 US only"
        );
        return;
    }

    // Resolve the target: an explicit ini address wins (a mod may call either
    // RwTextureRead wrapper); otherwise the RenderWare-core entry.
    DWORD target = m_textureEntryAddress;
    if (target == 0)
        target = kGtaSa10RwTextureRead;

    // A value that is not a plausible game-image address is a misconfiguration
    // (e.g. set to 1 as "on"), not a request to patch there - fall back to the
    // default rather than disabling the hook.
    const HMODULE gameImage = GetModuleHandleA(nullptr);
    const DWORD imageBase = reinterpret_cast<DWORD>(gameImage);
    if (target < imageBase || target >= imageBase + 0x01000000)
    {
        WriteFrameTime(
            "[texturewatch] TextureEntryAddress=0x%08X is outside the game image "
            "(base 0x%08X); using the built-in default 0x%08X instead",
            static_cast<unsigned>(target),
            static_cast<unsigned>(imageBase),
            static_cast<unsigned>(kGtaSa10RwTextureRead)
        );
        target = kGtaSa10RwTextureRead;
    }

    // A 5-byte entry JMP is not relocatable (the decoder refuses relative
    // branches), and 0x4C7510 may itself be a thunk that jumps to the real body.
    // Follow a simple `jmp rel32` / `jmp [abs32]` chain to the real entry so the
    // hook installs on the function rather than on a stub. Bounded to a few hops.
    for (int hop = 0; hop < 4; ++hop)
    {
        // Only read if the page is committed and readable: the address came from
        // the ini, so a bad value must not fault here.
        MEMORY_BASIC_INFORMATION peek{};
        if (VirtualQuery(reinterpret_cast<void*>(target), &peek, sizeof(peek)) == 0 ||
            peek.State != MEM_COMMIT ||
            (peek.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
                             PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY |
                             PAGE_EXECUTE)) == 0)
        {
            break;
        }

        const BYTE* p = reinterpret_cast<const BYTE*>(target);
        if (p[0] == 0xE9) // jmp rel32
        {
            INT32 rel = 0;
            memcpy(&rel, p + 1, sizeof(rel));
            target = static_cast<DWORD>(target + 5 + rel);
        }
        else if (p[0] == 0xFF && p[1] == 0x25) // jmp dword ptr [abs32]
        {
            DWORD abs = 0;
            memcpy(&abs, p + 2, sizeof(abs));
            target = *reinterpret_cast<const DWORD*>(abs);
        }
        else
        {
            break;
        }
    }

    // The target must be executable code inside the game image: patching a wrong
    // address would corrupt arbitrary memory.
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<void*>(target), &info, sizeof(info)) == 0 ||
        info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                         PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) == 0 ||
        info.Type != MEM_IMAGE ||
        info.AllocationBase != GetModuleHandleA(nullptr))
    {
        WriteFrameTime(
            "[texturewatch] exact caller disabled: 0x%08X is not executable game code "
            "(only the stack scan will name the mod)",
            static_cast<unsigned>(target)
        );
        return;
    }

    // Copy whole instructions covering at least the 5 bytes the JMP replaces.
    // Any rel32 operand inside those instructions is recorded so it can be
    // re-based after the copy (its target stays the same, the source moved).
    int relOffsets[4];
    int relCount = 0;
    const int prologue = PrologueLengthForJump(
        reinterpret_cast<const BYTE*>(target), 5, relOffsets, &relCount, 4);
    if (prologue <= 0)
    {
        const BYTE* p = reinterpret_cast<const BYTE*>(target);
        WriteFrameTime(
            "[texturewatch] exact caller disabled: prologue at 0x%08X is not "
            "relocatable (bytes %02X %02X %02X %02X %02X %02X %02X %02X); only the "
            "stack scan will name the mod",
            static_cast<unsigned>(target),
            p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]
        );
        return;
    }

    // Executable trampoline: the displaced prologue, then a JMP back to the
    // first instruction after the patch.
    BYTE* tramp = static_cast<BYTE*>(VirtualAlloc(
        nullptr, prologue + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (tramp == nullptr)
    {
        WriteFrameTime("[texturewatch] exact caller disabled: trampoline allocation failed");
        return;
    }

    memcpy(tramp, reinterpret_cast<const void*>(target), prologue);

    // Re-base any rel32 inside the moved prologue: it pointed at an absolute
    // address relative to `target`; from the trampoline it must point at the same
    // absolute address. rel = oldRel + (target - tramp).
    for (int r = 0; r < relCount; ++r)
    {
        INT32 oldRel = 0;
        memcpy(&oldRel, tramp + relOffsets[r], sizeof(oldRel));
        const INT32 newRel = oldRel + static_cast<INT32>(
            target - reinterpret_cast<DWORD>(tramp));
        memcpy(tramp + relOffsets[r], &newRel, sizeof(newRel));
    }

    tramp[prologue] = 0xE9; // jmp rel32 back into RwTextureRead
    const DWORD back = (target + prologue) - (reinterpret_cast<DWORD>(tramp) + prologue + 5);
    memcpy(tramp + prologue + 1, &back, sizeof(back));

    // Save the prologue, then overwrite it with jmp TextureReadEntryWatch.
    memcpy(m_textureEntrySaved, reinterpret_cast<const void*>(target), 5);

    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(target), 5, PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        VirtualFree(tramp, 0, MEM_RELEASE);
        WriteFrameTime("[texturewatch] exact caller disabled: could not make the entry writable");
        return;
    }

    BYTE patch[5];
    patch[0] = 0xE9;
    const DWORD rel = reinterpret_cast<DWORD>(&DebugUtils::TextureReadEntryWatch) -
                      (target + 5);
    memcpy(patch + 1, &rel, sizeof(rel));
    memcpy(reinterpret_cast<void*>(target), patch, sizeof(patch));
    VirtualProtect(reinterpret_cast<void*>(target), 5, oldProtect, &oldProtect);

    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(target), 5);

    m_textureEntryTarget = reinterpret_cast<void*>(target);
    m_textureEntryTrampoline = tramp;
    m_textureEntryHookInstalled = true;

    WriteFrameTime(
        "[texturewatch] exact caller enabled: RwTextureRead at 0x%08X is hooked "
        "(prologue %d byte(s)); the mod that requests a failed texture is named exactly",
        static_cast<unsigned>(target), prologue
    );
}

void DebugUtils::RemoveTextureEntryHook()
{
    if (!m_textureEntryHookInstalled || m_textureEntryTarget == nullptr)
        return;

    DWORD oldProtect = 0;
    if (VirtualProtect(m_textureEntryTarget, 5, PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        memcpy(m_textureEntryTarget, m_textureEntrySaved, 5);
        VirtualProtect(m_textureEntryTarget, 5, oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), m_textureEntryTarget, 5);
    }

    if (m_textureEntryTrampoline != nullptr)
    {
        VirtualFree(m_textureEntryTrampoline, 0, MEM_RELEASE);
        m_textureEntryTrampoline = nullptr;
    }

    m_textureEntryHookInstalled = false;
    m_textureEntryTarget = nullptr;
    m_lastTextureExactCaller.clear();
}

void DebugUtils::RecordTextureReadFailure(const char* textureName, const void* exactCaller)
{
    const DWORD now = GetTickCount();
    const DWORD uptimeSec = (now - m_memorySessionStartTick) / 1000u;
    const std::string script = CurrentScriptName();

    // Key by texture name so the report says *which* texture failed as well as
    // which mod asked for it; an unnamed read (stream path) collapses to one row.
    const std::string key =
        (textureName != nullptr && textureName[0] != '\0')
            ? std::string(textureName)
            : std::string("(unnamed texture read)");

    std::lock_guard<std::mutex> lock(m_textureReadMutex);

    auto it = m_textureReadFailures.find(key);
    if (it == m_textureReadFailures.end())
    {
        TextureReadFailure failure;
        failure.count = 1;
        failure.firstUptimeSec = uptimeSec;
        failure.lastUptimeSec = uptimeSec;
        failure.lastScript = script;

        m_textureReadOrder.push_back(key);
        it = m_textureReadFailures.emplace(key, std::move(failure)).first;
    }
    else
    {
        TextureReadFailure& failure = it->second;
        ++failure.count;
        failure.lastUptimeSec = uptimeSec;
        failure.lastScript = script;
    }

    // Capture the chain only on the first failure of that texture, while the
    // raising frame is still on the stack; the scan cost is paid once per name.
    TextureReadFailure& stored = it->second;

    // Prefer the exact caller from the RwTextureRead entry hook: it is the direct
    // caller, so it beats any scan. Fall back to the string the wrapper left (the
    // callback path, which runs deeper) when no address was passed.
    if (!stored.modulesScanned || stored.exactCaller.empty())
    {
        std::string exact;
        if (exactCaller != nullptr)
            exact = LabelForModCaller(exactCaller);
        else if (!m_lastTextureExactCaller.empty())
            exact = m_lastTextureExactCaller;

        if (!exact.empty())
            stored.exactCaller = exact;
    }

    if (!stored.modulesScanned)
    {
        stored.modulesScanned = true;
        if (m_errorModuleScanEnabled)
        {
            const char* via = nullptr;
            stored.modules = CaptureRequesterChain(6, &via);
            stored.chainVia = via ? via : "";
        }
    }
}

void DebugUtils::WriteTextureReadReport(bool forceAll)
{
    std::map<std::string, TextureReadFailure> failures;
    std::vector<std::string> order;
    {
        std::lock_guard<std::mutex> lock(m_textureReadMutex);
        failures = m_textureReadFailures;
        order = m_textureReadOrder;
    }

    if (failures.empty())
        return;

    // Print only the rows that changed since the last report (a new texture, or
    // one whose count grew). Printing the whole table every interval turned a
    // handful of failures into thousands of identical lines.
    std::vector<const std::pair<const std::string, TextureReadFailure>*> changed;
    for (const auto& key : order)
    {
        auto it = failures.find(key);
        if (it == failures.end())
            continue;
        if (forceAll || it->second.count != it->second.lastReportedCount)
            changed.push_back(&*it);
    }

    if (changed.empty())
        return;

    // Group the changed rows by their requester chain: many textures are read by
    // the same mod, so the chain is printed once and the textures listed under it
    // instead of repeating the identical "requested by" line per texture.
    std::vector<std::string> chainOrder;
    std::map<std::string, std::vector<const std::pair<const std::string, TextureReadFailure>*>> byChain;
    for (const auto* entry : changed)
    {
        std::string chain;
        for (const auto& module : entry->second.modules)
        {
            if (!chain.empty())
                chain += " <- ";
            chain += module;
        }
        if (chain.empty())
            chain = "(no mod frame captured - enable ModuleScan)";

        auto it = byChain.find(chain);
        if (it == byChain.end())
            chainOrder.push_back(chain);
        byChain[chain].push_back(entry);
    }

    WriteFrameTime("================= failed texture reads =================");
    WriteFrameTime("%zu changed of %zu texture(s); grouped by the mod that read them",
                   changed.size(), failures.size());

    for (const auto& chain : chainOrder)
    {
        WriteFrameTime("  requested by: %s", chain.c_str());
        // The chain method is the same for every texture under it, so it is stated
        // once, from the first entry that has one.
        const auto& group = byChain[chain];
        for (const auto* entry : group)
        {
            if (!entry->second.chainVia.empty())
            {
                WriteFrameTime("    chain via: %s", entry->second.chainVia.c_str());
                break;
            }
        }

        // The exact caller (RwTextureRead entry hook) is the strongest answer -
        // the mod's own call site - so print it when present.
        for (const auto* entry : group)
        {
            if (!entry->second.exactCaller.empty())
            {
                WriteFrameTime(
                    "    exact caller: %s  (captured at RwTextureRead, 100%% exact)",
                    entry->second.exactCaller.c_str());
                break;
            }
        }

        for (const auto* entry : group)
        {
            std::string name = entry->first;
            if (name.size() > 34)
                name = std::string("...") + name.substr(name.size() - 31);

            WriteFrameTime(
                "    %-34s count=%-5u first=%-5u last=%-5u script=%s",
                name.c_str(),
                static_cast<unsigned>(entry->second.count),
                static_cast<unsigned>(entry->second.firstUptimeSec),
                static_cast<unsigned>(entry->second.lastUptimeSec),
                entry->second.lastScript.empty() ? "?" : entry->second.lastScript.c_str()
            );
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_textureReadMutex);
        for (auto& entry : m_textureReadFailures)
            entry.second.lastReportedCount = entry.second.count;
    }
}

