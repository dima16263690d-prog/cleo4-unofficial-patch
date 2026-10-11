#include "DebugUtils.h"
#include "DebugUtils.Internal.h"
#include "resource.h"
#include <windows.h>
#include <psapi.h>
#include <shellapi.h>
#include <dbghelp.h>
#pragma comment(lib, "Shell32.lib")
#include <TlHelp32.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <fstream>
#include <sstream>
#include <regex>
#include <CTimer.h>

namespace
{
    static volatile LONG g_dbgHelpActive = 0;

    static HMODULE GetDebugHelpModule()
    {
        return LoadLibraryA("DbgHelp.dll");
    }
}

namespace
{
    using StackWalkProc = decltype(&StackWalk);
    using StackWalk64Proc = decltype(&StackWalk64);
    using SymInitializeProc = decltype(&SymInitialize);
    using SymCleanupProc = decltype(&SymCleanup);
    using SymFunctionTableAccessProc = decltype(&SymFunctionTableAccess);
    using SymGetModuleBaseProc = decltype(&SymGetModuleBase);
    using SymFunctionTableAccess64Proc = decltype(&SymFunctionTableAccess64);
    using SymGetModuleBase64Proc = decltype(&SymGetModuleBase64);
    using SymLoadModule64Proc = decltype(&SymLoadModule64);

    enum StackWalkStopReason
    {
        STACKWALK_OK = 0,
        STACKWALK_INVALID_INPUT,
        STACKWALK_STEP_FAILED,
        STACKWALK_ZERO_PC,
        STACKWALK_DUPLICATE_PC,
        STACKWALK_EXCEPTION
    };

    static const char* StackWalkStopReasonName(int reason)
    {
        switch (reason)
        {
        case STACKWALK_OK: return "OK";
        case STACKWALK_INVALID_INPUT: return "INVALID_INPUT";
        case STACKWALK_STEP_FAILED: return "STEP_FAILED";
        case STACKWALK_ZERO_PC: return "ZERO_PC";
        case STACKWALK_DUPLICATE_PC: return "DUPLICATE_PC";
        case STACKWALK_EXCEPTION: return "EXCEPTION";
        default: return "UNKNOWN";
        }
    }

    static BOOL SafeStackWalkLegacyI386(
        StackWalkProc pStackWalk,
        SymFunctionTableAccessProc pSymFunctionTableAccess,
        SymGetModuleBaseProc pSymGetModuleBase,
        HANDLE process,
        HANDLE thread,
        const CONTEXT* sourceContext,
        DWORD* frameAddresses,
        DWORD capacity,
        DWORD* frameCount,
        DWORD* stepsAttempted,
        int* stopReason
    )
    {
        if (stepsAttempted != nullptr)
            *stepsAttempted = 0;
        if (stopReason != nullptr)
            *stopReason = STACKWALK_INVALID_INPUT;

        if (pStackWalk == nullptr ||
            pSymFunctionTableAccess == nullptr ||
            pSymGetModuleBase == nullptr ||
            sourceContext == nullptr ||
            frameAddresses == nullptr ||
            frameCount == nullptr ||
            capacity == 0)
        {
            return FALSE;
        }

        __try
        {
            CONTEXT context = *sourceContext;
            STACKFRAME frame = {};
            frame.AddrPC.Offset = context.Eip;
            frame.AddrPC.Mode = AddrModeFlat;
            frame.AddrFrame.Offset = context.Ebp;
            frame.AddrFrame.Mode = AddrModeFlat;
            frame.AddrStack.Offset = context.Esp;
            frame.AddrStack.Mode = AddrModeFlat;

            DWORD count = 0;
            frameAddresses[count++] = context.Eip;

            DWORD previousStack = frame.AddrStack.Offset;
            DWORD previousFrame = frame.AddrFrame.Offset;

            while (count < capacity)
            {
                if (stepsAttempted != nullptr)
                    ++(*stepsAttempted);

                const BOOL ok = pStackWalk(
                    IMAGE_FILE_MACHINE_I386,
                    process,
                    thread,
                    &frame,
                    &context,
                    nullptr,
                    reinterpret_cast<PFUNCTION_TABLE_ACCESS_ROUTINE>(
                        pSymFunctionTableAccess),
                    reinterpret_cast<PGET_MODULE_BASE_ROUTINE>(
                        pSymGetModuleBase),
                    nullptr
                );

                if (!ok)
                {
                    if (stopReason != nullptr)
                        *stopReason = STACKWALK_STEP_FAILED;
                    break;
                }

                const DWORD address =
                    static_cast<DWORD>(frame.AddrPC.Offset);

                if (address == 0)
                {
                    if (stopReason != nullptr)
                        *stopReason = STACKWALK_ZERO_PC;
                    break;
                }

                const DWORD currentStack =
                    static_cast<DWORD>(frame.AddrStack.Offset);
                const DWORD currentFrame =
                    static_cast<DWORD>(frame.AddrFrame.Offset);
                const DWORD previousAddress = frameAddresses[count - 1];

                // The same PC can legitimately occur in multiple frames
                // (recursion or multiple activations of one function). Treat
                // it as a duplicate only when DbgHelp also failed to advance
                // the stack/frame state.
                if (address == previousAddress &&
                    currentStack <= previousStack &&
                    currentFrame == previousFrame)
                {
                    if (stopReason != nullptr)
                        *stopReason = STACKWALK_DUPLICATE_PC;
                    break;
                }

                // A successful x86 unwind must move upward through the stack.
                // Reject a result that moves backwards or does not advance
                // at all, otherwise malformed unwind data could loop forever.
                if (currentStack <= previousStack)
                {
                    if (stopReason != nullptr)
                        *stopReason = STACKWALK_STEP_FAILED;
                    break;
                }

                frameAddresses[count++] = address;
                previousStack = currentStack;
                previousFrame = currentFrame;
            }

            if (count >= capacity && stopReason != nullptr)
                *stopReason = STACKWALK_OK;

            *frameCount = count;
            return count != 0 ? TRUE : FALSE;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            *frameCount = 0;
            if (stopReason != nullptr)
                *stopReason = STACKWALK_EXCEPTION;
            return FALSE;
        }
    }

    static BOOL SafeStackWalkI386(
        StackWalk64Proc pStackWalk64,
        SymFunctionTableAccess64Proc pSymFunctionTableAccess64,
        SymGetModuleBase64Proc pSymGetModuleBase64,
        HANDLE process,
        HANDLE thread,
        const CONTEXT* sourceContext,
        DWORD* frameAddresses,
        DWORD capacity,
        DWORD* frameCount,
        DWORD* stepsAttempted,
        int* stopReason
    )
    {
        if (stepsAttempted != nullptr)
            *stepsAttempted = 0;
        if (stopReason != nullptr)
            *stopReason = STACKWALK_INVALID_INPUT;

        if (pStackWalk64 == nullptr ||
            pSymFunctionTableAccess64 == nullptr ||
            pSymGetModuleBase64 == nullptr ||
            sourceContext == nullptr ||
            frameAddresses == nullptr ||
            frameCount == nullptr ||
            capacity == 0)
        {
            return FALSE;
        }

        __try
        {
            CONTEXT context = *sourceContext;
            STACKFRAME64 frame = {};

            frame.AddrPC.Offset = context.Eip;
            frame.AddrPC.Mode = AddrModeFlat;
            frame.AddrFrame.Offset = context.Ebp;
            frame.AddrFrame.Mode = AddrModeFlat;
            frame.AddrStack.Offset = context.Esp;
            frame.AddrStack.Mode = AddrModeFlat;

            DWORD count = 0;
            frameAddresses[count++] = context.Eip;

            DWORD64 previousStack = frame.AddrStack.Offset;
            DWORD64 previousFrame = frame.AddrFrame.Offset;
            while (count < capacity)
            {
                if (stepsAttempted != nullptr)
                    ++(*stepsAttempted);

                const BOOL ok = pStackWalk64(
                    IMAGE_FILE_MACHINE_I386,
                    process,
                    thread,
                    &frame,
                    &context,
                    nullptr,
                    reinterpret_cast<PFUNCTION_TABLE_ACCESS_ROUTINE64>(
                        pSymFunctionTableAccess64),
                    reinterpret_cast<PGET_MODULE_BASE_ROUTINE64>(
                        pSymGetModuleBase64),
                    nullptr
                );

                if (!ok)
                {
                    if (stopReason != nullptr)
                        *stopReason = STACKWALK_STEP_FAILED;
                    break;
                }

                const DWORD address =
                    static_cast<DWORD>(frame.AddrPC.Offset);

                if (address == 0)
                {
                    if (stopReason != nullptr)
                        *stopReason = STACKWALK_ZERO_PC;
                    break;
                }

                const DWORD64 currentStack = frame.AddrStack.Offset;
                const DWORD64 currentFrame = frame.AddrFrame.Offset;
                const DWORD previousAddress = frameAddresses[count - 1];

                if (address == previousAddress &&
                    currentStack <= previousStack &&
                    currentFrame == previousFrame)
                {
                    if (stopReason != nullptr)
                        *stopReason = STACKWALK_DUPLICATE_PC;
                    break;
                }

                if (currentStack <= previousStack)
                {
                    if (stopReason != nullptr)
                        *stopReason = STACKWALK_STEP_FAILED;
                    break;
                }

                frameAddresses[count++] = address;
                previousStack = currentStack;
                previousFrame = currentFrame;
            }

            if (count >= capacity)
            {
                if (stopReason != nullptr)
                    *stopReason = STACKWALK_OK;
            }

            *frameCount = count;
            return count != 0 ? TRUE : FALSE;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            *frameCount = 0;
            if (stopReason != nullptr)
                *stopReason = STACKWALK_EXCEPTION;
            return FALSE;
        }
    }
    static BOOL SafeEbpChainI386(
        const CONTEXT* sourceContext,
        DWORD* frameAddresses,
        DWORD capacity,
        DWORD* frameCount,
        DWORD* stepsAttempted
    )
    {
        if (frameCount != nullptr)
            *frameCount = 0;
        if (stepsAttempted != nullptr)
            *stepsAttempted = 0;

        if (sourceContext == nullptr ||
            frameAddresses == nullptr ||
            capacity < 2 ||
            sourceContext->Eip == 0 ||
            sourceContext->Esp == 0 ||
            sourceContext->Ebp == 0)
        {
            return FALSE;
        }

        __try
        {
            MEMORY_BASIC_INFORMATION stackRegion{};
            if (VirtualQuery(
                    reinterpret_cast<LPCVOID>(
                        static_cast<uintptr_t>(sourceContext->Esp)),
                    &stackRegion,
                    sizeof(stackRegion)) == 0 ||
                stackRegion.State != MEM_COMMIT)
            {
                return FALSE;
            }

            const DWORD stackBegin =
                static_cast<DWORD>(
                    reinterpret_cast<uintptr_t>(stackRegion.BaseAddress));
            const DWORD regionSize =
                static_cast<DWORD>(stackRegion.RegionSize);
            if (regionSize < 2u * sizeof(DWORD))
                return FALSE;

            const DWORD stackEnd = stackBegin + regionSize;
            if (stackEnd < stackBegin)
                return FALSE;

            const DWORD currentEbp = sourceContext->Ebp;
            if ((currentEbp & 3u) != 0 ||
                currentEbp < stackBegin ||
                currentEbp > stackEnd - (2u * sizeof(DWORD)))
            {
                return FALSE;
            }

            DWORD count = 0;
            frameAddresses[count++] = sourceContext->Eip;

            DWORD ebp = currentEbp;
            while (count < capacity)
            {
                if (stepsAttempted != nullptr)
                    ++(*stepsAttempted);

                DWORD nextEbp = 0;
                DWORD returnAddress = 0;

                bool readsValid = true;
                __try
                {
                    nextEbp =
                        *reinterpret_cast<const DWORD*>(
                            static_cast<uintptr_t>(ebp));
                    returnAddress =
                        *reinterpret_cast<const DWORD*>(
                            static_cast<uintptr_t>(ebp) + sizeof(DWORD));
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    readsValid = false;
                }

                if (!readsValid)
                    break;

                if (returnAddress == 0)
                    break;

                HMODULE module = nullptr;
                if (!GetModuleHandleExA(
                        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        reinterpret_cast<LPCSTR>(
                            static_cast<uintptr_t>(returnAddress)),
                        &module))
                {
                    break;
                }

                if (nextEbp <= ebp ||
                    (nextEbp & 3u) != 0 ||
                    nextEbp < stackBegin ||
                    nextEbp > stackEnd - (2u * sizeof(DWORD)))
                {
                    break;
                }

                frameAddresses[count++] = returnAddress;
                ebp = nextEbp;
            }

            if (frameCount != nullptr)
                *frameCount = count;

            return count > 1 ? TRUE : FALSE;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            if (frameCount != nullptr)
                *frameCount = 0;
            return FALSE;
        }
    }

}

std::string DebugUtils::Basename(const std::string& path)
{
    const size_t p = path.find_last_of("\\/");
    return p == std::string::npos ? path : path.substr(p + 1);
}

std::string DebugUtils::ModuleNameForAddress(DWORD address)
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(address),
        &module))
    {
        return "<unknown>";
    }

    char path[MAX_PATH] = {};
    if (!GetModuleFileNameA(module, path, sizeof(path)))
        return "<unknown>";

    return Basename(path);
}

bool DebugUtils::SafeReadBytes(const void* address, void* buffer, size_t size)
{
    if (address == nullptr || buffer == nullptr || size == 0)
        return false;

    __try
    {
        memcpy(buffer, address, size);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool DebugUtils::SafeReadDword(const DWORD* address, DWORD& value)
{
    if (address == nullptr)
        return false;

    __try
    {
        value = *address;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

std::vector<DWORD> DebugUtils::BuildStackWalk(
    PEXCEPTION_POINTERS info,
    DWORD maxFrames,
    std::string* diagnostics,
    bool* heuristicBacktrace
)
{
    if (heuristicBacktrace != nullptr)
        *heuristicBacktrace = false;

    std::vector<DWORD> frames;

    if (diagnostics != nullptr)
        *diagnostics = "status=NOT_STARTED";

    if (info == nullptr ||
        info->ContextRecord == nullptr ||
        maxFrames == 0)
    {
        if (diagnostics != nullptr)
            *diagnostics = "status=INVALID_INPUT";
        return frames;
    }

    if (info->ContextRecord->Eip == 0)
    {
        if (diagnostics != nullptr)
            *diagnostics = "status=NULL_EIP";
        return frames;
    }

    if (InterlockedCompareExchange(&g_dbgHelpActive, 1, 0) != 0)
    {
        if (diagnostics != nullptr)
            *diagnostics = "status=BUSY";
        return frames;
    }

    HMODULE dbgHelp = GetDebugHelpModule();
    if (dbgHelp == nullptr)
    {
        if (diagnostics != nullptr)
            *diagnostics = "status=DBGHELP_LOAD_FAILED";
        InterlockedExchange(&g_dbgHelpActive, 0);
        return frames;
    }

    const auto pStackWalk =
        reinterpret_cast<StackWalkProc>(
            GetProcAddress(dbgHelp, "StackWalk"));

    const auto pStackWalk64 =
        reinterpret_cast<StackWalk64Proc>(
            GetProcAddress(dbgHelp, "StackWalk64"));

    const auto pSymInitialize =
        reinterpret_cast<SymInitializeProc>(
            GetProcAddress(dbgHelp, "SymInitialize"));

    const auto pSymCleanup =
        reinterpret_cast<SymCleanupProc>(
            GetProcAddress(dbgHelp, "SymCleanup"));

    const auto pSymFunctionTableAccess =
        reinterpret_cast<SymFunctionTableAccessProc>(
            GetProcAddress(dbgHelp, "SymFunctionTableAccess"));

    const auto pSymGetModuleBase =
        reinterpret_cast<SymGetModuleBaseProc>(
            GetProcAddress(dbgHelp, "SymGetModuleBase"));

    const auto pSymFunctionTableAccess64 =
        reinterpret_cast<SymFunctionTableAccess64Proc>(
            GetProcAddress(dbgHelp, "SymFunctionTableAccess64"));

    const auto pSymGetModuleBase64 =
        reinterpret_cast<SymGetModuleBase64Proc>(
            GetProcAddress(dbgHelp, "SymGetModuleBase64"));

    const auto pSymLoadModule64 =
        reinterpret_cast<SymLoadModule64Proc>(
            GetProcAddress(dbgHelp, "SymLoadModule64"));

    if (pStackWalk64 == nullptr ||
        pSymFunctionTableAccess64 == nullptr ||
        pSymGetModuleBase64 == nullptr)
    {
        if (diagnostics != nullptr)
            *diagnostics = "status=DBGHELP_API_MISSING";
        FreeLibrary(dbgHelp);
        InterlockedExchange(&g_dbgHelpActive, 0);
        return frames;
    }

    const HANDLE process = GetCurrentProcess();
    const HANDLE thread = GetCurrentThread();

    bool symbolsInitialized = false;

    if (pSymInitialize != nullptr)
    {
        symbolsInitialized =
            pSymInitialize(process, nullptr, TRUE) != FALSE;
    }

    const DWORD capacity =
        std::min<DWORD>(maxFrames, 64u);

    // DebugUtils treats the faulting instruction as frame #00 for every
    // backtrace source. A heuristic scan can only discover return-address-like
    // values from the stack, so it must never replace the faulting EIP.
    const DWORD faultingEip = info->ContextRecord->Eip;

    auto NormalizeFaultingIp = [faultingEip, capacity](std::vector<DWORD>& stackFrames)
    {
        if (capacity == 0)
            return;

        if (stackFrames.empty() || stackFrames[0] != faultingEip)
        {
            std::vector<DWORD> normalized;
            normalized.reserve(std::min<size_t>(capacity, stackFrames.size() + 1));
            normalized.push_back(faultingEip);

            for (DWORD frame : stackFrames)
            {
                if (frame == faultingEip)
                    continue;

                if (normalized.size() >= capacity)
                    break;

                if (!normalized.empty() && normalized.back() == frame)
                    continue;

                normalized.push_back(frame);
            }

            stackFrames.swap(normalized);
        }
        else if (stackFrames.size() > capacity)
        {
            stackFrames.resize(capacity);
        }
    };

    // Register the faulting image explicitly with DbgHelp. SymInitialize(TRUE)
    // normally enumerates loaded modules, but an explicit load makes the x86
    // FPO lookup deterministic in the crash-only path.
    DWORD64 symbolModuleBase = 0;
    DWORD symbolLoadError = ERROR_SUCCESS;
    bool symbolModuleLoaded = false;
    bool functionTableAvailable = false;

    HMODULE faultImage = nullptr;
    MODULEINFO faultImageInfo{};
    char faultImagePath[MAX_PATH] = {};

    if (GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(
                static_cast<uintptr_t>(info->ContextRecord->Eip)),
            &faultImage) &&
        GetModuleInformation(
            process,
            faultImage,
            &faultImageInfo,
            sizeof(faultImageInfo)) &&
        GetModuleFileNameA(
            faultImage,
            faultImagePath,
            sizeof(faultImagePath)) != 0)
    {
        const DWORD64 imageBase =
            static_cast<DWORD64>(
                reinterpret_cast<uintptr_t>(faultImageInfo.lpBaseOfDll));
        const DWORD imageSize =
            faultImageInfo.SizeOfImage;

        if (pSymLoadModule64 != nullptr)
        {
            SetLastError(ERROR_SUCCESS);
            const DWORD64 loadedBase =
                pSymLoadModule64(
                    process,
                    nullptr,
                    faultImagePath,
                    nullptr,
                    imageBase,
                    imageSize);

            symbolLoadError = GetLastError();
            symbolModuleLoaded =
                loadedBase != 0 || symbolLoadError == ERROR_SUCCESS;
            symbolModuleBase =
                loadedBase != 0 ? loadedBase : imageBase;
        }
        else
        {
            symbolModuleBase = imageBase;
        }
    }

    if (pSymFunctionTableAccess64 != nullptr)
    {
        functionTableAvailable =
            pSymFunctionTableAccess64(
                process,
                static_cast<DWORD64>(info->ContextRecord->Eip)) != nullptr;
    }

    frames.resize(capacity);

    DWORD frameCount64 = 0;
    DWORD stepsAttempted64 = 0;
    int stopReason64 = STACKWALK_INVALID_INPUT;

    const BOOL ok64 =
        pStackWalk64 != nullptr
            ? SafeStackWalkI386(
                pStackWalk64,
                pSymFunctionTableAccess64,
                pSymGetModuleBase64,
                process,
                thread,
                info->ContextRecord,
                frames.data(),
                capacity,
                &frameCount64,
                &stepsAttempted64,
                &stopReason64)
            : FALSE;

    if (ok64 && frameCount64 > 1)
    {
        frames.resize(frameCount64);
        NormalizeFaultingIp(frames);
        const bool faultIpPresent =
            !frames.empty() && frames[0] == faultingEip;

        if (diagnostics != nullptr)
        {
            char text[512] = {};
            sprintf_s(
                text,
                sizeof(text),
                "status=OK method=PRIMARY primary=%s legacy=NOT_NEEDED ebp=NOT_NEEDED symbols_initialized=%d fpo_table=%d symbol_module_loaded=%d symbol_load_error=%u symbol_module_base=0x%08X primary_steps=%u primary_frames=%u legacy_steps=0 legacy_frames=0 ebp_steps=0 ebp_frames=0 frames=%u fault_ip_present=%d eip=0x%08X ebp_reg=0x%08X esp=0x%08X",
                StackWalkStopReasonName(stopReason64),
                symbolsInitialized ? 1 : 0,
                functionTableAvailable ? 1 : 0,
                symbolModuleLoaded ? 1 : 0,
                static_cast<unsigned>(symbolLoadError),
                static_cast<unsigned>(symbolModuleBase),
                stepsAttempted64,
                static_cast<unsigned>(frameCount64),
                static_cast<unsigned>(frameCount64),
                faultIpPresent ? 1 : 0,
                info->ContextRecord->Eip,
                info->ContextRecord->Ebp,
                info->ContextRecord->Esp
            );
            *diagnostics = text;
        }
    }
    else
    {
        // Keep writable storage for the legacy fallback. std::vector::clear()
        // invalidates the data buffer, so passing frames.data() with the old
        // capacity would leave the fallback writing through a null/zero buffer.
        frames.resize(capacity);

        DWORD frameCountLegacy = 0;
        DWORD stepsAttemptedLegacy = 0;
        int stopReasonLegacy = STACKWALK_INVALID_INPUT;

        const BOOL okLegacy =
            pStackWalk != nullptr
                ? SafeStackWalkLegacyI386(
                    pStackWalk,
                    pSymFunctionTableAccess,
                    pSymGetModuleBase,
                    process,
                    thread,
                    info->ContextRecord,
                    frames.data(),
                    capacity,
                    &frameCountLegacy,
                    &stepsAttemptedLegacy,
                    &stopReasonLegacy)
                : FALSE;

        bool ebpOk = false;
        DWORD frameCountEbp = 0;
        DWORD stepsAttemptedEbp = 0;
        bool heuristicOk = false;
        std::vector<DWORD> heuristicFrames;

        // Require a genuine unwind (more than the initial fault PC) before
        // accepting the DbgHelp result as a backtrace.
        if (okLegacy && frameCountLegacy > 1)
        {
            frames.resize(frameCountLegacy);
            NormalizeFaultingIp(frames);
        }
        else
        {
            // Keep writable storage for the EBP fallback as well.
            frames.resize(capacity);

            ebpOk =
                SafeEbpChainI386(
                    info->ContextRecord,
                    frames.data(),
                    capacity,
                    &frameCountEbp,
                    &stepsAttemptedEbp) != FALSE;

            if (ebpOk && frameCountEbp > 1)
            {
                frames.resize(frameCountEbp);
                NormalizeFaultingIp(frames);
            }
            else
            {
                heuristicFrames =
                    BuildHeuristicStackFrames(
                        info->ContextRecord->Esp,
                        0x4000u,
                        capacity > 0 ? capacity - 1 : 0);

                heuristicOk = !heuristicFrames.empty();
                if (heuristicOk)
                {
                    frames.clear();
                    frames.reserve(std::min<size_t>(
                        capacity,
                        heuristicFrames.size() + 1));

                    frames.push_back(faultingEip);

                    for (DWORD frame : heuristicFrames)
                    {
                        if (frame == faultingEip)
                            continue;

                        if (frames.size() >= capacity)
                            break;

                        if (frames.back() == frame)
                            continue;

                        frames.push_back(frame);
                    }

                    NormalizeFaultingIp(frames);
                    if (heuristicBacktrace != nullptr)
                        *heuristicBacktrace = true;
                }
                else
                    frames.clear();
            }
        }

        if (diagnostics != nullptr)
        {
            char text[512] = {};

            if (okLegacy && frameCountLegacy > 1)
            {
                const bool faultIpPresent =
                    !frames.empty() && frames[0] == faultingEip;

                sprintf_s(
                    text,
                    sizeof(text),
                    "status=OK method=LEGACY primary=%s legacy=%s symbols_initialized=%d fpo_table=%d symbol_module_loaded=%d symbol_load_error=%u symbol_module_base=0x%08X primary_steps=%u primary_frames=%u legacy_steps=%u legacy_frames=%u ebp=NOT_NEEDED frames=%u fault_ip_present=%d eip=0x%08X ebp_reg=0x%08X esp=0x%08X",
                    StackWalkStopReasonName(stopReason64),
                    StackWalkStopReasonName(stopReasonLegacy),
                    symbolsInitialized ? 1 : 0,
                    functionTableAvailable ? 1 : 0,
                    symbolModuleLoaded ? 1 : 0,
                    static_cast<unsigned>(symbolLoadError),
                    static_cast<unsigned>(symbolModuleBase),
                    stepsAttempted64,
                    static_cast<unsigned>(frameCount64),
                    stepsAttemptedLegacy,
                    static_cast<unsigned>(frameCountLegacy),
                    static_cast<unsigned>(frameCountLegacy),
                    faultIpPresent ? 1 : 0,
                    info->ContextRecord->Eip,
                    info->ContextRecord->Ebp,
                    info->ContextRecord->Esp
                );
            }
            else if (ebpOk && frameCountEbp > 1)
            {
                const bool faultIpPresent =
                    !frames.empty() && frames[0] == faultingEip;

                sprintf_s(
                    text,
                    sizeof(text),
                    "status=OK method=EBP_CHAIN primary=%s legacy=%s ebp=OK symbols_initialized=%d primary_steps=%u primary_frames=%u legacy_steps=%u legacy_frames=%u ebp_steps=%u ebp_frames=%u frames=%u fault_ip_present=%d eip=0x%08X ebp_reg=0x%08X esp=0x%08X",
                    StackWalkStopReasonName(stopReason64),
                    pStackWalk == nullptr
                        ? "UNAVAILABLE"
                        : StackWalkStopReasonName(stopReasonLegacy),
                    symbolsInitialized ? 1 : 0,
                    stepsAttempted64,
                    static_cast<unsigned>(frameCount64),
                    stepsAttemptedLegacy,
                    static_cast<unsigned>(frameCountLegacy),
                    stepsAttemptedEbp,
                    static_cast<unsigned>(frameCountEbp),
                    static_cast<unsigned>(frameCountEbp),
                    faultIpPresent ? 1 : 0,
                    info->ContextRecord->Eip,
                    info->ContextRecord->Ebp,
                    info->ContextRecord->Esp
                );
            }
            else if (heuristicOk)
            {
                sprintf_s(
                    text,
                    sizeof(text),
                    "status=OK method=STACK_SCAN heuristic=1 primary=%s legacy=%s ebp=NO_VALID_CHAIN scan=OK symbols_initialized=%d fpo_table=%d symbol_module_loaded=%d symbol_load_error=%u symbol_module_base=0x%08X primary_steps=%u primary_frames=%u legacy_steps=%u legacy_frames=%u ebp_steps=%u ebp_frames=%u scan_frames=%u frames=%u fault_ip_present=%d eip=0x%08X ebp_reg=0x%08X esp=0x%08X",
                    StackWalkStopReasonName(stopReason64),
                    pStackWalk == nullptr
                        ? "UNAVAILABLE"
                        : StackWalkStopReasonName(stopReasonLegacy),
                    symbolsInitialized ? 1 : 0,
                    functionTableAvailable ? 1 : 0,
                    symbolModuleLoaded ? 1 : 0,
                    static_cast<unsigned>(symbolLoadError),
                    static_cast<unsigned>(symbolModuleBase),
                    stepsAttempted64,
                    static_cast<unsigned>(frameCount64),
                    stepsAttemptedLegacy,
                    static_cast<unsigned>(frameCountLegacy),
                    stepsAttemptedEbp,
                    static_cast<unsigned>(frameCountEbp),
                    static_cast<unsigned>(heuristicFrames.size()),
                    static_cast<unsigned>(frames.size()),
                    (!frames.empty() && frames[0] == faultingEip) ? 1 : 0,
                    info->ContextRecord->Eip,
                    info->ContextRecord->Ebp,
                    info->ContextRecord->Esp
                );
            }
            else
            {
                sprintf_s(
                    text,
                    sizeof(text),
                    "status=FAILED primary=%s legacy=%s ebp=NO_VALID_CHAIN scan=NO_VALID_FRAMES symbols_initialized=%d fpo_table=%d symbol_module_loaded=%d symbol_load_error=%u symbol_module_base=0x%08X primary_steps=%u primary_frames=%u legacy_steps=%u legacy_frames=%u ebp_steps=%u ebp_frames=%u scan_frames=0 frames=0 fault_ip_present=0 eip=0x%08X ebp_reg=0x%08X esp=0x%08X",
                    StackWalkStopReasonName(stopReason64),
                    pStackWalk == nullptr
                        ? "UNAVAILABLE"
                        : StackWalkStopReasonName(stopReasonLegacy),
                    symbolsInitialized ? 1 : 0,
                    functionTableAvailable ? 1 : 0,
                    symbolModuleLoaded ? 1 : 0,
                    static_cast<unsigned>(symbolLoadError),
                    static_cast<unsigned>(symbolModuleBase),
                    stepsAttempted64,
                    static_cast<unsigned>(frameCount64),
                    stepsAttemptedLegacy,
                    static_cast<unsigned>(frameCountLegacy),
                    stepsAttemptedEbp,
                    static_cast<unsigned>(frameCountEbp),
                    info->ContextRecord->Eip,
                    info->ContextRecord->Ebp,
                    info->ContextRecord->Esp
                );
            }

            *diagnostics = text;
        }

        if (symbolsInitialized && pSymCleanup != nullptr)
            pSymCleanup(process);

        FreeLibrary(dbgHelp);
        InterlockedExchange(&g_dbgHelpActive, 0);
        return frames;
    }

    if (diagnostics != nullptr)
    {
        char text[384] = {};
        sprintf_s(
            text,
            sizeof(text),
            "status=PRIMARY_%s legacy=NOT_NEEDED symbols_initialized=%d primary_steps=%u primary_frames=%u eip=0x%08X ebp=0x%08X esp=0x%08X",
            StackWalkStopReasonName(stopReason64),
            symbolsInitialized ? 1 : 0,
            stepsAttempted64,
            static_cast<unsigned>(frameCount64),
            info->ContextRecord->Eip,
            info->ContextRecord->Ebp,
            info->ContextRecord->Esp
        );
        *diagnostics = text;
    }

    if (symbolsInitialized && pSymCleanup != nullptr)
        pSymCleanup(process);

    FreeLibrary(dbgHelp);
    InterlockedExchange(&g_dbgHelpActive, 0);

    return frames;
}

namespace
{
    static bool IsExecutableAddressForStackScan(DWORD address)
    {
        if (address == 0)
            return false;

        HMODULE module = nullptr;
        if (!GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(
                    static_cast<uintptr_t>(address)),
                &module))
        {
            return false;
        }

        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(
                reinterpret_cast<LPCVOID>(
                    static_cast<uintptr_t>(address)),
                &mbi,
                sizeof(mbi)) == 0 ||
            mbi.State != MEM_COMMIT)
        {
            return false;
        }

        const DWORD protection = mbi.Protect & 0xFFu;
        return protection == PAGE_EXECUTE ||
               protection == PAGE_EXECUTE_READ ||
               protection == PAGE_EXECUTE_READWRITE ||
               protection == PAGE_EXECUTE_WRITECOPY;
    }

    static bool IsDirectCallReturnAddress(DWORD address)
    {
        if (address < 5u || !IsExecutableAddressForStackScan(address))
            return false;

        BYTE bytes[5] = {};
        SIZE_T read = 0;
        if (!ReadProcessMemory(
                GetCurrentProcess(),
                reinterpret_cast<LPCVOID>(
                    static_cast<uintptr_t>(address) - 5u),
                bytes,
                sizeof(bytes),
                &read) ||
            read != sizeof(bytes))
        {
            return false;
        }

        // E8 rel32 is the canonical five-byte near CALL used by x86 code.
        return bytes[0] == 0xE8;
    }
}

std::vector<DWORD> DebugUtils::BuildHeuristicStackFrames(
    DWORD stackPointer,
    size_t scanBytes,
    size_t maxFrames
)
{
    std::vector<DWORD> frames;

    if (stackPointer == 0 ||
        scanBytes < sizeof(DWORD) ||
        maxFrames < 2)
    {
        return frames;
    }

    const size_t words =
        std::min<size_t>(scanBytes / sizeof(DWORD), 4096u);

    for (size_t i = 0; i < words && frames.size() < maxFrames; ++i)
    {
        DWORD candidate = 0;
        SIZE_T read = 0;

        if (!ReadProcessMemory(
                GetCurrentProcess(),
                reinterpret_cast<LPCVOID>(
                    static_cast<uintptr_t>(stackPointer) +
                    i * sizeof(DWORD)),
                &candidate,
                sizeof(candidate),
                &read) ||
            read != sizeof(candidate))
        {
            break;
        }

        if (!IsDirectCallReturnAddress(candidate))
            continue;

        if (!frames.empty() && frames.back() == candidate)
            continue;

        frames.push_back(candidate);
    }

    return frames;
}

std::vector<DWORD> DebugUtils::BuildRawStackCandidates(
    DWORD stackPointer,
    size_t scanBytes,
    size_t maxCandidates
)
{
    std::vector<DWORD> candidates;

    if (stackPointer == 0 || scanBytes < sizeof(DWORD) || maxCandidates == 0)
        return candidates;

    const size_t words =
        std::min<size_t>(scanBytes / sizeof(DWORD), 256u);

    for (size_t i = 0; i < words && candidates.size() < maxCandidates; ++i)
    {
        DWORD value = 0;
        if (!SafeReadDword(
                reinterpret_cast<const DWORD*>(
                    static_cast<uintptr_t>(stackPointer) +
                    i * sizeof(DWORD)),
                value))
        {
            break;
        }

        if (value == 0)
            continue;

        HMODULE module = nullptr;
        if (!GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(
                    static_cast<uintptr_t>(value)),
                &module))
        {
            continue;
        }

        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(
                reinterpret_cast<LPCVOID>(
                    static_cast<uintptr_t>(value)),
                &mbi,
                sizeof(mbi)) == 0 ||
            mbi.State != MEM_COMMIT)
        {
            continue;
        }

        const DWORD protection = mbi.Protect & 0xFFu;
        const bool executable =
            protection == PAGE_EXECUTE ||
            protection == PAGE_EXECUTE_READ ||
            protection == PAGE_EXECUTE_READWRITE ||
            protection == PAGE_EXECUTE_WRITECOPY;

        if (!executable)
            continue;

        candidates.push_back(value);
    }

    return candidates;
}
