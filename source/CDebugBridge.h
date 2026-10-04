#pragma once

#include "../cleo_sdk/CLEO_Debug.h"

#ifdef DEBUGIT
#define TRACE(format,...) CLEO_DebugLog(CLEO_DEBUG_INFO, format __VA_OPT__(,) __VA_ARGS__)
#define DIAG(format,...) CLEO_DebugLog(CLEO_DEBUG_DIAGNOSTIC, format __VA_OPT__(,) __VA_ARGS__)
#else
#define TRACE(...) __noop
#define DIAG(...) __noop
#endif

void Warning(const char *message);
void Error(const char *message);
