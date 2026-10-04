#include "stdafx.h"
#include "CHookSystem.h"
#include "CDebugBridge.h"
#include "Mem.h"

namespace CLEO
{
    HookRecord* CHookSystem::Find(size_t address)
    {
        for (auto& hook : m_hooks)
        {
            if (hook.address == address)
                return &hook;
        }

        return nullptr;
    }

    const HookRecord* CHookSystem::Find(size_t address) const
    {
        for (const auto& hook : m_hooks)
        {
            if (hook.address == address)
                return &hook;
        }

        return nullptr;
    }

    bool CHookSystem::InstallRaw(
        CCodeInjector& injector,
        const char* name,
        HookType type,
        memory_pointer address,
        size_t replacement,
        size_t* originalTarget
    )
    {
        const size_t patchAddress = static_cast<size_t>(address);

        if (patchAddress == 0 || replacement == 0)
        {
            Error("CHookSystem: invalid hook address or replacement");
            return false;
        }

        if (Find(patchAddress) != nullptr)
        {
            Error("CHookSystem: hook already installed at requested address");
            return false;
        }

        HookRecord record;
        record.type = type;
        record.address = patchAddress;
        record.replacement = replacement;
        record.name = name ? name : "";

        // CLEO's existing MemCall/MemJump patch model writes exactly 5 bytes.
        memcpy(record.originalBytes, reinterpret_cast<const void*>(patchAddress), sizeof(record.originalBytes));

        // Keep the address that was originally targeted by a CALL instruction.
        // For JMP hooks the original target is intentionally left unused.
        if (type == HOOK_CALL)
        {
            record.originalTarget =
                reinterpret_cast<size_t>(MemReadOffsetPtr<void*>(patchAddress + 1));

            if (originalTarget != nullptr)
                *originalTarget = record.originalTarget;
        }

        injector.OpenReadWriteAccess();

        if (type == HOOK_CALL)
            MemCall(patchAddress, replacement);
        else
            MemJump(patchAddress, replacement);

        m_hooks.push_back(record);

        TRACE(
            "[HookSystem] Installed %s '%s' at 0x%08X -> 0x%08X original=0x%08X",
            type == HOOK_CALL ? "CALL" : "JUMP",
            record.name.c_str(),
            (DWORD)record.address,
            (DWORD)record.replacement,
            (DWORD)record.originalTarget
        );

        return true;
    }

    bool CHookSystem::InstallCall(
        CCodeInjector& injector,
        const char* name,
        memory_pointer address,
        size_t replacement,
        size_t* originalTarget
    )
    {
        return InstallRaw(
            injector,
            name,
            HOOK_CALL,
            address,
            replacement,
            originalTarget
        );
    }

    bool CHookSystem::InstallJump(
        CCodeInjector& injector,
        const char* name,
        memory_pointer address,
        size_t replacement
    )
    {
        return InstallRaw(
            injector,
            name,
            HOOK_JUMP,
            address,
            replacement,
            nullptr
        );
    }

    bool CHookSystem::InstallPointer(
        CCodeInjector& injector,
        const char* name,
        memory_pointer address,
        size_t replacement,
        size_t* originalValue
    )
    {
        const size_t patchAddress = static_cast<size_t>(address);

        if (patchAddress == 0 || replacement == 0)
        {
            Error("CHookSystem: invalid pointer hook address or replacement");
            return false;
        }

        if (Find(patchAddress) != nullptr)
        {
            Error("CHookSystem: hook already installed at requested address");
            return false;
        }

        HookRecord record;
        record.type = HOOK_POINTER;
        record.address = patchAddress;
        record.replacement = replacement;
        record.originalValue = MemRead<DWORD>(patchAddress);
        record.name = name ? name : "";

        if (originalValue != nullptr)
            *originalValue = record.originalValue;

        injector.OpenReadWriteAccess();
        MemWrite<DWORD>(patchAddress, (DWORD)replacement);

        m_hooks.push_back(record);

        TRACE(
            "[HookSystem] Installed POINTER '%s' at 0x%08X -> 0x%08X original=0x%08X",
            record.name.c_str(),
            (DWORD)record.address,
            (DWORD)record.replacement,
            (DWORD)record.originalValue
        );

        return true;
    }

    bool CHookSystem::Remove(CCodeInjector& injector, memory_pointer address)
    {
        const size_t patchAddress = static_cast<size_t>(address);
        auto it = m_hooks.begin();

        while (it != m_hooks.end())
        {
            if (it->address == patchAddress)
                break;

            ++it;
        }

        if (it == m_hooks.end())
            return false;

        injector.OpenReadWriteAccess();

        if (it->type == HOOK_POINTER)
        {
            MemWrite<DWORD>(patchAddress, (DWORD)it->originalValue);
        }
        else
        {
            memcpy(
                reinterpret_cast<void*>(patchAddress),
                it->originalBytes,
                sizeof(it->originalBytes)
            );
        }

        const char* typeName =
            it->type == HOOK_CALL ? "CALL" :
            it->type == HOOK_JUMP ? "JUMP" :
            "POINTER";

        TRACE(
            "[HookSystem] Removed %s '%s' at 0x%08X",
            typeName,
            it->name.c_str(),
            (DWORD)patchAddress
        );

        m_hooks.erase(it);
        return true;
    }

    void CHookSystem::RemoveAll(CCodeInjector& injector)
    {
        if (m_hooks.empty())
            return;

        injector.OpenReadWriteAccess();

        for (const auto& hook : m_hooks)
        {
            memcpy(
                reinterpret_cast<void*>(hook.address),
                hook.originalBytes,
                sizeof(hook.originalBytes)
            );

            TRACE(
                "[HookSystem] Removed %s '%s' at 0x%08X",
                hook.type == HOOK_CALL ? "CALL" : "JUMP",
                hook.name.c_str(),
                (DWORD)hook.address
            );
        }

        m_hooks.clear();
    }

    bool CHookSystem::IsInstalled(memory_pointer address) const
    {
        return Find(static_cast<size_t>(address)) != nullptr;
    }
}
