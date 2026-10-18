#include "DebugUtils.h"
#include "DebugUtils.Internal.h"
#include <windows.h>
#include <psapi.h>
#include <cstring>
#include <string>
#include <vector>

// Stack-walking and module attribution: turn a raw frame list (or the live
// stack) into "mod.asi+0x1a2b" labels, ignoring the game/CLEO/DebugUtils frames
// and system DLLs. Shared by every error/watch path that needs to name the
// module responsible for something.

namespace
{
    bool IsPageReadable(const void* address)
    {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(address, &info, sizeof(info)) == 0)
            return false;

        if (info.State != MEM_COMMIT)
            return false;

        const DWORD readable =
            PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
            PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;

        return (info.Protect & readable) != 0;
    }

    // Portable (no SEH) guarded read of a DWORD, so the stack scan is safe under
    // both MSVC and the mingw syntax-check harness.
    bool ReadDwordIfReadable(const void* address, DWORD& value)
    {
        if (address == nullptr || !IsPageReadable(address))
            return false;

        value = *reinterpret_cast<const DWORD*>(address);
        return true;
    }

    bool IsExecutableAddress(const void* address)
    {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(address, &info, sizeof(info)) == 0)
            return false;

        if (info.State != MEM_COMMIT)
            return false;

        const DWORD executable =
            PAGE_EXECUTE | PAGE_EXECUTE_READ |
            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;

        return (info.Protect & executable) != 0;
    }

    // Only a return address may legitimately appear as a captured frame. On x86
    // RtlCaptureStackBackTrace follows the frame-pointer chain, and release
    // builds omit the frame pointer (FPO), so a stale stack word can land inside
    // a large DLL and be reported as a frame - which is why the innermost frame
    // of a requester chain flips between runs (d3d9.dll+0x5FE17 one launch,
    // OpenAL32.dll+0x1D6019 the next) for the same one-shot load. Two cheap tests
    // drop those words: the address must lie inside its module image, and the
    // bytes just before it must decode to a call. When the preceding bytes cannot
    // be read the frame is kept - never drop a frame on uncertainty alone.
    //
    // `strictE8`, when non-null, is set true only for the strongest evidence: a
    // near CALL (E8 rel32) immediately before the address. Indirect-call patterns
    // and the cannot-read case set it false, so the caller can report how much of
    // a chain is verified.
    bool IsPlausibleReturnAddress(DWORD address, HMODULE module, bool* strictE8 = nullptr)
    {
        if (strictE8 != nullptr)
            *strictE8 = false;

        if (module == nullptr || address < 0x10000)
            return false;

        MODULEINFO info{};
        if (GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info)) &&
            info.SizeOfImage != 0 &&
            address - reinterpret_cast<DWORD>(module) >= info.SizeOfImage)
        {
            return false;
        }

        BYTE pre[7] = {}; // pre[i] = byte at (address - i), 1-based
        for (int i = 1; i <= 6; ++i)
        {
            const void* p = reinterpret_cast<const void*>(address - i);
            if (!IsPageReadable(p))
                return true; // cannot judge: keep the frame
            pre[i] = *reinterpret_cast<const BYTE*>(p);
        }

        if (pre[5] == 0xE8)
        {
            if (strictE8 != nullptr)
                *strictE8 = true;
            return true; // call rel32
        }
        if (pre[6] == 0xFF && pre[5] == 0x15)           return true; // call [abs32]
        if (pre[2] == 0xFF && (pre[1] & 0xF8) == 0xD0)  return true; // call reg
        if (pre[2] == 0xFF && (pre[1] & 0xF8) == 0x10)  return true; // call [reg]
        if (pre[3] == 0xFF && (pre[2] & 0xF8) == 0x50)  return true; // call [reg+disp8]
        if (pre[6] == 0xFF && (pre[5] & 0xF8) == 0x90)  return true; // call [reg+disp32]

        return false;
    }
}


namespace
{
    typedef USHORT(WINAPI* RtlCaptureStackBackTraceFn)(
        ULONG FramesToSkip, ULONG FramesToCapture, PVOID* BackTrace, PULONG BackTraceHash);

    // RtlCaptureStackBackTrace lives in kernel32 on modern Windows and ntdll on
    // XP; resolved dynamically so no import is needed and the mingw syntax check
    // stays happy.
    RtlCaptureStackBackTraceFn ResolveCaptureBackTrace()
    {
        static RtlCaptureStackBackTraceFn cached = []() -> RtlCaptureStackBackTraceFn
        {
            HMODULE kernel = GetModuleHandleA("kernel32.dll");
            if (kernel != nullptr)
            {
                if (auto fn = reinterpret_cast<RtlCaptureStackBackTraceFn>(
                        reinterpret_cast<void*>(
                            GetProcAddress(kernel, "RtlCaptureStackBackTrace"))))
                {
                    return fn;
                }
            }

            HMODULE ntdll = GetModuleHandleA("ntdll.dll");
            if (ntdll != nullptr)
            {
                return reinterpret_cast<RtlCaptureStackBackTraceFn>(
                    reinterpret_cast<void*>(
                        GetProcAddress(ntdll, "RtlCaptureStackBackTrace")));
            }

            return nullptr;
        }();

        return cached;
    }
}


// True for the game, CLEO and DebugUtils itself: modules that are never the
// interesting answer when looking for "which mod did this".
bool DebugUtils::IsIgnoredModuleHandle(HMODULE module)
{
    if (module == nullptr)
        return true;

    const DWORD base = reinterpret_cast<DWORD>(module);
    if (base == reinterpret_cast<DWORD>(GetModuleHandleA(nullptr)))
        return true;
    if (base == reinterpret_cast<DWORD>(GetModuleHandleA("CLEO.asi")))
        return true;
    if (base == reinterpret_cast<DWORD>(GetModuleHandleA("DebugUtils.cleo")))
        return true;
    if (base == reinterpret_cast<DWORD>(GetModuleHandleA("DebugUtils.dll")))
        return true;

    // This module's own base, resolved once by address so it works whatever
    // the loaded file is called.
    static const DWORD selfBase = []() -> DWORD
    {
        HMODULE self = nullptr;
        GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&DebugUtils::IsIgnoredModuleHandle),
            &self);
        return reinterpret_cast<DWORD>(self);
    }();

    return base == selfBase;
}

// True when the module's file lives under the Windows directory (or is one of
// the known system images). A requester chain that passes through user32/kernel32
// or a system-supplied d3d9 is describing Windows' own call plumbing, not the mod
// that asked for the file, so those frames are dropped. A mod that ships its own
// d3d9.dll sits in the game folder, so its path does not match and it is kept.
bool DebugUtils::IsSystemModuleHandle(HMODULE module)
{
    if (module == nullptr)
        return true;

    char path[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameA(module, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH)
        return false; // unknown path: keep it rather than silently dropping

    // Normalize to lowercase for the prefix comparison.
    for (DWORD i = 0; i < len; ++i)
        path[i] = static_cast<char>(tolower(static_cast<unsigned char>(path[i])));

    static const char* kSystemDirs[] =
    {
        "c:\\windows\\", "c:\\winnt\\", "\\system32\\", "\\syswow64\\",
        "\\winsxs\\"
    };
    for (const char* dir : kSystemDirs)
    {
        if (strstr(path, dir) != nullptr)
            return true;
    }

    return false;
}

// True when the running game image is Large Address Aware (the "4GB patch"). Read
// straight from the PE header so it reflects the actual file being run, not a
// guess from the OS. A 32-bit process without the flag has a 2 GB user address
// space; with it, ~4 GB - so this changes what "running out of memory" means.
bool DebugUtils::IsLargeAddressAware()
{
    const HMODULE image = GetModuleHandleA(nullptr);
    if (image == nullptr)
        return false;

    const BYTE* base = reinterpret_cast<const BYTE*>(image);
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;

    const IMAGE_NT_HEADERS* nt =
        reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return false;

    return (nt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) != 0;
}

// "mod.asi+0x1a2b" - the module plus the offset inside it.
std::string DebugUtils::FormatFrameLabel(DWORD address)
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(address),
            &module) || module == nullptr)
    {
        return {};
    }

    const std::string name = ModuleNameForAddress(address);
    if (name.empty() || name == "<unknown>")
        return {};

    char label[260] = {};
    _snprintf_s(
        label, sizeof(label), _TRUNCATE, "%s+0x%X",
        name.c_str(),
        static_cast<unsigned>(address - reinterpret_cast<DWORD>(module)));

    return std::string(label);
}

// Append a frame label to the list unless the module is already listed, so
// one DLL is not repeated at several offsets.
void DebugUtils::PushUniqueFrame(std::vector<std::string>& modules, DWORD address)
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(address),
            &module) || module == nullptr)
    {
        return;
    }

    const std::string name = ModuleNameForAddress(address);
    if (name.empty() || name == "<unknown>")
        return;

    for (const auto& seen : modules)
    {
        if (seen == name || seen.compare(0, name.size() + 1, name + "+") == 0)
            return;
    }

    const std::string label = FormatFrameLabel(address);
    if (!label.empty())
        modules.push_back(label);
}

// Converts a raw frame list into module labels, dropping the game/CLEO/DebugUtils
// frames, system DLLs and stale stack words. Shared by every capture mechanism so
// they all attribute the same way.
std::vector<std::string> DebugUtils::ModulesFromFrames(const DWORD* frames, size_t count, size_t maxFrames)
{
    std::vector<std::string> modules;

    for (size_t i = 0; i < count && modules.size() < maxFrames; ++i)
    {
        const DWORD address = frames[i];
        if (address < 0x10000)
            continue;

        HMODULE module = nullptr;
        if (!GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(address),
                &module) || module == nullptr)
        {
            continue;
        }

        if (IsIgnoredModuleHandle(module) || IsSystemModuleHandle(module))
            continue;

        // Drop stale stack words that a frame-pointer walk reports as frames:
        // they land in large DLLs at offsets that are not the site of a call.
        if (!IsPlausibleReturnAddress(address, module))
            continue;

        PushUniqueFrame(modules, address);
    }

    return modules;
}

// Module chain of the callers of this function, nearest first, ignoring the
// game, CLEO and DebugUtils itself. Two mechanisms:
//   1. RtlCaptureStackBackTrace - a real unwind, but on x86 it follows the frame
//      pointer, which GTA/mods omit (FPO), so it often stops inside the EXE.
//   2. A stack scan over the live stack. Every accepted word must be a plausible
//      return address (the preceding bytes decode to a call), which needs no frame
//      pointer, so this is what actually reaches mod frames on x86.
// DbgHelp is deliberately NOT used: x86 Windows has no PE unwind tables (they are
// an x64/ARM64 feature) and the game/mods ship no PDBs, so StackWalk64 just falls
// back to the frame pointer here while costing a full SymInitialize.
//
// The `via` label reports the *reliability* of the winning chain:
//   "unwind"            - RtlCaptureStackBackTrace, a real unwind;
//   "scan-e8"           - a scan where every frame is preceded by a near CALL;
//   "scan-e8 N/M"       - a scan where N of M frames are E8-verified (the rest
//                         matched only a looser indirect-call pattern);
//   "scan"              - a scan where no frame is E8-verified (weakest).
std::vector<std::string> DebugUtils::CaptureRequesterChain(size_t maxFrames, const char** via)
{
    std::vector<std::string> modules;

    if (via != nullptr)
        *via = "scan";

    const auto capture = ResolveCaptureBackTrace();
    if (capture != nullptr)
    {
        PVOID frames[32] = {};
        const USHORT count = capture(0, 32, frames, nullptr);

        DWORD addresses[32] = {};
        size_t used = 0;
        for (USHORT i = 0; i < count && used < 32; ++i)
            addresses[used++] = reinterpret_cast<DWORD>(frames[i]);

        modules = ModulesFromFrames(addresses, used, maxFrames);
        if (!modules.empty())
        {
            if (via != nullptr)
                *via = "unwind";
            return modules;
        }
    }

    size_t strict = 0;
    size_t total = 0;
    modules = ScanErrorModules(maxFrames, &strict, &total);

    if (via != nullptr && total > 0)
    {
        if (strict == total)
        {
            *via = "scan-e8";
        }
        else
        {
            // Copied into a std::string by the caller before the next capture, so
            // a per-thread buffer is safe and avoids a heap allocation here.
            static thread_local char label[32];
            sprintf_s(label, sizeof(label), "scan-e8 %zu/%zu (partial)", strict, total);
            *via = label;
        }
    }

    return modules;
}


// Walks up the current thread's stack looking for return addresses that land in
// a module which is neither the game, the CLEO core, nor DebugUtils. Those are
// the candidate third-party modules (mods/plugins/loaders) responsible for the
// error. Uses the address of a local as the scan anchor, so it needs no
// compiler-specific intrinsic. Read-only and bounded.
std::vector<std::string> DebugUtils::ScanErrorModules(
    size_t maxModules, size_t* strictCount, size_t* totalCount)
{
    std::vector<std::string> modules;

    size_t strict = 0;
    size_t total = 0;

    // Anchor on a local; on x86 the stack grows down, so higher addresses hold
    // older (caller) frames. Scan a bounded window upward. The window is wide
    // enough to reach the outermost mod frame of a deep game/mod call chain.
    const DWORD anchor = reinterpret_cast<DWORD>(&modules);
    constexpr DWORD kScanBytes = 0x4000;

    for (DWORD offset = 0; offset < kScanBytes; offset += sizeof(DWORD))
    {
        DWORD candidate = 0;
        if (!ReadDwordIfReadable(
                reinterpret_cast<const void*>(anchor + offset), candidate))
            break;

        if (candidate < 0x10000 ||
            !IsExecutableAddress(reinterpret_cast<const void*>(candidate)))
            continue;

        HMODULE module = nullptr;
        if (!GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(candidate),
                &module) || module == nullptr)
        {
            continue;
        }

        if (IsIgnoredModuleHandle(module) || IsSystemModuleHandle(module))
            continue;

        bool frameStrict = false;
        if (!IsPlausibleReturnAddress(candidate, module, &frameStrict))
            continue;

        // Count FRAMES, not unique modules: `strict`/`total` is the share of
        // examined frames that were E8-verified. Counting `strict` per frame but
        // `total` per unique module let strict exceed total (e.g. "14/5").
        ++total;
        if (frameStrict)
            ++strict;

        PushUniqueFrame(modules, candidate);

        if (modules.size() >= maxModules)
            break;
    }

    if (strictCount != nullptr)
        *strictCount = strict;
    if (totalCount != nullptr)
        *totalCount = total;

    return modules;
}

