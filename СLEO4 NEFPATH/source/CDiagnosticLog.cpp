#include "stdafx.h"
#include "CDiagnosticLog.h"

CDiagnosticLog DiagnosticLog;

#ifdef DEBUGIT
CDiagnosticLog::CDiagnosticLog()
    : m_hFile("cleo_diagnostic.log", std::ios::out | std::ios::trunc)
{
    Trace("============================================================");
    Trace("[CLEO][DIAGNOSTIC] Log started");
    Trace("============================================================");
}

CDiagnosticLog::~CDiagnosticLog()
{
    Trace("[CLEO][DIAGNOSTIC] Log finished");
}

void CDiagnosticLog::Trace(const char *format, ...)
{
    SYSTEMTIME t;
    char szBuf[2048];

    GetLocalTime(&t);

    int offset = sprintf_s(
        szBuf,
        sizeof(szBuf),
        "%02d/%02d/%04d %02d:%02d:%02d.%03d ",
        t.wDay,
        t.wMonth,
        t.wYear,
        t.wHour,
        t.wMinute,
        t.wSecond,
        t.wMilliseconds
    );

    va_list arg;
    va_start(arg, format);
    vsnprintf_s(szBuf + offset, sizeof(szBuf) - offset, _TRUNCATE, format, arg);
    va_end(arg);

    m_hFile << szBuf << std::endl;
    m_hFile.flush();

    OutputDebugStringA(szBuf);
    OutputDebugStringA("\n");
}
#endif
