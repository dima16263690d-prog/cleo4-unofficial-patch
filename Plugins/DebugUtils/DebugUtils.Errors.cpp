#include "DebugUtils.h"
#include "DebugUtils.Internal.h"
#include <windows.h>
#include <psapi.h>
#include "DebugUtils.RwErrorNames.h"
#include "resource.h"
#include <algorithm>
#include <cctype>
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
//
// The stack-walking / module attribution helpers live in DebugUtils.StackScan.cpp
// and the file/texture watches in DebugUtils.Watches.cpp; this file owns the
// RenderWare error poll and the error record/report.

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
    std::vector<std::pair<unsigned int, DWORD>> reportedCounts;
    std::set<std::string> suspectModules;
    size_t changedCount = 0;
    {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        records.reserve(m_errorOrder.size());
        for (unsigned int key : m_errorOrder)
        {
            auto it = m_errorRecords.find(key);
            if (it != m_errorRecords.end())
            {
                records.push_back(it->second);
                reportedCounts.emplace_back(key, it->second.count);
                if (it->second.count != it->second.lastReportedCount)
                    ++changedCount;
            }
        }
        suspectModules = m_errorModulesSeen;
    }

    const bool forceAll = reason != nullptr && strcmp(reason, "game_end") == 0;

    const unsigned long long cleoErrors =
        static_cast<unsigned long long>(InterlockedCompareExchange(
            const_cast<volatile LONG*>(&m_cleoErrorCount), 0, 0));
    const unsigned long long cleoWarnings =
        static_cast<unsigned long long>(InterlockedCompareExchange(
            const_cast<volatile LONG*>(&m_cleoWarningCount), 0, 0));
    const bool cleoChanged =
        cleoErrors != m_lastReportedCleoErrors ||
        cleoWarnings != m_lastReportedCleoWarnings;

    // Dedup: print nothing at all when there is no new error since the last
    // report. A stable error is reported once (when it appears) instead of being
    // re-printed in full every interval.
    if (!forceAll && changedCount == 0 && !cleoChanged)
    {
        return;
    }

    WriteFrameTime("================= errors (%s) =================",
        reason ? reason : "interval");
    WriteFrameTime(
        "cleo errors=%llu warnings=%llu | distinct=%zu | suspect modules=%zu",
        cleoErrors,
        cleoWarnings,
        records.size(),
        suspectModules.size()
    );

    if (records.empty() && cleoErrors == 0 && cleoWarnings == 0)
    {
        WriteFrameTime("[errors] none: no CLEO error/warning and no RenderWare error seen");
        m_lastReportedCleoErrors = cleoErrors;
        m_lastReportedCleoWarnings = cleoWarnings;
        return;
    }

    // Only print the table header when there is a row to show; a CLEO-only change
    // with no new RenderWare record should not leave a dangling header.
    if (changedCount > 0 || forceAll)
    {
        WriteFrameTime("%-3s %-22s %8s %10s %-9s %5s %5s %-16s %s",
            "#", "name", "count", "plugin", "code", "first", "last", "script", "suspect module");
    }

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
    std::string unchanged;
    for (const auto& record : records)
    {
        // Dedup across intervals: a row that has not changed since it was last
        // printed is collapsed into a single "unchanged" line, so a stable error
        // is not re-printed in full (with its meaning/hint) every interval.
        const bool changed = forceAll || record.count != record.lastReportedCount;
        if (!changed)
        {
            if (!unchanged.empty())
                unchanged += ", ";
            unchanged += record.name;
            unchanged += " x";
            unchanged += std::to_string(record.count);
            continue;
        }

        ++index;

        // Detail lines (meaning/hint) are only useful the first time a record is
        // shown (or in the final game_end dump); a recurring error re-prints just
        // its row, not the same explanation every interval.
        const bool showDetail = forceAll || record.lastReportedCount == 0;

        // For a RenderWare row the script shown is the one active when the error
        // was *noticed* (next frame's poll), so mark it approximate with a '~'
        // instead of implying it raised the error.
        std::string script = record.lastScript.empty() ? "?" : record.lastScript;
        if (record.renderWare)
            script = "~" + script;

        // plugin-sdk builds a throwaway CRunningScript literally named "plg-sdk"
        // whenever a plugin calls a game script command through CallCommandById
        // (ScriptCommands.h). Seeing it here means the error surfaced inside such
        // a call from a plugin, not from a user CLEO script.
        const bool pluginBridge = script.find("plg-sdk") != std::string::npos;

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

        if (pluginBridge && showDetail)
        {
            WriteFrameTime(
                "      note: 'plg-sdk' is the plugin-sdk script-command bridge "
                "(CallCommandById), not a user script; the error came from a "
                "plugin calling a game command"
            );
        }

        if (!record.text.empty() && showDetail)
        {
            WriteFrameTime("      meaning: %s", record.text.c_str());
        }

        // Point the reader at the likely source for file/resource errors: this is
        // the "which mod causes it" answer for a missing/corrupt texture or model.
        if (isFileResourceError(record) && showDetail)
        {
            WriteFrameTime(
                "      hint: file/resource error - a missing or corrupt texture/"
                "model/.txd (or a mod loading it). Check the script/mod above and "
                "its resource files."
            );
            WriteFrameTime(
                "      hint: a rendering mod or D3D9/dinput proxy that replaces "
                "RenderWare's texture reader (NativeTextureRead, SA 0x4CD820) is a "
                "prime suspect - it can raise this for a texture the stock engine "
                "would load fine."
            );
            WriteFrameTime(
                "      hint: set [DebugUtils.Errors] TextureWatch=1 to name the mod "
                "that requested the failed texture read (the stack is captured at "
                "the read, which this poll cannot do)."
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

    if (index == 0)
    {
        WriteFrameTime("[errors] no new RenderWare errors this interval");
    }

    // Collapse the rows that did not change into one line so the table above is
    // only the errors that actually happened (or grew) in this interval.
    if (!unchanged.empty())
    {
        WriteFrameTime("[errors] unchanged since last report: %s", unchanged.c_str());
    }

    // Remember exactly what was printed (the snapshot counts) so the next report
    // only shows changes. Use the snapshotted values, not the live counts: a
    // record that grew after the snapshot must still count as changed next time.
    {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        for (const auto& entry : reportedCounts)
        {
            auto it = m_errorRecords.find(entry.first);
            if (it != m_errorRecords.end())
                it->second.lastReportedCount = entry.second;
        }
    }
    m_lastReportedCleoErrors = cleoErrors;
    m_lastReportedCleoWarnings = cleoWarnings;

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

    // Module column note: attribution is by stack scan, so state it plainly once
    // per session rather than repeating it on every report.
    if (!m_errorNotePrinted)
    {
        WriteFrameTime(
            "[errors] note: 'suspect module' is the first non-game/non-CLEO module on the "
            "stack at the error, or the script active then; it points at the likely source, "
            "not a proven root cause"
        );
        m_errorNotePrinted = true;
    }
}
