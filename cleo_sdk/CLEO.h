/*
	CLEO 4.4 header file;
	Copyright (c) 2020 Alien, Deji, Junior_Djjr;
*/
#pragma once

#include <wtypes.h>
#include <stdint.h>
#include "CLEO_Debug.h"

#define CLEO_VERSION 0x04040400
#define CLEO_VERSIONTEXT "4.4.4"

//result of CLEO_GetGameVersion()
#define GV_US10 0	//1.0 us
#define GV_US11 1	//1.01 us - not supported
#define GV_EU10 2	//1.0 eu
#define GV_EU11 3	//1.01 eu
#define GV_UNK -1	//any other

typedef union
{
	DWORD	dwParam;
	int		nParam;
	float	fParam;
	void *	pParam;
	char *	szParam;
} SCRIPT_VAR;

//operand types
#define	globalVar			2		//$
#define	localVar			3		//@
#define	globalArr			7		//$(,)
#define	localArr 			8		//@(,)
#define	imm8 				4		//char
#define	imm16 				5		//short
#define	imm32 				1		//long, unsigned long
#define	imm32f 				6		//float
#define	vstring 			0x0E	//""
#define	sstring 			9		//''
#define	globalVarVString 	0x10	//v$
#define	localVarVString 	0x11	//@v
#define	globalVarSString 	0x0A	//s$
#define	localVarSString 	0x0B	//@s


typedef int SCRIPT_HANDLE;
typedef SCRIPT_HANDLE HANDLE_ACTOR, ACTOR, HACTOR, PED, HPED, HANDLE_PED;
typedef SCRIPT_HANDLE HANDLE_CAR, CAR, HCAR, VEHICLE, HVEHICLE, HANDLE_VEHICLE;
typedef SCRIPT_HANDLE HANDLE_OBJECT, OBJECT, HOBJECT;
typedef SCRIPT_HANDLE HSTREAM;
 
typedef struct CScriptThread CScriptThread;

#pragma pack(push,1)
struct CScriptThread
{
	CScriptThread	*next;					//next script in queue
	CScriptThread	*prev;					//previous script in queue
	char			threadName[8];			//name of thread, given by 03A4 opcode
	BYTE			*baseIp;				//pointer to begin of script in memory
	BYTE			*ip;					//current index pointer
	BYTE			*stack[8];				//return stack for 0050, 0051
	WORD			sp;						//current item in stack
	WORD			_f3A;					//padding
	SCRIPT_VAR		tls[34];				//thread's local variables
	BYTE			isActive;				//is current thread active
	char			condResult;				//condition result (true or false)
	char			missionCleanupFlag;		//clean mission
	char			external;				//is thread external (from script.img)
	BYTE			_fC8;					//unknown
	BYTE			_fC9;					//unknown
	BYTE			_fCA;					//unknown
	BYTE			_fCB;					//unknown
	DWORD			wakeTime;				//time, when script starts again after 0001 opcode
	WORD			logicalOp;				//00D6 parameter
	BYTE			notFlag;				//opcode & 0x8000 != 0
	BYTE			wbCheckEnabled;			//wasted_or_busted check flag
	BYTE			wastedOrBusted;			//is player wasted or busted
	BYTE			_fD5;					//unknown
	WORD			_fD6;					//unknown
	DWORD			sceneSkip;				//scene skip label ptr
	BYTE			missionFlag;			//is mission thread
	BYTE			_fDD[3];				//padding
};
#pragma pack(pop)

#define OR_CONTINUE 0
#define OR_INTERRUPT 1

typedef int OpcodeResult;

typedef OpcodeResult (CALLBACK* _pOpcodeHandler)(CScriptThread*);
typedef void(*FuncScriptDeleteDelegateT) (CScriptThread*);

#ifdef __cplusplus
extern "C" {
#endif	//__cplusplus

DWORD WINAPI CLEO_GetVersion();
int   WINAPI CLEO_GetGameVersion();

BOOL  WINAPI CLEO_RegisterOpcode(WORD opcode, _pOpcodeHandler callback);

DWORD WINAPI CLEO_GetIntOpcodeParam(CScriptThread* thread);
float WINAPI CLEO_GetFloatOpcodeParam(CScriptThread* thread);

void  WINAPI CLEO_SetIntOpcodeParam(CScriptThread* thread, DWORD value);
void  WINAPI CLEO_SetFloatOpcodeParam(CScriptThread* thread, float value);

LPSTR WINAPI CLEO_ReadStringOpcodeParam(CScriptThread* thread, LPSTR buf, int size);
LPSTR WINAPI CLEO_ReadStringPointerOpcodeParam(CScriptThread* thread, LPSTR buf, int size);
void  WINAPI CLEO_WriteStringOpcodeParam(CScriptThread* thread, LPCSTR str);

void  WINAPI CLEO_SetThreadCondResult(CScriptThread* thread, BOOL result);

void  WINAPI CLEO_SkipOpcodeParams(CScriptThread* thread, int count);

void  WINAPI CLEO_ThreadJumpAtLabelPtr(CScriptThread* thread, int labelPtr);

int   WINAPI CLEO_GetOperandType(CScriptThread* thread);

extern SCRIPT_VAR *opcodeParams;

extern SCRIPT_VAR *missionLocals;

//intermediate data is stored in opcodeParams array
void WINAPI CLEO_RetrieveOpcodeParams(CScriptThread *thread, int count);
void WINAPI CLEO_RecordOpcodeParams(CScriptThread *thread, int count);

int WINAPI CLEO_FormatOpcodeString(CScriptThread* thread, LPSTR buffer, int size);

SCRIPT_VAR * WINAPI CLEO_GetPointerToScriptVariable(CScriptThread *thread);

DWORD WINAPI CLEO_GetScriptTextureById(CScriptThread* thread, int id); // ret RwTexture *

HSTREAM WINAPI CLEO_GetInternalAudioStream(CScriptThread* thread, DWORD stream); // arg CAudioStream *


/*
    Extended Audio API used by the optional Audio.cleo plugin.
    These functions operate on the same stream handles returned by the
    legacy CLEO 4 audio opcodes; they do not create a second BASS runtime.
*/
BOOL  WINAPI CLEO_Audio_IsValidStream(DWORD stream);
DWORD WINAPI CLEO_Audio_GetState(DWORD stream);
void  WINAPI CLEO_Audio_SetState(DWORD stream, DWORD action);
float WINAPI CLEO_Audio_GetLength(DWORD stream);
float WINAPI CLEO_Audio_GetDuration(DWORD stream);
float WINAPI CLEO_Audio_GetVolume(DWORD stream);
void  WINAPI CLEO_Audio_SetVolume(DWORD stream, float value);
void  WINAPI CLEO_Audio_SetVolumeTransition(DWORD stream, float value, float transitionSeconds);
float WINAPI CLEO_Audio_GetSpeed(DWORD stream);
void  WINAPI CLEO_Audio_SetSpeed(DWORD stream, float value, float transitionSeconds);
float WINAPI CLEO_Audio_GetProgress(DWORD stream);
void  WINAPI CLEO_Audio_SetProgress(DWORD stream, float value);
BOOL  WINAPI CLEO_Audio_GetLooping(DWORD stream);
void  WINAPI CLEO_Audio_SetLooping(DWORD stream, BOOL enable);
DWORD WINAPI CLEO_Audio_GetType(DWORD stream);
void  WINAPI CLEO_Audio_SetType(DWORD stream, DWORD type);
BOOL  WINAPI CLEO_Audio_Set3dSourceSize(DWORD stream, float radius);

/*
    --------------------------------------------------------------------------
    Plugin SDK 2.0
    --------------------------------------------------------------------------

    A single, safe surface for third-party plugins on top of the existing
    CLEO 4 opcode registry. It adds:

      - opcode metadata (name, parameter signature) exposed to tools and the
        crash reporter;
      - parameter validation helpers so a handler never has to trust the
        script blindly;
      - error reporting that names the script and opcode;
      - object lifetime callbacks (CLEO_CB_SCRIPT_CREATED / _DELETED) and a
        per-script context slot for plugin-owned state;
      - an opcode-registration notification (CLEO_CB_SCRIPT_OPCODE_REGISTERED).

    Legacy plugins that only call CLEO_RegisterOpcode keep working unchanged:
    the 2.0 entry points are additive and the old ABI is preserved.
*/

// Parameter signature characters understood by CLEO_OpcodeValidate:
//   'i' int, 'f' float, 's' string, 'v' variable/pointer, '*' any, digits are
//   ignored (used only for readability, e.g. "ii" == 2 ints).
// CLEO_OpcodeParamKind, CLEO_OpcodeInfo and CLEO_ScriptProfile are declared in
// CLEO_Debug.h, which this header includes.

typedef void (__stdcall *CLEO_ScriptCreatedCallback)(CScriptThread* thread);

// Register an opcode together with its metadata. The name/params/description
// pointers must stay valid for as long as the plugin is loaded (string
// literals are the normal case). Returns FALSE if the opcode is already taken
// or the callback is null.
BOOL  WINAPI CLEO_RegisterOpcodeEx(WORD opcode, _pOpcodeHandler callback,
                                   const char* name, const char* params,
                                   const char* description);

// Metadata query. Returns FALSE when the opcode is unknown. The returned
// CLEO_OpcodeInfo is filled from CLEO's own registry; string pointers are
// borrowed and must not be freed by the caller.
BOOL  WINAPI CLEO_GetOpcodeInfo(WORD opcode, CLEO_OpcodeInfo* info);

// Fill paramCount kinds from a signature string such as "iif". Returns the
// number of characters that were recognised, or -1 on invalid input.
int   WINAPI CLEO_ParseParamSignature(const char* signature,
                                      CLEO_OpcodeParamKind* out, int maxOut);

// Validate that the next parameters in the script match the signature. This
// only inspects the parameter type bytes; it does not consume them. Returns
// TRUE when every parameter is acceptable, FALSE otherwise.
BOOL  WINAPI CLEO_OpcodeValidate(CScriptThread* thread, const char* signature);

// Report a plugin error. The message is routed through the CLEO debug log
// (DebugUtils) so it lands in the same stream as core diagnostics.
void  WINAPI CLEO_ReportError(const char* message);

// Per-script plugin context. Values are keyed by the calling plugin module so
// two plugins can never collide. A key of 0 uses the calling module itself.
void* WINAPI CLEO_GetScriptContext(CScriptThread* thread, uintptr_t key);
BOOL  WINAPI CLEO_SetScriptContext(CScriptThread* thread, uintptr_t key, void* value);

// Script execution profiler (CLEO_ScriptProfile is declared in CLEO_Debug.h).
// Profiling is off by default; enable it before reading counters. Returns the
// previous enabled state.
BOOL  WINAPI CLEO_SetProfilingEnabled(BOOL enabled);

// Returns the number of scripts currently tracked (0 when profiling is off).
DWORD WINAPI CLEO_GetScriptProfileCount();

// Fills one entry by index (0 .. count-1). Returns FALSE when out of range.
BOOL  WINAPI CLEO_GetScriptProfile(DWORD index, CLEO_ScriptProfile* profile);

// Resets every per-script counter. Aggregate counters are kept.
void  WINAPI CLEO_ResetScriptProfiles();

// Unified plugin resource registry. Register any allocation whose lifetime
// should end when the plugin is unloaded; `release` runs once, right before the
// module is unmapped, and may be null for a resource that needs no cleanup.
// `kind` is a borrowed diagnostic tag (string literal). Registering the same
// pointer twice returns FALSE. CLEO owns the registry, not the resource.
// CLEO_PluginResourceReleaseFn is declared in CLEO_Debug.h, included above.
BOOL  WINAPI CLEO_RegisterPluginResource(const char* kind, void* resource,
                                         CLEO_PluginResourceReleaseFn release);

// Remove a resource from the registry without running its release callback.
// Safe to call for an unknown resource; returns FALSE then.
BOOL  WINAPI CLEO_UnregisterPluginResource(void* resource);

CScriptThread* WINAPI CLEO_CreateCustomScript(CScriptThread* fromThread, const char *script_name, int label);

CScriptThread* WINAPI CLEO_GetLastCreatedCustomScript();

void WINAPI CLEO_AddScriptDeleteDelegate(FuncScriptDeleteDelegateT func);

void WINAPI CLEO_RemoveScriptDeleteDelegate(FuncScriptDeleteDelegateT func);

#ifdef __cplusplus
}
#endif	//__cplusplus

