#pragma once

#include <cstdarg>
#include <fstream>
#include <mutex>
#include <set>
#include <string>

#define TRACE __noop

#ifdef DEBUGIT
#undef TRACE
#define TRACE(a,...) {Debug.Trace(a, __VA_ARGS__);}
#endif

const char szLogFileName[] = "cleo.log";

class CDebug
{
#ifdef DEBUGIT
    std::ofstream m_hFile;
    std::mutex m_mutex;
    std::set<std::string> m_infoMessages;

    void Write(const char *level, const char *message);
    void WriteFormatted(const char *level, const char *format, va_list args);
#endif

public:
#ifdef DEBUGIT

    CDebug();
    ~CDebug();

    void Trace(const char *format, ...);
    void TraceWarning(const char *format, ...);
    void TraceError(const char *format, ...);
    void TraceDiagnostic(const char *format, ...);

#endif
};

#ifdef DEBUGIT
#define DIAG(format,...) {Debug.TraceDiagnostic(format, __VA_ARGS__);}
#else
#define DIAG(...) __noop
#endif

extern CDebug Debug;
void Warning(const char *);
void Error(const char *);
