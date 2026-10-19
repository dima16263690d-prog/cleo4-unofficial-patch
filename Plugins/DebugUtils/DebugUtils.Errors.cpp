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

    // Are the RwErrorGet/Set addresses plausibly the SA 1.0 US build? The
    // addresses come from plugin-sdk (already wrong once for RwTextureRead), so
    // check both the page attributes (committed, executable, inside the game
    // image) AND the first code byte. This cannot prove the function is right,
    // but it rejects an address that points at data, a freed page, another
    // module, or padding - which is what a version mismatch looks like.
    bool RwErrorAddressesLookValid()
    {
        const HMODULE image = GetModuleHandleA(nullptr);
        if (image == nullptr)
            return false;

        const DWORD addresses[2] = { kGtaSa10RwErrorGet, kGtaSa10RwErrorSet };
        for (DWORD address : addresses)
        {
            MEMORY_BASIC_INFORMATION info{};
            if (VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) == 0 ||
                info.State != MEM_COMMIT ||
                (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                                 PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) == 0 ||
                info.Type != MEM_IMAGE ||
                info.AllocationBase != image)
            {
                return false;
            }

            // A real function entry does not start with padding/return bytes. This
            // catches an address that is committed and executable but is not code
            // (int3 filler, a data blob in an executable section, a `ret` stub).
            const BYTE first = *reinterpret_cast<const BYTE*>(address);
            switch (first)
            {
                case 0x00:   // not a valid first opcode byte here
                case 0x90:   // nop
                case 0xCC:   // int3
                case 0xC3:   // ret
                case 0xC2:   // ret imm16
                case 0xCB:   // retf
                    return false;
                default:
                    break;
            }
        }
        return true;
    }

    // SEH-guarded calls. Kept as free functions taking/returning POD only: a
    // __try block in a function that also owns C++ objects with destructors
    // (std::string etc.) is C2712 under /EHsc, so the guarded call must not share
    // a frame with the reporting code.
    bool RwErrorGetGuarded(RwErrorPair& out)
    {
        __try
        {
            reinterpret_cast<RwErrorGetFn>(kGtaSa10RwErrorGet)(&out);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool RwErrorSetGuarded(const RwErrorPair& pair)
    {
        RwErrorPair copy = pair;
        __try
        {
            reinterpret_cast<RwErrorSetFn>(kGtaSa10RwErrorSet)(&copy);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Pack a RenderWare error pair into one 64-bit dedup key: pluginID in the high
    // 32 bits, errorCode in the low 32. No collisions (the old 20/12-bit XOR mix
    // could alias two different pairs) and it fits both fields losslessly.
    unsigned long long RwKey(int pluginId, int errorCode)
    {
        return (static_cast<unsigned long long>(
                    static_cast<unsigned int>(pluginId)) << 32) |
               static_cast<unsigned int>(errorCode);
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
    std::string name;
    DWORD opcode = 0;
    CurrentScriptSnapshot(name, opcode);
    return name;
}

DWORD DebugUtils::CurrentScriptOpcode()
{
    std::string name;
    DWORD opcode = 0;
    CurrentScriptSnapshot(name, opcode);
    return opcode;
}

// Read the CLEO crash snapshot ONCE and derive both the script name and the
// opcode from that single read. Calling CurrentScriptName() and
// CurrentScriptOpcode() separately invokes CLEO_DebugGetCrashSnapshot twice, and
// the two reads can observe different scripts (the game thread advances between
// them), pairing one script's name with another's opcode. One snapshot, one pair.
void DebugUtils::CurrentScriptSnapshot(std::string& name, DWORD& opcode)
{
    name = "none";
    opcode = 0xFFFFFFFFu;

    CLEO_CrashSnapshot snapshot{};
    if (!CLEO_DebugGetCrashSnapshot(&snapshot) || snapshot.scriptPtr == 0)
        return;

    char raw[9];
    memcpy(raw, snapshot.scriptName, 8);
    raw[8] = '\0';

    // Trim trailing spaces/nulls so the log is tidy.
    size_t len = strlen(raw);
    while (len > 0 && (raw[len - 1] == ' ' || raw[len - 1] == '\0'))
        raw[--len] = '\0';

    if (len > 0)
        name.assign(raw, len);
    opcode = snapshot.opcode;
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

    // The addresses come from plugin-sdk, which has already been wrong once for
    // RwTextureRead, so do not trust them blindly: verify both functions look
    // like real code before calling them, and guard the call with SEH. A mismatch
    // disables the watch for the session (the rest of DebugUtils keeps working)
    // instead of crashing the game. The check is cached: two VirtualQuery calls
    // per frame is pure overhead when the answer never changes.
    if (m_rwErrorWatchDisabled)
        return;
    if (!m_rwErrorAddressCheckDone)
    {
        m_rwErrorAddressCheckDone = true;
        m_rwErrorAddressesValid = RwErrorAddressesLookValid();
    }
    if (!m_rwErrorAddressesValid)
    {
        m_rwErrorWatchDisabled = true;
        WriteFrameTime(
            "[rw] error watch disabled: RwErrorGet/Set at 0x%08X/0x%08X do not "
            "look like real code in the game image",
            static_cast<unsigned>(kGtaSa10RwErrorGet),
            static_cast<unsigned>(kGtaSa10RwErrorSet)
        );
        return;
    }

    RwErrorPair error{};
    if (!RwErrorGetGuarded(error))
    {
        // Disable the watch outright: the address is not the SA 1.0 US build, so
        // every poll would raise the same SEH exception. Returning without this
        // flag would fault once per frame for the whole session.
        m_rwErrorWatchDisabled = true;
        WriteFrameTime(
            "[rw] error watch disabled: RwErrorGet faulted (0x%08X); the "
            "address is not the SA 1.0 US build",
            static_cast<unsigned>(kGtaSa10RwErrorGet)
        );
        return;
    }

    // RwErrorGet *copies and clears* the global error as a side effect (verified
    // in RenderWare plcore/baerr.c: it reads the pair, then resets the global to
    // E_RW_NOERROR). RwErrorSet only stores when the global is currently NOERROR,
    // which it now is, so putting the pair straight back is exactly the state the
    // game would have seen: the monitor stays transparent. If the restore faults
    // the pair is lost to the game, but it is still reported below.
    RwErrorSetGuarded(error);

    const unsigned int pluginId = static_cast<unsigned int>(error.pluginID);
    const unsigned int errorCode = static_cast<unsigned int>(error.errorCode);

    // No error in the global this frame: nothing to report. {0, 0} and the
    // common E_RW_NOERROR are the same "clean" state.
    const bool noError =
        (pluginId == 0 && errorCode == 0) || (errorCode == kRwCommonErrorBit);

    if (!m_rwErrorPrimed)
    {
        // Report the very first observation even if it is a real error: an error
        // already in the global at startup is worth seeing, not discarding.
        m_rwErrorPrimed = true;
        m_rwLastPluginId = pluginId;
        m_rwLastErrorCode = errorCode;
        if (noError)
            return;
        // Fall through and record it.
    }
    else
    {
        if (pluginId == m_rwLastPluginId && errorCode == m_rwLastErrorCode)
            return;
        m_rwLastPluginId = pluginId;
        m_rwLastErrorCode = errorCode;
        if (noError)
            return;
    }

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
    unsigned long long key,
    const std::string& name,
    unsigned int pluginId,
    unsigned int errorCode,
    const std::vector<std::string>& modules,
    bool renderWare,
    const char* text
)
{
    // Bound the number of distinct records so a storm of unique messages cannot
    // grow the map without limit. Once full, new keys are dropped - and counted,
    // so the report can say how many were lost instead of silently truncating.
    constexpr size_t kMaxDistinctErrors = 128;

    // Take ONE consistent snapshot of the script state, before the lock. Calling
    // CurrentScriptName() and CurrentScriptOpcode() separately would read the CLEO
    // snapshot twice and could pair a script with another script's opcode; both
    // come from the same read here, so the name and the opcode always agree.
    //
    // A RenderWare error is noticed on the next frame's poll, so the script and
    // opcode active now are not the ones that raised it: leave the opcode unset
    // for those so the report does not print a misleading "last opcode" line.
    std::string scriptNow;
    DWORD opcodeNow = 0;
    CurrentScriptSnapshot(scriptNow, opcodeNow);
    const DWORD opcode = renderWare ? 0xFFFFFFFFu : opcodeNow;

    std::lock_guard<std::mutex> lock(m_errorMutex);

    auto it = m_errorRecords.find(key);
    if (it == m_errorRecords.end())
    {
        if (m_errorRecords.size() >= kMaxDistinctErrors)
        {
            ++m_errorRecordsDropped;
            return;
        }

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
        record.lastScript = scriptNow;
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
        record.lastScript = scriptNow;
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
    std::vector<std::pair<unsigned long long, DWORD>> reportedCounts;
    std::set<std::string> suspectModules;
    size_t changedCount = 0;
    {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        records.reserve(m_errorOrder.size());
        for (unsigned long long key : m_errorOrder)
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
        "cleo errors=%llu warnings=%llu | distinct=%zu | suspect modules=%zu%s",
        cleoErrors,
        cleoWarnings,
        records.size(),
        suspectModules.size(),
        m_errorRecordsDropped != 0
            ? " | DROPPED (table full): see note below"
            : ""
    );

    if (m_errorRecordsDropped != 0)
    {
        WriteFrameTime(
            "[errors] WARNING: %llu distinct error(s) were dropped because the "
            "record table (128) was full - the counts below are incomplete",
            m_errorRecordsDropped
        );
    }

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

    // Errors that are normal control flow, not a fault: E_RW_ENDOFSTREAM is
    // returned at the end of a stream by design, and the engine itself leaves it
    // in the global. Reporting it without saying so makes a harmless marker look
    // like a bug, so it is called out explicitly.
    auto isBenignError = [](const ErrorRecord& record)
    {
        return record.name == "E_RW_ENDOFSTREAM";
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

        if (isBenignError(record) && showDetail)
        {
            WriteFrameTime(
                "      note: BENIGN - E_RW_ENDOFSTREAM is the normal 'end of stream' "
                "marker the engine leaves in the global; it is not a fault and needs "
                "no fix."
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
                "RenderWare's texture read/find callback (RwTextureSetReadCallBack "
                "0x7F3500 / RwTextureSetFindCallBack 0x7F34D0) is a prime suspect - "
                "it can raise this for a texture the stock engine would load fine."
            );
            if (!m_textureWatchEnabled)
            {
                WriteFrameTime(
                    "      hint: set [DebugUtils.Errors] TextureWatch=1 to name the "
                    "mod that requested the failed texture read (the stack is "
                    "captured at the read, which this poll cannot do)."
                );
            }
            else
            {
                // TextureWatch is on, so the failed-read table below is the answer:
                // it distinguishes an engine-initiated read (a missing resource)
                // from a mod-initiated one (the mod asked for the wrong texture).
                WriteFrameTime(
                    "      hint: see the 'failed texture reads' table below - it "
                    "names the texture and says whether the engine or a mod "
                    "initiated the read."
                );
            }
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
        WriteFrameTime(
            "[errors] note: for a RenderWare row, count is how many times the polled "
            "{pluginID, errorCode} pair CHANGED (the pair is polled once per frame), "
            "not the number of errors raised - RenderWare keeps only the last error"
        );
        m_errorNotePrinted = true;
    }
}
