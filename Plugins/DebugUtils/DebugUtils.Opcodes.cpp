#include "DebugUtils.h"
#include "DebugUtils.Internal.h"
#include "resource.h"
#include <windows.h>
#include <psapi.h>
#include <shellapi.h>
#include <dbghelp.h>
#pragma comment(lib, "Shell32.lib")
#include <TlHelp32.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <fstream>
#include <sstream>
#include <regex>
#include <CTimer.h>

int __stdcall DebugUtils::Opcode_DebugOn(CScriptThread* thread)
{
    if (!s_instance || !thread)
        return OR_CONTINUE;

    s_instance->m_debugScripts.insert(reinterpret_cast<uintptr_t>(thread));
    s_instance->WriteCore("[debug] enabled script='%.8s' ptr=%p", thread->threadName, thread);
    return OR_CONTINUE;
}

int __stdcall DebugUtils::Opcode_DebugOff(CScriptThread* thread)
{
    if (!s_instance || !thread)
        return OR_CONTINUE;

    const uintptr_t scriptPtr = reinterpret_cast<uintptr_t>(thread);
    s_instance->m_debugScripts.erase(scriptPtr);
    s_instance->m_seenScripts.erase(scriptPtr);
    s_instance->WriteCore("[debug] disabled script='%.8s' ptr=%p", thread->threadName, thread);
    return OR_CONTINUE;
}

int __stdcall DebugUtils::Opcode_Breakpoint(CScriptThread* thread)
{
    if (!s_instance || !thread)
        return OR_CONTINUE;

    const uintptr_t scriptPtr = reinterpret_cast<uintptr_t>(thread);
    if (s_instance->m_debugScripts.find(scriptPtr) == s_instance->m_debugScripts.end())
    {
        SkipUnusedVarArgs(thread);
        return OR_CONTINUE;
    }

    bool blocking = true;
    if (CLEO_GetOperandType(thread) == imm8)
        blocking = CLEO_GetIntOpcodeParam(thread) != 0;

    std::string message;
    char buffer[1024] = {};

    if (CLEO_GetOperandType(thread) == 0)
    {
        thread->ip++;
    }
    else
    {
        if (CLEO_FormatOpcodeString(thread, buffer, sizeof(buffer)) >= 0)
            message = buffer;
    }

    char scriptName[9];
    SafeThreadName(thread, scriptName);

    s_instance->m_breakpoints.emplace_back(
        BreakpointInfo{ scriptPtr, scriptName, message, blocking }
    );

    s_instance->WriteCore(
        "[breakpoint] script='%.8s' ptr=%p blocking=%d message='%s' off=0x%zX",
        thread->threadName,
        thread,
        blocking ? 1 : 0,
        message.c_str(),
        ScriptOffset(thread)
    );

    if (blocking)
    {
        CTimer::m_CodePause = true;
        s_instance->WriteCore("[breakpoint] game paused");
    }

    return OR_INTERRUPT;
}

int __stdcall DebugUtils::Opcode_Trace(CScriptThread* thread)
{
    if (!s_instance || !thread)
        return OR_CONTINUE;

    if (s_instance->m_debugScripts.find(reinterpret_cast<uintptr_t>(thread)) ==
        s_instance->m_debugScripts.end())
    {
        SkipUnusedVarArgs(thread);
        return OR_CONTINUE;
    }

    char message[1024] = {};
    if (CLEO_FormatOpcodeString(thread, message, sizeof(message)) < 0)
        return OR_CONTINUE;

    s_instance->WriteCore(
        "[trace] script='%.8s' ptr=%p message='%s'",
        thread->threadName,
        thread,
        message
    );

    s_instance->WriteScript(
        "[trace] script='%.8s' ptr=%p %s",
        thread->threadName,
        thread,
        message
    );

    return OR_CONTINUE;
}

int __stdcall DebugUtils::Opcode_LogToFile(CScriptThread* thread)
{
    if (!s_instance || !thread)
        return OR_CONTINUE;

    char filename[MAX_PATH] = {};
    CLEO_ReadStringOpcodeParam(thread, filename, sizeof(filename));

    const int timestamp = CLEO_GetIntOpcodeParam(thread) != 0;

    char message[1024] = {};
    if (CLEO_FormatOpcodeString(thread, message, sizeof(message)) < 0)
    {
        s_instance->WriteCore(
            "[log_to_file] formatting failed script='%.8s' file='%s'",
            thread->threadName,
            filename
        );
        return OR_CONTINUE;
    }

    s_instance->WriteExternal(filename, timestamp != 0, message);

    s_instance->WriteCore(
        "[log_to_file] script='%.8s' file='%s' timestamp=%d",
        thread->threadName,
        filename,
        timestamp
    );

    return OR_CONTINUE;
}

int __stdcall DebugUtils::Opcode_PrintString(CScriptThread* thread)
{
    if (!s_instance || !thread)
        return OR_CONTINUE;

    if (s_instance->m_debugScripts.find(reinterpret_cast<uintptr_t>(thread)) ==
        s_instance->m_debugScripts.end())
    {
        CLEO_SkipOpcodeParams(thread, 1);
        return OR_CONTINUE;
    }

    char label[1024] = {};
    CLEO_ReadStringOpcodeParam(thread, label, sizeof(label));

    s_instance->WriteCore(
        "[legacy_debug] script='%.8s' printstring='%s'",
        thread->threadName,
        label
    );
    s_instance->WriteScript(
        "[legacy_debug] script='%.8s' printstring='%s'",
        thread->threadName,
        label
    );

    return OR_CONTINUE;
}

int __stdcall DebugUtils::Opcode_PrintInt(CScriptThread* thread)
{
    if (!s_instance || !thread)
        return OR_CONTINUE;

    if (s_instance->m_debugScripts.find(reinterpret_cast<uintptr_t>(thread)) ==
        s_instance->m_debugScripts.end())
    {
        CLEO_SkipOpcodeParams(thread, 2);
        return OR_CONTINUE;
    }

    char label[1024] = {};
    CLEO_ReadStringOpcodeParam(thread, label, sizeof(label));
    const DWORD value = CLEO_GetIntOpcodeParam(thread);

    s_instance->WriteCore(
        "[legacy_debug] script='%.8s' printint='%s: %lu'",
        thread->threadName,
        label,
        static_cast<unsigned long>(value)
    );
    s_instance->WriteScript(
        "[legacy_debug] script='%.8s' printint='%s: %lu'",
        thread->threadName,
        label,
        static_cast<unsigned long>(value)
    );

    return OR_CONTINUE;
}

int __stdcall DebugUtils::Opcode_PrintFloat(CScriptThread* thread)
{
    if (!s_instance || !thread)
        return OR_CONTINUE;

    if (s_instance->m_debugScripts.find(reinterpret_cast<uintptr_t>(thread)) ==
        s_instance->m_debugScripts.end())
    {
        CLEO_SkipOpcodeParams(thread, 2);
        return OR_CONTINUE;
    }

    char label[1024] = {};
    CLEO_ReadStringOpcodeParam(thread, label, sizeof(label));
    const float value = CLEO_GetFloatOpcodeParam(thread);

    s_instance->WriteCore(
        "[legacy_debug] script='%.8s' printfloat='%s: %.6f'",
        thread->threadName,
        label,
        static_cast<double>(value)
    );
    s_instance->WriteScript(
        "[legacy_debug] script='%.8s' printfloat='%s: %.6f'",
        thread->threadName,
        label,
        static_cast<double>(value)
    );

    return OR_CONTINUE;
}
