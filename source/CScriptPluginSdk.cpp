#include "stdafx.h"
#include "cleo.h"
#include "CScriptPluginSdk.h"
#include "CCustomOpcodeSystem.h"
#include "CCustomOpcodeSystem.Internal.h"
#include "CDebugBridge.h"

#include "CDebugCallbackSystem.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace CLEO
{
    namespace PluginSdk
    {
        // Size of GTA SA's global script space (CTheScripts::ScriptSpace). Native
        // SCM threads and mission code execute inside this single buffer.
        static const size_t SCM_BLOCK_SIZE = 200000;

        // Compute the byte range that the parameter stream of `thread` lives in.
        // CLEO custom scripts own their code buffer; child scripts and mission
        // scripts share a parent/mission buffer. Native SCM threads run in the
        // global script space. Returns false when no reliable range is known.
        static bool GetScriptParamRange(CRunningScript* thread, const BYTE** begin, const BYTE** end);

        // Size in bytes of one parameter, including its type byte. `end` bounds
        // the read; returns <= 0 when the parameter is invalid or does not fit.
        static int SkipParam(const BYTE* ip, const BYTE* end);

        // One recorded opcode. String pointers are borrowed from the
        // registering plugin (normally string literals).
        struct OpcodeRecord
        {
            const char* name = nullptr;
            const char* params = nullptr;
            const char* description = nullptr;
            uintptr_t handler = 0;
            HMODULE module = nullptr;
            bool legacy = false;
        };

        // One opaque resource a plugin registered for cleanup on unload.
        struct ResourceRecord
        {
            const char* kind = nullptr;
            PluginResourceReleaseFn release = nullptr;
        };

        // Per-script contexts, keyed by script then by owning module then by
        // plugin key. Keying the owner explicitly (rather than deriving it from
        // the user key) is what lets ReleaseModule drop every entry a plugin
        // created, including entries that used arbitrary non-zero keys.
        static std::mutex g_mutex;
        static std::unordered_map<WORD, OpcodeRecord> g_opcodes;
        static std::unordered_map<uintptr_t,
            std::unordered_map<HMODULE,
                std::unordered_map<uintptr_t, void*>>> g_scriptContextsByScript;

        // Unified per-module resource registry: owner module -> resource -> how
        // to release it. Populated through RegisterResource and drained by
        // ReleaseModule before the owning DLL is unmapped.
        static std::unordered_map<HMODULE,
            std::unordered_map<void*, ResourceRecord>> g_resources;

        // Modules already released. Makes ReleaseModule idempotent even for
        // modules that own no resources, and stops a stale handle from being
        // reused if the loader later maps a different DLL at the same address.
        static std::unordered_set<HMODULE> g_releasedModules;

        HMODULE ModuleForAddress(const void* address)
        {
            if (address == nullptr)
                return nullptr;

            HMODULE module = nullptr;
            if (GetModuleHandleExA(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCSTR>(address),
                    &module))
            {
                return module;
            }

            return nullptr;
        }

        static uintptr_t ResolveKey(uintptr_t key, HMODULE module)
        {
            if (key != 0)
                return key;
            return reinterpret_cast<uintptr_t>(module);
        }

        bool RegisterOpcode(WORD opcode, CustomOpcodeHandler callback,
                            const char* name, const char* params,
                            const char* description, HMODULE module, bool legacy)
        {
            if (callback == nullptr || opcode > 0x7FFF)
                return false;

            std::lock_guard<std::mutex> lock(g_mutex);

            auto it = g_opcodes.find(opcode);
            if (it != g_opcodes.end() && it->second.handler != 0)
                return false;

            OpcodeRecord& record = g_opcodes[opcode];
            record.name = name;
            record.params = params;
            record.description = description;
            record.handler = reinterpret_cast<uintptr_t>(callback);
            record.module = module;
            record.legacy = legacy;
            return true;
        }

        bool GetOpcodeInfo(WORD opcode, CLEO_OpcodeInfo* info)
        {
            if (info == nullptr)
                return false;

            std::lock_guard<std::mutex> lock(g_mutex);

            auto it = g_opcodes.find(opcode);
            if (it == g_opcodes.end() || it->second.handler == 0)
                return false;

            const OpcodeRecord& record = it->second;

            memset(info, 0, sizeof(*info));
            info->opcode = opcode;
            info->name = record.name;
            info->params = record.params;
            info->description = record.description;
            info->handler = record.handler;
            info->pluginModule = reinterpret_cast<DWORD>(record.module);
            info->legacy = record.legacy ? TRUE : FALSE;

            CLEO_OpcodeParamKind kinds[16] = {};
            const int count = ParseSignature(record.params, kinds, 16);
            if (count > 0)
            {
                info->paramCount = static_cast<WORD>(count);
                for (int i = 0; i < count; ++i)
                    info->paramKind[i] = kinds[i];
            }

            return true;
        }

        int ParseSignature(const char* signature, CLEO_OpcodeParamKind* out, int maxOut)
        {
            if (signature == nullptr || out == nullptr || maxOut <= 0)
                return -1;

            int count = 0;
            for (const char* p = signature; *p != '\0'; ++p)
            {
                CLEO_OpcodeParamKind kind;
                switch (*p)
                {
                case 'i': kind = CLEO_OPARAM_INT; break;
                case 'f': kind = CLEO_OPARAM_FLOAT; break;
                case 's': kind = CLEO_OPARAM_STRING; break;
                case 'v': kind = CLEO_OPARAM_VAR; break;
                case '*': kind = CLEO_OPARAM_ANY; break;
                case ' ': case '\t': case ',': case '|': case '-':
                case '0': case '1': case '2': case '3': case '4':
                case '5': case '6': case '7': case '8': case '9':
                    continue; // separators and counts are cosmetic
                default:
                    return -1; // unknown signature character
                }

                // Never silently truncate a signature: callers must know
                // whether every declared parameter was parsed and validated.
                if (count >= maxOut)
                    return -1;

                out[count++] = kind;
            }

            return count;
        }

        static CLEO_OpcodeParamKind ClassifyType(int type)
        {
            switch (type)
            {
            case DT_DWORD:
            case DT_BYTE:
            case DT_WORD:
                return CLEO_OPARAM_INT;
            case DT_FLOAT:
                return CLEO_OPARAM_FLOAT;
            case DT_STRING:
            case DT_TEXTLABEL:
            case DT_VARLEN_STRING:
                return CLEO_OPARAM_STRING;
            case DT_VAR:
            case DT_LVAR:
            case DT_VAR_ARRAY:
            case DT_LVAR_ARRAY:
            case DT_VAR_TEXTLABEL:
            case DT_LVAR_TEXTLABEL:
            case DT_VAR_TEXTLABEL_ARRAY:
            case DT_LVAR_TEXTLABEL_ARRAY:
            case DT_VAR_STRING:
            case DT_LVAR_STRING:
            case DT_VAR_STRING_ARRAY:
            case DT_LVAR_STRING_ARRAY:
                return CLEO_OPARAM_VAR;
            default:
                return CLEO_OPARAM_ANY;
            }
        }

        static bool GetScriptParamRange(CRunningScript* thread, const BYTE** begin, const BYTE** end)
        {
            const BYTE* base = thread->GetBasePointer();
            if (base == nullptr)
                return false;

            // Custom scripts (and their label children) know their code size.
            auto* custom = GetInstance().ScriptEngine.FindCustomScriptByPtr(thread);
            if (custom != nullptr)
            {
                const size_t size = custom->GetCodeSize();
                if (size == 0)
                    return false; // unknown size: do not guess a range

                *begin = base;
                *end = base + size;
                return true;
            }

            // A native SCM thread runs in the global script space.
            if (thread->IsExternal() && scmBlock != nullptr)
            {
                *begin = scmBlock;
                *end = scmBlock + SCM_BLOCK_SIZE;
                return true;
            }

            // Mission threads and anything else: no range we can trust.
            return false;
        }

        bool ValidateParams(CRunningScript* thread, const char* signature)
        {
            if (thread == nullptr || signature == nullptr)
                return false;

            // 16 is the documented parameter limit for a CLEO opcode.
            CLEO_OpcodeParamKind kinds[16] = {};
            const int count = ParseSignature(signature, kinds, 16);
            if (count < 0)
                return false; // malformed signature
            if (count > 16)
                return false; // signature exceeds the supported parameter limit

            const BYTE* begin = nullptr;
            const BYTE* end = nullptr;
            if (!GetScriptParamRange(thread, &begin, &end))
                return false; // no trustworthy range: refuse rather than guess

            const BYTE* ip = thread->GetBytePointer();
            for (int i = 0; i < count; ++i)
            {
                // The type byte itself must lie inside the script buffer.
                if (ip < begin || ip >= end)
                    return false;

                const int type = *ip;
                if (type == DT_END)
                    return false;

                // An 'any' wildcard accepts the type but still consumes the
                // parameter, so the following ones stay aligned.
                if (kinds[i] != CLEO_OPARAM_ANY && ClassifyType(type) != kinds[i])
                    return false;

                const int size = SkipParam(ip, end);
                if (size <= 0)
                    return false; // operand runs past the buffer or is invalid

                // Advance and confirm the whole operand stayed in range. This
                // also stops a corrupted length byte from walking off the end.
                if (size > end - ip)
                    return false;

                ip += size;
            }

            return true;
        }

        // Size in bytes of one parameter, including its type byte. `end` bounds
        // the read so a corrupted length byte cannot run past the buffer.
        // Returns <= 0 when the parameter is invalid or does not fit.
        static int SkipParam(const BYTE* ip, const BYTE* end)
        {
            const BYTE* limit = end ? end : ip + 1; // 1 = only the type byte
            if (ip >= limit)
                return 0;

            const int type = *ip;
            switch (type)
            {
            case DT_VAR:
            case DT_LVAR:
            case DT_VAR_STRING:
            case DT_LVAR_STRING:
            case DT_VAR_TEXTLABEL:
            case DT_LVAR_TEXTLABEL:
                return 3;
            case DT_VAR_ARRAY:
            case DT_LVAR_ARRAY:
            case DT_VAR_STRING_ARRAY:
            case DT_LVAR_STRING_ARRAY:
            case DT_VAR_TEXTLABEL_ARRAY:
            case DT_LVAR_TEXTLABEL_ARRAY:
                return 7;
            case DT_BYTE:
                return 2;
            case DT_WORD:
                return 3;
            case DT_DWORD:
            case DT_FLOAT:
                return 5;
            case DT_VARLEN_STRING:
                // type byte + length byte + payload; both header bytes must fit.
                if (ip + 1 >= limit)
                    return 0;
                return 2 + ip[1];
            case DT_TEXTLABEL:
                return 9;
            case DT_STRING:
                return 17;
            default:
                return 1;
            }
        }

        void ReportError(const char* message)
        {
            CLEO_DebugLog(CLEO_DEBUG_ERROR, "[plugin] %s", message ? message : "");
        }

        void* GetScriptContext(CRunningScript* thread, uintptr_t key, HMODULE caller)
        {
            if (thread == nullptr)
                return nullptr;

            const uintptr_t resolved = ResolveKey(key, caller);
            const uintptr_t script = reinterpret_cast<uintptr_t>(thread);

            std::lock_guard<std::mutex> lock(g_mutex);

            auto scriptIt = g_scriptContextsByScript.find(script);
            if (scriptIt == g_scriptContextsByScript.end())
                return nullptr;

            auto ownerIt = scriptIt->second.find(caller);
            if (ownerIt == scriptIt->second.end())
                return nullptr;

            auto valueIt = ownerIt->second.find(resolved);
            return valueIt == ownerIt->second.end() ? nullptr : valueIt->second;
        }

        bool SetScriptContext(CRunningScript* thread, uintptr_t key, void* value, HMODULE caller)
        {
            if (thread == nullptr)
                return false;

            const uintptr_t resolved = ResolveKey(key, caller);
            const uintptr_t script = reinterpret_cast<uintptr_t>(thread);

            std::lock_guard<std::mutex> lock(g_mutex);

            auto scriptIt = g_scriptContextsByScript.find(script);
            if (value == nullptr)
            {
                if (scriptIt == g_scriptContextsByScript.end())
                    return true;

                auto ownerIt = scriptIt->second.find(caller);
                if (ownerIt != scriptIt->second.end())
                {
                    ownerIt->second.erase(resolved);
                    if (ownerIt->second.empty())
                        scriptIt->second.erase(ownerIt);
                }
                if (scriptIt->second.empty())
                    g_scriptContextsByScript.erase(scriptIt);
                return true;
            }

            g_scriptContextsByScript[script][caller][resolved] = value;
            return true;
        }

        void OnScriptDeleted(CRunningScript* thread)
        {
            if (thread == nullptr)
                return;

            const uintptr_t script = reinterpret_cast<uintptr_t>(thread);

            std::lock_guard<std::mutex> lock(g_mutex);
            g_scriptContextsByScript.erase(script);
        }

        bool RegisterResource(HMODULE owner, const char* kind, void* resource,
                              PluginResourceReleaseFn release)
        {
            if (owner == nullptr || resource == nullptr)
                return false;

            std::lock_guard<std::mutex> lock(g_mutex);

            if (g_releasedModules.find(owner) != g_releasedModules.end())
                return false; // plugin already unloaded

            auto& bucket = g_resources[owner];
            if (bucket.find(resource) != bucket.end())
                return false; // already registered; keep cleanup idempotent

            bucket[resource] = ResourceRecord{kind, release};
            return true;
        }

        bool UnregisterResource(HMODULE owner, void* resource)
        {
            if (owner == nullptr || resource == nullptr)
                return false;

            std::lock_guard<std::mutex> lock(g_mutex);

            auto moduleIt = g_resources.find(owner);
            if (moduleIt == g_resources.end())
                return false;

            const size_t removed = moduleIt->second.erase(resource);
            if (moduleIt->second.empty())
                g_resources.erase(moduleIt);
            return removed != 0;
        }

        void ReleaseModule(HMODULE module)
        {
            if (module == nullptr)
                return;

            // Drain the resource registry and collect the release callbacks
            // first: invoking plugin code while holding g_mutex would deadlock
            // if the callback called back into the SDK.
            std::vector<std::pair<PluginResourceReleaseFn, void*>> releases;
            // Opcodes owned by this module, so their dispatch slots can be
            // cleared after the lock is dropped (see UnregisterOpcodeSlot).
            std::vector<WORD> ownedOpcodes;
            bool alreadyReleased = false;

            {
                std::lock_guard<std::mutex> lock(g_mutex);

                if (!g_releasedModules.insert(module).second)
                    alreadyReleased = true;

                if (!alreadyReleased)
                {
                    auto resourceIt = g_resources.find(module);
                    if (resourceIt != g_resources.end())
                    {
                        for (auto& entry : resourceIt->second)
                        {
                            if (entry.second.release != nullptr)
                                releases.emplace_back(entry.second.release, entry.first);
                        }
                        g_resources.erase(resourceIt);
                    }

                    for (auto it = g_opcodes.begin(); it != g_opcodes.end(); )
                    {
                        if (it->second.module == module)
                        {
                            ownedOpcodes.push_back(it->first);
                            it = g_opcodes.erase(it);
                        }
                        else
                            ++it;
                    }

                    for (auto it = g_scriptContextsByScript.begin(); it != g_scriptContextsByScript.end(); )
                    {
                        it->second.erase(module);
                        if (it->second.empty())
                            it = g_scriptContextsByScript.erase(it);
                        else
                            ++it;
                    }
                }
            }

            if (alreadyReleased)
                return;

            // Clear the dispatch slots so a dangling handler into the unmapped
            // module can never be called and the opcode can be re-registered.
            for (WORD opcode : ownedOpcodes)
                UnregisterOpcodeSlot(opcode);

            for (auto& release : releases)
                release.first(release.second);

            // Event subscriptions are owned by the callback system; ask it to
            // drop this module's callbacks too. Also idempotent.
            ReleaseCallbacksForModule(module);
        }
    }
}
