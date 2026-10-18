#include "DebugUtils.h"
#include "DebugUtils.Internal.h"
#include <windows.h>
#include <psapi.h>
#include <cctype>
#include <cstdio>
#include <cstring>
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

    typedef void* (__cdecl* RwTextureCallBackReadFn)(const char* name, const char* maskName);
    typedef RwTextureCallBackReadFn(__cdecl* RwTextureGetReadCallBackFn)(void);
    typedef int(__cdecl* RwTextureSetReadCallBackFn)(RwTextureCallBackReadFn callBack);

    typedef int(__cdecl* RwNativeTextureReadFn)(void* stream, void** texture, int length);

    // Assigned during InstallTextureReadWatch, read on every intercepted call.
    RwNativeTextureReadFn g_rwTextureReadOriginal = nullptr;
    RwTextureCallBackReadFn g_rwTextureReadCbOriginal = nullptr;
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
        s_instance->RecordTextureReadFailure(name);

    return texture;
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
}

void DebugUtils::RecordTextureReadFailure(const char* textureName)
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

