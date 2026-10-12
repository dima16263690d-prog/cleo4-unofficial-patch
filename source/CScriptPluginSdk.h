#pragma once

// Plugin SDK 2.0 core.
//
// Provides the opcode metadata index, parameter validation, error reporting,
// per-script plugin contexts and module ownership bookkeeping that back the
// public CLEO_*Ex entry points. It sits on top of the existing CLEO 4 opcode
// registry; it never replaces the legacy handler tables.

#include "CCustomOpcodeSystem.h"
#include "CTheScripts.h"
#include "../cleo_sdk/CLEO_Debug.h"
#include <windows.h>
#include <cstdint>

namespace CLEO
{
    namespace PluginSdk
    {
        // Register an opcode and remember its metadata. The handler tables
        // (lowOpcodeHandlers / extraOpcodeHandlers) remain the single source of
        // truth for dispatch; this only records who owns the opcode and how its
        // parameters are shaped. Returns FALSE when the slot is already taken.
        bool RegisterOpcode(WORD opcode, CustomOpcodeHandler callback,
                            const char* name, const char* params,
                            const char* description, HMODULE module,
                            bool legacy = false);

        // Copy metadata for one opcode into a caller-provided CLEO_OpcodeInfo.
        bool GetOpcodeInfo(WORD opcode, CLEO_OpcodeInfo* info);

        bool ValidateParams(CRunningScript* thread, const char* signature);

        int ParseSignature(const char* signature, CLEO_OpcodeParamKind* out, int maxOut);

        void ReportError(const char* message);

        // Per-script plugin context. `caller` is the module of the plugin that
        // invoked the public entry point; it disambiguates keys between plugins.
        void* GetScriptContext(CRunningScript* thread, uintptr_t key, HMODULE caller);
        bool SetScriptContext(CRunningScript* thread, uintptr_t key, void* value, HMODULE caller);

        // Lifetime hooks driven by the script engine / opcode system.
        void OnScriptDeleted(CRunningScript* thread);

        // Release everything a plugin module registered. Called right before
        // the module is unloaded so its metadata and script contexts never
        // dangle into freed code.
        void ReleaseModule(HMODULE module);

        // HMODULE that owns the address, or 0 when it cannot be resolved.
        HMODULE ModuleForAddress(const void* address);
    }
}
