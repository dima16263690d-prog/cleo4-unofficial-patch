#include "DebugUtils.h"
#include "DebugUtils.Internal.h"
#include "DebugUtils.RwErrorNames.h"
#include "resource.h"
#include <windows.h>
#include <psapi.h>
#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// RenderWare / CLEO error diagnostics.
//
// Two independent error sources are observed and correlated with the frame in
// which they appeared:
//
//   * RenderWare errors. RenderWare keeps the last error as a small
//     RwError {pluginID, errorCode} pair. plugin-sdk resolves RwErrorGet at a
//     fixed address for GTA SA 1.0 US (0x00808880); calling it is read-only and
//     copies the current pair into the caller's struct. The monitor polls it once
//     per frame and reports any *change* of the pair, which is the only way to
//     see a RenderWare error that did not (yet) crash the process. Nothing in the
//     game is patched: the monitor never modifies code or data.
//
//   * CLEO errors. The core routes CLEO_DEBUG_ERROR / CLEO_DEBUG_WARNING messages
//     through OnCoreLog synchronously on the thread that raised them. The stack at
//     that moment is scanned for the first return address in a module that is
//     neither the game, nor the CLEO core, nor DebugUtils; that module is the
//     mod/plugin/loader responsible.
//
// Attribution is deliberately conservative and labelled as such in the log: a
// module seen on the stack (or a script active at the time) is a strong suspect,
// not proof. The goal is to point a human at the right DLL or .cs file, not to
// claim a root cause.

namespace
{
    // plugin-sdk: RwError* RwErrorGet(RwError* code) at 0x00808880 (SA 1.0 US).
    // RenderWare is statically linked into gta_sa.exe, so this lives in the game
    // module. RwErrorGet *copies and clears* the current global error (RenderWare
    // resets it to E_RW_NOERROR once read), so the pair is restored afterwards
    // with RwErrorSet to leave the game's own error handling untouched.
    typedef void* (__cdecl *RwErrorGetFn)(void* code);
    typedef void* (__cdecl *RwErrorSetFn)(void* code);

    constexpr DWORD kGtaSa10RwErrorGet = 0x00808880;
    constexpr DWORD kGtaSa10RwErrorSet = 0x00808820;

    struct RwErrorPair
    {
        int pluginID;
        int errorCode;
    };

    // Highest errorCode bit means "common" error, whose numeric value is
    // 0x80000000 + index in the generated table.
    constexpr unsigned int kRwCommonErrorBit = 0x80000000u;

    // Pack a RenderWare error pair into one dedup key. pluginID and errorCode are
    // each small enough that 20/12 bits never collide in practice.
    unsigned int RwKey(int pluginId, int errorCode)
    {
        return (static_cast<unsigned int>(pluginId) << 12) ^
               (static_cast<unsigned int>(errorCode) & 0xFFFu);
    }

    // Resolve a RenderWare error pair to a name and a human-readable text.
    // Returns false when the code is not in either generated table (an unknown
    // plugin error), in which case the caller reports the raw numbers.
    bool RwResolveError(unsigned int pluginId, unsigned int errorCode,
                        const char*& name, const char*& text)
    {
        using namespace DebugUtilsRwErrorNames;

        if ((errorCode & 0x80000000u) != 0)
        {
            for (unsigned i = 0; i < kCount; ++i)
            {
                if (kNames[i].code == errorCode)
                {
                    name = kNames[i].name;
                    text = kNames[i].text;
                    return true;
                }
            }
            name = "RW_COMMON_?";
            text = "";
            return true;
        }

        if (pluginId == kCorePluginId)
        {
            for (unsigned i = 0; i < kCoreCount; ++i)
            {
                if (kCoreNames[i].code == errorCode)
                {
                    name = kCoreNames[i].name;
                    text = kCoreNames[i].text;
                    return true;
                }
            }
        }

        name = "RW_PLUGIN_ERR";
        text = "";
        return false;
    }

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
    // Case-sensitive glob for already-lowercased input: '*' matches any run of
    // characters (including none), '?' matches exactly one. Used for the
    // FileWatchNames masks such as "*.txd" or "lunar.*".
    bool GlobMatchImpl(const char* text, const char* pattern)
    {
        while (*pattern != '\0')
        {
            if (*pattern == '*')
            {
                // Collapse consecutive stars, then try to match the rest of the
                // pattern at every remaining position of the text.
                while (*pattern == '*')
                    ++pattern;
                if (*pattern == '\0')
                    return true;
                while (*text != '\0')
                {
                    if (GlobMatchImpl(text, pattern))
                        return true;
                    ++text;
                }
                return GlobMatchImpl(text, pattern);
            }

            if (*text == '\0')
                return false;

            if (*pattern != '?' && *pattern != *text)
                return false;

            ++text;
            ++pattern;
        }

        return *text == '\0';
    }

    bool MatchGlob(const std::string& text, const std::string& pattern)
    {
        return GlobMatchImpl(text.c_str(), pattern.c_str());
    }

    // Absolute path the game actually looks in for a (possibly relative) name.
    // A relative name is resolved against the process working directory, which
    // for GTA SA is the folder holding gta_sa.exe - so this shows exactly where
    // a missing file was expected.
    std::string ResolveReportPath(const char* path)
    {
        if (path == nullptr)
            return {};

        char full[MAX_PATH] = {};
        const DWORD length = GetFullPathNameA(path, MAX_PATH, full, nullptr);
        if (length == 0 || length >= MAX_PATH)
            return std::string(path);

        return std::string(full);
    }

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


std::string DebugUtils::CurrentScriptName()
{
    CLEO_CrashSnapshot snapshot{};
    if (CLEO_DebugGetCrashSnapshot(&snapshot) && snapshot.scriptPtr != 0)
    {
        char name[9];
        memcpy(name, snapshot.scriptName, 8);
        name[8] = '\0';

        // Trim trailing spaces/nulls so the log is tidy.
        size_t len = strlen(name);
        while (len > 0 && (name[len - 1] == ' ' || name[len - 1] == '\0'))
            name[--len] = '\0';

        if (len > 0)
            return std::string(name, len);
    }

    return "none";
}

DWORD DebugUtils::CurrentScriptOpcode()
{
    CLEO_CrashSnapshot snapshot{};
    if (CLEO_DebugGetCrashSnapshot(&snapshot) && snapshot.scriptPtr != 0)
        return snapshot.opcode;

    return 0xFFFFFFFFu;
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

        if (frameStrict)
            ++strict;

        PushUniqueFrame(modules, candidate);

        if (modules.size() >= maxModules)
            break;
    }

    if (strictCount != nullptr)
        *strictCount = strict;
    if (totalCount != nullptr)
        *totalCount = modules.size();

    return modules;
}

void DebugUtils::PollRenderWareError()
{
    if (!m_frameTimeEnabled || !m_errorDiagEnabled || !m_rwErrorWatchEnabled)
        return;

    // The hardcoded address is the SA 1.0 US RenderWare build; skip on anything
    // else rather than read a wrong address.
    if (CLEO_GetGameVersion() != GV_US10)
    {
        if (!m_rwErrorPrimed)
        {
            m_rwErrorPrimed = true;
            WriteFrameTime(
                "[rw] error watch disabled: RenderWare error address is SA 1.0 US only"
            );
        }
        return;
    }

    const auto rwErrorGet = reinterpret_cast<RwErrorGetFn>(kGtaSa10RwErrorGet);
    RwErrorPair error{};
    rwErrorGet(&error);

    // RwErrorGet clears the global error as a side effect. Put it straight back
    // so the monitor is transparent: the game's own RwErrorGet still sees it.
    // RwErrorSet only stores when the global is currently E_RW_NOERROR, which it
    // now is, so this restores exactly the pair that was just read.
    const auto rwErrorSet = reinterpret_cast<RwErrorSetFn>(kGtaSa10RwErrorSet);
    rwErrorSet(&error);

    const unsigned int pluginId = static_cast<unsigned int>(error.pluginID);
    const unsigned int errorCode = static_cast<unsigned int>(error.errorCode);

    // Prime on the first poll so a pre-existing error is not reported as if it
    // had just happened.
    if (!m_rwErrorPrimed)
    {
        m_rwErrorPrimed = true;
        m_rwLastPluginId = pluginId;
        m_rwLastErrorCode = errorCode;
        return;
    }

    if (pluginId == m_rwLastPluginId && errorCode == m_rwLastErrorCode)
        return;

    m_rwLastPluginId = pluginId;
    m_rwLastErrorCode = errorCode;

    // RwErrorGet is a *consuming* read: once it has handed back a real error,
    // RenderWare resets the global to E_RW_NOERROR, so that value is observed
    // exactly once after every real error and is never itself an error. {0, 0}
    // is the same "no error" state. Reporting either would turn each real error
    // into a phantom E_RW_NOERROR record.
    const bool noError =
        (pluginId == 0 && errorCode == 0) || (errorCode == kRwCommonErrorBit);
    if (noError)
        return;

    const char* resolvedName = nullptr;
    const char* resolvedText = nullptr;
    const bool known = RwResolveError(pluginId, errorCode, resolvedName, resolvedText);

    // RenderWare errors are noticed on the next frame's poll, not at the instant
    // they are raised, so the current call stack is not the raising stack and is
    // deliberately not scanned here. Attribution for these comes from the script
    // active at the time; the report says so explicitly.
    const std::vector<std::string> modules;

    RecordError(
        RwKey(error.pluginID, error.errorCode),
        known ? std::string(resolvedName) : std::string("RW_PLUGIN_ERR"),
        pluginId,
        errorCode,
        modules,
        true,
        known ? resolvedText : nullptr
    );
}

// ---------------------------------------------------------------------------
// RenderWare file-open watch.
//
// E_RW_BADOPEN is raised inside RenderWare's stream-open-by-name path, which
// reaches the OS through the file-function table returned by
// RwOsGetFileInterface() (a RwFileFunctions* living in RenderWare's globals).
// Replacing that table's rwfopen pointer with our own wrapper is a *data-only*
// patch: no instruction is overwritten and no trampoline is needed, so it
// carries none of the risk of a code detour. The wrapper calls the original and
// records the name whenever it returns null, which is exactly the condition that
// makes RenderWare raise E_RW_BADOPEN. This turns "some texture failed" into the
// exact path, e.g. "models\cutscene\foo.txd".
// ---------------------------------------------------------------------------
namespace
{
    // plugin-sdk: RwFileFunctions* RwOsGetFileInterface(void) at 0x00804130.
    constexpr DWORD kGtaSa10RwOsGetFileInterface = 0x00804130;

    // Prefix of RwFileFunctions that we need. The real struct is
    // { rwfexist, rwfopen, rwfclose, rwfread, ... }; only the first two matter.
    struct RwFileFunctionsHead
    {
        void* rwfexist; // RwBool (*)(const RwChar* name)
        void* rwfopen;  // void*  (*)(const RwChar* name, const RwChar* mode)
    };

    typedef RwFileFunctionsHead* (__cdecl *RwOsGetFileInterfaceFn)(void);
    typedef void* (__cdecl *RwFopenFn)(const char* name, const char* mode);

    // Assigned during InstallFileOpenWatch, read on every intercepted open.
    RwFopenFn g_rwFopenOriginal = nullptr;
}
void* __cdecl DebugUtils::FileOpenWatchFopen(const char* name, const char* mode)
{
    RwFopenFn original = g_rwFopenOriginal;
    if (original == nullptr)
        return nullptr;

    void* handle = original(name, mode);

    if (name != nullptr && name[0] != '\0' && s_instance != nullptr)
    {
        // A null handle is a failed open, which is what RenderWare turns into
        // E_RW_BADOPEN. Anything else opened fine and is not interesting.
        if (handle == nullptr)
            s_instance->RecordFileOpenFailure(name);

        // Files named on the watchlist are recorded whether or not the open
        // succeeded, so "who loads particle.txd" can be answered even when the
        // file exists.
        s_instance->RecordWatchedOpen(name, handle == nullptr);
    }

    return handle;
}

bool DebugUtils::IsWatchedFileName(const char* path) const
{
    if (path == nullptr || m_fileWatchNames.empty())
        return false;

    std::string lower(path);
    for (char& c : lower)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));

    // Each entry may be a plain substring ("particle.txd"), a glob mask
    // ("*.txd", "lunar.*"), or a bare name ("lunar"). A match on any entry
    // watches the file.
    for (const auto& needle : m_fileWatchNames)
    {
        if (needle.empty())
            continue;

        // Glob: '*' matches any run of characters, '?' matches one. A pattern
        // without a wildcard behaves exactly like the old substring test.
        const bool hasWildcard =
            needle.find('*') != std::string::npos || needle.find('?') != std::string::npos;

        if (!hasWildcard)
        {
            if (lower.find(needle) != std::string::npos)
                return true;
            continue;
        }

        // Anchor the glob to the file name (after the last path separator), so
        // "*.txd" means "any .txd file", not "any path ending in .txd".
        std::string fileName = lower;
        const size_t slash = fileName.find_last_of("\\/");
        if (slash != std::string::npos)
            fileName = fileName.substr(slash + 1);

        if (MatchGlob(fileName, needle) || MatchGlob(lower, needle))
            return true;
    }

    return false;
}

void DebugUtils::RecordWatchedOpen(const char* path, bool failed)
{
    if (!IsWatchedFileName(path))
        return;

    const std::string key(path);
    const DWORD now = GetTickCount();
    const DWORD uptimeSec = (now - m_memorySessionStartTick) / 1000u;
    const std::string script = CurrentScriptName();
    const std::string resolved = ResolveReportPath(path);

    std::lock_guard<std::mutex> lock(m_fileWatchRecordMutex);

    auto it = m_fileWatchRecords.find(key);
    if (it == m_fileWatchRecords.end())
    {
        FileOpenRecord record;
        record.count = 1;
        record.firstUptimeSec = uptimeSec;
        record.lastUptimeSec = uptimeSec;
        record.failed = failed;
        record.lastScript = script;
        record.resolvedPath = resolved;

        m_fileWatchRecordOrder.push_back(key);
        it = m_fileWatchRecords.emplace(key, std::move(record)).first;
    }
    else
    {
        FileOpenRecord& record = it->second;
        ++record.count;
        record.lastUptimeSec = uptimeSec;
        record.failed = failed;
        record.lastScript = script;
        record.resolvedPath = resolved;
    }

    FileOpenRecord& stored = it->second;
    if (!stored.modulesScanned)
    {
        stored.modulesScanned = true;
        if (m_errorModuleScanEnabled)
        {
            const char* via = nullptr;
            stored.modules = CaptureRequesterChain(4, &via);
            stored.chainVia = via ? via : "";
        }
    }
}

void DebugUtils::WriteWatchedOpenReport(bool forceAll)
{
    std::map<std::string, FileOpenRecord> records;
    std::vector<std::string> order;
    {
        std::lock_guard<std::mutex> lock(m_fileWatchRecordMutex);
        records = m_fileWatchRecords;
        order = m_fileWatchRecordOrder;
    }

    if (records.empty())
        return;

    // Dedup like the failed-open report: print only what changed since last time.
    std::vector<std::string> changedKeys;
    for (const auto& key : order)
    {
        auto it = records.find(key);
        if (it == records.end())
            continue;
        if (forceAll || it->second.count != it->second.lastReportedCount)
            changedKeys.push_back(key);
    }

    if (changedKeys.empty())
        return;

    WriteFrameTime("================= watched file opens =================");
    WriteFrameTime("%-3s %-46s %6s %5s %5s %-6s %-14s", "#", "path", "count", "first", "last", "result", "script");

    size_t index = 0;
    for (const auto& key : changedKeys)
    {
        auto it = records.find(key);
        if (it == records.end())
            continue;

        const FileOpenRecord& record = it->second;
        ++index;

        std::string path = key;
        if (path.size() > 46)
            path = std::string("...") + path.substr(path.size() - 43);

        WriteFrameTime(
            "%-3zu %-46s %6u %5u %5u %-6s %-14s",
            index,
            path.c_str(),
            static_cast<unsigned>(record.count),
            static_cast<unsigned>(record.firstUptimeSec),
            static_cast<unsigned>(record.lastUptimeSec),
            record.failed ? "FAILED" : "ok",
            record.lastScript.empty() ? "?" : record.lastScript.c_str()
        );

        if (!record.modules.empty())
        {
            std::string chain;
            for (const auto& module : record.modules)
            {
                if (!chain.empty())
                    chain += " <- ";
                chain += module;
            }
            WriteFrameTime("      requested by: %s", chain.c_str());
            if (!record.chainVia.empty())
                WriteFrameTime("      chain via: %s", record.chainVia.c_str());
        }

        if (!record.resolvedPath.empty())
            WriteFrameTime("      looked in: %s", record.resolvedPath.c_str());
    }

    WriteFrameTime(
        "[filewatch] watchlist: %zu path(s) matched (FileWatchNames)",
        records.size()
    );

    {
        std::lock_guard<std::mutex> lock(m_fileWatchRecordMutex);
        for (auto& entry : m_fileWatchRecords)
            entry.second.lastReportedCount = entry.second.count;
    }
}

void DebugUtils::InstallFileOpenWatch()
{
    if (m_fileWatchInstalled || !m_fileWatchEnabled)
        return;

    // The table address is for the SA 1.0 US RenderWare build; refuse anything
    // else rather than poke a wrong pointer.
    if (CLEO_GetGameVersion() != GV_US10)
    {
        WriteFrameTime(
            "[filewatch] disabled: RenderWare file interface address is SA 1.0 US only"
        );
        return;
    }

    const auto getFileInterface =
        reinterpret_cast<RwOsGetFileInterfaceFn>(kGtaSa10RwOsGetFileInterface);

    RwFileFunctionsHead* table = getFileInterface();
    if (table == nullptr || table->rwfopen == nullptr)
    {
        WriteFrameTime("[filewatch] disabled: RenderWare file interface unavailable");
        return;
    }

    // Guard against a wrong table layout: the pointer we are about to replace
    // must be executable code inside the game image. Wrapping a data pointer and
    // then calling it would crash, so refuse anything that is not executable.
    {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(table->rwfopen, &info, sizeof(info)) == 0 ||
            (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                             PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) == 0)
        {
            WriteFrameTime(
                "[filewatch] disabled: RenderWare rwfopen is not executable code"
            );
            return;
        }
    }

    // Keep the original so our wrapper (and, on unload, the game) still opens
    // files exactly as before.
    g_rwFopenOriginal = reinterpret_cast<RwFopenFn>(table->rwfopen);
    m_fileWatchOriginalFopen = table->rwfopen;
    m_fileWatchTable = table;

    DWORD oldProtect = 0;
    if (!VirtualProtect(&table->rwfopen, sizeof(void*), PAGE_READWRITE, &oldProtect))
    {
        g_rwFopenOriginal = nullptr;
        WriteFrameTime("[filewatch] disabled: could not make the table writable");
        return;
    }

    table->rwfopen = reinterpret_cast<void*>(&DebugUtils::FileOpenWatchFopen);
    VirtualProtect(&table->rwfopen, sizeof(void*), oldProtect, &oldProtect);

    m_fileWatchInstalled = true;
    WriteFrameTime(
        "[filewatch] enabled: RenderWare file opens are captured; failures are "
        "reported with the exact path"
    );
}

void DebugUtils::RemoveFileOpenWatch()
{
    if (!m_fileWatchInstalled || m_fileWatchTable == nullptr)
        return;

    auto* table = reinterpret_cast<RwFileFunctionsHead*>(m_fileWatchTable);

    DWORD oldProtect = 0;
    if (VirtualProtect(&table->rwfopen, sizeof(void*), PAGE_READWRITE, &oldProtect))
    {
        table->rwfopen = m_fileWatchOriginalFopen;
        VirtualProtect(&table->rwfopen, sizeof(void*), oldProtect, &oldProtect);
    }

    m_fileWatchInstalled = false;
    m_fileWatchTable = nullptr;
    m_fileWatchOriginalFopen = nullptr;
    g_rwFopenOriginal = nullptr;
}

void DebugUtils::RecordFileOpenFailure(const char* path)
{
    if (!m_fileWatchFailuresEnabled)
        return;

    if (path == nullptr || path[0] == '\0')
        return;

    const std::string key(path);
    const DWORD now = GetTickCount();
    const DWORD uptimeSec = (now - m_memorySessionStartTick) / 1000u;
    const std::string script = CurrentScriptName();
    const std::string resolved = ResolveReportPath(path);

    std::lock_guard<std::mutex> lock(m_fileOpenMutex);

    auto it = m_fileOpenFailures.find(key);
    if (it == m_fileOpenFailures.end())
    {
        FileOpenFailure failure;
        failure.count = 1;
        failure.firstUptimeSec = uptimeSec;
        failure.lastUptimeSec = uptimeSec;
        failure.lastScript = script;
        failure.resolvedPath = resolved;

        m_fileOpenOrder.push_back(key);
        it = m_fileOpenFailures.emplace(key, std::move(failure)).first;
    }
    else
    {
        FileOpenFailure& failure = it->second;
        ++failure.count;
        failure.lastUptimeSec = uptimeSec;
        failure.lastScript = script;
        failure.resolvedPath = resolved;
    }

    // Walk the stack once per distinct path, at the moment of the failed open:
    // the caller chain is the mod/plugin that asked RenderWare for the file. Done
    // under the lock only on first sight so the scan cost is paid once per path.
    FileOpenFailure& stored = it->second;
    if (!stored.modulesScanned)
    {
        stored.modulesScanned = true;
        if (m_errorModuleScanEnabled)
        {
            const char* via = nullptr;
            stored.modules = CaptureRequesterChain(4, &via);
            stored.chainVia = via ? via : "";
        }
    }
}

void DebugUtils::WriteFileOpenReport(bool forceAll)
{
    std::map<std::string, FileOpenFailure> failures;
    std::vector<std::string> order;
    {
        std::lock_guard<std::mutex> lock(m_fileOpenMutex);
        failures = m_fileOpenFailures;
        order = m_fileOpenOrder;
    }

    if (failures.empty())
        return;

    // Dedup like the error report: print only what changed since last time (a new
    // path, or one whose count grew), never the whole table again.
    std::vector<std::string> changedKeys;
    for (const auto& key : order)
    {
        auto it = failures.find(key);
        if (it == failures.end())
            continue;
        if (forceAll || it->second.count != it->second.lastReportedCount)
            changedKeys.push_back(key);
    }

    if (changedKeys.empty())
        return;

    WriteFrameTime("================= failed file opens =================");
    WriteFrameTime("%-3s %-52s %8s %5s %5s %-16s", "#", "path", "count", "first", "last", "script");

    size_t index = 0;
    for (const auto& key : changedKeys)
    {
        auto it = failures.find(key);
        if (it == failures.end())
            continue;

        const FileOpenFailure& failure = it->second;
        ++index;

        std::string path = key;
        // Keep the row readable when a path is very long.
        if (path.size() > 52)
            path = std::string("...") + path.substr(path.size() - 49);
        WriteFrameTime(
            "%-3zu %-52s %8u %5u %5u %-16s",
            index,
            path.c_str(),
            static_cast<unsigned>(failure.count),
            static_cast<unsigned>(failure.firstUptimeSec),
            static_cast<unsigned>(failure.lastUptimeSec),
            failure.lastScript.empty() ? "?" : failure.lastScript.c_str()
        );

        // Name the module(s) that requested the file: this is the concrete
        // "which mod" answer (e.g. a .asi plugin), more specific than the
        // script bridge in the script column.
        if (!failure.modules.empty())
        {
            std::string chain;
            for (const auto& module : failure.modules)
            {
                if (!chain.empty())
                    chain += " <- ";
                chain += module;
            }
            WriteFrameTime("      requested by: %s", chain.c_str());
            if (!failure.chainVia.empty())
                WriteFrameTime("      chain via: %s", failure.chainVia.c_str());
        }

        // The absolute path the game looked in, so the exact folder to put the
        // file into is unambiguous.
        if (!failure.resolvedPath.empty())
            WriteFrameTime("      looked in: %s", failure.resolvedPath.c_str());

        // A bare name (no directory) is resolved against the game's working
        // directory (the GTA SA folder), which is the usual reason a file a mod
        // ships "goes missing": it was put in the wrong place.
        if (key.find('\\') == std::string::npos && key.find('/') == std::string::npos)
        {
            WriteFrameTime(
                "      hint: bare name - it is looked up in the game folder "
                "(where gta_sa.exe is); put the file there or fix the mod's path"
            );
        }
    }

    WriteFrameTime(
        "[filewatch] %zu distinct path(s) failed to open; these are the files "
        "behind E_RW_BADOPEN / E_RW_READTEXMASK",
        failures.size()
    );

    {
        std::lock_guard<std::mutex> lock(m_fileOpenMutex);
        for (auto& entry : m_fileOpenFailures)
            entry.second.lastReportedCount = entry.second.count;
    }
}

// ---------------------------------------------------------------------------
// RenderWare texture-read watch.
//
// E_RW_READTEXMASK ("Unable to read Texture %s / Mask %s") is raised by the
// *name-based* reader RwTextureRead (SA 1.0 US 0x7F3AC0) when its read callback
// returns NULL. The error poll in PollRenderWareError runs one frame later, when
// the raising stack is already gone, so it cannot name the mod. We wrap that read
// callback (through the public RwTextureGetReadCallBack/RwTextureSetReadCallBack,
// SA 0x7F3520/0x7F3500) and, as a secondary, the stream reader
// RwGlobals.stdFunc[26] (SA 0x4CD820, the TXD-internal path). Both run while the
// game thread is inside the mod that asked for the texture, so the requester
// chain is captured at the right moment. Both are data-only swaps (no code is
// modified), like the rwfopen watch, so they carry none of the risk of a detour.
// ---------------------------------------------------------------------------
namespace
{
    // RwEngineInstance holds a pointer to RwGlobals; stdFunc[26] sits at +0xB0.
    constexpr DWORD kGtaSa10RwEngineInstance = 0xC97B24;
    constexpr DWORD kRwStdFuncNativeTextureReadOffset = 0xB0;

    // RwTextureRead's read callback, swapped through the public API so we never
    // touch code: RwTextureCallBackRead is RwTexture* cdecl (name, maskName).
    constexpr DWORD kGtaSa10RwTextureGetReadCallBack = 0x7F3520;
    constexpr DWORD kGtaSa10RwTextureSetReadCallBack = 0x7F3500;

    typedef void* (__cdecl* RwTextureCallBackReadFn)(const char* name, const char* maskName);
    typedef RwTextureCallBackReadFn(__cdecl* RwTextureGetReadCallBackFn)(void);
    typedef int(__cdecl* RwTextureSetReadCallBackFn)(RwTextureCallBackReadFn callBack);

    typedef int(__cdecl* RwNativeTextureReadFn)(void* stream, void** texture, int length);

    // Assigned during InstallTextureReadWatch, read on every intercepted call.
    RwNativeTextureReadFn g_rwTextureReadOriginal = nullptr;
    RwTextureCallBackReadFn g_rwTextureReadCbOriginal = nullptr;
}

// Wraps RwTextureRead's read callback (the name-based path). A NULL return is
// exactly what makes RwTextureRead raise E_RW_READTEXMASK, and it happens while
// the requesting mod is still on the stack.
void* __cdecl DebugUtils::TextureReadCallbackWatch(const char* name, const char* maskName)
{
    RwTextureCallBackReadFn original = g_rwTextureReadCbOriginal;
    if (original == nullptr)
        return nullptr;

    void* texture = original(name, maskName);

    if (texture == nullptr && s_instance != nullptr)
        s_instance->RecordTextureReadFailure(name);

    return texture;
}

int __cdecl DebugUtils::TextureReadWatchRead(void* stream, void** texture, int length)
{
    RwNativeTextureReadFn original = g_rwTextureReadOriginal;
    if (original == nullptr)
        return 0;

    const int result = original(stream, texture, length);

    // The stream (TXD-internal) path does not raise E_RW_READTEXMASK, but a false
    // return still means a texture failed to read, so it is recorded too.
    if (result == 0 && s_instance != nullptr)
        s_instance->RecordTextureReadFailure(nullptr);

    return result;
}

void DebugUtils::InstallTextureReadWatch()
{
    if (!m_textureWatchEnabled)
        return;
    if (m_textureReadCbInstalled || m_textureWatchInstalled)
        return;

    // The addresses are SA 1.0 US specific; refuse anything else rather than call
    // a wrong pointer and crash the game.
    if (CLEO_GetGameVersion() != GV_US10)
    {
        WriteFrameTime(
            "[texturewatch] disabled: RenderWare texture-read addresses are SA 1.0 US only"
        );
        return;
    }

    // 1) The name-based reader's callback - the raiser of E_RW_READTEXMASK. This
    //    is a public RW API, so swapping it is a data-only change.
    auto getReadCallback = reinterpret_cast<RwTextureGetReadCallBackFn>(
        kGtaSa10RwTextureGetReadCallBack);
    auto setReadCallback = reinterpret_cast<RwTextureSetReadCallBackFn>(
        kGtaSa10RwTextureSetReadCallBack);

    RwTextureCallBackReadFn current = getReadCallback();
    {
        MEMORY_BASIC_INFORMATION info{};
        if (current == nullptr ||
            VirtualQuery(reinterpret_cast<void*>(current), &info, sizeof(info)) == 0 ||
            (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                             PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) == 0)
        {
            WriteFrameTime(
                "[texturewatch] disabled: RwTextureRead callback is not executable code"
            );
            return;
        }
    }

    g_rwTextureReadCbOriginal = current;
    m_textureReadCbOriginal = reinterpret_cast<void*>(current);
    setReadCallback(&DebugUtils::TextureReadCallbackWatch);
    m_textureReadCbInstalled = true;

    // 2) The stream reader RwGlobals.stdFunc[26] (the TXD-internal path). Optional:
    //    if the layout guard fails we still keep the callback watch above.
    void* rwGlobals = *reinterpret_cast<void**>(kGtaSa10RwEngineInstance);
    if (rwGlobals != nullptr)
    {
        void** slot = reinterpret_cast<void**>(
            reinterpret_cast<uintptr_t>(rwGlobals) + kRwStdFuncNativeTextureReadOffset);

        MEMORY_BASIC_INFORMATION info{};
        if (*slot != nullptr &&
            VirtualQuery(*slot, &info, sizeof(info)) != 0 &&
            (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                             PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0)
        {
            g_rwTextureReadOriginal = reinterpret_cast<RwNativeTextureReadFn>(*slot);
            m_textureWatchOriginalRead = *slot;
            m_textureWatchSlot = slot;

            DWORD oldProtect = 0;
            if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect))
            {
                *slot = reinterpret_cast<void*>(&DebugUtils::TextureReadWatchRead);
                VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
                m_textureWatchInstalled = true;
            }
            else
            {
                g_rwTextureReadOriginal = nullptr;
                m_textureWatchSlot = nullptr;
            }
        }
    }

    WriteFrameTime(
        "[texturewatch] enabled: RenderWare texture reads are captured "
        "(read callback=%u, stream slot=%u); a failed read is reported with the "
        "mod that requested it",
        m_textureReadCbInstalled ? 1u : 0u,
        m_textureWatchInstalled ? 1u : 0u
    );
}

void DebugUtils::RemoveTextureReadWatch()
{
    if (m_textureReadCbInstalled)
    {
        auto setReadCallback = reinterpret_cast<RwTextureSetReadCallBackFn>(
            kGtaSa10RwTextureSetReadCallBack);
        setReadCallback(reinterpret_cast<RwTextureCallBackReadFn>(m_textureReadCbOriginal));
        m_textureReadCbInstalled = false;
        m_textureReadCbOriginal = nullptr;
        g_rwTextureReadCbOriginal = nullptr;
    }

    if (m_textureWatchInstalled && m_textureWatchSlot != nullptr)
    {
        auto* slot = reinterpret_cast<void**>(m_textureWatchSlot);

        DWORD oldProtect = 0;
        if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect))
        {
            *slot = m_textureWatchOriginalRead;
            VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
        }

        m_textureWatchInstalled = false;
        m_textureWatchSlot = nullptr;
        m_textureWatchOriginalRead = nullptr;
        g_rwTextureReadOriginal = nullptr;
    }
}

void DebugUtils::RecordTextureReadFailure(const char* textureName)
{
    const DWORD now = GetTickCount();
    const DWORD uptimeSec = (now - m_memorySessionStartTick) / 1000u;
    const std::string script = CurrentScriptName();

    // Key by texture name so the report says *which* texture failed as well as
    // which mod asked for it; an unnamed read (stream path) collapses to one row.
    const std::string key =
        (textureName != nullptr && textureName[0] != '\0')
            ? std::string(textureName)
            : std::string("(unnamed texture read)");

    std::lock_guard<std::mutex> lock(m_textureReadMutex);

    auto it = m_textureReadFailures.find(key);
    if (it == m_textureReadFailures.end())
    {
        TextureReadFailure failure;
        failure.count = 1;
        failure.firstUptimeSec = uptimeSec;
        failure.lastUptimeSec = uptimeSec;
        failure.lastScript = script;

        m_textureReadOrder.push_back(key);
        it = m_textureReadFailures.emplace(key, std::move(failure)).first;
    }
    else
    {
        TextureReadFailure& failure = it->second;
        ++failure.count;
        failure.lastUptimeSec = uptimeSec;
        failure.lastScript = script;
    }

    // Capture the chain only on the first failure of that texture, while the
    // raising frame is still on the stack; the scan cost is paid once per name.
    TextureReadFailure& stored = it->second;
    if (!stored.modulesScanned)
    {
        stored.modulesScanned = true;
        if (m_errorModuleScanEnabled)
        {
            const char* via = nullptr;
            stored.modules = CaptureRequesterChain(6, &via);
            stored.chainVia = via ? via : "";
        }
    }
}

void DebugUtils::WriteTextureReadReport(bool forceAll)
{
    std::map<std::string, TextureReadFailure> failures;
    std::vector<std::string> order;
    {
        std::lock_guard<std::mutex> lock(m_textureReadMutex);
        failures = m_textureReadFailures;
        order = m_textureReadOrder;
    }

    if (failures.empty())
        return;

    // Print only the rows that changed since the last report (a new texture, or
    // one whose count grew). Printing the whole table every interval turned a
    // handful of failures into thousands of identical lines.
    std::vector<const std::pair<const std::string, TextureReadFailure>*> changed;
    for (const auto& key : order)
    {
        auto it = failures.find(key);
        if (it == failures.end())
            continue;
        if (forceAll || it->second.count != it->second.lastReportedCount)
            changed.push_back(&*it);
    }

    if (changed.empty())
        return;

    // Group the changed rows by their requester chain: many textures are read by
    // the same mod, so the chain is printed once and the textures listed under it
    // instead of repeating the identical "requested by" line per texture.
    std::vector<std::string> chainOrder;
    std::map<std::string, std::vector<const std::pair<const std::string, TextureReadFailure>*>> byChain;
    for (const auto* entry : changed)
    {
        std::string chain;
        for (const auto& module : entry->second.modules)
        {
            if (!chain.empty())
                chain += " <- ";
            chain += module;
        }
        if (chain.empty())
            chain = "(no mod frame captured - enable ModuleScan)";

        auto it = byChain.find(chain);
        if (it == byChain.end())
            chainOrder.push_back(chain);
        byChain[chain].push_back(entry);
    }

    WriteFrameTime("================= failed texture reads =================");
    WriteFrameTime("%zu changed of %zu texture(s); grouped by the mod that read them",
                   changed.size(), failures.size());

    for (const auto& chain : chainOrder)
    {
        WriteFrameTime("  requested by: %s", chain.c_str());
        // The chain method is the same for every texture under it, so it is stated
        // once, from the first entry that has one.
        const auto& group = byChain[chain];
        for (const auto* entry : group)
        {
            if (!entry->second.chainVia.empty())
            {
                WriteFrameTime("    chain via: %s", entry->second.chainVia.c_str());
                break;
            }
        }

        for (const auto* entry : group)
        {
            std::string name = entry->first;
            if (name.size() > 34)
                name = std::string("...") + name.substr(name.size() - 31);

            WriteFrameTime(
                "    %-34s count=%-5u first=%-5u last=%-5u script=%s",
                name.c_str(),
                static_cast<unsigned>(entry->second.count),
                static_cast<unsigned>(entry->second.firstUptimeSec),
                static_cast<unsigned>(entry->second.lastUptimeSec),
                entry->second.lastScript.empty() ? "?" : entry->second.lastScript.c_str()
            );
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_textureReadMutex);
        for (auto& entry : m_textureReadFailures)
            entry.second.lastReportedCount = entry.second.count;
    }
}

void DebugUtils::RecordError(
    unsigned int key,
    const std::string& name,
    unsigned int pluginId,
    unsigned int errorCode,
    const std::vector<std::string>& modules,
    bool renderWare,
    const char* text
)
{
    // Bound the number of distinct records so a storm of unique messages cannot
    // grow the map without limit. Once full, only existing keys are counted.
    constexpr size_t kMaxDistinctErrors = 128;

    // A RenderWare error is noticed on the next frame's poll, so the script and
    // opcode active now are not the ones that raised it; leave the opcode unset
    // so the report does not print a misleading "last opcode" line for it.
    const DWORD opcode = renderWare ? 0xFFFFFFFFu : CurrentScriptOpcode();

    std::lock_guard<std::mutex> lock(m_errorMutex);

    auto it = m_errorRecords.find(key);
    if (it == m_errorRecords.end())
    {
        if (m_errorRecords.size() >= kMaxDistinctErrors)
            return;

        ErrorRecord record;
        record.key = key;
        record.name = name;
        record.pluginId = pluginId;
        record.errorCode = errorCode;
        record.firstTick = GetTickCount();
        record.lastTick = record.firstTick;
        record.lastUptimeSec =
            (GetTickCount() - m_memorySessionStartTick) / 1000u;
        record.firstUptimeSec = record.lastUptimeSec;
        record.count = 1;
        record.lastScript = CurrentScriptName();
        record.lastOpcode = opcode;
        record.modules = modules;
        record.renderWare = renderWare;
        record.text = text ? text : "";
        if (!modules.empty())
            record.module = modules.front();

        m_errorOrder.push_back(key);
        m_errorRecords.emplace(key, std::move(record));
    }
    else
    {
        ErrorRecord& record = it->second;
        ++record.count;
        record.lastTick = GetTickCount();
        record.lastUptimeSec =
            (GetTickCount() - m_memorySessionStartTick) / 1000u;
        record.lastScript = CurrentScriptName();
        record.lastOpcode = opcode;
        for (const auto& module : modules)
        {
            if (std::find(record.modules.begin(), record.modules.end(), module) ==
                record.modules.end())
            {
                record.modules.push_back(module);
            }
        }
        if (record.module.empty() && !modules.empty())
            record.module = modules.front();
    }

    for (const auto& module : modules)
        m_errorModulesSeen.insert(module);
}

void DebugUtils::WriteErrorReport(const char* reason)
{
    if (!m_frameTimeEnabled || !m_errorDiagEnabled)
        return;

    // Snapshot the records and the module set under the lock, then format
    // outside it.
    std::vector<ErrorRecord> records;
    std::vector<std::pair<unsigned int, DWORD>> reportedCounts;
    std::set<std::string> suspectModules;
    size_t changedCount = 0;
    {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        records.reserve(m_errorOrder.size());
        for (unsigned int key : m_errorOrder)
        {
            auto it = m_errorRecords.find(key);
            if (it != m_errorRecords.end())
            {
                records.push_back(it->second);
                reportedCounts.emplace_back(key, it->second.count);
                if (it->second.count != it->second.lastReportedCount)
                    ++changedCount;
            }
        }
        suspectModules = m_errorModulesSeen;
    }

    const bool forceAll = reason != nullptr && strcmp(reason, "game_end") == 0;

    const unsigned long long cleoErrors =
        static_cast<unsigned long long>(InterlockedCompareExchange(
            const_cast<volatile LONG*>(&m_cleoErrorCount), 0, 0));
    const unsigned long long cleoWarnings =
        static_cast<unsigned long long>(InterlockedCompareExchange(
            const_cast<volatile LONG*>(&m_cleoWarningCount), 0, 0));
    const bool cleoChanged =
        cleoErrors != m_lastReportedCleoErrors ||
        cleoWarnings != m_lastReportedCleoWarnings;

    // Dedup: print nothing at all when there is no new error since the last
    // report. A stable error is reported once (when it appears) instead of being
    // re-printed in full every interval.
    if (!forceAll && changedCount == 0 && !cleoChanged)
    {
        return;
    }

    WriteFrameTime("================= errors (%s) =================",
        reason ? reason : "interval");
    WriteFrameTime(
        "cleo errors=%llu warnings=%llu | distinct=%zu | suspect modules=%zu",
        cleoErrors,
        cleoWarnings,
        records.size(),
        suspectModules.size()
    );

    if (records.empty() && cleoErrors == 0 && cleoWarnings == 0)
    {
        WriteFrameTime("[errors] none: no CLEO error/warning and no RenderWare error seen");
        m_lastReportedCleoErrors = cleoErrors;
        m_lastReportedCleoWarnings = cleoWarnings;
        return;
    }

    // Only print the table header when there is a row to show; a CLEO-only change
    // with no new RenderWare record should not leave a dangling header.
    if (changedCount > 0 || forceAll)
    {
        WriteFrameTime("%-3s %-22s %8s %10s %-9s %5s %5s %-16s %s",
            "#", "name", "count", "plugin", "code", "first", "last", "script", "suspect module");
    }

    // Names whose meaning is "a file/resource could not be opened or read": a
    // missing or corrupt texture/model, almost always a mod/script that loads it.
    auto isFileResourceError = [](const ErrorRecord& record)
    {
        static const char* const kNames[] = {
            "E_RW_BADOPEN", "E_RW_READTEXMASK", "E_RW_READ",
            "E_RW_STREAMOPEN", "E_RW_BADFORMAT",
        };
        for (const char* name : kNames)
        {
            if (record.name == name)
                return true;
        }
        return false;
    };

    size_t index = 0;
    std::string unchanged;
    for (const auto& record : records)
    {
        // Dedup across intervals: a row that has not changed since it was last
        // printed is collapsed into a single "unchanged" line, so a stable error
        // is not re-printed in full (with its meaning/hint) every interval.
        const bool changed = forceAll || record.count != record.lastReportedCount;
        if (!changed)
        {
            if (!unchanged.empty())
                unchanged += ", ";
            unchanged += record.name;
            unchanged += " x";
            unchanged += std::to_string(record.count);
            continue;
        }

        ++index;

        // Detail lines (meaning/hint) are only useful the first time a record is
        // shown (or in the final game_end dump); a recurring error re-prints just
        // its row, not the same explanation every interval.
        const bool showDetail = forceAll || record.lastReportedCount == 0;

        // For a RenderWare row the script shown is the one active when the error
        // was *noticed* (next frame's poll), so mark it approximate with a '~'
        // instead of implying it raised the error.
        std::string script = record.lastScript.empty() ? "?" : record.lastScript;
        if (record.renderWare)
            script = "~" + script;

        // plugin-sdk builds a throwaway CRunningScript literally named "plg-sdk"
        // whenever a plugin calls a game script command through CallCommandById
        // (ScriptCommands.h). Seeing it here means the error surfaced inside such
        // a call from a plugin, not from a user CLEO script.
        const bool pluginBridge = script.find("plg-sdk") != std::string::npos;

        WriteFrameTime(
            "%-3zu %-22s %8u 0x%06X 0x%08X %5u %5u %-16s %s",
            index,
            record.name.c_str(),
            static_cast<unsigned>(record.count),
            record.pluginId,
            record.errorCode,
            static_cast<unsigned>(record.firstUptimeSec),
            static_cast<unsigned>(record.lastUptimeSec),
            script.c_str(),
            record.module.empty() ? "<none>" : record.module.c_str()
        );

        if (pluginBridge && showDetail)
        {
            WriteFrameTime(
                "      note: 'plg-sdk' is the plugin-sdk script-command bridge "
                "(CallCommandById), not a user script; the error came from a "
                "plugin calling a game command"
            );
        }

        if (!record.text.empty() && showDetail)
        {
            WriteFrameTime("      meaning: %s", record.text.c_str());
        }

        // Point the reader at the likely source for file/resource errors: this is
        // the "which mod causes it" answer for a missing/corrupt texture or model.
        if (isFileResourceError(record) && showDetail)
        {
            WriteFrameTime(
                "      hint: file/resource error - a missing or corrupt texture/"
                "model/.txd (or a mod loading it). Check the script/mod above and "
                "its resource files."
            );
            WriteFrameTime(
                "      hint: a rendering mod or D3D9/dinput proxy that replaces "
                "RenderWare's texture reader (NativeTextureRead, SA 0x4CD820) is a "
                "prime suspect - it can raise this for a texture the stock engine "
                "would load fine."
            );
            WriteFrameTime(
                "      hint: set [DebugUtils.Errors] TextureWatch=1 to name the mod "
                "that requested the failed texture read (the stack is captured at "
                "the read, which this poll cannot do)."
            );
        }

        // The opcode is only meaningful where the error was raised. For a
        // RenderWare row it is the poll-time opcode, not the raising one, so it
        // is omitted rather than shown as if it caused the error.
        if (!record.renderWare && record.lastOpcode != 0xFFFFFFFFu)
        {
            const DWORD opcode = record.lastOpcode & 0x7FFF;
            const char* opcodeName = OpcodeName(opcode);
            WriteFrameTime(
                "      last opcode=0x%04X %s",
                static_cast<unsigned>(opcode),
                opcodeName ? opcodeName : "unknown"
            );
        }

        if (record.modules.size() > 1)
        {
            std::string chain;
            for (const auto& module : record.modules)
            {
                if (!chain.empty())
                    chain += " -> ";
                chain += module;
            }
            WriteFrameTime("      call chain: %s", chain.c_str());
        }
    }

    if (index == 0)
    {
        WriteFrameTime("[errors] no new RenderWare errors this interval");
    }

    // Collapse the rows that did not change into one line so the table above is
    // only the errors that actually happened (or grew) in this interval.
    if (!unchanged.empty())
    {
        WriteFrameTime("[errors] unchanged since last report: %s", unchanged.c_str());
    }

    // Remember exactly what was printed (the snapshot counts) so the next report
    // only shows changes. Use the snapshotted values, not the live counts: a
    // record that grew after the snapshot must still count as changed next time.
    {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        for (const auto& entry : reportedCounts)
        {
            auto it = m_errorRecords.find(entry.first);
            if (it != m_errorRecords.end())
                it->second.lastReportedCount = entry.second;
        }
    }
    m_lastReportedCleoErrors = cleoErrors;
    m_lastReportedCleoWarnings = cleoWarnings;

    // Explicit "who to blame" summary, kept separate so it is easy to grep.
    if (!suspectModules.empty())
    {
        std::string summary;
        for (const auto& module : suspectModules)
        {
            if (!summary.empty())
                summary += ", ";
            summary += module;
        }
        WriteFrameTime(
            "[errors] suspect modules (mods/plugins seen on the error stack): %s",
            summary.c_str()
        );
    }

    // Module column note: attribution is by stack scan, so state it plainly once
    // per session rather than repeating it on every report.
    if (!m_errorNotePrinted)
    {
        WriteFrameTime(
            "[errors] note: 'suspect module' is the first non-game/non-CLEO module on the "
            "stack at the error, or the script active then; it points at the likely source, "
            "not a proven root cause"
        );
        m_errorNotePrinted = true;
    }
}
