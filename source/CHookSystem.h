#pragma once

#include "CCodeInjector.h"

#include <cstddef>
#include <string>
#include <vector>

namespace CLEO
{
    enum HookType
    {
        HOOK_CALL = 0,
        HOOK_JUMP = 1,
        HOOK_POINTER = 2
    };

    struct HookRecord
    {
        HookType type = HOOK_CALL;
        size_t address = 0;
        size_t replacement = 0;
        size_t originalTarget = 0;
        size_t originalValue = 0;
        BYTE originalBytes[5] = {};
        std::string name;
    };

    // First-generation hook manager.
    // It manages the existing 5-byte CALL/JMP patch model used by CLEO 4.
    // It does not replace the legacy injection implementation yet.
    class CHookSystem final
    {
        std::vector<HookRecord> m_hooks;

        HookRecord* Find(size_t address);
        const HookRecord* Find(size_t address) const;

        bool InstallRaw(
            CCodeInjector& injector,
            const char* name,
            HookType type,
            memory_pointer address,
            size_t replacement,
            size_t* originalTarget
        );

    public:
        bool InstallCall(
            CCodeInjector& injector,
            const char* name,
            memory_pointer address,
            size_t replacement,
            size_t* originalTarget = nullptr
        );

        bool InstallJump(
            CCodeInjector& injector,
            const char* name,
            memory_pointer address,
            size_t replacement
        );

        // Replaces the value stored at a pointer/data slot and remembers
        // the exact original slot value for later restoration.
        bool InstallPointer(
            CCodeInjector& injector,
            const char* name,
            memory_pointer address,
            size_t replacement,
            size_t* originalValue = nullptr
        );

        bool Remove(CCodeInjector& injector, memory_pointer address);
        void RemoveAll(CCodeInjector& injector);

        bool IsInstalled(memory_pointer address) const;
        size_t GetCount() const { return m_hooks.size(); }
        const std::vector<HookRecord>& GetHooks() const { return m_hooks; }
    };
}
