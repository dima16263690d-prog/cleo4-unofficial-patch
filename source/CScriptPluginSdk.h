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

        // Per-script plugin context. Every entry is owned by the calling module
        // (`caller`) and lives in that module's private namespace, so a key used
        // by one plugin can never be observed by another. Values are opaque to
        // CLEO; the owning plugin decides what they point to.
        void* GetScriptContext(CRunningScript* thread, uintptr_t key, HMODULE caller);
        bool SetScriptContext(CRunningScript* thread, uintptr_t key, void* value, HMODULE caller);

        // Lifetime hooks driven by the script engine / opcode system.
        void OnScriptDeleted(CRunningScript* thread);

        // Unified plugin resource registry. Anything a plugin allocates whose
        // lifetime should end with the plugin (opaque buffers, handles, ...) can
        // be registered here with a release callback. ReleaseModule invokes the
        // callback before the module is unloaded. `kind` is a borrowed tag used
        // only for diagnostics.
        typedef void (__stdcall *PluginResourceReleaseFn)(void* resource);

        // Register a resource for `owner`. Returns FALSE for null input or when
        // `resource` is already registered by that owner. Re-registering the
        // same pointer is rejected so cleanup stays idempotent.
        bool RegisterResource(HMODULE owner, const char* kind, void* resource,
                             PluginResourceReleaseFn release);

        // Remove a resource without invoking its release callback. Safe to call
        // for a resource that was never registered.
        bool UnregisterResource(HMODULE owner, void* resource);

        // Release everything a plugin module registered: opcode metadata,
        // per-script contexts, registered resources and event callbacks. Called
        // right before the module is unloaded so nothing can dangle into freed
        // code. Idempotent: a second call for the same module does nothing.
        void ReleaseModule(HMODULE module);

        // HMODULE that owns the address, or 0 when it cannot be resolved.
        HMODULE ModuleForAddress(const void* address);
    }
}
