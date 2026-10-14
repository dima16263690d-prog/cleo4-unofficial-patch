#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <unordered_map>

namespace CLEO
{
    // Mod Loader interop (LINK/2012 Mod Loader).
    //
    // Mod Loader loads ASI/CLEO modules and *patches their import tables* so
    // that kernel32 / CRT path calls (GetFileAttributesA, CreateFileA, fopen,
    // FindFirstFileA, ...) are translated into the modloader/ tree. Modules it
    // does not own - most importantly CLEO.asi itself - keep talking to the
    // real filesystem through their own static imports and therefore never see
    // files placed under modloader/<mod>/...
    //
    // That is why, even with Mod Loader installed, CLEO opcodes such as
    // `0AAB file_exists "cleo/audio/GANG_CASH.mp3"` and the audio opcodes only
    // found files next to the game root. This manager restores parity for
    // CLEO's own file and audio entry points: it indexes the files under
    // modloader/ and resolves a game-relative path to the real file inside a
    // mod folder, honoring modloader.ini priorities/ignores and the Mod Loader
    // wildcard rules.
    class CModLoaderCompat
    {
    public:
        static CModLoaderCompat& Get();

        // True when a loaded Mod Loader module is present. Cheap; does not
        // touch the filesystem.
        bool IsActive();

        // Resolves @path (relative to the game root, e.g.
        // "cleo/audio/x.mp3") to an absolute path of the real file inside
        // modloader/. Returns false and leaves @out untouched when no such
        // file exists under modloader/.
        bool ResolveFile(const char* path, std::string& out);

        // Same as ResolveFile but only matches directories.
        bool ResolveDirectory(const char* path, std::string& out);

        // Appends the absolute paths of every indexed file whose in-mod path
        // matches the FindFirstFile-style @mask (e.g. "cleo/*.cs"). Used to
        // feed find_first_file / find_next_file.
        void MatchFiles(const char* mask, std::vector<std::string>& out);

        // Root folder that contains the indexed files, game-relative
        // (normally "modloader"). Empty when Mod Loader is not installed.
        const std::string& GetRootFolder() { EnsureIndexed(); return m_rootFolder; }

    private:
        CModLoaderCompat() = default;

        struct Entry
        {
            std::string relPath;    // game-relative, e.g. "modloader\\mod\\cleo\\x.cs"
            std::string modPath;    // relative to the mod root, e.g. "cleo\\x.cs"
            std::string absPath;    // absolute real path
        };

        void EnsureIndexed();
        bool DetectGameRoot(std::string& baseDir);
        bool DetectRoot(const std::string& baseDir, std::string& root);
        void BuildIndex(const std::string& baseDir, const std::string& modloaderDir);
        void WalkMod(const std::string& absDir, const std::string& relDir,
                     const std::string& modPath);
        void LoadIgnoreRules(const std::string& modloaderDir);

        bool IsModIgnored(const std::string& modName) const;
        bool IsPathIgnored(const std::string& modPath) const;

        bool m_active = false;
        bool m_indexed = false;
        std::string m_baseDir;                  // absolute game root
        std::string m_rootFolder;               // "modloader" (game-relative)
        std::vector<Entry> m_entries;

        std::unordered_map<std::string, const Entry*> m_byModPath;
        std::unordered_map<std::string, const Entry*> m_byRelPath;
        // filename -> entries, for the folder-agnostic fuzzy fallback.
        std::unordered_map<std::string, std::vector<const Entry*>> m_byFileName;

        // modloader.ini derived filters (profile-aware subset).
        std::vector<std::string> m_ignoreMods;
        std::vector<std::string> m_ignoreFiles;
    };
}
