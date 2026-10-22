#include "RenderFix.h"
#include "RenderFix.Patch.h"

#include <map>

RenderFix* RenderFix::s_instance = nullptr;

// ---------------------------------------------------------------------------
// RenderFix — opt-in frame-pacing / renderer tweaks for GTA SA 1.0 US.
//
// Every address lives in RenderFix.Patch.h (verified for GTA SA 1.0 US only),
// so the whole plugin is a no-op on any other build (ApplyFixes checks the game
// version first). Sources for the addresses and byte values:
//   - SilentPatch (CookiePLMonster, MIT)  — no-frame-delay
//   - SA-MixSets (Junior_Djjr, MIT)       — FPS limiter byte, streaming budget
//   - FramerateVigilante (Junior_Djjr)    — 1.0 US limiter call sites
// ---------------------------------------------------------------------------

using namespace RenderFixPlan;

RenderFix::RenderFix()
{
    if (s_instance != nullptr)
        return;

    if (CLEO_GetVersion() < CLEO_VERSION)
        return;

    s_instance = this;

    LoadConfig();

    if (!m_enabled)
        return;

    m_supportedVersion = CLEO_GetGameVersion() == kGtaSa10Us;

    if (m_logEnabled)
    {
        m_log.open(LogPath(), std::ios::out | std::ios::app);
        WriteLog(
            "[renderfix] build_id=%s version=%s supported=%d",
            kRenderFixBuildId,
            CLEO_GetGameVersion() == kGtaSa10Us ? "1.0 US" : "other",
            m_supportedVersion ? 1 : 0
        );
    }

    if (!m_supportedVersion)
    {
        WriteLog(
            "[renderfix] game version is not 1.0 US; no tweaks applied "
            "(all addresses are 1.0 US specific)"
        );
        return;
    }

    // The streaming budget must be requested before SCM init, so it is handed
    // to the core now. Everything else is applied at game begin.
    ApplyStreamingMemory();

    CLEO_RegisterCallback(
        CLEO_CB_GAME_BEGIN,
        reinterpret_cast<uintptr_t>(&RenderFix::OnGameBegin)
    );
    CLEO_RegisterCallback(
        CLEO_CB_GAME_PROCESS_BEFORE,
        reinterpret_cast<uintptr_t>(&RenderFix::OnGameProcessBefore)
    );

    WriteLog("[renderfix] configuration=%s", ConfigPath().c_str());
}

RenderFix::~RenderFix()
{
    if (s_instance == this)
        s_instance = nullptr;
}

void RenderFix::OnGameBegin()
{
    if (s_instance != nullptr)
        s_instance->ApplyFixes();
}

void RenderFix::OnGameProcessBefore()
{
    if (s_instance != nullptr && s_instance->m_pendingReapply)
        s_instance->ReapplyFrameLimit();
}

void RenderFix::ApplyFixes()
{
    if (!m_supportedVersion)
        return;

    WriteLog(
        "[renderfix] applying frame_limit=%d remove_frame_delay=%d",
        m_frameLimit,
        m_removeFrameDelay ? 1 : 0
    );

    ApplyFrameLimit();
    ApplyFrameDelayRemoval();

    // The engine re-initialises the limiter shortly after the scripts start, so
    // the value written at game begin can be overwritten. Re-apply it once on
    // the first processed frame (this is what SA-MixSets and FramerateVigilante
    // do as well).
    m_pendingReapply = m_frameLimit > 0;

    WriteLog("[renderfix] done");
}

void RenderFix::ReapplyFrameLimit()
{
    m_pendingReapply = false;

    const uint8_t value = static_cast<uint8_t>(m_frameLimit);
    if (PatchBytes(kFrameLimitByte, &value, sizeof(value)))
        WriteLog("[renderfix] frame limit re-applied on first frame: %u FPS", m_frameLimit);
    else
        WriteLog("[renderfix] frame limit re-apply failed");
}

void RenderFix::ApplyFrameLimit()
{
    if (m_frameLimit <= 0)
        return;

    // The limiter byte is unsigned; values above 255 cannot be represented.
    if (!IsValidFrameLimit(m_frameLimit))
    {
        WriteLog(
            "[renderfix] FrameLimit=%d is out of range (%d..%d); ignored",
            m_frameLimit,
            kFrameLimitMin,
            kFrameLimitMax
        );
        return;
    }

    const uint8_t previous = ReadByte(kFrameLimitByte);
    const uint8_t value = static_cast<uint8_t>(m_frameLimit);

    if (!PatchBytes(kFrameLimitByte, &value, sizeof(value)))
    {
        WriteLog("[renderfix] frame limit: write to 0x%08X failed", kFrameLimitByte);
        return;
    }

    WriteLog(
        "[renderfix] frame limit %u -> %u FPS (0x%08X)",
        static_cast<unsigned>(previous),
        static_cast<unsigned>(value),
        kFrameLimitByte
    );
}

void RenderFix::ApplyFrameDelayRemoval()
{
    if (!m_removeFrameDelay)
        return;

    const bool ok =
        PatchBytes(kFrameDelayPatch1, kFrameDelayJump, sizeof(kFrameDelayJump)) &&
        PatchBytes(kFrameDelayPatch2, &kFrameDelayArg, sizeof(kFrameDelayArg)) &&
        NopBytes(kFrameDelayNop, 1);

    if (ok)
        WriteLog("[renderfix] removed the ~14 ms per-frame delay");
    else
        WriteLog("[renderfix] frame-delay patch failed (0x%08X)", kFrameDelayPatch1);
}

void RenderFix::ApplyStreamingMemory()
{
    if (m_streamingMemoryMb == 0)
        return;

    // The core owns CStreaming::ms_memoryAvailable and applies the request at
    // SCM init, so the engine's own streaming initialisation cannot clobber it.
    const int previous = CLEO_SetStreamingMemoryLimit(m_streamingMemoryMb);

    WriteLog(
        "[renderfix] streaming budget requested=%d MB previous=%d MB "
        "(applied by the core at SCM init)",
        m_streamingMemoryMb,
        previous
    );
}

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

namespace
{
    std::string TrimAscii(const std::string& s)
    {
        size_t a = 0;
        size_t b = s.size();
        while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n'))
            ++a;
        while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n'))
            --b;
        return s.substr(a, b - a);
    }

    // Reads every key=value from the ini so a rewrite keeps the values the user
    // set. Empty when the file is absent.
    std::map<std::string, std::string> ReadRawConfig(const std::string& path)
    {
        std::map<std::string, std::string> out;

        std::ifstream file(path);
        if (!file.is_open())
            return out;

        std::string line;
        std::string section;
        while (std::getline(file, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            const std::string trimmed = TrimAscii(line);
            if (trimmed.empty() || trimmed[0] == ';' || trimmed[0] == '#')
                continue;

            if (trimmed.front() == '[' && trimmed.back() == ']')
            {
                section = trimmed.substr(1, trimmed.size() - 2);
                continue;
            }

            const size_t eq = trimmed.find('=');
            if (eq == std::string::npos)
                continue;

            out[section + "." + TrimAscii(trimmed.substr(0, eq))] =
                TrimAscii(trimmed.substr(eq + 1));
        }

        return out;
    }

    std::string ValueOr(const std::map<std::string, std::string>& raw,
        const char* section, const char* key, const char* fallback)
    {
        const auto it = raw.find(std::string(section) + "." + key);
        if (it != raw.end() && !it->second.empty())
            return it->second;
        return fallback;
    }
}

void RenderFix::LoadConfig()
{
    const std::string path = ConfigPath();

    const std::map<std::string, std::string> raw = ReadRawConfig(path);

    m_enabled =
        GetPrivateProfileIntA("RenderFix.General", "Enabled", 0, path.c_str()) != 0;
    m_logEnabled =
        GetPrivateProfileIntA("RenderFix.General", "Log", 1, path.c_str()) != 0;

    m_frameLimit =
        GetPrivateProfileIntA("RenderFix.FPS", "FrameLimit", 0, path.c_str());
    m_removeFrameDelay =
        GetPrivateProfileIntA("RenderFix.FPS", "RemoveFrameDelay", 0, path.c_str()) != 0;

    m_streamingMemoryMb =
        GetPrivateProfileIntA("RenderFix.Streaming", "MemoryMB", 0, path.c_str());

    // The ini is shipped next to the plugin, but it can be missing (the DLL was
    // copied alone) or stale (an older build's keys). Regenerate it with the full
    // descriptions in either case, keeping the values the user already set, so the
    // keys are always discoverable without reading the source.
    const bool fileMissing =
        GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES;
    const bool missingNewKeys =
        !fileMissing && raw.find("RenderFix.Streaming.MemoryMB") == raw.end();

    if (fileMissing || missingNewKeys)
    {
        if (!fileMissing &&
            GetFileAttributesA((path + ".bak").c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            CopyFileA(path.c_str(), (path + ".bak").c_str(), TRUE);
        }

        WriteConfigFile(path, raw);
    }
}

// Builds the ini text with the descriptions. Values already present in `raw` are
// kept, everything else falls back to the default. Descriptions are ';' comments,
// otherwise the engine would read them as keys.
std::string RenderFix::BuildConfigText(const std::map<std::string, std::string>& raw) const
{
    std::string out;
    auto line = [&out](const std::string& text) { out += text; out += '\n'; };
    auto desc = [&line](const std::string& text) { line("; " + text); };
    auto kv = [&](const char* section, const char* key, const char* fallback)
    {
        line(std::string(key) + "=" + ValueOr(raw, section, key, fallback));
    };

    desc("RenderFix configuration. Values are read once, when GTA starts.");
    desc("Everything here is OPT-IN: at the defaults RenderFix changes nothing.");
    desc("1 = enabled, 0 = disabled (except where a number is expected).");
    desc("See cleo\\debug\\cleo_renderfix.log for what was actually applied.");
    line("");

    line("[RenderFix.General]");
    desc("Master switch. When 0, the plugin does nothing at all.");
    kv("RenderFix.General", "Enabled", "0");
    desc("Write cleo\\debug\\cleo_renderfix.log.");
    kv("RenderFix.General", "Log", "1");
    line("");

    line("[RenderFix.FPS]");
    desc("Frame cap written to the engine limiter byte (0xC1704C). 1..255.");
    desc("0 = leave the game's own limiter untouched. 60 is a common choice.");
    kv("RenderFix.FPS", "FrameLimit", "0");
    desc("Remove the engine's ~14 ms per-frame sleep (SilentPatch 'No framedelay').");
    desc("This is what usually unlocks a smooth high FPS. 1 = on, 0 = off.");
    kv("RenderFix.FPS", "RemoveFrameDelay", "0");
    line("");

    line("[RenderFix.Streaming]");
    desc("Streaming memory budget in MB. Raised only, never lowered.");
    desc("0 = untouched, -1 = auto (1024 MB with a 4GB/LAA patch, else 512 MB),");
    desc("> 0 = that many MB. Applied by the core at SCM init.");
    kv("RenderFix.Streaming", "MemoryMB", "0");
    line("");

    return out;
}

void RenderFix::WriteConfigFile(
    const std::string& path,
    const std::map<std::string, std::string>& raw) const
{
    std::string text = BuildConfigText(raw);

    // The ini format expects CRLF; the builder uses LF for readability.
    std::string crlf;
    crlf.reserve(text.size() + text.size() / 16);
    for (char c : text)
    {
        if (c == '\n')
            crlf += "\r\n";
        else
            crlf += c;
    }

    std::ofstream config(path, std::ios::out | std::ios::trunc | std::ios::binary);
    if (config.is_open())
        config.write(crlf.data(), static_cast<std::streamsize>(crlf.size()));
}

std::string RenderFix::ConfigPath() const
{
    const std::string standardPath = "cleo\\cleo_plugins\\RenderFix.ini";

    CreateDirectoryA("cleo", nullptr);
    CreateDirectoryA("cleo\\cleo_plugins", nullptr);

    HMODULE module = nullptr;
    if (GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&RenderFix::s_instance),
            &module))
    {
        char modulePath[MAX_PATH] = {};
        const DWORD length =
            GetModuleFileNameA(module, modulePath, sizeof(modulePath));

        if (length != 0 && length < sizeof(modulePath))
        {
            std::string pluginPath(modulePath, length);
            const size_t slash = pluginPath.find_last_of("\\/");
            if (slash != std::string::npos)
            {
                const std::string pluginIni =
                    pluginPath.substr(0, slash + 1) + "RenderFix.ini";

                if (GetFileAttributesA(pluginIni.c_str()) != INVALID_FILE_ATTRIBUTES)
                    return pluginIni;
            }
        }
    }

    return standardPath;
}

std::string RenderFix::LogPath() const
{
    CreateDirectoryA("cleo", nullptr);
    CreateDirectoryA("cleo\\debug", nullptr);
    return "cleo\\debug\\cleo_renderfix.log";
}

void RenderFix::WriteLog(const char* format, ...)
{
    if (!m_logEnabled || !m_log.is_open())
        return;

    char buffer[1024] = {};
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    std::lock_guard<std::mutex> lock(m_logMutex);
    m_log << buffer << '\n';
    m_log.flush();
}

// ---------------------------------------------------------------------------
// Memory helpers
// ---------------------------------------------------------------------------

bool RenderFix::PatchBytes(uintptr_t address, const void* data, size_t size)
{
    DWORD oldProtect = 0;
    if (!VirtualProtect(
            reinterpret_cast<void*>(address),
            size,
            PAGE_EXECUTE_READWRITE,
            &oldProtect))
    {
        return false;
    }

    memcpy(reinterpret_cast<void*>(address), data, size);

    DWORD ignored = 0;
    VirtualProtect(
        reinterpret_cast<void*>(address),
        size,
        oldProtect,
        &ignored
    );

    return true;
}

bool RenderFix::NopBytes(uintptr_t address, size_t size)
{
    DWORD oldProtect = 0;
    if (!VirtualProtect(
            reinterpret_cast<void*>(address),
            size,
            PAGE_EXECUTE_READWRITE,
            &oldProtect))
    {
        return false;
    }

    memset(reinterpret_cast<void*>(address), 0x90, size);

    DWORD ignored = 0;
    VirtualProtect(
        reinterpret_cast<void*>(address),
        size,
        oldProtect,
        &ignored
    );

    return true;
}

uint8_t RenderFix::ReadByte(uintptr_t address)
{
    return *reinterpret_cast<const uint8_t*>(address);
}

static RenderFix g_renderFix;
