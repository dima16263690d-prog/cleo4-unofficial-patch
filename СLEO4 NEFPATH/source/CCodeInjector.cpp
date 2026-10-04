#include "stdafx.h"
#include "cleo.h"
#include "CDebug.h"
#include "CCodeInjector.h"

namespace CLEO
{
    void CCodeInjector::OpenReadWriteAccess()
    {
        if (bAccessOpen) return;

        auto dwLoadOffset = static_cast<memory_pointer>(GetModuleHandle(nullptr));

        // Temporarily make the game's .text and .rdata sections writable.
        // CMemoryProtection keeps the original protection and restores it
        // automatically when CCodeInjector is destroyed.
        auto pImageBase = (BYTE *)dwLoadOffset;
        auto pDosHeader = (PIMAGE_DOS_HEADER)dwLoadOffset;
        auto pNtHeader = (PIMAGE_NT_HEADERS)(pImageBase + pDosHeader->e_lfanew);
        auto pSection = IMAGE_FIRST_SECTION(pNtHeader);

        m_memoryProtections.clear();
        m_memoryProtections.reserve(pNtHeader->FileHeader.NumberOfSections);

        for (int i = pNtHeader->FileHeader.NumberOfSections; i; i--, pSection++)
        {
            const bool isText = !strcmp((char*)pSection->Name, ".text");
            const bool isRdata = !strcmp((char*)pSection->Name, ".rdata");

            if (!isText && !isRdata)
                continue;

            DWORD dwPhysSize = (pSection->Misc.VirtualSize + 4095) & ~4095;
            DWORD newProtect = (pSection->Characteristics & IMAGE_SCN_MEM_EXECUTE)
                ? PAGE_EXECUTE_READWRITE
                : PAGE_READWRITE;

            TRACE("Unprotecting memory region '%s': 0x%08X (size: 0x%08X)",
                pSection->Name,
                (DWORD)pSection->VirtualAddress,
                (DWORD)dwPhysSize
            );

            void* address = pImageBase + pSection->VirtualAddress;
            m_memoryProtections.emplace_back(address, dwPhysSize, newProtect);
        }

        bAccessOpen = true;
    }

    void CCodeInjector::CloseReadWriteAccess()
    {
        if (!bAccessOpen) return;

        // Restore every section protection captured by OpenReadWriteAccess().
        // Multiple .text sections can exist in the executable, so each one
        // must keep its own RAII protection state.
        for (auto& protection : m_memoryProtections)
            protection.Reset();

        m_memoryProtections.clear();
        bAccessOpen = false;
    }
}
