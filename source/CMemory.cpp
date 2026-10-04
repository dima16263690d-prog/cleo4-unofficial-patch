#include "stdafx.h"
#include "CMemory.h"
#include "CDebugBridge.h"

namespace CLEO
{
    CMemoryProtection::CMemoryProtection(void* address, size_t size, DWORD newProtection)
        : m_address(address), m_size(size)
    {
        if (m_address == nullptr || m_size == 0)
            return;

        m_active = VirtualProtect(m_address, m_size, newProtection, &m_oldProtection) != FALSE;
        if (!m_active)
        {
            Error("VirtualProtect failed while changing memory protection");
            m_address = nullptr;
            m_size = 0;
            m_oldProtection = 0;
        }
    }

    CMemoryProtection::~CMemoryProtection()
    {
        Reset();
    }

    CMemoryProtection::CMemoryProtection(CMemoryProtection&& other) noexcept
        : m_address(other.m_address),
          m_size(other.m_size),
          m_oldProtection(other.m_oldProtection),
          m_active(other.m_active)
    {
        other.m_address = nullptr;
        other.m_size = 0;
        other.m_oldProtection = 0;
        other.m_active = false;
    }

    CMemoryProtection& CMemoryProtection::operator=(CMemoryProtection&& other) noexcept
    {
        if (this == &other)
            return *this;

        Reset();

        m_address = other.m_address;
        m_size = other.m_size;
        m_oldProtection = other.m_oldProtection;
        m_active = other.m_active;

        other.m_address = nullptr;
        other.m_size = 0;
        other.m_oldProtection = 0;
        other.m_active = false;

        return *this;
    }

    void CMemoryProtection::Reset()
    {
        if (!m_active)
            return;

        MEMORY_TRACE("PROTECT_RESTORE address=0x%08X size=0x%08X old=0x%08X",
            (DWORD)m_address,
            (DWORD)m_size,
            (DWORD)m_oldProtection
        );

        DWORD ignored = 0;
        if (!VirtualProtect(m_address, m_size, m_oldProtection, &ignored))
            Error("VirtualProtect failed while restoring memory protection");

        m_address = nullptr;
        m_size = 0;
        m_oldProtection = 0;
        m_active = false;
    }
}
