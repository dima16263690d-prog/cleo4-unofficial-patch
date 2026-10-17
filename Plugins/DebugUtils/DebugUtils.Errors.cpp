#include "DebugUtils.h"
#include "DebugUtils.Internal.h"
#include "DebugUtils.RwErrorNames.h"
#include "resource.h"
#include <windows.h>
#include <psapi.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// RenderWare / CLEO error diagnostics.
//
// Two independent error sources are observed and correlated with the frame in
// which they appeared:
//
//   * RenderWare errors. RenderWare keeps the last error as a small
//     RwError {pluginID, errorCode} pair. plugin-sdk resolves RwErrorGet at a
//     fixed address for GTA SA 1.0 US (0x00808880); calling it is read-only and
//     copies the current pair into the caller's struct. The monitor polls it once
//     per frame and reports any *change* of the pair, which is the only way to
//     see a RenderWare error that did not (yet) crash the process. Nothing in the
//     game is patched: the monitor never modifies code or data.
//
//   * CLEO errors. The core routes CLEO_DEBUG_ERROR / CLEO_DEBUG_WARNING messages
//     through OnCoreLog synchronously on the thread that raised them. The stack at
//     that moment is scanned for the first return address in a module that is
//     neither the game, nor the CLEO core, nor DebugUtils; that module is the
//     mod/plugin/loader responsible.
//
// Attribution is deliberately conservative and labelled as such in the log: a
// module seen on the stack (or a script active at the time) is a strong suspect,
// not proof. The goal is to point a human at the right DLL or .cs file, not to
// claim a root cause.

namespace
{
    // plugin-sdk: RwError* RwErrorGet(RwError* code) at 0x00808880 (SA 1.0 US).
    // RenderWare is statically linked into gta_sa.exe, so this lives in the game
    // module. RwErrorGet *copies and clears* the current global error (RenderWare
    // resets it to E_RW_NOERROR once read), so the pair is restored afterwards
    // with RwErrorSet to leave the game's own error handling untouched.
    typedef void* (__cdecl *RwErrorGetFn)(void* code);
    typedef void* (__cdecl *RwErrorSetFn)(void* code);

    constexpr DWORD kGtaSa10RwErrorGet = 0x00808880;
    constexpr DWORD kGtaSa10RwErrorSet = 0x00808820;

    struct RwErrorPair
    {
        int pluginID;
        int errorCode;
    };

    // Highest errorCode bit means "common" error, whose numeric value is
    // 0x80000000 + index in the generated table.
    constexpr unsigned int kRwCommonErrorBit = 0x80000000u;

    // Pack a RenderWare error pair into one dedup key. pluginID and errorCode are
    // each small enough that 20/12 bits never collide in practice.
    unsigned int RwKey(int pluginId, int errorCode)
    {
        return (static_cast<unsigned int>(pluginId) << 12) ^
               (static_cast<unsigned int>(errorCode) & 0xFFFu);
    }

    // Resolve a RenderWare error pair to a name and a human-readable text.
    // Returns false when the code is not in either generated table (an unknown
    // plugin error), in which case the caller reports the raw numbers.
    bool RwResolveError(unsigned int pluginId, unsigned int errorCode,
                        const char*& name, const char*& text)
    {
        using namespace DebugUtilsRwErrorNames;

        if ((errorCode & 0x80000000u) != 0)
        {
            for (unsigned i = 0; i < kCount; ++i)
            {
                if (kNames[i].code == errorCode)
                {
                    name = kNames[i].name;
                    text = kNames[i].text;
                    return true;
                }
            }
            name = "RW_COMMON_?";
            text = "";
            return true;
        }

        if (pluginId == kCorePluginId)
        {
            for (unsigned i = 0; i < kCoreCount; ++i)
            {
                if (kCoreNames[i].code == errorCode)
                {
                    name = kCoreNames[i].name;
                    text = kCoreNames[i].text;
                    return true;
                }
            }
        }

        name = "RW_PLUGIN_ERR";
        text = "";
        return false;
    }

    bool IsPageReadable(const void* address)
    {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(address, &info, sizeof(info)) == 0)
            return false;

        if (info.State != MEM_COMMIT)
            return false;

        const DWORD readable =
            PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
            PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;

        return (info.Protect & readable) != 0;
    }

    // Portable (no SEH) guarded read of a DWORD, so the stack scan is safe under
    // both MSVC and the mingw syntax-check harness.
    bool ReadDwordIfReadable(const void* address, DWORD& value)
    {
        if (address == nullptr || !IsPageReadable(address))
            return false;

        value = *reinterpret_cast<const DWORD*>(address);
        return true;
    }

    bool IsExecutableAddress(const void* address)
    {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(address, &info, sizeof(info)) == 0)
            return false;

        if (info.State != MEM_COMMIT)
            return false;

        const DWORD executable =
            PAGE_EXECUTE | PAGE_EXECUTE_READ |
            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;

        return (info.Protect & executable) != 0;
    }
}

std::string DebugUtils::CurrentScriptName()
{
    CLEO_CrashSnapshot snapshot{};
    if (CLEO_DebugGetCrashSnapshot(&snapshot) && snapshot.scriptPtr != 0)
    {
        char name[9];
        memcpy(name, snapshot.scriptName, 8);
        name[8] = '\0';

        // Trim trailing spaces/nulls so the log is tidy.
        size_t len = strlen(name);
        while (len > 0 && (name[len - 1] == ' ' || name[len - 1] == '\0'))
            name[--len] = '\0';

        if (len > 0)
            return std::string(name, len);
    }

    return "none";
}

DWORD DebugUtils::CurrentScriptOpcode()
{
    CLEO_CrashSnapshot snapshot{};
    if (CLEO_DebugGetCrashSnapshot(&snapshot) && snapshot.scriptPtr != 0)
        return snapshot.opcode;

    return 0xFFFFFFFFu;
}

// Walks up the current thread's stack looking for return addresses that land in
// a module which is neither the game, the CLEO core, nor DebugUtils. Those are
// the candidate third-party modules (mods/plugins/loaders) responsible for the
// error. Uses the address of a local as the scan anchor, so it needs no
// compiler-specific intrinsic. Read-only and bounded.
std::vector<std::string> DebugUtils::ScanErrorModules(size_t maxModules)
{
    std::vector<std::string> modules;

    const DWORD gameBase = reinterpret_cast<DWORD>(GetModuleHandleA(nullptr));
    const DWORD cleoBase = reinterpret_cast<DWORD>(GetModuleHandleA("CLEO.asi"));
    const DWORD debugBase = reinterpret_cast<DWORD>(GetModuleHandleA("DebugUtils.cleo"));
    const DWORD debugBaseDll = reinterpret_cast<DWORD>(GetModuleHandleA("DebugUtils.dll"));

    // The module containing this function is DebugUtils itself; filter it by
    // address so the result does not depend on the exact loaded file name.
    HMODULE self = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&DebugUtils::ScanErrorModules),
        &self);

    auto isIgnored = [&](DWORD base)
    {
        return base != 0 &&
            (base == gameBase || base == cleoBase ||
             base == debugBase || base == debugBaseDll ||
             base == reinterpret_cast<DWORD>(self));
    };

    // Anchor on a local; on x86 the stack grows down, so higher addresses hold
    // older (caller) frames. Scan a bounded window upward.
    const DWORD anchor = reinterpret_cast<DWORD>(&modules);
    constexpr DWORD kScanBytes = 0x2000;

    for (DWORD offset = 0; offset < kScanBytes; offset += sizeof(DWORD))
    {
        DWORD candidate = 0;
        if (!ReadDwordIfReadable(
                reinterpret_cast<const void*>(anchor + offset), candidate))
            break;

        if (candidate < 0x10000 || !IsExecutableAddress(reinterpret_cast<const void*>(candidate)))
            continue;

        HMODULE module = nullptr;
        if (!GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(candidate),
                &module) || module == nullptr)
        {
            continue;
        }

        if (isIgnored(reinterpret_cast<DWORD>(module)))
            continue;

        const std::string name = ModuleNameForAddress(candidate);
        if (name.empty() || name == "<unknown>")
            continue;

        if (std::find(modules.begin(), modules.end(), name) == modules.end())
            modules.push_back(name);

        if (modules.size() >= maxModules)
            break;
    }

    return modules;
}

void DebugUtils::PollRenderWareError()
{
    if (!m_frameTimeEnabled || !m_errorDiagEnabled || !m_rwErrorWatchEnabled)
        return;

    // The hardcoded address is the SA 1.0 US RenderWare build; skip on anything
    // else rather than read a wrong address.
    if (CLEO_GetGameVersion() != GV_US10)
    {
        if (!m_rwErrorPrimed)
        {
            m_rwErrorPrimed = true;
            WriteFrameTime(
                "[rw] error watch disabled: RenderWare error address is SA 1.0 US only"
            );
        }
        return;
    }

    const auto rwErrorGet = reinterpret_cast<RwErrorGetFn>(kGtaSa10RwErrorGet);
    RwErrorPair error{};
    rwErrorGet(&error);

    // RwErrorGet clears the global error as a side effect. Put it straight back
    // so the monitor is transparent: the game's own RwErrorGet still sees it.
    // RwErrorSet only stores when the global is currently E_RW_NOERROR, which it
    // now is, so this restores exactly the pair that was just read.
    const auto rwErrorSet = reinterpret_cast<RwErrorSetFn>(kGtaSa10RwErrorSet);
    rwErrorSet(&error);

    const unsigned int pluginId = static_cast<unsigned int>(error.pluginID);
    const unsigned int errorCode = static_cast<unsigned int>(error.errorCode);

    // Prime on the first poll so a pre-existing error is not reported as if it
    // had just happened.
    if (!m_rwErrorPrimed)
    {
        m_rwErrorPrimed = true;
        m_rwLastPluginId = pluginId;
        m_rwLastErrorCode = errorCode;
        return;
    }

    if (pluginId == m_rwLastPluginId && errorCode == m_rwLastErrorCode)
        return;

    m_rwLastPluginId = pluginId;
    m_rwLastErrorCode = errorCode;

    // RwErrorGet is a *consuming* read: once it has handed back a real error,
    // RenderWare resets the global to E_RW_NOERROR, so that value is observed
    // exactly once after every real error and is never itself an error. {0, 0}
    // is the same "no error" state. Reporting either would turn each real error
    // into a phantom E_RW_NOERROR record.
    const bool noError =
        (pluginId == 0 && errorCode == 0) || (errorCode == kRwCommonErrorBit);
    if (noError)
        return;

    const char* resolvedName = nullptr;
    const char* resolvedText = nullptr;
    const bool known = RwResolveError(pluginId, errorCode, resolvedName, resolvedText);

    // RenderWare errors are noticed on the next frame's poll, not at the instant
    // they are raised, so the current call stack is not the raising stack and is
    // deliberately not scanned here. Attribution for these comes from the script
    // active at the time; the report says so explicitly.
    const std::vector<std::string> modules;

    RecordError(
        RwKey(error.pluginID, error.errorCode),
        known ? std::string(resolvedName) : std::string("RW_PLUGIN_ERR"),
        pluginId,
        errorCode,
        modules,
        true,
        known ? resolvedText : nullptr
    );
}

void DebugUtils::RecordError(
    unsigned int key,
    const std::string& name,
    unsigned int pluginId,
    unsigned int errorCode,
    const std::vector<std::string>& modules,
    bool renderWare,
    const char* text
)
{
    // Bound the number of distinct records so a storm of unique messages cannot
    // grow the map without limit. Once full, only existing keys are counted.
    constexpr size_t kMaxDistinctErrors = 128;

    // A RenderWare error is noticed on the next frame's poll, so the script and
    // opcode active now are not the ones that raised it; leave the opcode unset
    // so the report does not print a misleading "last opcode" line for it.
    const DWORD opcode = renderWare ? 0xFFFFFFFFu : CurrentScriptOpcode();

    std::lock_guard<std::mutex> lock(m_errorMutex);

    auto it = m_errorRecords.find(key);
    if (it == m_errorRecords.end())
    {
        if (m_errorRecords.size() >= kMaxDistinctErrors)
            return;

        ErrorRecord record;
        record.key = key;
        record.name = name;
        record.pluginId = pluginId;
        record.errorCode = errorCode;
        record.firstTick = GetTickCount();
        record.lastTick = record.firstTick;
        record.lastUptimeSec =
            (GetTickCount() - m_memorySessionStartTick) / 1000u;
        record.firstUptimeSec = record.lastUptimeSec;
        record.count = 1;
        record.lastScript = CurrentScriptName();
        record.lastOpcode = opcode;
        record.modules = modules;
        record.renderWare = renderWare;
        record.text = text ? text : "";
        if (!modules.empty())
            record.module = modules.front();

        m_errorOrder.push_back(key);
        m_errorRecords.emplace(key, std::move(record));
    }
    else
    {
        ErrorRecord& record = it->second;
        ++record.count;
        record.lastTick = GetTickCount();
        record.lastUptimeSec =
            (GetTickCount() - m_memorySessionStartTick) / 1000u;
        record.lastScript = CurrentScriptName();
        record.lastOpcode = opcode;
        for (const auto& module : modules)
        {
            if (std::find(record.modules.begin(), record.modules.end(), module) ==
                record.modules.end())
            {
                record.modules.push_back(module);
            }
        }
        if (record.module.empty() && !modules.empty())
            record.module = modules.front();
    }

    for (const auto& module : modules)
        m_errorModulesSeen.insert(module);
}

void DebugUtils::WriteErrorReport(const char* reason)
{
    if (!m_frameTimeEnabled || !m_errorDiagEnabled)
        return;

    // Snapshot the records and the module set under the lock, then format
    // outside it.
    std::vector<ErrorRecord> records;
    std::set<std::string> suspectModules;
    {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        records.reserve(m_errorOrder.size());
        for (unsigned int key : m_errorOrder)
        {
            auto it = m_errorRecords.find(key);
            if (it != m_errorRecords.end())
                records.push_back(it->second);
        }
        suspectModules = m_errorModulesSeen;
    }

    WriteFrameTime("================= errors (%s) =================",
        reason ? reason : "interval");
    const unsigned long long cleoErrors =
        static_cast<unsigned long long>(InterlockedCompareExchange(
            const_cast<volatile LONG*>(&m_cleoErrorCount), 0, 0));
    const unsigned long long cleoWarnings =
        static_cast<unsigned long long>(InterlockedCompareExchange(
            const_cast<volatile LONG*>(&m_cleoWarningCount), 0, 0));
    WriteFrameTime(
        "cleo errors=%llu warnings=%llu | rw distinct=%zu | suspect modules=%zu",
        cleoErrors,
        cleoWarnings,
        records.size(),
        suspectModules.size()
    );

    if (records.empty() && cleoErrors == 0 && cleoWarnings == 0)
    {
        WriteFrameTime("[errors] none: no CLEO error/warning and no RenderWare error seen");
        return;
    }

    WriteFrameTime("%-3s %-22s %8s %10s %-9s %5s %5s %-16s %s",
        "#", "name", "count", "plugin", "code", "first", "last", "script", "suspect module");

    // Names whose meaning is "a file/resource could not be opened or read": a
    // missing or corrupt texture/model, almost always a mod/script that loads it.
    auto isFileResourceError = [](const ErrorRecord& record)
    {
        static const char* const kNames[] = {
            "E_RW_BADOPEN", "E_RW_READTEXMASK", "E_RW_READ",
            "E_RW_STREAMOPEN", "E_RW_BADFORMAT",
        };
        for (const char* name : kNames)
        {
            if (record.name == name)
                return true;
        }
        return false;
    };

    size_t index = 0;
    for (const auto& record : records)
    {
        ++index;

        // For a RenderWare row the script shown is the one active when the error
        // was *noticed* (next frame's poll), so mark it approximate with a '~'
        // instead of implying it raised the error.
        std::string script = record.lastScript.empty() ? "?" : record.lastScript;
        if (record.renderWare)
            script = "~" + script;

        WriteFrameTime(
            "%-3zu %-22s %8u 0x%06X 0x%08X %5u %5u %-16s %s",
            index,
            record.name.c_str(),
            static_cast<unsigned>(record.count),
            record.pluginId,
            record.errorCode,
            static_cast<unsigned>(record.firstUptimeSec),
            static_cast<unsigned>(record.lastUptimeSec),
            script.c_str(),
            record.module.empty() ? "<none>" : record.module.c_str()
        );

        if (!record.text.empty())
        {
            WriteFrameTime("      meaning: %s", record.text.c_str());
        }

        // Point the reader at the likely source for file/resource errors: this is
        // the "which mod causes it" answer for a missing/corrupt texture or model.
        if (isFileResourceError(record))
        {
            WriteFrameTime(
                "      hint: file/resource error - a missing or corrupt texture/"
                "model/.txd (or a mod loading it). Check the script/mod above and "
                "its resource files."
            );
        }

        // The opcode is only meaningful where the error was raised. For a
        // RenderWare row it is the poll-time opcode, not the raising one, so it
        // is omitted rather than shown as if it caused the error.
        if (!record.renderWare && record.lastOpcode != 0xFFFFFFFFu)
        {
            const DWORD opcode = record.lastOpcode & 0x7FFF;
            const char* opcodeName = OpcodeName(opcode);
            WriteFrameTime(
                "      last opcode=0x%04X %s",
                static_cast<unsigned>(opcode),
                opcodeName ? opcodeName : "unknown"
            );
        }

        if (record.modules.size() > 1)
        {
            std::string chain;
            for (const auto& module : record.modules)
            {
                if (!chain.empty())
                    chain += " -> ";
                chain += module;
            }
            WriteFrameTime("      call chain: %s", chain.c_str());
        }
    }

    // Explicit "who to blame" summary, kept separate so it is easy to grep.
    if (!suspectModules.empty())
    {
        std::string summary;
        for (const auto& module : suspectModules)
        {
            if (!summary.empty())
                summary += ", ";
            summary += module;
        }
        WriteFrameTime(
            "[errors] suspect modules (mods/plugins seen on the error stack): %s",
            summary.c_str()
        );
    }

    // Module column note: attribution is by stack scan, so state it plainly.
    WriteFrameTime(
        "[errors] note: 'suspect module' is the first non-game/non-CLEO module on the "
        "stack at the error, or the script active then; it points at the likely source, "
        "not a proven root cause"
    );
}
