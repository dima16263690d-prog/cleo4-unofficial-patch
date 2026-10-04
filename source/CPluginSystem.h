#pragma once
#include <list>
#include <algorithm>
#include <set>
#include <string>
#include <windows.h>
#include "FileEnumerator.h"
#include "CDebugBridge.h"

namespace CLEO
{
    class CPluginSystem
    {
        std::list<HMODULE> plugins;
        std::set<std::string> loadedPluginPaths;

        void LoadDirectory(const char* directory, const char* mask)
        {
            char searchPath[MAX_PATH] = {};
            sprintf_s(searchPath, sizeof(searchPath), "%s/%s", directory, mask);

            FilesWalk(searchPath, [this, directory](const char* libName) {
                char libPath[MAX_PATH] = {};
                sprintf_s(libPath, sizeof(libPath), "%s/%s", directory, libName);

                if (!loadedPluginPaths.insert(libPath).second)
                    return;

                TRACE("[PluginSystem] Loading plugin %s", libPath);

                HMODULE hlib = LoadLibraryA(libPath);
                if (!hlib)
                {
                    char message[MAX_PATH + 40] = {};
                    sprintf_s(message, sizeof(message), "Error loading plugin %s", libPath);
                    Warning(message);
                    return;
                }

                plugins.push_back(hlib);
            });
        }

    public:
        CPluginSystem()
        {
            CreateDirectoryA("cleo", nullptr);
            CreateDirectoryA("cleo\\cleo_plugins", nullptr);

            // Load DebugUtils first so the optional diagnostic backend is
            // attached before other optional plugins start their work.
            LoadDirectory("cleo/cleo_plugins", "DebugUtils.cleo");

            // Keep legacy CLEO 4 plugin placement working.
            LoadDirectory("cleo", "*.cleo");

            // New plugins use the CLEO 5-style location.
            LoadDirectory("cleo/cleo_plugins", "*.cleo");
        }

        ~CPluginSystem()
        {
            TRACE("[PluginSystem] Unloading plugins...");
            std::for_each(plugins.rbegin(), plugins.rend(), FreeLibrary);
        }

        inline size_t GetNumPlugins() const { return plugins.size(); }
    };
}
