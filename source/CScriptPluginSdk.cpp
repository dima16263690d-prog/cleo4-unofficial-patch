#include "stdafx.h"
#include "cleo.h"
#include "CScriptPluginSdk.h"
#include "CCustomOpcodeSystem.h"
#include "CDebugBridge.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace CLEO
{
    namespace PluginSdk
    {
        // Size in bytes of one parameter, including its type byte.
        static int SkipParam(const BYTE* ip);

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

        // Per-script context maps, keyed by plugin module then by plugin key.
        // The map only holds entries for scripts that actually use contexts.
        static std::mutex g_mutex;
        static std::unordered_map<WORD, OpcodeRecord> g_opcodes;
        static std::unordered_map<uintptr_t,
            std::unordered_map<uintptr_t, void*>> g_scriptContexts;

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

                if (count >= maxOut)
                    break;

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

        bool ValidateParams(CRunningScript* thread, const char* signature)
        {
            if (thread == nullptr || signature == nullptr)
                return false;

            CLEO_OpcodeParamKind kinds[16] = {};
            const int count = ParseSignature(signature, kinds, 16);
            if (count < 0)
                return false; // malformed signature

            const BYTE* ip = thread->GetBytePointer();
            for (int i = 0; i < count; ++i)
            {
                const int type = *ip;
                if (type == DT_END)
                    return false;

                // An 'any' wildcard accepts the type but still consumes the
                // parameter, so the following ones stay aligned.
                if (kinds[i] != CLEO_OPARAM_ANY && ClassifyType(type) != kinds[i])
                    return false;

                ip += SkipParam(ip);
            }

            return true;
        }

        static int SkipParam(const BYTE* ip)
        {
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

            auto scriptIt = g_scriptContexts.find(script);
            if (scriptIt == g_scriptContexts.end())
                return nullptr;

            auto valueIt = scriptIt->second.find(resolved);
            return valueIt == scriptIt->second.end() ? nullptr : valueIt->second;
        }

        bool SetScriptContext(CRunningScript* thread, uintptr_t key, void* value, HMODULE caller)
        {
            if (thread == nullptr)
                return false;

            const uintptr_t resolved = ResolveKey(key, caller);
            const uintptr_t script = reinterpret_cast<uintptr_t>(thread);

            std::lock_guard<std::mutex> lock(g_mutex);

            if (value == nullptr)
            {
                auto scriptIt = g_scriptContexts.find(script);
                if (scriptIt != g_scriptContexts.end())
                {
                    scriptIt->second.erase(resolved);
                    if (scriptIt->second.empty())
                        g_scriptContexts.erase(scriptIt);
                }
                return true;
            }

            g_scriptContexts[script][resolved] = value;
            return true;
        }

        void OnScriptDeleted(CRunningScript* thread)
        {
            if (thread == nullptr)
                return;

            const uintptr_t script = reinterpret_cast<uintptr_t>(thread);

            std::lock_guard<std::mutex> lock(g_mutex);
            g_scriptContexts.erase(script);
        }

        void ReleaseModule(HMODULE module)
        {
            if (module == nullptr)
                return;

            std::lock_guard<std::mutex> lock(g_mutex);

            for (auto it = g_opcodes.begin(); it != g_opcodes.end(); )
            {
                if (it->second.module == module)
                    it = g_opcodes.erase(it);
                else
                    ++it;
            }

            const uintptr_t moduleKey = reinterpret_cast<uintptr_t>(module);
            for (auto it = g_scriptContexts.begin(); it != g_scriptContexts.end(); )
            {
                it->second.erase(moduleKey);
                if (it->second.empty())
                    it = g_scriptContexts.erase(it);
                else
                    ++it;
            }
        }
    }
}
