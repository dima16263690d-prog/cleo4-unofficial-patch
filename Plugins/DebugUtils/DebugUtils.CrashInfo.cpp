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

void DebugUtils::EnsureCrashInfoDatabase()
{
    const std::string pluginCrashInfoDir = "cleo\\cleo_plugins\\CrashInfo\\";
    const std::string pluginCrashInfoPath = pluginCrashInfoDir + "CLEO-CrashList.txt";
    const std::string pluginCrashInfoRuPath =
        pluginCrashInfoDir + "CLEO-CrashList-RU.txt";
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

    bool russianDatabaseReady = false;
    {
        std::ifstream file(pluginCrashInfoRuPath);
        if (file.is_open())
        {
            char header[512] = {};
            file.read(header, sizeof(header) - 1);
            header[file.gcount()] = '\0';
            russianDatabaseReady =
                strstr(header, "# DebugUtils-RU-Database-Version: 1") != nullptr;
        }
    }

    if (!russianDatabaseReady)
    {
        HMODULE module = nullptr;
        if (GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(&DebugUtils::s_instance),
                &module))
        {
            HRSRC resource = FindResourceA(
                module,
                MAKEINTRESOURCEA(IDR_CRASHINFO_RU),
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
                        pluginCrashInfoRuPath.c_str(),
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

                        WriteCore(
                            "[crashinfo] Russian localization resource %s path=%s",
                            ok && written == resourceSize ? "installed" : "failed",
                            pluginCrashInfoRuPath.c_str()
                        );
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

        if (line.rfind("Last command:", 0) == 0 ||
            line.rfind("Last Command:", 0) == 0)
        {
            scriptSection = false;

            if (!beginEntry())
                break;

            current->lastCommands.clear();
            // SCRLog commands are written as [0001] or [0B10]; the plain
            // "0x" hex scan alone would miss every one of them.
            ExtractHexAddresses(line, 0, current->lastCommands);
            ExtractBracketedOpcodes(line, 0, current->lastCommands);

            if (!current->lastCommands.empty())
            {
                // Keep the "Last command [0x....]" label only when the entry
                // matches a single opcode. Multi-opcode command groups get a
                // generic name and rely on the lastCommands list for matching.
                if (current->lastCommands.size() == 1)
                {
                    current->lastOpcode = current->lastCommands.front();
                    char name[64] = {};
                    sprintf_s(
                        name, sizeof(name),
                        "Last command [0x%04X]",
                        current->lastOpcode & 0x7FFF
                    );
                    current->name = name;
                }
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

        // "Mod:" names the mod a script-related entry belongs to. It is folded
        // into the description so the crash dialog can show it; the issue text
        // stays authoritative for the diagnosis.
        if (line.rfind("Mod: ", 0) == 0)
        {
            if (!current->description.empty())
                current->description += " | ";
            current->description += line;
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

        // "Stack: 0x4CDCDE > Error" maps a stack address to the entry, the same
        // way the CrashInfo web tool matches the SCRLog stack. Treat it as a
        // backtrace address so the matcher can use it.
        if (line.rfind("Stack:", 0) == 0)
        {
            ExtractHexAddresses(line, 6, current->backtraceAddresses);
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

    // Remove only byte-for-byte equivalent diagnostic records. Different
    // solutions for the same fault address remain separate entries.
    {
        std::set<std::string> seen;
        m_crashInfo.erase(
            std::remove_if(
                m_crashInfo.begin(),
                m_crashInfo.end(),
                [&seen](const CrashInfoEntry& entry)
                {
                    std::string key;
                    key.reserve(512);

                    auto appendVector = [&key](const std::vector<DWORD>& values)
                    {
                        for (DWORD value : values)
                        {
                            char part[16] = {};
                            sprintf_s(part, sizeof(part), "%08X,", value);
                            key += part;
                        }
                        key += ";";
                    };

                    auto appendStringVector =
                        [&key](const std::vector<std::string>& values)
                    {
                        for (const auto& value : values)
                        {
                            key += value;
                            key += ",";
                        }
                        key += ";";
                    };

                    appendVector(entry.errorAddresses);
                    appendStringVector(entry.errorModules);
                    appendVector(entry.backtraceAddresses);
                    appendStringVector(entry.backtraceModules);
                    appendVector(entry.lastCommands);

                    key += entry.scriptName;
                    key += "|";
                    key += entry.name;
                    key += "|";
                    key += entry.issue;
                    key += "|";
                    key += entry.about;
                    key += "|";
                    key += entry.solution;

                    if (entry.exceptionCode != 0)
                    {
                        char value[16] = {};
                        sprintf_s(
                            value,
                            sizeof(value),
                            "%08X",
                            entry.exceptionCode
                        );
                        key += "|EX=";
                        key += value;
                    }

                    key += "|AT=";
                    key += std::to_string(entry.accessType);
                    key += "|OP=";
                    key += std::to_string(entry.lastOpcode);

                    if (seen.insert(key).second)
                        return false;

                    return true;
                }
            ),
            m_crashInfo.end()
        );
    }

    LoadCrashInfoLocalization();

    WriteCore(
        "[crashinfo] loaded verified entries=%u unique=%u path=%s",
        static_cast<unsigned>(loadedEntries),
        static_cast<unsigned>(m_crashInfo.size()),
        path.c_str()
    );
}

void DebugUtils::LoadCrashInfoLocalization()
{
    if (!m_languageRussian)
        return;

    const std::string path = CrashInfoRuPath();
    std::ifstream file(path);
    if (!file.is_open())
    {
        WriteCore(
            "[crashinfo] Russian localization unavailable path=%s; English text will be used",
            path.c_str()
        );
        return;
    }

    CrashInfoEntry localized{};
    size_t localizedEntries = 0;

    auto applyEntry = [this, &localized, &localizedEntries]()
    {
        if (localized.errorAddresses.empty() &&
            localized.scriptName.empty())
        {
            localized = {};
            return;
        }

        CrashInfoEntry* target = nullptr;

        for (auto& entry : m_crashInfo)
        {
            bool addressMatch = false;

            for (DWORD expected : localized.errorAddresses)
            {
                if (ContainsAddress(entry.errorAddresses, expected))
                {
                    addressMatch = true;
                    break;
                }
            }

            const bool scriptMatch =
                !localized.scriptName.empty() &&
                !_stricmp(
                    localized.scriptName.c_str(),
                    entry.scriptName.c_str());

            if (addressMatch || scriptMatch)
            {
                target = &entry;
                break;
            }
        }

        if (target != nullptr)
        {
            if (!localized.name.empty())
                target->name = localized.name;
            if (!localized.issue.empty())
                target->issue = localized.issue;
            if (!localized.about.empty())
                target->about = localized.about;
            if (!localized.solution.empty())
                target->solution = localized.solution;
            ++localizedEntries;
        }

        localized = {};
    };

    std::string line;
    while (std::getline(file, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        if (line.rfind("Error: ", 0) == 0)
        {
            applyEntry();
            ExtractHexAddresses(line, 7, localized.errorAddresses);
            continue;
        }

        if (line.rfind("Last script: ", 0) == 0)
        {
            localized.scriptName = line.substr(13);
            continue;
        }

        if (line.rfind("Name: ", 0) == 0)
        {
            localized.name = line.substr(6);
            continue;
        }

        if (line.rfind("Problem: ", 0) == 0)
        {
            localized.issue = line.substr(9);
            continue;
        }

        if (line.rfind("Issue: ", 0) == 0)
        {
            localized.issue = line.substr(7);
            continue;
        }

        if (line.rfind("About: ", 0) == 0)
        {
            localized.about = line.substr(7);
            continue;
        }

        if (line.rfind("Solution: ", 0) == 0)
        {
            localized.solution = line.substr(10);
            continue;
        }
    }

    applyEntry();

    WriteCore(
        "[crashinfo] Russian localization applied entries=%u path=%s",
        static_cast<unsigned>(localizedEntries),
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

        // Address 0 is a special null-instruction state. Do not let an
        // old/generic "0x00000000" database record masquerade as a normal
        // exact-address crash match. Context/module matching can still work.
        const bool exactError =
            address != 0 && ContainsAddress(entry.errorAddresses, address);
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

            // Module matches are counted separately from address matches:
            // reusing backtraceMatches would make the module threshold depend
            // on how many address frames already matched.
            int moduleMatches = 0;

            for (const auto& expected : entry.backtraceModules)
            {
                if (!ContainsModule(backtraceModules, expected))
                    continue;

                ++moduleMatches;
            }

            const int requiredModules =
                static_cast<int>(entry.backtraceModules.size());

            if (moduleMatches < requiredModules)
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

        if (entry.lastOpcode != 0xFFFFFFFFu)
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
            entry.lastOpcode != 0xFFFFFFFFu ||
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
