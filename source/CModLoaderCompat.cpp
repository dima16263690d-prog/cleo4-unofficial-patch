#include "stdafx.h"
#include "CModLoaderCompat.h"
#include "CModLoaderPaths.h"
#include "CDebugBridge.h"

#include <fstream>

namespace CLEO
{
    using namespace ModLoaderPaths;

    CModLoaderCompat& CModLoaderCompat::Get()
    {
        static CModLoaderCompat instance;
        return instance;
    }

    bool CModLoaderCompat::IsActive()
    {
        // Once detected, latch: the module cannot be unloaded while the game
        // runs, so this avoids a loader-lock lookup on every file opcode.
        if (m_active)
            return true;
        m_active = GetModuleHandleA("modloader.asi") != nullptr ||
                   GetModuleHandleA("modloader.dll") != nullptr;
        return m_active;
    }

    bool CModLoaderCompat::DetectGameRoot(std::string& baseDir)
    {
        // Prefer the directory of the main executable: it is the game root
        // regardless of what CLEO's scripts may have chdir'd into.
        char exePath[MAX_PATH] = {};
        if (GetModuleFileNameA(nullptr, exePath, MAX_PATH) > 0)
        {
            std::string dir = DirName(exePath);
            if (!dir.empty())
            {
                baseDir = dir;
                return true;
            }
        }

        char cwd[MAX_PATH] = {};
        if (GetCurrentDirectoryA(MAX_PATH, cwd) > 0)
        {
            baseDir = cwd;
            return true;
        }
        return false;
    }

    bool CModLoaderCompat::DetectRoot(const std::string& baseDir, std::string& root)
    {
        const char* candidates[] = { "modloader", "ModLoader" };
        for (const char* name : candidates)
        {
            const std::string abs = baseDir + "\\" + name;
            const DWORD attr = GetFileAttributesA(abs.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
            {
                root = name;
                return true;
            }
        }
        return false;
    }

    void CModLoaderCompat::LoadIgnoreRules(const std::string& modloaderDir)
    {
        std::ifstream in(modloaderDir + "\\modloader.ini");
        if (!in.is_open())
            return;

        std::string profile = "default";
        std::string section;
        std::string line;

        // modloader.ini lists [Folder.Config] before the profile sections, so a
        // single forward pass is enough to learn the active profile first.
        while (std::getline(in, line))
        {
            line = Trim(line);
            if (line.empty() || line[0] == ';' || line[0] == '#')
                continue;

            if (line.front() == '[' && line.back() == ']')
            {
                section = Trim(line.substr(1, line.size() - 2));
                continue;
            }

            const size_t eq = line.find('=');
            const std::string value = Trim(eq == std::string::npos ? std::string() : line.substr(eq + 1));
            const std::string normSection = ToLower(section);

            if (normSection == "folder.config" &&
                ToLower(Trim(eq == std::string::npos ? line : line.substr(0, eq))) == "profile")
            {
                profile = ToLower(value);
                continue;
            }

            // Expected section: "Profiles.<profile>.IgnoreMods" / ".IgnoreFiles"
            // / ".Priority".
            if (normSection.rfind("profiles.", 0) != 0)
                continue;

            const size_t lastDot = normSection.find_last_of('.');
            const std::string kind = normSection.substr(lastDot + 1);
            const std::string name = section.substr(9, lastDot - 9); // strip "Profiles."
            if (ToLower(name) != profile)
                continue;

            const std::string key = Normalize(Trim(eq == std::string::npos ? line : line.substr(0, eq)));
            if (key.empty())
                continue;

            if (kind == "ignoremods")
                m_ignoreMods.push_back(key);
            else if (kind == "ignorefiles")
                m_ignoreFiles.push_back(key);
            else if (kind == "priority" && value == "0")
                m_ignoreMods.push_back(key); // priority 0 disables a mod
        }
    }

    bool CModLoaderCompat::IsModIgnored(const std::string& modName) const
    {
        for (const std::string& glob : m_ignoreMods)
            if (MatchWildcard(glob.c_str(), modName.c_str()))
                return true;
        return false;
    }

    bool CModLoaderCompat::IsPathIgnored(const std::string& modPath) const
    {
        for (const std::string& glob : m_ignoreFiles)
            if (MatchWildcard(glob.c_str(), modPath.c_str()))
                return true;
        return false;
    }

    void CModLoaderCompat::WalkMod(const std::string& absDir, const std::string& relDir,
                                   const std::string& modPath)
    {
        WIN32_FIND_DATAA ffd;
        const std::string mask = absDir + "\\*";
        HANDLE h = FindFirstFileA(mask.c_str(), &ffd);
        if (h == INVALID_HANDLE_VALUE)
            return;

        do
        {
            const std::string name = ffd.cFileName;
            if (name == "." || name == "..")
                continue;

            const std::string childRel = relDir + "\\" + name;
            const std::string childMod = modPath.empty() ? Normalize(name) : modPath + "\\" + Normalize(name);

            if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                WalkMod(absDir + "\\" + name, childRel, childMod);
            }
            else if (!IsPathIgnored(childMod))
            {
                Entry e;
                e.relPath = Normalize(childRel);
                e.modPath = childMod;
                e.absPath = absDir + "\\" + name;
                m_entries.push_back(std::move(e));
            }
        } while (FindNextFileA(h, &ffd));

        FindClose(h);
    }

    void CModLoaderCompat::BuildIndex(const std::string& baseDir, const std::string& modloaderDir)
    {
        WIN32_FIND_DATAA ffd;
        const std::string absML = baseDir + "\\" + modloaderDir;
        const std::string mask = absML + "\\*";
        HANDLE h = FindFirstFileA(mask.c_str(), &ffd);
        if (h == INVALID_HANDLE_VALUE)
            return;

        do
        {
            const std::string name = ffd.cFileName;
            if (name == "." || name == ".." || name == ".data" || name == ".profiles")
                continue;
            if (!(ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                continue;

            const std::string modName = Normalize(name);
            if (IsModIgnored(modName))
            {
                TRACE("[modloader] Ignoring mod '%s' (modloader.ini)", name.c_str());
                continue;
            }

            // relDir keeps the modloader folder prefix so that a script writing
            // the full "modloader/<mod>/..." path also resolves.
            WalkMod(absML + "\\" + name, modloaderDir + "\\" + name, "");
        } while (FindNextFileA(h, &ffd));

        FindClose(h);
    }

    void CModLoaderCompat::EnsureIndexed()
    {
        if (m_indexed)
            return;

        // Do not latch while Mod Loader's module is not loaded yet: CLEO.asi may
        // be queried before modloader.asi, and the index must still be built
        // once it appears. The check is a cheap GetModuleHandle.
        if (!IsActive())
            return;

        m_indexed = true;

        if (!DetectGameRoot(m_baseDir))
            return;

        std::string root;
        if (!DetectRoot(m_baseDir, root))
        {
            TRACE("[modloader] Module is loaded but no modloader/ folder was found");
            return;
        }

        m_rootFolder = root;
        LoadIgnoreRules(m_baseDir + "\\" + root);
        BuildIndex(m_baseDir, root);

        for (const Entry& e : m_entries)
        {
            m_byModPath.emplace(e.modPath, &e);
            m_byRelPath.emplace(e.relPath, &e);
        }

        TRACE("[modloader] Indexed %u files under '%s' (%u ignored mods, %u ignored file rules)",
            static_cast<unsigned>(m_entries.size()), root.c_str(),
            static_cast<unsigned>(m_ignoreMods.size()),
            static_cast<unsigned>(m_ignoreFiles.size()));
    }

    bool CModLoaderCompat::ResolveFile(const char* path, std::string& out)
    {
        if (!path || !*path || !IsActive())
            return false;

        EnsureIndexed();
        if (m_entries.empty())
            return false;

        const std::string query = Normalize(path);
        if (query.empty())
            return false;

        // 1) Path relative to a mod root, e.g. "cleo/audio/GANG_CASH.mp3" ->
        //    "modloader/<mod>/cleo/audio/GANG_CASH.mp3". This is what CLEO
        //    scripts expect, since they address files relative to the game root.
        if (auto it = m_byModPath.find(query); it != m_byModPath.end())
        {
            out = it->second->absPath;
            return true;
        }

        // 2) Path already including the "modloader/<mod>/" prefix.
        if (auto it = m_byRelPath.find(query); it != m_byRelPath.end())
        {
            out = it->second->absPath;
            return true;
        }

        return false;
    }

    bool CModLoaderCompat::ResolveDirectory(const char* path, std::string& out)
    {
        if (!path || !*path || !IsActive())
            return false;

        EnsureIndexed();
        if (m_entries.empty())
            return false;

        const std::string dir = Normalize(path);
        if (dir.empty())
            return false;

        for (const Entry& e : m_entries)
        {
            if (DirName(e.modPath) == dir)
            {
                out = m_baseDir + "\\" + DirName(e.relPath);
                return true;
            }
        }
        return false;
    }

    void CModLoaderCompat::MatchFiles(const char* mask, std::vector<std::string>& out)
    {
        if (!mask || !*mask || !IsActive())
            return;

        EnsureIndexed();
        if (m_entries.empty())
            return;

        const std::string nmask = Normalize(mask);
        const std::string patFile = FileName(nmask);
        const std::string patDir = DirName(nmask);

        // Accept both "cleo/*.cs" (game/mod-relative) and
        // "modloader/Mod/cleo/*.cs" (full path).
        std::string patDirInRoot = patDir;
        const std::string rootNorm = Normalize(m_rootFolder);
        if (patDirInRoot.rfind(rootNorm + "\\", 0) == 0)
            patDirInRoot = patDirInRoot.substr(rootNorm.size() + 1);

        for (const Entry& e : m_entries)
        {
            const bool dirMatches =
                patDir == DirName(e.relPath) ||        // full game-relative dir
                patDirInRoot == DirName(e.modPath) ||  // mod-relative dir
                patDir == DirName(e.modPath);          // bare dir, no mod prefix
            if (!dirMatches)
                continue;

            if (MatchWildcard(patFile.c_str(), FileName(e.relPath).c_str()))
                out.push_back(e.absPath);
        }
    }
}
