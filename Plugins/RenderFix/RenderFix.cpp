#include "RenderFix.h"
#include "RenderFix.Patch.h"
#include "RenderFix.CLEO.h"

#include <map>

RenderFix* RenderFix::s_instance = nullptr;

// ---------------------------------------------------------------------------
// RenderFix — opt-in frame-pacing / renderer tweaks for GTA SA 1.0 US.
//
// Every address lives in RenderFix.Patch.h (verified for GTA SA 1.0 US only),
// so the whole plugin is a no-op on any other build (ApplyFixes checks the game
// version first). Sources for the addresses and byte values:
//   - SilentPatch (CookiePLMonster, MIT)  — no-frame-delay, Core Fixes
//   - SA-MixSets (Junior_Djjr, MIT)       — FPS limiter byte, streaming budget
//   - FramerateVigilante (Junior_Djjr)    — 1.0 US limiter call sites
// The FPS overlay draws with the game's own CFont through raw 1.0 US pointers.
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
    RenderFixCleo::Configure(
        m_supportedVersion && m_cleoEnabled,
        m_cleoRenderStateGuard
    );

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

    // RenderFix owns its GTA streaming budget and applies it after the game's
    // initialization, then retries once on the first processed frame.

    CLEO_RegisterCallback(
        CLEO_CB_GAME_BEGIN,
        reinterpret_cast<uintptr_t>(&RenderFix::OnGameBegin)
    );
    CLEO_RegisterCallback(
        CLEO_CB_GAME_PROCESS_BEFORE,
        reinterpret_cast<uintptr_t>(&RenderFix::OnGameProcessBefore)
    );
    CLEO_RegisterCallback(
        CLEO_CB_GAME_PROCESS_AFTER,
        reinterpret_cast<uintptr_t>(&RenderFix::OnGameProcessAfter)
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
    RenderFix* self = s_instance;
    if (self == nullptr || !self->m_supportedVersion)
        return;

    RenderFixCleo::OnFrameBegin();

    if (self->m_gtaEnabled)
        self->UpdateAutoFpsLimit();

    if (self->m_gtaEnabled && self->m_pendingReapply)
        self->ReapplyFrameLimit();

    if (self->m_gtaEnabled && self->m_pendingStreamingReapply)
    {
        self->m_pendingStreamingReapply = false;
        self->ApplyStreamingMemory();
    }
}

void RenderFix::OnGameProcessAfter()
{
    RenderFix* self = s_instance;
    if (self == nullptr || !self->m_supportedVersion)
        return;

    RenderFixCleo::OnFrameEnd();
}


// Repoints the engine's CHud::Draw call at our wrapper, which draws the HUD and
// then the FPS counter. The original entry is preserved, so removing the hook
// (or the whole plugin) leaves the game exactly as it was.


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

    // The comment language is recorded by a marker line written at the top of the
    // ini, so a rewrite in the other language is detectable without parsing the
    // (localized) descriptions.
    bool IniUsesRussian(const std::string& path)
    {
        std::ifstream file(path);
        if (!file.is_open())
            return false;

        std::string line;
        while (std::getline(file, line))
        {
            if (line.find("; language=ru") != std::string::npos)
                return true;
            if (line.find("; language=en") != std::string::npos)
                return false;
        }
        return false;
    }
}

void RenderFix::LoadConfig()
{
    const std::string path = ConfigPath();

    // The comment language is stored as a marker line at the top of the ini; the
    // user-facing key is RenderFix.General.Language (en|ru).
    char language[16] = {};
    GetPrivateProfileStringA(
        "RenderFix.General", "Language", "en",
        language, sizeof(language), path.c_str());
    for (char* p = language; *p != '\0'; ++p)
        *p = static_cast<char>(tolower(static_cast<unsigned char>(*p)));
    m_languageRussian = _stricmp(language, "ru") == 0;

    const std::map<std::string, std::string> raw = ReadRawConfig(path);

    m_enabled =
        GetPrivateProfileIntA("RenderFix.General", "Enabled", 0, path.c_str()) != 0;
    m_logEnabled =
        GetPrivateProfileIntA("RenderFix.General", "Log", 1, path.c_str()) != 0;

    m_gtaEnabled =
        GetPrivateProfileIntA("RenderFix.GTA", "Enabled", 1, path.c_str()) != 0;
    m_cleoEnabled =
        GetPrivateProfileIntA("RenderFix.CLEO", "Enabled", 1, path.c_str()) != 0;
    m_cleoRenderStateGuard =
        GetPrivateProfileIntA("RenderFix.CLEO", "RenderStateGuard", 1, path.c_str()) != 0;

    m_frameLimit =
        GetPrivateProfileIntA("RenderFix.FPS", "FrameLimit", 0, path.c_str());
    m_removeFrameDelay =
        GetPrivateProfileIntA("RenderFix.FPS", "RemoveFrameDelay", 0, path.c_str()) != 0;
    m_minRefreshRate =
        GetPrivateProfileIntA("RenderFix.FPS", "MinRefreshRate", 0, path.c_str());

    m_autoFpsEnabled =
        GetPrivateProfileIntA("RenderFix.AutoLimitFPS", "Enabled", 0, path.c_str()) != 0;
    m_autoFpsOptions.forCutscenes =
        GetPrivateProfileIntA("RenderFix.AutoLimitFPS", "ForCutscenes", 1, path.c_str()) != 0;
    m_autoFpsOptions.forScriptedScenes =
        GetPrivateProfileIntA("RenderFix.AutoLimitFPS", "ForScriptedScenes", 1, path.c_str()) != 0;
    m_autoFpsOptions.forPauseMenu =
        GetPrivateProfileIntA("RenderFix.AutoLimitFPS", "ForPauseMenu", 1, path.c_str()) != 0;
    m_autoFpsOptions.forMinigames =
        GetPrivateProfileIntA("RenderFix.AutoLimitFPS", "ForMinigames", 1, path.c_str()) != 0;
    m_autoFpsOptions.forMissions =
        GetPrivateProfileIntA("RenderFix.AutoLimitFPS", "ForMissions", 1, path.c_str()) != 0;
    m_autoFpsOptions.forSchools =
        GetPrivateProfileIntA("RenderFix.AutoLimitFPS", "ForSchools", 1, path.c_str()) != 0;

    m_autoFpsOptions.cutsceneFps =
        GetPrivateProfileIntA("RenderFix.AutoLimitFPS", "CutsceneFPS", kAutoFpsCutscene, path.c_str());
    m_autoFpsOptions.scriptedSceneFps =
        GetPrivateProfileIntA("RenderFix.AutoLimitFPS", "ScriptedSceneFPS", kAutoFpsScriptedScene, path.c_str());
    m_autoFpsOptions.pauseMenuFps =
        GetPrivateProfileIntA("RenderFix.AutoLimitFPS", "PauseMenuFPS", kAutoFpsPauseMenu, path.c_str());
    m_autoFpsOptions.minigameFps =
        GetPrivateProfileIntA("RenderFix.AutoLimitFPS", "MinigameFPS", kAutoFpsMinigame, path.c_str());
    m_autoFpsOptions.missionInteriorFps =
        GetPrivateProfileIntA("RenderFix.AutoLimitFPS", "MissionInteriorFPS", kAutoFpsMissionInterior, path.c_str());
    m_autoFpsOptions.schoolFps =
        GetPrivateProfileIntA("RenderFix.AutoLimitFPS", "SchoolFPS", kAutoFpsSchool, path.c_str());

    m_mouseReinitFix =
        GetPrivateProfileIntA("RenderFix.CoreFixes", "MouseReinitFix", 0, path.c_str()) != 0;
    m_refFix =
        GetPrivateProfileIntA("RenderFix.CoreFixes", "RefFix", 0, path.c_str()) != 0;
    m_doubleRwheelsFix =
        GetPrivateProfileIntA("RenderFix.CoreFixes", "DoubleRwheelsFix", 0, path.c_str()) != 0;

    m_showFps =
        GetPrivateProfileIntA("RenderFix.Overlay", "ShowFPS", 0, path.c_str()) != 0;

    m_streamingMemoryMb =
        GetPrivateProfileIntA("RenderFix.Streaming", "MemoryMB", 0, path.c_str());

    // The ini is shipped next to the plugin, but it can be missing (the DLL was
    // copied alone) or stale (an older build's keys). Regenerate it with the full
    // descriptions in either case, keeping the values the user already set, so the
    // keys are always discoverable without reading the source.
    const bool fileMissing =
        GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES;
    const bool languageMismatch =
        !fileMissing && (IniUsesRussian(path) != m_languageRussian);
    const bool missingNewKeys =
        !fileMissing &&
        (raw.find("RenderFix.Streaming.MemoryMB") == raw.end() ||
         raw.find("RenderFix.AutoLimitFPS.Enabled") == raw.end() ||
         raw.find("RenderFix.CoreFixes.MouseReinitFix") == raw.end() ||
         raw.find("RenderFix.Overlay.ShowFPS") == raw.end() ||
         raw.find("RenderFix.General.Language") == raw.end() ||
         raw.find("RenderFix.GTA.Enabled") == raw.end() ||
         raw.find("RenderFix.CLEO.Enabled") == raw.end() ||
         raw.find("RenderFix.CLEO.RenderStateGuard") == raw.end());

    if (fileMissing || languageMismatch || missingNewKeys)
    {
        if (!fileMissing &&
            GetFileAttributesA((path + ".bak").c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            CopyFileA(path.c_str(), (path + ".bak").c_str(), TRUE);
        }

        WriteConfigFile(path, m_languageRussian, raw);
    }
}

// Builds the ini text with the descriptions in the chosen language. Only the
// comments are localized; keys and values never change. Values already present in
// `raw` are kept, everything else falls back to the defaults. Descriptions are
// ';' comments, otherwise the engine would read them as keys.
std::string RenderFix::BuildConfigText(
    bool ru, const std::map<std::string, std::string>& raw) const
{
    std::string out;
    auto line = [&out](const std::string& text) { out += text; out += '\n'; };
    auto desc = [&line](const std::string& text) { line("; " + text); };
    auto kv = [&](const char* section, const char* key, const char* fallback)
    {
        line(std::string(key) + "=" + ValueOr(raw, section, key, fallback));
    };

    line(ru ? "; language=ru" : "; language=en");
    desc(ru ? "Конфигурация RenderFix. Значения читаются при запуске GTA."
            : "RenderFix configuration. Values are read when GTA starts.");
    desc(ru ? "Измените Language и перезапустите игру — файл перепишется с описанием"
            : "Change Language and restart the game - the file is rewritten with the");
    desc(ru ? "на выбранном языке, а ваши значения сохранятся."
            : "descriptions in the chosen language, keeping your values.");
    desc(ru ? "Всё здесь ВЫКЛЮЧЕНО по умолчанию: при стандартных значениях RenderFix"
            : "Everything here is OPT-IN: at the defaults RenderFix changes nothing.");
    desc(ru ? "ничего не меняет. 1 = включено, 0 = выключено (кроме чисел)."
            : "1 = enabled, 0 = disabled (except where a number is expected).");
    desc(ru ? "Что было применено — в cleo\\debug\\cleo_renderfix.log."
            : "See cleo\\debug\\cleo_renderfix.log for what was actually applied.");
    line("");

    line("[RenderFix.General]");
    desc(ru ? "Язык описаний в этом файле: en или ru."
            : "Comment language in this file: en or ru.");
    kv("RenderFix.General", "Language", "en");
    desc(ru ? "Главный выключатель. 0 = плагин не делает ничего."
            : "Master switch. When 0, the plugin does nothing at all.");
    kv("RenderFix.General", "Enabled", "0");
    desc(ru ? "Писать cleo\\debug\\cleo_renderfix.log."
            : "Write cleo\\debug\\cleo_renderfix.log.");
    kv("RenderFix.General", "Log", "1");
    line("");

    line("[RenderFix.GTA]");
    desc(ru ? "Исправления самой GTA: frame pacing, рендерер, таймер и стриминг."
            : "GTA-side fixes: frame pacing, renderer, timer and streaming.");
    desc(ru ? "1 = включено, 0 = выключено независимо от защитного слоя CLEO."
            : "1 = enabled, 0 = disabled independently from the CLEO-side guard.");
    kv("RenderFix.GTA", "Enabled", "1");
    line("");

    line("[RenderFix.CLEO]");
    desc(ru ? "Защитный слой для графических операций плагинов и скриптов CLEO."
            : "Guard layer for drawing performed by CLEO plugins and scripts.");
    kv("RenderFix.CLEO", "Enabled", "1");
    desc(ru ? "Сохранять и восстанавливать выбранные состояния RenderWare вокруг игрового цикла."
            : "Save and restore selected RenderWare states around the game-process callback.");
    desc(ru ? "Отключите при несовместимости с графическими модами, зависящими от состояния между кадрами."
            : "Disable if another graphics mod relies on changed RenderWare states across frames.");
    kv("RenderFix.CLEO", "RenderStateGuard", "1");
    line("");

    line("[RenderFix.FPS]");
    desc(ru ? "Ограничение частоты кадров (записывается в байт лимитера 0xC1704C)."
            : "Frame cap written to the engine limiter byte (0xC1704C). 1..255.");
    desc(ru ? "1..255. 0 = не трогать лимитер игры. Обычно ставят 60."
            : "0 = leave the game's own limiter untouched. 60 is a common choice.");
    kv("RenderFix.FPS", "FrameLimit", "0");
    desc(ru ? "Убрать встроенную задержку ~14 мс на кадр (SilentPatch 'No framedelay')."
            : "Remove the engine's ~14 ms per-frame sleep (SilentPatch 'No framedelay').");
    desc(ru ? "Именно это обычно даёт плавный высокий FPS. 1 = вкл, 0 = выкл."
            : "This is what usually unlocks a smooth high FPS. 1 = on, 0 = off.");
    kv("RenderFix.FPS", "RemoveFrameDelay", "0");
    desc(ru ? "Минимальная частота обновления монитора, Гц (FramerateVigilante 'RefreshRate')."
            : "Minimum monitor refresh rate in Hz (FramerateVigilante 'RefreshRate').");
    desc(ru ? "0 = не трогать. На мониторе 144 Гц попробуйте половину (72)."
            : "0 = untouched. On a 144 Hz monitor try half (72) for steadier pacing.");
    kv("RenderFix.FPS", "MinRefreshRate", "0");
    line("");

    line("[RenderFix.AutoLimitFPS]");
    desc(ru ? "Временно СНИЖАТЬ лимит кадров, пока идёт активность, которая ломается"
            : "Temporarily lower the frame cap while an activity that breaks at high");
    desc(ru ? "на высоком FPS, затем возвращать обратно. Порт из FramerateVigilante."
            : "FPS runs (ported from FramerateVigilante). The cap is restored after.");
    desc(ru ? "Требует FrameLimit > 0 (понижение работает только от базового лимита)."
            : "Requires FrameLimit > 0 to have a base cap to lower.");
    kv("RenderFix.AutoLimitFPS", "Enabled", "0");
    desc(ru ? "Кат-сцены. 1 = вкл, 0 = выкл."
            : "Cutscenes. 1 = on, 0 = off.");
    kv("RenderFix.AutoLimitFPS", "ForCutscenes", "1");
    desc(ru ? "Сценарные сцены (широкоэкранные полосы)."
            : "Scripted scenes (widescreen borders).");
    kv("RenderFix.AutoLimitFPS", "ForScriptedScenes", "1");
    desc(ru ? "Меню паузы / настроек."
            : "Pause / options menu.");
    kv("RenderFix.AutoLimitFPS", "ForPauseMenu", "1");
    desc(ru ? "Мини-игры (бассейн 'POOL2', hot coffee 'GFSEX')."
            : "Minigames (pool 'POOL2', hot coffee 'GFSEX').");
    kv("RenderFix.AutoLimitFPS", "ForMinigames", "1");
    desc(ru ? "Интерьеры миссий (Big Smoke 'DRUGS1' внутри здания)."
            : "Mission interiors (Big Smoke 'DRUGS1' inside).");
    kv("RenderFix.AutoLimitFPS", "ForMissions", "1");
    desc(ru ? "Авто/лодка/мотошколы ('DSKOOL','BOAT','BSKOOL')."
            : "Driving/boat/bike schools ('DSKOOL','BOAT','BSKOOL').");
    kv("RenderFix.AutoLimitFPS", "ForSchools", "1");
    desc(ru ? "Лимит кадров для каждой активности (1..255). Значения по умолчанию —"
            : "Frame cap for each activity (1..255). Defaults are FramerateVigilante's.");
    desc(ru ? "из FramerateVigilante: 60/80/60/30/50/80."
            : "CutsceneFPS=60, ScriptedSceneFPS=80, PauseMenuFPS=60.");
    kv("RenderFix.AutoLimitFPS", "CutsceneFPS", "60");
    kv("RenderFix.AutoLimitFPS", "ScriptedSceneFPS", "80");
    kv("RenderFix.AutoLimitFPS", "PauseMenuFPS", "60");
    kv("RenderFix.AutoLimitFPS", "MinigameFPS", "30");
    kv("RenderFix.AutoLimitFPS", "MissionInteriorFPS", "50");
    kv("RenderFix.AutoLimitFPS", "SchoolFPS", "80");
    line("");

    line("[RenderFix.Overlay]");
    desc(ru ? "Показывать счётчик FPS в правом нижнем углу экрана."
            : "Show the FPS counter in the bottom-right corner of the screen.");
    desc(ru ? "1 = показывать, 0 = не показывать (по умолчанию)."
            : "1 = show, 0 = hide (default).");
    kv("RenderFix.Overlay", "ShowFPS", "0");
    line("");

    line("[RenderFix.CoreFixes]");
    desc(ru ? "Отдельные исправления ошибок (порт из SilentPatch SA, MIT)."
            : "Standalone bug fixes (ported from SilentPatch SA, MIT).");
    desc(ru ? "Каждое можно включить независимо. По умолчанию всё выключено."
            : "Each can be enabled on its own. All off by default.");
    desc(ru ? "Не даёт игре переинициализировать мышь DirectInput каждый кадр"
            : "Stop the game re-initialising the DirectInput mouse device every frame");
    desc(ru ? "(лечит дёрганье/залипание мыши). 1 = вкл, 0 = выкл."
            : "(fixes mouse jitter / stuck cursor). 1 = on, 0 = off.");
    kv("RenderFix.CoreFixes", "MouseReinitFix", "0");
    desc(ru ? "RefFix: исправляет деление на ноль в отражениях (Reflection fix)."
            : "RefFix: fixes a divide-by-zero in the reflection renderer.");
    kv("RenderFix.CoreFixes", "RefFix", "0");
    desc(ru ? "DOUBLE_RWHEELS: правильные флаги матрицы для сдвоённых задних колёс."
            : "DOUBLE_RWHEELS: correct RW matrix flags for double rear wheels.");
    kv("RenderFix.CoreFixes", "DoubleRwheelsFix", "0");
    line("");

    line("[RenderFix.Streaming]");
    desc(ru ? "Бюджет памяти стриминга (CStreaming::ms_memoryAvailable), МБ."
            : "Streaming memory budget (CStreaming::ms_memoryAvailable), in MB.");
    desc(ru ? "0 = не трогать; -1 = авто (1024 МБ с LAA, иначе 512 МБ);"
            : "0 = untouched; -1 = auto (1024 MB with a 4GB/LAA patch, else 512 MB);");
    desc(ru ? "> 0 = ровно столько МБ. Применяется ядром при инициализации SCM."
            : "> 0 = that many MB. Applied by the core at SCM init.");
    kv("RenderFix.Streaming", "MemoryMB", "0");
    line("");

    return out;
}

void RenderFix::WriteConfigFile(const std::string& path, bool ru,
    const std::map<std::string, std::string>& raw) const
{
    std::string text = BuildConfigText(ru, raw);

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

// Repoints the 5-byte `call rel32` at `callSite` to `newTarget`, but only when
// it currently targets `expectedTarget` (and starts with the call opcode). The
// guard means we never clobber a hook another plugin (e.g. plugin-sdk's own
// drawHudEvent) already placed there. Returns false and changes nothing on any
// mismatch.
bool RenderFix::PatchCall(uintptr_t callSite, uintptr_t expectedTarget,
                          uintptr_t newTarget)
{
    if (*reinterpret_cast<const uint8_t*>(callSite) != kCallOpcode)
        return false;

    const int32_t existing = *reinterpret_cast<const int32_t*>(callSite + 1);
    if (callSite + 5 + existing != expectedTarget)
        return false;

    const int32_t rel = static_cast<int32_t>(newTarget - (callSite + 5));
    return PatchBytes(callSite + 1, &rel, sizeof(rel));
}

static RenderFix g_renderFix;
