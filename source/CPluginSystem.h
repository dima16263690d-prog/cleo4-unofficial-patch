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

        void LoadDirectory(const char* directory, const char* prefix, const char* extension)
        {
            char searchPath[MAX_PATH] = {};
            sprintf_s(searchPath, sizeof(searchPath), "%s/%s*%s", directory, prefix, extension);

            std::vector<std::string> files;
            FilesWalk(searchPath, [&files](const char* libName) {
                files.emplace_back(libName);
            });

            std::sort(files.begin(), files.end(),
                [](const std::string& a, const std::string& b) {
                    return _stricmp(a.c_str(), b.c_str()) < 0;
                });

            for (const std::string& libName : files)
            {
                char libPath[MAX_PATH] = {};
                sprintf_s(libPath, sizeof(libPath), "%s/%s", directory, libName.c_str());

                // A plugin filename is its identity. This prevents the same
                // plugin from being loaded twice by overlapping scan groups.
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
            TRACE("[PluginSystem] Listing CLEO plugins from cleo/cleo_plugins:");

            // All native CLEO plugins belong to ONE dedicated directory.
            // This deliberately keeps *.cleo separate from *.cs/*.cs3/*.cs4.
            //
            // First collect SA.*.cleo, then the remaining *.cleo files.
            // CLEO+ (CLEO+.cleo) is therefore loaded from cleo/cleo_plugins
            // just like every other native CLEO plugin.
            LoadDirectory("cleo/cleo_plugins", "SA.", ".cleo");
            LoadDirectory("cleo/cleo_plugins", "", ".cleo");

            // Do not scan cleo/*.cleo. The CLEO root is reserved for scripts
            // and resource directories; plugins must live in cleo_plugins.

            // Load in reverse discovery order, matching the CLEO 5 override
            // principle while keeping discovery deterministic.
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

            TRACE("[PluginSystem] Unloading CLEO plugins...");

            // Reverse the actual load order during shutdown.
            for (auto it = plugins.rbegin(); it != plugins.rend(); ++it)
            {
                if (*it)
                    FreeLibrary(*it);
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
