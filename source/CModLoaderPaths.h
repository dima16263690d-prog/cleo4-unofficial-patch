#pragma once
// Pure, platform-independent path helpers for Mod Loader interop.
// Kept free of <windows.h> so the logic can be unit-tested on any host.
#include <string>
#include <cctype>

namespace CLEO
{
    namespace ModLoaderPaths
    {
        inline std::string ToLower(std::string s)
        {
            for (char& c : s)
                c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        inline std::string Trim(const std::string& s)
        {
            size_t b = 0, e = s.size();
            while (b < e && ::isspace(static_cast<unsigned char>(s[b]))) ++b;
            while (e > b && ::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
            return s.substr(b, e - b);
        }

        // Mod Loader path convention: lowercase, '\\' separators, no trailing
        // separator (mirrors modloader::NormalizePath so comparisons line up).
        inline std::string Normalize(std::string s)
        {
            for (char& c : s)
            {
                if (c == '/') c = '\\';
                c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
            }
            while (!s.empty() && s.back() == '\\')
                s.pop_back();
            return s;
        }

        inline std::string DirName(const std::string& path)
        {
            const size_t pos = path.find_last_of('\\');
            return pos == std::string::npos ? std::string() : path.substr(0, pos);
        }

        inline std::string FileName(const std::string& path)
        {
            const size_t pos = path.find_last_of('\\');
            return pos == std::string::npos ? path : path.substr(pos + 1);
        }

        // Same wildcard rules as Mod Loader (src/core/wildcard.cpp): case
        // insensitive, '\\' or '/' separate path components, a '*' with no
        // directory component matches across subdirectories, '**' always does,
        // '?' is a single non-separator character, '\\' escapes the next char.
        inline bool MatchWildcard(const char* pattern, const char* string)
        {
            bool allow_subdir = false;
            bool had_anydir_in_pattern = false;
            while (true)
            {
                switch (*pattern++)
                {
                case '\0':
                    if (*string == '\0') return true;
                    if ((*string == '/' || *string == '\\') && (*(string + 1) == '\0')) return true;
                    return false;

                case '/':
                case '\\':
                    if (*string != '/' && *string != '\\')
                    {
                        if (*string == '\0' && *pattern == '\0')
                            break;
                        return false;
                    }
                    ++string;
                    had_anydir_in_pattern = true;
                    break;

                case '*':
                    allow_subdir = (!had_anydir_in_pattern || (*pattern == '*'));
                    while (*pattern == '*') ++pattern;

                    if (*pattern == '\0')
                        return true;

                    for (; *string; ++string)
                    {
                        if (MatchWildcard(string, pattern))
                            return true;
                        else if ((*string == '/' || *string == '\\') && !allow_subdir)
                            return false;
                    }
                    return false;

                case '?':
                    if ((*string == '/' || *string == '\\') || *string == '\0')
                        return false;
                    ++string;
                    break;

                default:
                    if (*string == '\0' || ::tolower(static_cast<unsigned char>(*string)) !=
                        ::tolower(static_cast<unsigned char>(*(pattern - 1))))
                        return false;
                    ++string;
                    break;
                }
            }
        }
    }
}
