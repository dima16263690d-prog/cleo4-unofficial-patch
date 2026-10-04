#pragma once

class CDiagnosticLog
{
#ifdef DEBUGIT
    std::ofstream m_hFile;
#endif

public:
#ifdef DEBUGIT
    CDiagnosticLog();
    ~CDiagnosticLog();

    void Trace(const char *format, ...);
#endif
};

extern CDiagnosticLog DiagnosticLog;

#ifdef DEBUGIT
#define DIAG(format,...) { DiagnosticLog.Trace(format, __VA_ARGS__); }
#else
#define DIAG(...) __noop
#endif
