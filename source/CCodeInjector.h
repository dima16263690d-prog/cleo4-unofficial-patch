#pragma once
#include "CDebugBridge.h"
#include "CMemory.h"
#include <vector>

namespace CLEO
{
    // a pointer automatically convertible to integral types
    union memory_pointer
    {
        size_t address;
        void *pointer;

        inline memory_pointer(void *p) : pointer(p) { }
        inline memory_pointer(size_t a) : address(a) { }
        inline operator void *() { return pointer; }
        inline operator size_t() { return address; }
        inline memory_pointer& operator=(void *p) { return *this = p; }
        inline memory_pointer& operator=(size_t p) { return *this = p; }

        // conversion to/from any-type pointer
        template<typename T>
        inline memory_pointer(T *p) : pointer(reinterpret_cast<void *>(p)) {}
        template<typename T>
        inline operator T *() { return reinterpret_cast<T *>(pointer); }
        template<typename T>
        inline memory_pointer& operator=(T *p) { return *this = reinterpret_cast<void *>(p); }
    };

    VALIDATE_SIZE(memory_pointer, 4);

    const memory_pointer memory_und = nullptr;

    // Implements dirty tricks for low-level memory managemant
    class CCodeInjector
    {
        bool bAccessOpen;
        std::vector<CMemoryProtection> m_memoryProtections;

    public:
        CCodeInjector() {
            bAccessOpen = false;

            // moved here so that access is open for plugins
            OpenReadWriteAccess();			// we might as well leave it open, too
                                            //GetInstance().Start();
        }
        ~CCodeInjector()
        {
            // Restore all game image protections before the injector is destroyed.
            CloseReadWriteAccess();
        };

        void OpenReadWriteAccess();
        void CloseReadWriteAccess();

        template<typename T>
        void ReplaceFunction(T *funcPtr, memory_pointer Position)
        {
            TRACE("Replacing call: 0x%08X", (DWORD)Position);
            MemCall((size_t)Position, (size_t)funcPtr);		// *whistle*
        }

        template<typename T>
        void InjectFunction(T *funcPtr, memory_pointer Position)
        {
            TRACE("Injecting function at: 0x%08X", (DWORD)Position);
            MemJump((size_t)Position, (size_t)funcPtr);
        }

        void Nop(memory_pointer addr, size_t size)
        {
            MemFill(addr, OP_NOP, size);
        }

        template<typename T> void MemoryReadOffset(memory_pointer addr, T& result, bool force_vp = false)
        {
            CMemoryProtection protection;
            if (force_vp)
                protection = CMemoryProtection(addr.pointer, sizeof(T), PAGE_EXECUTE_READWRITE);
            if (force_vp && !protection.IsActive())
                return;

            result = MemReadOffsetPtr<T>(addr.address);
        }

        // copies object given by memory address @addr to the @result object
        template<typename T> void MemoryRead(memory_pointer addr, T& result, bool force_vp = false)
        {
            CMemoryProtection protection;
            if (force_vp)
                protection = CMemoryProtection(addr.pointer, sizeof(T), PAGE_EXECUTE_READWRITE);
            if (force_vp && !protection.IsActive())
                return;

            result = MemRead<T>(addr);
        }

        // copies array of objects given by memory address @addr to the @result array of length @cnt
        template<typename T> void MemoryRead(memory_pointer addr, T *result, size_t n, bool force_vp = false)
        {
            CMemoryProtection protection;
            if (force_vp)
                protection = CMemoryProtection(addr.pointer, sizeof(T) * n, PAGE_EXECUTE_READWRITE);
            if (force_vp && !protection.IsActive())
                return;

            MemCopy(result, addr, n);
        }

        // copies @proto object to the object given by memory address @addr
        template<typename T> void MemoryWrite(memory_pointer addr, const T& proto, bool force_vp = false, size_t n = 1)
        {
            CMemoryProtection protection;
            if (force_vp)
                protection = CMemoryProtection(addr.pointer, sizeof(T) * n, PAGE_EXECUTE_READWRITE);
            if (force_vp && !protection.IsActive())
                return;

            if (n != 1) MemFill(addr, proto, n);
            else MemWrite(addr, proto);
        }

        // copies array of objects @proto of length @cnt to array of objects given by memory address @addr
        template<typename T> void MemoryWrite(memory_pointer addr, const T *proto, size_t n, bool force_vp = false)
        {
            CMemoryProtection protection;
            if (force_vp)
                protection = CMemoryProtection(addr.pointer, sizeof(T) * n, PAGE_EXECUTE_READWRITE);
            if (force_vp && !protection.IsActive())
                return;

            MemCopy(addr, proto, n);
        }
    };

    // determines an object, that should be injected into the game engine
    // on startup
    class VInjectible
    {
    public:
        virtual void Inject(CCodeInjector& inj) = 0;
    };
}
