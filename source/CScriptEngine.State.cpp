#include "stdafx.h"
#include "cleo.h"
#include "CCustomScript.h"
#include "ScmFunction.h"
#include "CScriptEngine.Internal.h"
#include "CScriptEngine.State.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace CLEO
{
    const unsigned CleoSafeHeader::sign = 0x31345653;
    const unsigned ChildSaveHeader::sign = 0x31484343; // CCH1
    const unsigned ChildSaveHeader::format_version = 1;
    const unsigned ScmFunctionSaveHeader::sign = 0x31465343; // CSF1
    const unsigned ScmFunctionSaveHeader::format_version = 1;

    CleoSafeHeader safe_header;
    ThreadSavingInfo *safe_info = nullptr;
    unsigned long *stopped_info = nullptr;
    std::unique_ptr<ThreadSavingInfo[]> safe_info_utilizer;
    std::unique_ptr<unsigned long[]> stopped_info_utilizer;
    std::vector<ChildThreadSavingInfo> pendingChildSaves;
    std::vector<ScmFunctionSaveInfo> pendingScmFunctionSaves;
    std::vector<bool> safeInfoUsed;

    SCRIPT_VAR CScriptEngine::CleoVariables[0x1000];

    // Preserve the legacy csN.sav layout (1024 shared variables) and store
    // the additional variables in a versioned sidecar file.
    static constexpr size_t kSavedCleoVarCount = 0x400;
    static constexpr size_t kTotalCleoVarCount = 0x1000;
    static constexpr size_t kExtendedCleoVarCount = kTotalCleoVarCount - kSavedCleoVarCount;
    static constexpr uint32_t kSharedVarsSaveSignature = 0x31565343; // "CSV1"
    static constexpr uint32_t kSharedVarsSaveVersion = 1;

template<typename T>
    void inline ReadBinary(std::istream& stream, T& value);

    template<typename T>
    void inline ReadBinary(std::istream& stream, T* value, size_t size);

    template<typename T>
    void inline WriteBinary(std::ostream& stream, const T& value);

    template<typename T>
    void inline WriteBinary(std::ostream& stream, const T* value, size_t size);

    inline int32_t SavePointerOffset(const BYTE *ptr, const BYTE *base)
    {
        if (!ptr)
            return INT32_MIN;

        return static_cast<int32_t>(
            reinterpret_cast<intptr_t>(ptr) - reinterpret_cast<intptr_t>(base)
        );
    }

    void SaveStringBinary(std::ostream& stream, const std::string& value)
    {
        uint32_t length = static_cast<uint32_t>(value.size());
        WriteBinary(stream, length);
        if (length)
            stream.write(value.data(), length);
    }

    bool LoadStringBinary(std::istream& stream, std::string& value)
    {
        uint32_t length = 0;
        ReadBinary(stream, length);

        // Refuse obviously corrupt/hostile lengths before allocating.
        if (length > 16 * 1024 * 1024)
            return false;

        value.resize(length);
        if (length)
            stream.read(&value[0], length);

        return stream.good();
    }

    void CollectScmFunctionSaves(
        CCustomScript *cs,
        unsigned nodeId,
        std::vector<ScmFunctionSaveInfo>& output)
    {
        if (!cs || nodeId == 0)
            return;

        std::vector<ScmFunction*> chain;
        std::set<WORD> visited;

        WORD id = cs->GetScmFunction();
        while (id < ScmFunction::store_size && ScmFunction::Store[id] && visited.insert(id).second)
        {
            chain.push_back(ScmFunction::Store[id]);
            id = ScmFunction::Store[id]->prevScmFunctionId;
        }

        std::reverse(chain.begin(), chain.end());

        const BYTE *base = cs->GetBasePointer();

        for (size_t depth = 0; depth < chain.size(); ++depth)
        {
            ScmFunction *fn = chain[depth];

            ScmFunctionSaveInfo saved{};
            saved.node_id = nodeId;
            saved.depth = static_cast<unsigned>(depth);
            saved.callArgCount = fn->callArgCount;
            saved.callOffset = SavePointerOffset(fn->callIP, base);
            saved.retnOffset = SavePointerOffset(fn->retnAddress, base);
            saved.savedBaseOffset = fn->savedBaseIP
                ? static_cast<int32_t>(
                    reinterpret_cast<intptr_t>(fn->savedBaseIP) -
                    reinterpret_cast<intptr_t>(base))
                : INT32_MIN;
            saved.savedCodeSize = static_cast<uint32_t>(fn->savedCodeSize);
            saved.savedSP = fn->savedSP;
            saved.savedCondResult = fn->savedCondResult;
            saved.savedLogicalOp = static_cast<int32_t>(fn->savedLogicalOp);
            saved.savedNotFlag = fn->savedNotFlag;
            saved.savedScriptFileDir = fn->savedScriptFileDir;
            saved.savedScriptFileName = fn->savedScriptFileName;

            for (size_t i = 0; i < 8; ++i)
                saved.savedStackOffsets[i] = SavePointerOffset(fn->savedStack[i], base);

            std::copy(fn->savedTls, fn->savedTls + 32, saved.savedTls);

            for (const auto& stringValue : fn->stringParams)
            {
                SavedScmStringParam stringParam{};
                stringParam.oldPointer = static_cast<DWORD>(
                    reinterpret_cast<uintptr_t>(stringValue.c_str()));
                stringParam.value = stringValue;
                saved.stringParams.push_back(std::move(stringParam));
            }

            output.push_back(std::move(saved));
        }
    }



    template<typename T>
    void inline ReadBinary(std::istream& s, T& buf)
    {
        s.read(reinterpret_cast<char *>(&buf), sizeof(T));
    }

    template<typename T>
    void inline ReadBinary(std::istream& s, T *buf, size_t size)
    {
        s.read(reinterpret_cast<char *>(buf), sizeof(T) * size);
    }

    template<typename T>
    void inline WriteBinary(std::ostream& s, const T& data)
    {
        s.write(reinterpret_cast<const char *>(&data), sizeof(T));
    }

    template<typename T>
    void inline WriteBinary(std::ostream& s, const T*data, size_t size)
    {
        s.write(reinterpret_cast<const char *>(data), sizeof(T) * size);
    }

    void CScriptEngine::LoadCustomScripts(bool load_mode)
    {
        char safe_name[MAX_PATH];
        bool baseSaveLoaded = false;

        // Never let extended shared variables leak across save slots or reloads.
        memset(CleoVariables, 0, sizeof(CleoVariables));

        // steam offset is different, so get it manually for now
        CGameVersionManager& gvm = GetInstance().VersionManager;
        int nSlot = gvm.GetGameVersion() != GV_STEAM ? *(BYTE*)&MenuManager->m_nSelectedSaveGame : *((BYTE*)MenuManager + 0x15B);

        sprintf(safe_name, "./cleo/cleo_saves/cs%d.sav", nSlot);

        safe_info = nullptr;
        stopped_info = nullptr;
        safe_header.n_saved_threads = safe_header.n_stopped_threads = 0;
        pendingChildSaves.clear();
        pendingScmFunctionSaves.clear();
        safeInfoUsed.clear();

        if (load_mode)
        {
            // load cleo saving file
            try
            {
                TRACE("Loading cleo safe %s", safe_name);
                std::ifstream ss(safe_name, std::ios::binary);
                if (ss.is_open())
                {
                    ss.exceptions(std::ios::eofbit | std::ios::badbit | std::ios::failbit);
                    ReadBinary(ss, safe_header);
                    if (safe_header.signature != CleoSafeHeader::sign)
                        throw std::runtime_error("Invalid file format");
                    safe_info = new ThreadSavingInfo[safe_header.n_saved_threads];
                    safe_info_utilizer.reset(safe_info);
                    stopped_info = new unsigned long[safe_header.n_stopped_threads];
                    stopped_info_utilizer.reset(stopped_info);
                    ReadBinary(ss, CleoVariables, kSavedCleoVarCount);
                    ReadBinary(ss, safe_info, safe_header.n_saved_threads);
                    ReadBinary(ss, stopped_info, safe_header.n_stopped_threads);
                    safeInfoUsed.assign(safe_header.n_saved_threads, false);
                    for (size_t i = 0; i < safe_header.n_stopped_threads; ++i)
                        InactiveScriptHashes.insert(stopped_info[i]);
                    baseSaveLoaded = true;
                    TRACE("Finished. Loaded %u cleo variables, %u saved threads info, %u stopped threads info",
                        (unsigned)kSavedCleoVarCount, safe_header.n_saved_threads, safe_header.n_stopped_threads);
                }
                else
                {
                    memset(CleoVariables, 0, sizeof(CleoVariables));
                }
            }
            catch (std::exception& ex)
            {
                TRACE("Loading of cleo safe %s failed: %s", safe_name, ex.what());
                safe_header.n_saved_threads = safe_header.n_stopped_threads = 0;
                memset(CleoVariables, 0, sizeof(CleoVariables));
            }
        }
        else
        {
            memset(CleoVariables, 0, sizeof(CleoVariables));
        }

        // Load extended shared variables only when the matching legacy save loaded
        // successfully. Old versions have no sidecar, so the already-cleared tail stays zero.
        if (load_mode && baseSaveLoaded)
        {
            char vars_safe_name[MAX_PATH];
            _snprintf_s(vars_safe_name, sizeof(vars_safe_name), _TRUNCATE,
                "./cleo/cleo_saves/cs%d.vars.sav", nSlot);
            try
            {
                std::ifstream varsFile(vars_safe_name, std::ios::binary);
                if (varsFile.is_open())
                {
                    varsFile.exceptions(std::ios::eofbit | std::ios::badbit | std::ios::failbit);
                    uint32_t signature = 0;
                    uint32_t version = 0;
                    uint32_t count = 0;
                    ReadBinary(varsFile, signature);
                    ReadBinary(varsFile, version);
                    ReadBinary(varsFile, count);
                    if (signature != kSharedVarsSaveSignature ||
                        version != kSharedVarsSaveVersion || count != kExtendedCleoVarCount)
                        throw std::runtime_error("Invalid extended shared-variable save format");

                    std::array<SCRIPT_VAR, kExtendedCleoVarCount> extendedVariables{};
                    ReadBinary(varsFile, extendedVariables.data(), extendedVariables.size());
                    std::copy(extendedVariables.begin(), extendedVariables.end(),
                        CleoVariables + kSavedCleoVarCount);
                    TRACE("Loaded %u extended CLEO shared variables from %s",
                        (unsigned)kExtendedCleoVarCount, vars_safe_name);
                }
            }
            catch (std::exception& ex)
            {
                memset(CleoVariables + kSavedCleoVarCount, 0,
                    sizeof(CleoVariables) - kSavedCleoVarCount * sizeof(CleoVariables[0]));
                TRACE("Loading extended shared variables %s failed: %s", vars_safe_name, ex.what());
            }
        }

        if (load_mode)
        {
            try
            {
                int nSlot = gvm.GetGameVersion() != GV_STEAM ? *(BYTE*)&MenuManager->m_nSelectedSaveGame : *((BYTE*)MenuManager + 0x15B);
                char child_safe_name[MAX_PATH];
                _snprintf_s(child_safe_name, sizeof(child_safe_name), _TRUNCATE,
                    "./cleo/cleo_saves/cs%d.children.sav", nSlot);

                std::ifstream cs(child_safe_name, std::ios::binary);
                if (cs.is_open())
                {
                    cs.exceptions(std::ios::eofbit | std::ios::badbit | std::ios::failbit);

                    ChildSaveHeader header{};
                    ReadBinary(cs, header);
                    if (header.signature != ChildSaveHeader::sign || header.version != ChildSaveHeader::format_version)
                        throw std::runtime_error("Invalid child save format");

                    pendingChildSaves.resize(header.n_children);
                    if (header.n_children)
                        ReadBinary(cs, pendingChildSaves.data(), header.n_children);

                    DIAG("[CLEO][LOAD][CUSTOM] loaded child states=%u file=%s", header.n_children, child_safe_name);
                }
            }
            catch (std::exception& ex)
            {
                pendingChildSaves.clear();
                DIAG("[CLEO][ERROR][LOAD] child state load failed: %s", ex.what());
            }
        }

        try
        {
            char function_safe_name[MAX_PATH];
            _snprintf_s(function_safe_name, sizeof(function_safe_name), _TRUNCATE,
                "./cleo/cleo_saves/cs%d.functions.sav", nSlot);

            std::ifstream fs(function_safe_name, std::ios::binary);
            if (fs.is_open())
            {
                fs.exceptions(std::ios::eofbit | std::ios::badbit | std::ios::failbit);

                ScmFunctionSaveHeader header{};
                ReadBinary(fs, header);
                if (header.signature != ScmFunctionSaveHeader::sign ||
                    header.version != ScmFunctionSaveHeader::format_version)
                    throw std::runtime_error("Invalid ScmFunction save format");

                pendingScmFunctionSaves.reserve(header.n_functions);

                for (unsigned i = 0; i < header.n_functions; ++i)
                {
                    ScmFunctionSaveInfo saved{};
                    uint32_t stringCount = 0;

                    ReadBinary(fs, saved.node_id);
                    ReadBinary(fs, saved.depth);
                    ReadBinary(fs, saved.callArgCount);
                    ReadBinary(fs, saved.callOffset);
                    ReadBinary(fs, saved.retnOffset);
                    ReadBinary(fs, saved.savedBaseOffset);
                    ReadBinary(fs, saved.savedCodeSize);
                    ReadBinary(fs, saved.savedStackOffsets, 8);
                    ReadBinary(fs, saved.savedSP);
                    ReadBinary(fs, saved.savedTls, 32);
                    ReadBinary(fs, saved.savedCondResult);
                    ReadBinary(fs, saved.savedLogicalOp);
                    ReadBinary(fs, saved.savedNotFlag);
                    if (!LoadStringBinary(fs, saved.savedScriptFileDir) ||
                        !LoadStringBinary(fs, saved.savedScriptFileName))
                        throw std::runtime_error("Invalid ScmFunction string state");

                    ReadBinary(fs, stringCount);
                    if (stringCount > 1024)
                        throw std::runtime_error("Invalid ScmFunction string count");

                    saved.stringParams.reserve(stringCount);
                    for (uint32_t s = 0; s < stringCount; ++s)
                    {
                        SavedScmStringParam param{};
                        ReadBinary(fs, param.oldPointer);
                        if (!LoadStringBinary(fs, param.value))
                            throw std::runtime_error("Invalid ScmFunction string parameter");
                        saved.stringParams.push_back(std::move(param));
                    }

                    pendingScmFunctionSaves.push_back(std::move(saved));
                }

                DIAG("[CLEO][LOAD][ScmFunction] loaded function states=%u file=%s",
                    header.n_functions, function_safe_name);
            }
        }
        catch (std::exception& ex)
        {
            pendingScmFunctionSaves.clear();
            DIAG("[CLEO][ERROR][LOAD] ScmFunction state load failed: %s", ex.what());
        }

        char cwd[MAX_PATH];
        _getcwd(cwd, sizeof(cwd));
        _chdir(cleo_dir);

        TRACE("Searching for cleo scripts");

        FilesWalk(cs_mask, [this](const char *filename) {
            auto cs = LoadScript(filename);
            if (cs)
                RestorePendingChildTree(cs);
        });
        FilesWalk(cs4_mask, [this](const char *filename) {
            auto cs = LoadScript(filename);
            if (cs)
            {
                cs->SetCompatibility(CLEO_VER_4);
                RestorePendingChildTree(cs);
            }
        });
        FilesWalk(cs3_mask, [this](const char *filename) {
            auto cs = LoadScript(filename);
            if (cs)
            {
                cs->SetCompatibility(CLEO_VER_3);
                RestorePendingChildTree(cs);
            }
        });

        _chdir(cwd);
    }

    CCustomScript * CScriptEngine::LoadScript(const char * szFilePath)
    {
        auto cs = new CCustomScript(szFilePath);

        if (!cs || !cs->cleoState.bOK)
        {
            TRACE("Loading of custom script %s failed", szFilePath);
            if (cs) delete cs;
            return nullptr;
        }

        // check whether the script is in stop-list
        if (stopped_info)
        {
            for (size_t i = 0; i < safe_header.n_stopped_threads; ++i)
            {
                if (stopped_info[i] == cs->cleoState.dwChecksum)
                {
                    TRACE("Custom script %s found in the stop-list", szFilePath);
                    InactiveScriptHashes.insert(stopped_info[i]);
                    delete cs;
                    return nullptr;
                }
            }
        }

        // check whether the script is in safe-list
        if (safe_info)
        {
            for (size_t i = 0; i < safe_header.n_saved_threads; ++i)
            {
                if (safeInfoUsed.size() > i && safeInfoUsed[i])
                    continue;

                if (safe_info[i].hash == cs->cleoState.dwChecksum)
                {
                    TRACE("Custom script %s found in the safe-list", szFilePath);
                    safe_info[i].Apply(cs);

                    if (safeInfoUsed.size() > i)
                    {
                        safeInfoUsed[i] = true;
                        cs->cleoState.savedNodeId = 0x80000000u | static_cast<unsigned>(i + 1);
                    }
                    break;
                }
            }
        }

        AddCustomScript(cs);
        RestorePendingScmFunctions(cs);
        return cs;
    }

    void CScriptEngine::SaveState()
    {
        try
        {
            std::list<CCustomScript *> savedThreads;
            std::for_each(CustomScripts.begin(), CustomScripts.end(), [this, &savedThreads](CCustomScript *cs) {
                if ((cs->cleoState.bSaveEnabled || !cs->childThreads.empty()) && cs->parentThread == nullptr)
                    savedThreads.push_back(cs);
            });

            CleoSafeHeader header = { CleoSafeHeader::sign, savedThreads.size(), InactiveScriptHashes.size() };

            // Assign stable node ids for this save operation. Root scripts use
            // the legacy saved-thread index; child scripts use a separate id
            // space stored only in the sidecar file.
            unsigned rootIndex = 0;
            for (auto cs : savedThreads)
                cs->cleoState.savedNodeId = 0x80000000u | (++rootIndex);

            std::vector<ChildThreadSavingInfo> childSaves;
            std::vector<ScmFunctionSaveInfo> functionSaves;
            unsigned nextChildNodeId = 1;

            auto collectChildren = [&](auto&& self, CCustomScript *parent, unsigned parentNodeId) -> void
            {
                for (auto child : parent->childThreads)
                {
                    unsigned ordinal = 0;
                    for (auto sibling : parent->childThreads)
                    {
                        if (sibling->childLabel != child->childLabel)
                            continue;
                        if (sibling == child)
                            break;
                        ++ordinal;
                    }

                    const unsigned nodeId = nextChildNodeId++;
                    child->cleoState.savedNodeId = nodeId;
                    childSaves.emplace_back(child, parentNodeId, nodeId, ordinal);
                    CollectScmFunctionSaves(child, nodeId, functionSaves);
                    self(self, child, nodeId);
                }
            };

            for (auto root : savedThreads)
            {
                CollectScmFunctionSaves(root, root->cleoState.savedNodeId, functionSaves);
                collectChildren(collectChildren, root, root->cleoState.savedNodeId);
            }

            // steam offset is different, so get it manually for now
            CGameVersionManager& gvm = GetInstance().VersionManager;
            int nSlot = gvm.GetGameVersion() != GV_STEAM ? *(BYTE*)&MenuManager->m_nSelectedSaveGame : *((BYTE*)MenuManager + 0x15B);

            char safe_name[MAX_PATH];
            char vars_safe_name[MAX_PATH];
            char child_safe_name[MAX_PATH];
            sprintf(safe_name, "./cleo/cleo_saves/cs%d.sav", nSlot);
            _snprintf_s(vars_safe_name, sizeof(vars_safe_name), _TRUNCATE,
                "./cleo/cleo_saves/cs%d.vars.sav", nSlot);
            _snprintf_s(child_safe_name, sizeof(child_safe_name), _TRUNCATE,
                "./cleo/cleo_saves/cs%d.children.sav", nSlot);
            TRACE("Saving script engine state to the file %s", safe_name);

            CreateDirectory("cleo", NULL);
            CreateDirectory("cleo/cleo_saves", NULL);

            // Save extended variables separately so the original csN.sav binary
            // layout remains compatible with existing save files.
            try
            {
                std::ofstream varsFile(vars_safe_name, std::ios::binary);
                if (varsFile.is_open())
                {
                    varsFile.exceptions(std::ios::failbit | std::ios::badbit);
                    const uint32_t signature = kSharedVarsSaveSignature;
                    const uint32_t version = kSharedVarsSaveVersion;
                    const uint32_t count = static_cast<uint32_t>(kExtendedCleoVarCount);
                    WriteBinary(varsFile, signature);
                    WriteBinary(varsFile, version);
                    WriteBinary(varsFile, count);
                    WriteBinary(varsFile, CleoVariables + kSavedCleoVarCount, kExtendedCleoVarCount);
                    TRACE("Saved %u extended CLEO shared variables to %s",
                        (unsigned)kExtendedCleoVarCount, vars_safe_name);
                }
                else
                {
                    TRACE("Failed to write extended shared-variable file '%s'!", vars_safe_name);
                }
            }
            catch (std::exception& ex)
            {
                TRACE("Saving extended shared variables %s failed: %s", vars_safe_name, ex.what());
            }

            std::ofstream ss(safe_name, std::ios::binary);
            if (ss.is_open())
            {
                ss.exceptions(std::ios::failbit | std::ios::badbit);

                WriteBinary(ss, header);
                WriteBinary(ss, CleoVariables, kSavedCleoVarCount);

                std::for_each(savedThreads.begin(), savedThreads.end(), [&savedThreads, &ss](CCustomScript *cs)
                {
                    ThreadSavingInfo savingInfo(cs);
                    WriteBinary(ss, savingInfo);
                });

                std::for_each(InactiveScriptHashes.begin(), InactiveScriptHashes.end(), [&ss](unsigned long hash) {
                    WriteBinary(ss, hash);
                });

                TRACE("Done. Saved %u cleo variables, %u saved threads, %u stopped threads",
                    (unsigned)kSavedCleoVarCount, header.n_saved_threads, header.n_stopped_threads);
            }
            else
            {
                TRACE("Failed to write save file '%s'!", safe_name);
            }

            try
            {
                std::ofstream childFile(child_safe_name, std::ios::binary);
                if (childFile.is_open())
                {
                    childFile.exceptions(std::ios::failbit | std::ios::badbit);

                    ChildSaveHeader childHeader = {
                        ChildSaveHeader::sign,
                        ChildSaveHeader::format_version,
                        childSaves.size()
                    };

                    WriteBinary(childFile, childHeader);
                    if (!childSaves.empty())
                        WriteBinary(childFile, childSaves.data(), childSaves.size());

                    DIAG("[CLEO][SAVE][CUSTOM] saved child states=%u file=%s",
                        childHeader.n_children, child_safe_name);
                }
                else
                {
                    DIAG("[CLEO][ERROR][SAVE] child state file write failed file=%s", child_safe_name);
                }
            }
            catch (std::exception& ex)
            {
                DIAG("[CLEO][ERROR][SAVE] child state save failed: %s", ex.what());
            }

            try
            {
                char function_safe_name[MAX_PATH];
                _snprintf_s(function_safe_name, sizeof(function_safe_name), _TRUNCATE,
                    "./cleo/cleo_saves/cs%d.functions.sav", nSlot);

                std::ofstream functionFile(function_safe_name, std::ios::binary);

                if (functionFile.is_open())
                {
                    functionFile.exceptions(std::ios::failbit | std::ios::badbit);

                    ScmFunctionSaveHeader functionHeader = {
                        ScmFunctionSaveHeader::sign,
                        ScmFunctionSaveHeader::format_version,
                        static_cast<unsigned>(functionSaves.size())
                    };

                    WriteBinary(functionFile, functionHeader);

                    for (const auto& saved : functionSaves)
                    {
                        WriteBinary(functionFile, saved.node_id);
                        WriteBinary(functionFile, saved.depth);
                        WriteBinary(functionFile, saved.callArgCount);
                        WriteBinary(functionFile, saved.callOffset);
                        WriteBinary(functionFile, saved.retnOffset);
                        WriteBinary(functionFile, saved.savedBaseOffset);
                        WriteBinary(functionFile, saved.savedCodeSize);
                        WriteBinary(functionFile, saved.savedStackOffsets, 8);
                        WriteBinary(functionFile, saved.savedSP);
                        WriteBinary(functionFile, saved.savedTls, 32);
                        WriteBinary(functionFile, saved.savedCondResult);
                        WriteBinary(functionFile, saved.savedLogicalOp);
                        WriteBinary(functionFile, saved.savedNotFlag);
                        SaveStringBinary(functionFile, saved.savedScriptFileDir);
                        SaveStringBinary(functionFile, saved.savedScriptFileName);

                        const uint32_t stringCount = static_cast<uint32_t>(saved.stringParams.size());
                        WriteBinary(functionFile, stringCount);
                        for (const auto& stringParam : saved.stringParams)
                        {
                            WriteBinary(functionFile, stringParam.oldPointer);
                            SaveStringBinary(functionFile, stringParam.value);
                        }
                    }

                    DIAG("[CLEO][SAVE][ScmFunction] saved function states=%u file=%s",
                        functionHeader.n_functions, function_safe_name);
                }
                else
                {
                    DIAG("[CLEO][ERROR][SAVE] ScmFunction state file write failed slot=%d", nSlot);
                }
            }
            catch (std::exception& ex)
            {
                DIAG("[CLEO][ERROR][SAVE] ScmFunction state save failed: %s", ex.what());
            }
        }
        catch (std::exception& ex)
        {
            TRACE("Saving failed. %s", ex.what());
        }
    }

}
