#include "stdafx.h"
#include "CDebug.h"

CDebug Debug;

#ifdef DEBUGIT

namespace
{
    void FormatMessage(char *buffer, size_t bufferSize, const char *format, va_list args)
    {
        vsnprintf_s(buffer, bufferSize, _TRUNCATE, format, args);
    }

    void NormalizeLogText(char *message)
    {
        struct Tag
        {
            const char *from;
            const char *to;
        };

        // All substitutions below intentionally have identical byte length.
        // This keeps normalization in-place and prevents buffer growth.
        static const Tag tags[] =
        {
            { "[CLEO]",     "[Cleo]"     },
            { "[ERROR]",    "[Error]"    },
            { "[WARNING]",  "[Warning]"  },
            { "[INFO]",     "[Info]"     },
            { "[CUSTOM]",   "[Custom]"   },
            { "[RESTORE]",  "[Restore]"  },
            { "[LOAD]",     "[Load]"     },
            { "[SAVE]",     "[Save]"     },
            { "[END]",      "[End]"      },
            { "[STOP]",     "[Stop]"     },
            { "[DELETE]",   "[Delete]"   },
            { "[CREATE]",   "[Create]"   },
            { "[REGISTER]", "[Register]" }
        };

        for (char *p = message; *p; ++p)
        {
            if (*p != '[')
                continue;

            for (const auto &tag : tags)
            {
                const size_t fromLen = strlen(tag.from);
                const size_t toLen = strlen(tag.to);

                if (fromLen != toLen)
                    continue;

                if (_strnicmp(p, tag.from, fromLen) != 0)
                    continue;

                memcpy(p, tag.to, toLen);
                p += toLen - 1;
                break;
            }
        }
    }

}

CDebug::CDebug()
    : m_hFile(szLogFileName)
{
    Write("Info", "Log started.");
}

CDebug::~CDebug()
{
    Write("Info", "Log finished.");
}

void CDebug::Write(const char *level, const char *message)
{
    char normalized[2048];
    strncpy_s(normalized, sizeof(normalized), message ? message : "", _TRUNCATE);
    NormalizeLogText(normalized);

    const std::string key = std::string(level) + "|" + normalized;

    std::lock_guard<std::mutex> lock(m_mutex);

    // Normal informational messages are deduplicated for the whole game
    // session. This prevents high-frequency successful operations such as
    // repeated 0AB1/0AB2 calls from flooding the log.
    // Warnings and errors are never suppressed here.
    if (_stricmp(level, "Info") == 0)
    {
        if (!m_infoMessages.insert(key).second)
            return;
    }

    if (!m_hFile.is_open())
    {
        OutputDebugStringA("[Cleo][Error] Failed to open cleo.log\n");
        return;
    }

    SYSTEMTIME t;
    char szBuf[4096];

    GetLocalTime(&t);

    sprintf_s(
        szBuf,
        sizeof(szBuf),
        "%04d-%02d-%02d %02d:%02d:%02d.%03d [%-7.7s] %s",
        t.wYear,
        t.wMonth,
        t.wDay,
        t.wHour,
        t.wMinute,
        t.wSecond,
        t.wMilliseconds,
        level,
        normalized
    );

    m_hFile << szBuf << std::endl;
    m_hFile.flush();

    OutputDebugStringA(szBuf);
    OutputDebugStringA("\n");
}

void CDebug::WriteFormatted(const char *level, const char *format, va_list args)
{
    char message[2048];
    FormatMessage(message, sizeof(message), format, args);
    Write(level, message);
}

void CDebug::Trace(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    WriteFormatted("Info", format, args);
    va_end(args);
}

void CDebug::TraceWarning(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    WriteFormatted("Warning", format, args);
    va_end(args);
}

void CDebug::TraceError(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    WriteFormatted("Error", format, args);
    va_end(args);
}

void CDebug::TraceDiagnostic(const char *format, ...)
{
    char message[2048];

    va_list args;
    va_start(args, format);
    FormatMessage(message, sizeof(message), format, args);
    va_end(args);

    const char *level = "Info";

    if (strstr(message, "[ERROR]") || strstr(message, "[Error]") ||
        strstr(message, "error") || strstr(message, "Error") ||
        strstr(message, "failed") || strstr(message, "Failed"))
    {
        level = "Error";
    }
    else if (strstr(message, "[WARNING]") || strstr(message, "[Warning]") ||
             strstr(message, "warning") || strstr(message, "Warning"))
    {
        level = "Warning";
    }

    Write(level, message);
}

#endif

void Error(const char *szStr)
{
    MessageBox(nullptr, szStr, "CLEO error", MB_ICONERROR | MB_OK);
#ifdef DEBUGIT
    Debug.TraceError("%s", szStr);
#endif
    //exit(1);
}

void Warning(const char *szStr)
{
    MessageBox(nullptr, szStr, "CLEO warning", MB_ICONWARNING | MB_OK);
#ifdef DEBUGIT
    Debug.TraceWarning("%s", szStr);
#endif
    //exit(1);
}
