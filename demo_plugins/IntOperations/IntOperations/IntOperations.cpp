#include "plugin.h"
#include "CLEO.h"
using namespace plugin;
class IntOperations{
public:
 IntOperations(){if(CLEO_GetVersion()>=CLEO_VERSION){CLEO_RegisterOpcode(0x0B10,Script_IntOp_AND);CLEO_RegisterOpcode(0x0B11,Script_IntOp_OR);CLEO_RegisterOpcode(0x0B12,Script_IntOp_XOR);CLEO_RegisterOpcode(0x0B13,Script_IntOp_NOT);CLEO_RegisterOpcode(0x0B14,Script_IntOp_MOD);CLEO_RegisterOpcode(0x0B15,Script_IntOp_SHR);CLEO_RegisterOpcode(0x0B16,Script_IntOp_SHL);CLEO_RegisterOpcode(0x0B17,Scr_IntOp_AND);CLEO_RegisterOpcode(0x0B18,Scr_IntOp_OR);CLEO_RegisterOpcode(0x0B19,Scr_IntOp_XOR);CLEO_RegisterOpcode(0x0B1A,Scr_IntOp_NOT);CLEO_RegisterOpcode(0x0B1B,Scr_IntOp_MOD);CLEO_RegisterOpcode(0x0B1C,Scr_IntOp_SHR);CLEO_RegisterOpcode(0x0B1D,Scr_IntOp_SHL);}else MessageBox(HWND_DESKTOP,"An incorrect version of CLEO was loaded.","IntOperations.cleo",MB_ICONERROR);}
 static OpcodeResult WINAPI Script_IntOp_AND(CScriptThread*t){int a=static_cast<int>(CLEO_GetIntOpcodeParam(t)),b=static_cast<int>(CLEO_GetIntOpcodeParam(t));CLEO_SetIntOpcodeParam(t,static_cast<DWORD>(a&b));return OR_CONTINUE;}
 static OpcodeResult WINAPI Script_IntOp_OR(CScriptThread*t){int a=static_cast<int>(CLEO_GetIntOpcodeParam(t)),b=static_cast<int>(CLEO_GetIntOpcodeParam(t));CLEO_SetIntOpcodeParam(t,static_cast<DWORD>(a|b));return OR_CONTINUE;}
 static OpcodeResult WINAPI Script_IntOp_XOR(CScriptThread*t){int a=static_cast<int>(CLEO_GetIntOpcodeParam(t)),b=static_cast<int>(CLEO_GetIntOpcodeParam(t));CLEO_SetIntOpcodeParam(t,static_cast<DWORD>(a^b));return OR_CONTINUE;}
 static OpcodeResult WINAPI Script_IntOp_NOT(CScriptThread*t){int a=static_cast<int>(CLEO_GetIntOpcodeParam(t));CLEO_SetIntOpcodeParam(t,static_cast<DWORD>(~a));return OR_CONTINUE;}
 static OpcodeResult WINAPI Script_IntOp_MOD(CScriptThread*t){int a=static_cast<int>(CLEO_GetIntOpcodeParam(t)),b=static_cast<int>(CLEO_GetIntOpcodeParam(t));CLEO_SetIntOpcodeParam(t,static_cast<DWORD>(a%b));return OR_CONTINUE;}
 static OpcodeResult WINAPI Script_IntOp_SHR(CScriptThread*t){int a=static_cast<int>(CLEO_GetIntOpcodeParam(t)),b=static_cast<int>(CLEO_GetIntOpcodeParam(t));CLEO_SetIntOpcodeParam(t,static_cast<DWORD>(a>>b));return OR_CONTINUE;}
 static OpcodeResult WINAPI Script_IntOp_SHL(CScriptThread*t){int a=static_cast<int>(CLEO_GetIntOpcodeParam(t)),b=static_cast<int>(CLEO_GetIntOpcodeParam(t));CLEO_SetIntOpcodeParam(t,static_cast<DWORD>(a<<b));return OR_CONTINUE;}
 static OpcodeResult WINAPI Scr_IntOp_AND(CScriptThread*t){SCRIPT_VAR*o=CLEO_GetPointerToScriptVariable(t);o->dwParam&=CLEO_GetIntOpcodeParam(t);return OR_CONTINUE;}
 static OpcodeResult WINAPI Scr_IntOp_OR(CScriptThread*t){SCRIPT_VAR*o=CLEO_GetPointerToScriptVariable(t);o->dwParam|=CLEO_GetIntOpcodeParam(t);return OR_CONTINUE;}
 static OpcodeResult WINAPI Scr_IntOp_XOR(CScriptThread*t){SCRIPT_VAR*o=CLEO_GetPointerToScriptVariable(t);o->dwParam^=CLEO_GetIntOpcodeParam(t);return OR_CONTINUE;}
 static OpcodeResult WINAPI Scr_IntOp_NOT(CScriptThread*t){SCRIPT_VAR*o=CLEO_GetPointerToScriptVariable(t);o->dwParam=~o->dwParam;return OR_CONTINUE;}
 static OpcodeResult WINAPI Scr_IntOp_MOD(CScriptThread*t){SCRIPT_VAR*o=CLEO_GetPointerToScriptVariable(t);o->dwParam%=CLEO_GetIntOpcodeParam(t);return OR_CONTINUE;}
 static OpcodeResult WINAPI Scr_IntOp_SHR(CScriptThread*t){SCRIPT_VAR*o=CLEO_GetPointerToScriptVariable(t);o->dwParam>>=CLEO_GetIntOpcodeParam(t);return OR_CONTINUE;}
 static OpcodeResult WINAPI Scr_IntOp_SHL(CScriptThread*t){SCRIPT_VAR*o=CLEO_GetPointerToScriptVariable(t);o->dwParam<<=CLEO_GetIntOpcodeParam(t);return OR_CONTINUE;}
} intOperations;