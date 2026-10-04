#pragma once

#include <Windows.h>
#include <cstddef>

namespace CLEO
{
    // Temporarily changes page protection and restores the exact previous
    // protection automatically when the object leaves scope.
    class CMemoryProtection final
    {
        void* m_address = nullptr;
        size_t m_size = 0;
        DWORD m_oldProtection = 0;
        bool m_active = false;

    public:
        CMemoryProtection() = default;
        CMemoryProtection(void* address, size_t size, DWORD newProtection);
        ~CMemoryProtection();

        CMemoryProtection(const CMemoryProtection&) = delete;
        CMemoryProtection& operator=(const CMemoryProtection&) = delete;

        CMemoryProtection(CMemoryProtection&& other) noexcept;
        CMemoryProtection& operator=(CMemoryProtection&& other) noexcept;

        bool IsActive() const { return m_active; }

        // Restore the original page protection immediately.
        void Reset();
    };
}
