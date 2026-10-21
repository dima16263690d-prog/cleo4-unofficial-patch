#include "DebugUtils.h"
#include "DebugUtils.Internal.h"
#include "DebugUtils.X86Decoder.h"
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

    // Only restore if our wrapper is still in the slot. If a later hook (another
    // ASI, or a mod that re-reads the interface) put its own rwfopen there,
    // overwriting it with the original would silently delete that hook and break
    // whoever installed it.
    if (table->rwfopen != reinterpret_cast<void*>(&DebugUtils::FileOpenWatchFopen))
    {
        WriteFrameTime(
            "[filewatch] NOT removed: RenderWare rwfopen no longer holds our hook "
            "(another hook took the slot); leaving it untouched"
        );
        m_fileWatchInstalled = false;
        m_fileWatchTable = nullptr;
        m_fileWatchOriginalFopen = nullptr;
        g_rwFopenOriginal = nullptr;
        return;
    }

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
// *name-based* reader RwTextureRead (SA 1.0 US 0x7F3AC0) when its read callback
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
    // The find callback (RwTextureGetFindCallBack/SetFindCallBack). The engine
    // installs CVehicleModelInfo::FindTextureCB here; a mod that swaps it changes
    // how every texture name is resolved, so its owner is worth reporting.
    constexpr DWORD kGtaSa10RwTextureGetFindCallBack = 0x7F34F0;

    // Entry of RwTextureRead (SA 1.0 US). Verified against gta-reversed
    // (source/game_sa/RenderWare/rw/rwcore.cpp: RwTextureRead = CallAndReturn<...,
    // 0x7F3AC0>) and against dozens of independent projects (MTA:SA, fastman92
    // limit adjuster, sa-render, skygfx, VehFuncs, gtaRenderHook, VCSPC): mods
    // call 0x7F3AC0 directly to read a texture. plugin-sdk's RenderWare.cpp is
    // WRONG here - it lists 0x4C7510 for RwTextureRead, but 0x4C7510 is
    // CVehicleModelInfo::FindTextureCB (gta-reversed VehicleModelInfo.cpp:67,
    // RH_ScopedInstall(FindTextureCB, 0x4C7510)) - the *find* callback the engine
    // calls from inside RwTextureRead, which is why hooking it only ever named the
    // game. The real E_RW_READTEXMASK raiser is RwTextureRead itself.
    constexpr DWORD kGtaSa10RwTextureRead = 0x7F3AC0;
    // 0x4C7510 is CVehicleModelInfo::FindTextureCB, not RwTextureRead: an ini
    // override can point at it, but the built-in default is the real reader.

    typedef void* (__cdecl* RwTextureCallBackReadFn)(const char* name, const char* maskName);
    typedef RwTextureCallBackReadFn(__cdecl* RwTextureGetReadCallBackFn)(void);
    typedef int(__cdecl* RwTextureSetReadCallBackFn)(RwTextureCallBackReadFn callBack);
    // The find callback takes only the name (rwcore.h: RwTextureCallBackFind).
    typedef void* (__cdecl* RwTextureCallBackFindFn)(const char* name);
    typedef RwTextureCallBackFindFn(__cdecl* RwTextureGetFindCallBackFn)(void);

    typedef int(__cdecl* RwNativeTextureReadFn)(void* stream, void** texture, int length);
    // RwTextureRead itself: RwTexture* cdecl (const char* name, const char* maskName).
    typedef void* (__cdecl* RwTextureReadFn)(const char*, const char*);

    // Assigned during InstallTextureReadWatch, read on every intercepted call.
    RwNativeTextureReadFn g_rwTextureReadOriginal = nullptr;
    RwTextureCallBackReadFn g_rwTextureReadCbOriginal = nullptr;

    // The exact-caller entry hook's trampoline (displaced prologue + JMP back).
    // A GLOBAL, not a DebugUtils instance field: the 5-byte JMP stays in the
    // game's code until RemoveTextureEntryHook restores it, and TextureReadEntryWatch
    // must reach the original prologue even when s_instance is null (plugin being
    // unloaded, or the hook left in place because another hook took the slot).
    // Never VirtualFree'd, so it stays valid for the process lifetime.
    RwTextureReadFn g_textureEntryTrampoline = nullptr;

}

// Per-thread state for the entry hook. thread_local, not a class field: a texture
// read can happen on a non-game thread (a mod's loader thread), and two threads
// reading at once must not share "the last caller" - that race produced a wrong
// attribution and was undefined behaviour on the std::string.
namespace
{
    // True while this thread is inside TextureReadEntryWatch, i.e. the read
    // callback (which fires deeper, inside the game) must not record the failure
    // itself - the entry hook records it once, with the exact caller.
    thread_local bool g_textureReadInEntryHook = false;
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

    // When the entry hook is installed it wraps this whole call and will record
    // the failure itself with the exact caller; recording here too would double
    // every count. The caller seen here is inside RwTextureRead (the game), so it
    // is only a weak fallback - the scan is preferred (callerIsDirect=false).
    if (texture == nullptr && s_instance != nullptr && !g_textureReadInEntryHook)
        s_instance->RecordTextureReadFailure(name, _ReturnAddress(), false);

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
    // Reach the trampoline through the GLOBAL, not s_instance: the JMP stays in
    // the game's code until RemoveTextureEntryHook restores the prologue, so this
    // wrapper can be entered after the plugin unloaded (s_instance == null) or
    // when another hook took the slot and we deliberately left ours in place.
    // Returning NULL in those cases would make every texture read fail; running
    // the trampoline keeps the game working even when we can no longer report.
    RwTextureReadFn trampoline = g_textureEntryTrampoline;
    if (trampoline == nullptr)
        return nullptr;

    // Keep only the raw return address; it is turned into a module label later,
    // and only when the read actually fails. Formatting (GetModuleHandleEx +
    // std::string) on every successful read was pure overhead on the hottest path.
    const void* caller = _ReturnAddress();

    // Mark the window so the read callback (fired inside) does not record the
    // failure as well - this function records it with the exact caller below.
    g_textureReadInEntryHook = true;
    if (s_instance != nullptr)
        s_instance->m_textureEntryHookCalls.fetch_add(1, std::memory_order_relaxed);
    void* texture = trampoline(name, maskName);
    g_textureReadInEntryHook = false;

    if (texture == nullptr && s_instance != nullptr)
        s_instance->RecordTextureReadFailure(name, caller, true);

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

// Same as LabelForModCaller but without the "is it interesting" filters, so the
// game and CLEO are labelled too. Used only for diagnostics: an engine-initiated
// RwTextureRead (the engine loading a .txd) has no mod as its direct caller, and
// this shows which engine function reached the read.
std::string DebugUtils::LabelAnyModule(const void* address)
{
    const DWORD addr = reinterpret_cast<DWORD>(address);
    if (addr < 0x10000)
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
    // Who installed the read callback before us? If it is not the game image, a
    // mod already replaced RenderWare's texture loader - the prime suspect.
    {
        const std::string owner = LabelAnyModule(reinterpret_cast<const void*>(current));
        if (!owner.empty() && owner.compare(0, 12, "GTA_SA.exe+") != 0 &&
            owner.compare(0, 11, "gta_sa.exe+") != 0)
        {
            m_textureReadCbOwner = owner;
        }
    }
    setReadCallback(&DebugUtils::TextureReadCallbackWatch);
    m_textureReadCbInstalled = true;

    // Also record who owns the *find* callback: the engine uses
    // CVehicleModelInfo::FindTextureCB, so anything else is a mod that changed
    // texture-name resolution (a texture/D3D9 proxy is the usual culprit).
    {
        auto getFindCallback = reinterpret_cast<RwTextureGetFindCallBackFn>(
            kGtaSa10RwTextureGetFindCallBack);
        RwTextureCallBackFindFn findCb = getFindCallback();
        if (findCb != nullptr)
        {
            const std::string owner = LabelAnyModule(reinterpret_cast<const void*>(findCb));
            if (!owner.empty() && owner.compare(0, 12, "GTA_SA.exe+") != 0 &&
                owner.compare(0, 11, "gta_sa.exe+") != 0)
            {
                m_textureFindCbOwner = owner;
            }
        }
    }

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
        auto getReadCallback = reinterpret_cast<RwTextureGetReadCallBackFn>(
            kGtaSa10RwTextureGetReadCallBack);

        // Only restore if the current callback is still ours. Another mod may have
        // re-installed its own callback after us; blindly writing the original back
        // would silently remove that mod's callback.
        if (getReadCallback() == &DebugUtils::TextureReadCallbackWatch)
        {
            setReadCallback(reinterpret_cast<RwTextureCallBackReadFn>(m_textureReadCbOriginal));
        }
        else
        {
            WriteFrameTime(
                "[texturewatch] NOT removed: the RwTextureRead callback no longer "
                "holds our hook (another mod replaced it); leaving it untouched"
            );
        }

        m_textureReadCbInstalled = false;
        m_textureReadCbOriginal = nullptr;
        g_rwTextureReadCbOriginal = nullptr;
    }

    if (m_textureWatchInstalled && m_textureWatchSlot != nullptr)
    {
        auto* slot = reinterpret_cast<void**>(m_textureWatchSlot);

        // Same guard: restore the stream slot only if our wrapper is still there.
        if (*slot == reinterpret_cast<void*>(&DebugUtils::TextureReadWatchRead))
        {
            DWORD oldProtect = 0;
            if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect))
            {
                *slot = m_textureWatchOriginalRead;
                VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
            }
        }
        else
        {
            WriteFrameTime(
                "[texturewatch] NOT removed: RwGlobals.stdFunc[26] no longer holds "
                "our hook (another mod replaced it); leaving it untouched"
            );
        }

        m_textureWatchInstalled = false;
        m_textureWatchSlot = nullptr;
        m_textureWatchOriginalRead = nullptr;
        g_rwTextureReadOriginal = nullptr;
    }

    RemoveTextureEntryHook();
}

// Install a 5-byte JMP detour at `target`, redirecting to `wrapper`. The
// displaced prologue is copied into an executable trampoline that jumps back to
// the first instruction after the patch; `*trampolineOut` receives it (or stays
// null on failure). `savedOut` keeps the original 5 bytes for a later restore.
//
// Every step is guarded: the target must be committed, executable code inside the
// game image, the prologue must decode to relocatable instructions (else we
// refuse), and the trampoline must allocate. A failure returns false and leaves
// the target untouched, so the caller can fall back to a data-only watch.
//
// A JMP pushes no return address, so inside the wrapper _ReturnAddress() is the
// caller of the hooked function - an exact answer, unlike a stack scan. This is
// the ONLY kind of code patch in the plugin.
bool DebugUtils::InstallEntryDetour(
    DWORD target,
    const void* wrapper,
    BYTE savedOut[5],
    void** trampolineOut,
    const char* label)
{
    *trampolineOut = nullptr;

    // A 5-byte entry JMP is not relocatable (the decoder refuses relative
    // branches), and a target may itself be a thunk that jumps to the real body.
    // Follow a simple `jmp rel32` / `jmp [abs32]` chain to the real entry so the
    // hook installs on the function rather than on a stub. Bounded to a few hops.
    for (int hop = 0; hop < 4; ++hop)
    {
        // Only read if the page is committed and readable: the address may come
        // from the ini, so a bad value must not fault here.
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
            "[%s] disabled: 0x%08X is not executable game code",
            label,
            static_cast<unsigned>(target)
        );
        return false;
    }

    // Copy whole instructions covering at least the 5 bytes the JMP replaces.
    // Any rel32 operand inside those instructions is recorded so it can be
    // re-based after the copy (its target stays the same, the source moved).
    int relOffsets[4];
    int relCount = 0;
    const int prologue = DebugUtilsX86::PrologueLengthForJump(
        reinterpret_cast<const BYTE*>(target), 5, relOffsets, &relCount, 4);
    if (prologue <= 0)
    {
        const BYTE* p = reinterpret_cast<const BYTE*>(target);
        WriteFrameTime(
            "[%s] disabled: prologue at 0x%08X is not relocatable (bytes %02X %02X "
            "%02X %02X %02X %02X %02X %02X)",
            label,
            static_cast<unsigned>(target),
            p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]
        );
        return false;
    }

    // Executable trampoline: the displaced prologue, then a JMP back to the
    // first instruction after the patch.
    BYTE* tramp = static_cast<BYTE*>(VirtualAlloc(
        nullptr, prologue + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (tramp == nullptr)
    {
        WriteFrameTime("[%s] disabled: trampoline allocation failed", label);
        return false;
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

    tramp[prologue] = 0xE9; // jmp rel32 back into the original function
    const DWORD back = (target + prologue) - (reinterpret_cast<DWORD>(tramp) + prologue + 5);
    memcpy(tramp + prologue + 1, &back, sizeof(back));

    // Save the prologue, then overwrite it with jmp wrapper.
    memcpy(savedOut, reinterpret_cast<const void*>(target), 5);

    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(target), 5, PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        VirtualFree(tramp, 0, MEM_RELEASE);
        WriteFrameTime("[%s] disabled: could not make the entry writable", label);
        return false;
    }

    BYTE patch[5];
    patch[0] = 0xE9;
    const DWORD rel = reinterpret_cast<DWORD>(wrapper) - (target + 5);
    memcpy(patch + 1, &rel, sizeof(rel));
    memcpy(reinterpret_cast<void*>(target), patch, sizeof(patch));
    VirtualProtect(reinterpret_cast<void*>(target), 5, oldProtect, &oldProtect);

    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(target), 5);

    *trampolineOut = tramp;
    return true;
}

// Restore the prologue saved by InstallEntryDetour, but only when our own JMP is
// still in place: if another hook was installed on top of ours, writing the saved
// bytes back would silently delete that hook. The trampoline is deliberately NOT
// freed - a thread may still be executing inside it, and leaking a few dozen bytes
// is far safer than unmapping a page under a running thread.
bool DebugUtils::RemoveEntryDetour(
    DWORD target, const void* wrapper, const BYTE saved[5], const char* label)
{
    if (target == 0)
        return false;

    const BYTE* at = reinterpret_cast<const BYTE*>(target);
    const DWORD expected = reinterpret_cast<DWORD>(wrapper) - (target + 5);
    INT32 presentRel = 0;
    memcpy(&presentRel, at + 1, sizeof(presentRel));
    if (at[0] != 0xE9 || static_cast<DWORD>(presentRel) != expected)
    {
        WriteFrameTime(
            "[%s] NOT removed: 0x%08X no longer holds our hook (another hook took "
            "the slot); leaving it untouched",
            label,
            static_cast<unsigned>(target)
        );
        return false;
    }

    DWORD oldProtect = 0;
    if (VirtualProtect(reinterpret_cast<void*>(target), 5, PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        memcpy(reinterpret_cast<void*>(target), saved, 5);
        VirtualProtect(reinterpret_cast<void*>(target), 5, oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(target), 5);
    }
    return true;
}

// Install the exact-caller entry hook on RwTextureRead. A JMP pushes no return
// address, so inside the wrapper _ReturnAddress() is the mod that called
// RwTextureRead - an exact answer, unlike the stack scan. Any failure just
// disables this hook and leaves the data-only callback watch (and the labelled
// scan) in charge.
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

    void* trampoline = nullptr;
    if (!InstallEntryDetour(
            target,
            reinterpret_cast<const void*>(&DebugUtils::TextureReadEntryWatch),
            m_textureEntrySaved,
            &trampoline,
            "texturewatch"))
    {
        return;
    }

    m_textureEntryTarget = reinterpret_cast<void*>(target);
    g_textureEntryTrampoline = reinterpret_cast<RwTextureReadFn>(trampoline);
    m_textureEntryHookInstalled = true;

    WriteFrameTime(
        "[texturewatch] exact caller enabled: RwTextureRead at 0x%08X is hooked; "
        "the mod that requests a failed texture is named exactly",
        static_cast<unsigned>(target)
    );
}

void DebugUtils::RemoveTextureEntryHook()
{
    if (!m_textureEntryHookInstalled || m_textureEntryTarget == nullptr)
        return;

    RemoveEntryDetour(
        reinterpret_cast<DWORD>(m_textureEntryTarget),
        reinterpret_cast<const void*>(&DebugUtils::TextureReadEntryWatch),
        m_textureEntrySaved,
        "texturewatch");

    // Do NOT VirtualFree the trampoline: a thread may be executing inside it
    // right now (the JMP that sent it there was still live until the line above),
    // and freeing the page under a running thread crashes the game. The prologue
    // bytes plus a JMP are a few dozen bytes, so leaking it is the safe choice.
    // The global trampoline pointer is deliberately NOT cleared either: if our JMP
    // is somehow still reachable (another hook copied it, or the slot was only
    // partly restored), TextureReadEntryWatch must still be able to run the
    // original instead of returning NULL for every texture read.
    m_textureEntryHookInstalled = false;
    m_textureEntryTarget = nullptr;
}

void DebugUtils::RecordTextureReadFailure(
    const char* textureName, const void* caller, bool callerIsDirect)
{
    const DWORD now = GetTickCount();
    const DWORD uptimeSec = (now - m_memorySessionStartTick) / 1000u;
    const std::string script = CurrentScriptName();

    // Label the caller here (not in the hook) so the module lookup and string
    // formatting are paid only for an actual failure, never for a success.
    const std::string modCaller = LabelForModCaller(caller);
    const std::string rawCaller = LabelAnyModule(caller);

    // Key by texture name so the report says *which* texture failed as well as
    // which mod asked for it. An unnamed read (stream path) has no name, so it is
    // keyed by the caller too - otherwise every stream failure, whoever caused it,
    // collapsed into one "(unnamed texture read)" row with the first caller.
    std::string key;
    if (textureName != nullptr && textureName[0] != '\0')
    {
        key = textureName;
    }
    else
    {
        key = "(unnamed texture read)";
        if (!rawCaller.empty())
            key += " via " + rawCaller;
    }

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

    // Capture the requester chain the first time this texture fails AND again
    // whenever the caller changes: a texture can be requested by the engine at
    // load and later by a mod (or by a different mod), and freezing the first scan
    // would report the wrong requester for every later failure. The scan cost is
    // paid only when the caller label actually changes, not per failure.
    TextureReadFailure& stored = it->second;

    // Which model was being converted on this thread when the read failed? Set by
    // the ConvertBufferToObject hook; -1 when the streaming hook is off or the read
    // happened outside a model conversion (engine/script path). The name is
    // resolved lazily (and cached in the record) so the IMG index is built at most
    // once, on the first stream-time failure.
    const int streamingModelId = CurrentStreamingModelId();
    if (streamingModelId >= 0 && stored.streamingModelId != streamingModelId)
    {
        stored.streamingModelId = streamingModelId;
        stored.streamingModel = ModelNameForId(streamingModelId);
        const auto txdAndLod = ModelTxdIndexAndLod(streamingModelId);
        stored.streamingIsLod = txdAndLod.second;
        stored.streamingTxd = txdAndLod.first >= 0
            ? ModelNameForId(kTxdModelBase + txdAndLod.first)
            : std::string();
    }

    const std::string callerKey = callerIsDirect ? modCaller : rawCaller;
    if (!stored.modulesScanned || stored.lastScanCaller != callerKey)
    {
        stored.modulesScanned = true;
        stored.lastScanCaller = callerKey;
        if (m_errorModuleScanEnabled)
        {
            const char* via = nullptr;
            stored.modules = CaptureRequesterChain(6, &via);
            stored.chainVia = via ? via : "";
        }
    }

    // Resolve the exact caller (the mod's own call site). Two sources:
    //  - the entry hook (callerIsDirect=true): `caller` is the DIRECT caller of
    //    RwTextureRead - the mod itself. That is the exact answer.
    //  - the callback path (callerIsDirect=false): `caller` is inside the game
    //    (RwTextureRead calling its own callback), so it is only a fallback and
    //    the scanned mod frame is preferred.
    // The entry hook wins whenever it recorded a mod; otherwise fall back.
    if (stored.exactCaller.empty() || (callerIsDirect && !stored.exactCallerDirect))
    {
        std::string exact;
        bool direct = false;
        if (callerIsDirect)
        {
            exact = modCaller;
            direct = !exact.empty();
            if (exact.empty() && !rawCaller.empty() && stored.directCallerRaw.empty())
                stored.directCallerRaw = rawCaller;
        }
        if (exact.empty() && !stored.modules.empty())
        {
            exact = stored.modules.front();
            direct = false;
        }
        if (!exact.empty())
        {
            stored.exactCaller = exact;
            stored.exactCallerDirect = direct;
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

    // Which modules are on EVERY chain? A frame that shows up in every chain is
    // not the requester - it is a hook/wrapper that sits on the stack no matter
    // what actually asked for the texture (e.g. the nested main-loop hooks of
    // modloader, a HUD ASI, a SilentPatch-style mod). Naming such a module as the
    // "probable caller" sends the reader after the wrong mod, so it is pulled out
    // of the chain and labelled separately. Computed over ALL recorded chains (not
    // just the changed rows) so the verdict is stable between reports.
    std::set<std::string> alwaysOnStack;
    {
        std::map<std::string, size_t> moduleChainCount;
        size_t chainsWithModules = 0;
        for (const auto& kv : failures)
        {
            if (kv.second.modules.empty())
                continue;
            ++chainsWithModules;
            std::set<std::string> seen;
            for (const auto& module : kv.second.modules)
            {
                if (seen.insert(module).second)
                    ++moduleChainCount[module];
            }
        }
        // "Every chain" with a small tolerance (>= 80%), and only once there are
        // enough chains for the notion to mean anything (with 2 chains any shared
        // frame would look universal).
        if (chainsWithModules >= 3)
        {
            const size_t threshold = (chainsWithModules * 8 + 9) / 10; // ceil(80%)
            for (const auto& kv : moduleChainCount)
            {
                if (kv.second >= threshold)
                    alwaysOnStack.insert(kv.first);
            }
        }
    }

    // Group the changed rows by the DISTINCTIVE part of their requester chain (the
    // frames that are not on every chain): entries that differ only in the
    // always-on-stack tail share the same requester, so they are grouped together
    // and the main-loop tail is printed once per group.
    std::vector<std::string> chainOrder;
    std::map<std::string, std::vector<const std::pair<const std::string, TextureReadFailure>*>> byChain;
    std::map<std::string, std::string> commonByChain; // group key -> main-loop tail
    for (const auto* entry : changed)
    {
        std::string distinctive;
        std::string common;
        for (const auto& module : entry->second.modules)
        {
            std::string& target = alwaysOnStack.count(module) != 0 ? common : distinctive;
            if (!target.empty())
                target += " <- ";
            target += module;
        }

        const std::string key = distinctive.empty()
            ? std::string("(no requester frame captured)")
            : distinctive;
        auto it = byChain.find(key);
        if (it == byChain.end())
            chainOrder.push_back(key);
        byChain[key].push_back(entry);
        commonByChain[key] = common;
    }

    WriteFrameTime("================= failed texture reads =================");
    WriteFrameTime("%zu changed of %zu texture(s); grouped by the mod that read them",
                   changed.size(), failures.size());
    if (m_textureEntryHookInstalled)
    {
        WriteFrameTime(
            "    exact-caller hook: active, %llu RwTextureRead call(s) seen so far%s",
            m_textureEntryHookCalls.load(std::memory_order_relaxed),
            m_textureEntryHookCalls.load(std::memory_order_relaxed) == 0
                ? " - no mod read a texture via 0x7F3AC0 yet, so only the stack "
                  "scan can name the requester"
                : "");
    }
    // If a mod had already replaced RenderWare's read/find callback, it is a prime
    // suspect: it changes how every texture name resolves, so a failed read may be
    // its doing (a texture looked up in the wrong place) rather than a missing file.
    if (!m_textureReadCbOwner.empty())
    {
        WriteFrameTime(
            "    WARNING: the RenderWare read callback was replaced by %s before "
            "DebugUtils loaded - a texture-loader mod/proxy is the prime suspect",
            m_textureReadCbOwner.c_str()
        );
    }
    if (!m_textureFindCbOwner.empty())
    {
        WriteFrameTime(
            "    WARNING: the RenderWare find callback was replaced by %s - texture "
            "name resolution is not the stock engine's (wrong TXD/archive is possible)",
            m_textureFindCbOwner.c_str()
        );
    }

    for (const auto& chain : chainOrder)
    {
        const auto& group = byChain[chain];
        const bool hasRequester = chain != "(no requester frame captured)";
        const std::string& common = commonByChain[chain];
        if (hasRequester)
            WriteFrameTime("  requested by: %s", chain.c_str());
        else if (common.empty())
            WriteFrameTime(
                "  requested by: (no mod frame captured - enable ModuleScan to "
                "scan the stack at the failure)");
        else
            WriteFrameTime(
                "  requested by: (no distinctive mod frame - every frame was on "
                "all chains, so either the engine/streamer asked, or one mod "
                "wraps every read; enable TextureExactCaller to tell them apart)");

        // The always-present tail: frames that are on every chain, so they are NOT
        // the requester. Printed separately so they are not mistaken for the culprit.
        if (!common.empty())
        {
            WriteFrameTime(
                "    also on stack every time (main-loop hook chain, NOT the "
                "requester): %s",
                common.c_str());
        }

        // The chain method is the same for every texture under it, so it is stated
        // once, from the first entry that has one.
        for (const auto* entry : group)
        {
            if (!entry->second.chainVia.empty())
            {
                WriteFrameTime("    chain via: %s", entry->second.chainVia.c_str());
                break;
            }
        }

        // A "partial" scan (fewer than half the frames confirmed by an E8 call)
        // is a weak chain, so say so instead of presenting it as fact.
        for (const auto* entry : group)
        {
            const std::string& via = entry->second.chainVia;
            if (via.find("partial") != std::string::npos)
            {
                WriteFrameTime(
                    "    note: this chain is only partially verified (most frames "
                    "are not confirmed call sites), so treat the order with caution");
                break;
            }
        }

        // The exact caller (RwTextureRead entry hook) is the strongest answer -
        // the mod's own call site - so print it when present. When it is only the
        // nearest mod frame from the scan, say so rather than overclaim.
        for (const auto* entry : group)
        {
            if (!entry->second.exactCaller.empty())
            {
                if (entry->second.exactCallerDirect)
                {
                    WriteFrameTime(
                        "    exact caller: %s  (direct caller of RwTextureRead, seen "
                        "at its entry hook)",
                        entry->second.exactCaller.c_str());
                }
                else if (alwaysOnStack.count(entry->second.exactCaller) != 0 &&
                         hasRequester)
                {
                    // The recorded nearest frame is a module that is on EVERY chain
                    // (a main-loop hook/wrapper), so it is NOT the requester - it is
                    // the same tail printed above. Use the nearest DISTINCTIVE frame
                    // instead, which is the first link of the "requested by" chain.
                    const size_t sep = chain.find(" <- ");
                    const std::string nearest =
                        sep == std::string::npos ? chain : chain.substr(0, sep);
                    WriteFrameTime(
                        "    probable caller: %s  (nearest mod frame that is not the "
                        "always-present hook chain; %s was on every chain, so it is "
                        "not the requester)",
                        nearest.c_str(),
                        entry->second.exactCaller.c_str());
                }
                else if (hasRequester)
                {
                    WriteFrameTime(
                        "    probable caller: %s  (nearest mod frame on the stack - a "
                        "strong hint, not the direct caller)",
                        entry->second.exactCaller.c_str());
                }
                break;
            }
        }

        // When the direct caller of RwTextureRead was the engine or CLEO (not a
        // mod), say which one: it means the engine loaded the .txd itself, so no
        // mod is to blame and the mod frame above is only the requester's context.
        for (const auto* entry : group)
        {
            if (entry->second.exactCallerDirect == false &&
                !entry->second.directCallerRaw.empty())
            {
                WriteFrameTime(
                    "    direct caller of RwTextureRead: %s (engine/CLEO-initiated "
                    "read - the mod named above is only the nearest frame, not the "
                    "requester)",
                    entry->second.directCallerRaw.c_str());
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

            // Name the model/txd being streamed (StreamingModelNames=1). This is
            // the file to look at: the texture was referenced by the model loaded
            // at that moment, so a missing/misnamed texture points at this file.
            if (entry->second.streamingModelId >= 0)
            {
                if (!entry->second.streamingModel.empty())
                    WriteFrameTime(
                        "        streamed model: %s (id %d)%s - the .dff being loaded",
                        entry->second.streamingModel.c_str(),
                        entry->second.streamingModelId,
                        entry->second.streamingIsLod ? " [LOD]" : "");
                else
                    WriteFrameTime(
                        "        streamed model id: %d%s (name not in the IMG index)",
                        entry->second.streamingModelId,
                        entry->second.streamingIsLod ? " [LOD]" : "");

                // The dictionary that should hold the texture. This is the FILE to
                // open: a missing texture is either absent from this .txd or the
                // .txd itself is missing/corrupt.
                if (!entry->second.streamingTxd.empty())
                    WriteFrameTime(
                        "        its texture dictionary: %s.txd - the texture should "
                        "be inside this file",
                        entry->second.streamingTxd.c_str());

                if (entry->second.streamingIsLod)
                    WriteFrameTime(
                        "        note: this is a LOD model; LODs use a reduced texture "
                        "set, so a miss here is often the engine's normal LOD probe "
                        "and not a broken file");
            }
        }
    }

    // Verdict line: was the read initiated by the engine (so the *file* is at
    // fault) or by a mod (so the mod is)? The direct caller captured by the entry
    // hook tells them apart, which is the difference between "install the missing
    // .txd" and "fix the mod that asks for the wrong texture".
    {
        size_t engineInitiated = 0;
        size_t modInitiated = 0;
        for (const auto* entry : changed)
        {
            if (entry->second.exactCallerDirect)
                ++modInitiated;
            else if (!entry->second.directCallerRaw.empty())
                ++engineInitiated;
        }
        if (engineInitiated > 0 || modInitiated > 0)
        {
            WriteFrameTime(
                "[texturewatch] %zu of %zu changed read(s): engine-initiated "
                "(a missing/corrupt resource), %zu mod-initiated (the mod asked for "
                "the texture)",
                engineInitiated, changed.size(), modInitiated
            );
            if (engineInitiated > 0)
            {
                WriteFrameTime(
                    "    hint: an engine-initiated failed read means the .txd/.dff "
                    "that should contain the texture is missing, not renamed, or "
                    "corrupt - check the mod that replaced the model/archive, not "
                    "the nearest mod frame in the stack."
                );
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_textureReadMutex);
        for (auto& entry : m_textureReadFailures)
            entry.second.lastReportedCount = entry.second.count;
    }
}

// The data-only watches install once at game begin, but a mod can re-install its
// own callback/table entry over ours afterwards, which silently disables the
// watch for the rest of the session (no failure is captured, and the report looks
// like "nothing failed"). Re-check each slot on a slow cadence and, if ours was
// displaced, report it and stop claiming the watch is active. The slot is NOT
// re-patched: another module now owns it, and fighting over it would be worse
// than losing the capture. The check is read-only, so it is cheap.
void DebugUtils::VerifyWatches()
{
    // Nothing installed -> nothing to verify; skip even the tick check.
    if (!m_fileWatchInstalled && !m_textureReadCbInstalled &&
        !m_textureWatchInstalled && !m_textureEntryHookInstalled &&
        !m_streamingHookInstalled)
        return;

    const DWORD now = GetTickCount();
    if (now - m_lastWatchVerifyTick < 5000)
        return;
    m_lastWatchVerifyTick = now;

    if (m_fileWatchInstalled && m_fileWatchTable != nullptr)
    {
        auto* table = reinterpret_cast<RwFileFunctionsHead*>(m_fileWatchTable);
        if (table->rwfopen != reinterpret_cast<void*>(&DebugUtils::FileOpenWatchFopen))
        {
            WriteFrameTime(
                "[filewatch] our rwfopen hook was replaced by another module; the "
                "file-open watch is no longer capturing (not re-installed)"
            );
            m_fileWatchInstalled = false;
            m_fileWatchTable = nullptr;
            m_fileWatchOriginalFopen = nullptr;
            g_rwFopenOriginal = nullptr;
        }
    }

    if (m_textureReadCbInstalled)
    {
        auto getReadCallback = reinterpret_cast<RwTextureGetReadCallBackFn>(
            kGtaSa10RwTextureGetReadCallBack);
        if (getReadCallback() != &DebugUtils::TextureReadCallbackWatch)
        {
            WriteFrameTime(
                "[texturewatch] our RwTextureRead callback was replaced by another "
                "module; the texture-read watch is no longer capturing (not re-installed)"
            );
            m_textureReadCbInstalled = false;
            m_textureReadCbOriginal = nullptr;
            g_rwTextureReadCbOriginal = nullptr;
        }
    }

    if (m_textureWatchInstalled && m_textureWatchSlot != nullptr)
    {
        auto* slot = reinterpret_cast<void**>(m_textureWatchSlot);
        if (*slot != reinterpret_cast<void*>(&DebugUtils::TextureReadWatchRead))
        {
            WriteFrameTime(
                "[texturewatch] our RwGlobals.stdFunc[26] hook was replaced by "
                "another module; the stream watch is no longer capturing (not re-installed)"
            );
            m_textureWatchInstalled = false;
            m_textureWatchSlot = nullptr;
            m_textureWatchOriginalRead = nullptr;
            g_rwTextureReadOriginal = nullptr;
        }
    }

    if (m_textureEntryHookInstalled && m_textureEntryTarget != nullptr)
    {
        const BYTE* at = reinterpret_cast<const BYTE*>(m_textureEntryTarget);
        INT32 presentRel = 0;
        memcpy(&presentRel, at + 1, sizeof(presentRel));
        const DWORD expected =
            reinterpret_cast<DWORD>(&DebugUtils::TextureReadEntryWatch) -
            (reinterpret_cast<DWORD>(m_textureEntryTarget) + 5);
        if (at[0] != 0xE9 || static_cast<DWORD>(presentRel) != expected)
        {
            WriteFrameTime(
                "[texturewatch] our exact-caller entry hook was replaced by another "
                "hook; the exact caller is no longer captured (not re-installed)"
            );
            m_textureEntryHookInstalled = false;
            m_textureEntryTarget = nullptr;
        }
    }

    if (m_streamingHookInstalled && m_streamingTarget != nullptr)
    {
        const BYTE* at = reinterpret_cast<const BYTE*>(m_streamingTarget);
        INT32 presentRel = 0;
        memcpy(&presentRel, at + 1, sizeof(presentRel));
        const DWORD expected =
            reinterpret_cast<DWORD>(&DebugUtils::ConvertBufferToObjectWatch) -
            (reinterpret_cast<DWORD>(m_streamingTarget) + 5);
        if (at[0] != 0xE9 || static_cast<DWORD>(presentRel) != expected)
        {
            WriteFrameTime(
                "[streaming] our ConvertBufferToObject hook was replaced by another "
                "hook; the streamed-model name is no longer captured (not re-installed)"
            );
            m_streamingHookInstalled = false;
            m_streamingTarget = nullptr;
        }
    }
}