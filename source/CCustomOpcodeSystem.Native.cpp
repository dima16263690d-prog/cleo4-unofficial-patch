#include "stdafx.h"
#include "cleo.h"
#include "CLegacy.h"
#include "CGameVersionManager.h"
#include "CCustomOpcodeSystem.h"
#include "ScmFunction.h"
#include "CTextManager.h"
#include "CCleoMemoryManager.h"
#include "CModelInfo.h"
#include "CDebugCallbackSystem.h"
#include "CModLoaderCompat.h"
#include "CCustomOpcodeSystem.Internal.h"

namespace CLEO
{
	//0AA2=2,%2h% = load_library %1d% // IF and SET
	OpcodeResult __stdcall opcode_0AA2(CRunningScript *thread)
	{
		const char *lib = readString(thread);
		// A library a mod ships under modloader/ must be loadable, exactly as if
		// Mod Loader had translated the path (CLEO.asi is not one of its patched
		// modules). Resolve through the same tree as the file opcodes.
		std::string resolved;
		const char *loadPath = ResolveModLoaderPath(lib, resolved) ? resolved.c_str() : lib;
		auto libHandle = LoadLibrary(loadPath);
		*thread << libHandle;
		SetScriptCondResult(thread, libHandle != nullptr);
		if (libHandle) GetInstance().OpcodeSystem.m_hNativeLibs.insert(libHandle);

		return OR_CONTINUE;
	}

	//0AA3=1,free_library %1h%
	OpcodeResult __stdcall opcode_0AA3(CRunningScript *thread)
	{
		HMODULE libHandle;
		*thread >> libHandle;
		FreeLibrary(libHandle);
		GetInstance().OpcodeSystem.m_hNativeLibs.erase(libHandle);
		return OR_CONTINUE;
	}

	//0AA4=3,%3d% = get_proc_address %1d% library %2d% // IF and SET
	OpcodeResult __stdcall opcode_0AA4(CRunningScript *thread)
	{
		char *funcName = readString(thread);
		HMODULE libHandle;
		*thread >> libHandle;
		void *funcAddr = (void *)GetProcAddress(libHandle, funcName);
		*thread << funcAddr;
		SetScriptCondResult(thread, funcAddr != nullptr);
		return OR_CONTINUE;
	}

	//0AA5=-1,call %1d% num_params %2h% pop %3h%
	OpcodeResult __stdcall opcode_0AA5(CRunningScript *thread)
	{
		static char textParams[5][MAX_STR_LEN]; unsigned currTextParam = 0;
		static SCRIPT_VAR arguments[50] = { 0 };
		void(*func)();
		DWORD numParams;
		DWORD stackAlign;
		*thread >> func >> numParams >> stackAlign;
		if (numParams > (sizeof(arguments) / sizeof(SCRIPT_VAR))) numParams = sizeof(arguments) / sizeof(SCRIPT_VAR);
		stackAlign *= 4;
		SCRIPT_VAR	*arguments_end = arguments + numParams;

		// retrieve parameters
		for (SCRIPT_VAR *arg = arguments; arg != arguments_end; ++arg)
		{
			switch (*thread->GetBytePointer())
			{
			case DT_FLOAT:
			case DT_DWORD:
			case DT_WORD:
			case DT_BYTE:
			case DT_VAR:
			case DT_LVAR:
			case DT_VAR_ARRAY:
			case DT_LVAR_ARRAY:
				*thread >> arg->dwParam;
				break;
			case DT_VAR_STRING:
			case DT_LVAR_STRING:
			case DT_VAR_TEXTLABEL:
			case DT_LVAR_TEXTLABEL:
				arg->pParam = GetScriptParamPointer(thread);
				break;
			case DT_VARLEN_STRING:
			case DT_TEXTLABEL:
				(*arg).pcParam = readString(thread, textParams[currTextParam++], MAX_STR_LEN);
			}
		}

		// call function
		_asm
		{
			lea ecx, arguments
			loop_0AA5 :
			cmp ecx, arguments_end
				jae loop_end_0AA5
				push[ecx]
				add ecx, 0x4
				jmp loop_0AA5
				loop_end_0AA5 :
			call func
				add esp, stackAlign
		}

		SkipUnusedParameters(thread);
		return OR_CONTINUE;
	}

	//0AA6=-1,call_method %1d% struct %2d% num_params %3h% pop %4h%
	OpcodeResult __stdcall opcode_0AA6(CRunningScript *thread)
	{
		static char textParams[5][MAX_STR_LEN]; unsigned currTextParam = 0;
		static SCRIPT_VAR arguments[50] = { 0 };
		void(*func)();
		void *struc;
		DWORD numParams;
		DWORD stackAlign;
		*thread >> func >> struc >> numParams >> stackAlign;
		if (numParams > (sizeof(arguments) / sizeof(SCRIPT_VAR))) numParams = sizeof(arguments) / sizeof(SCRIPT_VAR);
		stackAlign *= 4;
		SCRIPT_VAR *arguments_end = arguments + numParams;

		// retrieve parameters
		for (SCRIPT_VAR *arg = arguments; arg != arguments_end; ++arg)
		{
			switch (*thread->GetBytePointer())
			{
			case DT_FLOAT:
			case DT_DWORD:
			case DT_WORD:
			case DT_BYTE:
			case DT_VAR:
			case DT_LVAR:
			case DT_VAR_ARRAY:
			case DT_LVAR_ARRAY:
				*thread >> arg->dwParam;
				break;
			case DT_VAR_STRING:
			case DT_LVAR_STRING:
			case DT_VAR_TEXTLABEL:
			case DT_LVAR_TEXTLABEL:
				arg->pParam = GetScriptParamPointer(thread);
				break;
			case DT_VARLEN_STRING:
			case DT_TEXTLABEL:
				arg->pcParam = readString(thread, textParams[currTextParam++], MAX_STR_LEN);
			}
		}

		_asm
		{
			lea ecx, arguments
			loop_0AA6 :
			cmp ecx, arguments_end
				jae loop_end_0AA6
				push[ecx]
				add ecx, 0x4
				jmp loop_0AA6
				loop_end_0AA6 :
			mov ecx, struc
				call func
				add esp, stackAlign
		}

		SkipUnusedParameters(thread);
		return OR_CONTINUE;
	}

	//0AA7=-1,call_function %1d% num_params %2h% pop %3h%
	OpcodeResult __stdcall opcode_0AA7(CRunningScript *thread)
	{
		static char textParams[5][MAX_STR_LEN];
		static SCRIPT_VAR arguments[50] = { 0 };
		DWORD currTextParam = 0;
		void(*func)();
		DWORD numParams;
		DWORD stackAlign;
		*thread >> func >> numParams >> stackAlign;
		if (numParams > (sizeof(arguments) / sizeof(SCRIPT_VAR))) numParams = sizeof(arguments) / sizeof(SCRIPT_VAR);
		stackAlign *= 4;
		SCRIPT_VAR	*	arguments_end = arguments + numParams;
		// retrieve parameters
		for (SCRIPT_VAR *arg = arguments; arg != arguments_end; ++arg)
		{
			switch (*thread->GetBytePointer())
			{
			case DT_FLOAT:
			case DT_DWORD:
			case DT_WORD:
			case DT_BYTE:
			case DT_VAR:
			case DT_LVAR:
			case DT_VAR_ARRAY:
			case DT_LVAR_ARRAY:
				*thread >> arg->dwParam;
				break;
			case DT_VAR_STRING:
			case DT_LVAR_STRING:
			case DT_VAR_TEXTLABEL:
			case DT_LVAR_TEXTLABEL:
				arg->pParam = GetScriptParamPointer(thread);
				break;
			case DT_VARLEN_STRING:
			case DT_TEXTLABEL:
				arg->pcParam = readString(thread, textParams[currTextParam++], MAX_STR_LEN);
				break;
			}
		}

		DWORD result;

		_asm
		{
			lea ecx, arguments
			loop_0AA7 :
			cmp ecx, arguments_end
				jae loop_end_0AA7
				push[ecx]
				add ecx, 0x4
				jmp loop_0AA7
				loop_end_0AA7 :
			call func
				mov result, eax
				add esp, stackAlign
		}

		*thread << result;
		SkipUnusedParameters(thread);
		return OR_CONTINUE;
	}

	//0AA8=-1,call_function_method %1d% struct %2d% num_params %3h% pop %4h%
	OpcodeResult __stdcall opcode_0AA8(CRunningScript *thread)
	{
		static char textParams[5][MAX_STR_LEN];
		static SCRIPT_VAR arguments[50] = { 0 };
		DWORD currTextParam = 0;
		void(*func)();
		void *struc;
		DWORD numParams;
		DWORD stackAlign;
		*thread >> func >> struc >> numParams >> stackAlign;
		if (numParams > (sizeof(arguments) / sizeof(SCRIPT_VAR))) numParams = sizeof(arguments) / sizeof(SCRIPT_VAR);
		stackAlign *= 4;
		SCRIPT_VAR	*arguments_end = arguments + numParams;

		// retrieve parameters
		for (SCRIPT_VAR *arg = arguments; arg != arguments_end; ++arg)
		{
			switch (*thread->GetBytePointer())
			{
			case DT_FLOAT:
			case DT_DWORD:
			case DT_WORD:
			case DT_BYTE:
			case DT_VAR:
			case DT_LVAR:
			case DT_VAR_ARRAY:
			case DT_LVAR_ARRAY:
				*thread >> arg->dwParam;
				break;
			case DT_VAR_STRING:
			case DT_LVAR_STRING:
			case DT_VAR_TEXTLABEL:
			case DT_LVAR_TEXTLABEL:
				arg->pParam = GetScriptParamPointer(thread);
				break;
			case DT_VARLEN_STRING:
			case DT_TEXTLABEL:
				arg->pcParam = readString(thread, textParams[currTextParam++], MAX_STR_LEN);
			}
		}

		DWORD result;

		_asm
		{
			lea ecx, arguments
			loop_0AA8 :
			cmp ecx, arguments_end
				jae loop_end_0AA8
				push[ecx]
				add ecx, 0x4
				jmp loop_0AA8
				loop_end_0AA8 :
			mov ecx, struc
				call func
				mov result, eax
				add esp, stackAlign
		}

		*thread << result;
		SkipUnusedParameters(thread);
		return OR_CONTINUE;
	}

	//0AA9=0,  is_game_version_original
	OpcodeResult __stdcall opcode_0AA9(CRunningScript *thread)
	{
		auto gv = GetInstance().VersionManager.GetGameVersion();
		auto cs = (CCustomScript*)thread;
		SetScriptCondResult(thread, gv == GV_US10 || (cs->IsCustom() && cs->GetCompatibility() <= CLEO_VER_4_MIN && gv == GV_EU10));
		return OR_CONTINUE;
	}
}
