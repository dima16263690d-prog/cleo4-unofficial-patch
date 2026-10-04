#pragma once
#include <list>
#include <algorithm>
#include <set>
#include <string>
#include <vector>
#include <windows.h>
#include "FileEnumerator.h"
#include "CDebugBridge.h"

namespace CLEO
{
    class CPluginSystem
    {
        std::list<HMODULE> plugins;
        std::set<std::string> loadedPluginNames;
        bool initialized = false;

        static bool SameName(const std::string& a, const std::string& b)
        {
            return _stricmp(a.c_str(), b.c_str()) == 0;
        }

        void LoadDirectory(const char* directory, const char* prefix, const char* extension)
        {
            char searchPath[MAX_PATH] = {};
            sprintf_s(searchPath, sizeof(searchPath), "%s/%s*%s", directory, prefix, extension);

            std::vector<std::string> files;
            FilesWalk(searchPath, [&files](const char* libName) {
                files.emplace_back(libName);
            });

            // Keep plugin discovery deterministic. CLEO 5 builds its plugin
            // list first and only then loads it, so discovery order matters.
            std::sort(files.begin(), files.end(),
                [](const std::string& a, const std::string& b) {
                    return _stricmp(a.c_str(), b.c_str()) < 0;
                });

            for (const std::string& libName : files)
            {
                char libPath[MAX_PATH] = {};
                sprintf_s(libPath, sizeof(libPath), "%s/%s", directory, libName.c_str());

                // CLEO 5 treats the plugin filename as the identity. This
                // prevents the same plugin from being loaded twice when it
                // exists in both cleo_plugins and the legacy cleo directory.
                if (std::find_if(loadedPluginNames.begin(), loadedPluginNames.end(),
                    [&libName](const std::string& name) {
                        return _stricmp(name.c_str(), libName.c_str()) == 0;
                    }) != loadedPluginNames.end())
                {
                    TRACE("[PluginSystem] Skipping duplicate plugin %s", libPath);
                    continue;
                }

                loadedPluginNames.insert(libName);
                TRACE("[PluginSystem] Found plugin %s", libPath);

                // Store the path now. Actual loading is deliberately deferred
                // until all four CLEO 5-style scan groups are collected.
                pluginPaths.emplace_back(libPath);
            }
        }

        std::vector<std::string> pluginPaths;

    public:
        CPluginSystem() = default;

        void LoadPlugins()
        {
            if (initialized)
                return;

            initialized = true;
            pluginPaths.clear();
            loadedPluginNames.clear();

            TRACE("");
            TRACE("[PluginSystem] Listing CLEO plugins:");

            // Same discovery groups as CLEO 5:
            // 1) SA.*.cleo in cleo_plugins
            // 2) legacy *.cleo in cleo_plugins
            // 3) legacy *.cleo in cleo
            LoadDirectory("cleo/cleo_plugins", "SA.", ".cleo");
            LoadDirectory("cleo/cleo_plugins", "", ".cleo");
            LoadDirectory("cleo", "", ".cleo");

            // CLEO 5 loads the collected list in reverse order so that
            // newer CLEO plugins can overwrite handlers from legacy plugins.
            for (auto it = pluginPaths.rbegin(); it != pluginPaths.rend(); ++it)
            {
                TRACE("");
                TRACE("[PluginSystem] Loading plugin %s", it->c_str());

                HMODULE hlib = LoadLibraryA(it->c_str());
                if (!hlib)
                {
                    char message[MAX_PATH + 64] = {};
                    sprintf_s(message, sizeof(message),
                        "Error loading plugin %s", it->c_str());
                    Warning(message);
                    continue;
                }

                plugins.push_back(hlib);
            }

            pluginPaths.clear();
            TRACE("");
        }

        void UnloadPlugins()
        {
            if (!initialized)
                return;

            TRACE("[PluginSystem] Unloading plugins...");

            // Plugins were loaded in reverse discovery order. Unload in the
            // same order as CLEO 5 to preserve dependency/override symmetry.
            for (HMODULE hlib : plugins)
            {
                if (hlib)
                    FreeLibrary(hlib);
            }

            plugins.clear();
            loadedPluginNames.clear();
            initialized = false;
        }

        ~CPluginSystem()
        {
            UnloadPlugins();
        }

        inline size_t GetNumPlugins() const { return plugins.size(); }
    };
}
