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
#include "CCustomOpcodeSystem.Internal.h"

namespace CLEO
{
	//0ACA=1,show_text_box %1d%
	OpcodeResult __stdcall opcode_0ACA(CRunningScript *thread)
	{
		PrintHelp(readString(thread));
		return OR_CONTINUE;
	}

	//0ACB=3,show_styled_text %1d% time %2d% style %3d%
	OpcodeResult __stdcall opcode_0ACB(CRunningScript *thread)
	{
		const char *text = readString(thread);
		DWORD	time,
			style;
		*thread >> time >> style;
		PrintBig(text, time, style);
		return OR_CONTINUE;
	}

	//0ACC=2,show_text_lowpriority %1d% time %2d%
	OpcodeResult __stdcall opcode_0ACC(CRunningScript *thread)
	{
		const char *text = readString(thread);
		DWORD time;
		*thread >> time;
		Print(text, time);
		return OR_CONTINUE;
	}

	//0ACD=2,show_text_highpriority %1d% time %2d%
	OpcodeResult __stdcall opcode_0ACD(CRunningScript *thread)
	{
		const char *text = readString(thread);
		DWORD time;
		*thread >> time;
		PrintNow(text, time);
		return OR_CONTINUE;
	}

	//0ACE=-1,show_formatted_text_box %1d%
	OpcodeResult __stdcall opcode_0ACE(CRunningScript *thread)
	{
		char fmt[MAX_STR_LEN];
		char text[MAX_STR_LEN];
		readString(thread, fmt, sizeof(fmt));
		format(thread, text, sizeof(text), fmt);
		PrintHelp(text);
		SkipUnusedParameters(thread);
		return OR_CONTINUE;
	}

	//0ACF=-1,show_formatted_styled_text %1d% time %2d% style %3d%
	OpcodeResult __stdcall opcode_0ACF(CRunningScript *thread)
	{
		char fmt[MAX_STR_LEN]; char text[MAX_STR_LEN];
		DWORD time, style;
		readString(thread, fmt, sizeof(fmt));
		*thread >> time >> style;
		format(thread, text, sizeof(text), fmt);
		PrintBig(text, time, style);
		SkipUnusedParameters(thread);
		return OR_CONTINUE;
	}

	//0AD0=-1,show_formatted_text_lowpriority %1d% time %2d%
	OpcodeResult __stdcall opcode_0AD0(CRunningScript *thread)
	{
		char fmt[MAX_STR_LEN]; char text[MAX_STR_LEN];
		DWORD time;
		readString(thread, fmt, sizeof(fmt));
		*thread >> time;
		format(thread, text, sizeof(text), fmt);
		Print(text, time);
		SkipUnusedParameters(thread);
		return OR_CONTINUE;
	}

	//0AD1=-1,show_formatted_text_highpriority %1d% time %2d%
	OpcodeResult __stdcall opcode_0AD1(CRunningScript *thread)
	{
		char fmt[MAX_STR_LEN]; char text[MAX_STR_LEN];
		DWORD time;
		readString(thread, fmt, sizeof(fmt));
		*thread >> time;
		format(thread, text, sizeof(text), fmt);
		PrintNow(text, time);
		SkipUnusedParameters(thread);
		return OR_CONTINUE;
	}

	//0AD3=-1,string %1d% format %2d% ...
	OpcodeResult __stdcall opcode_0AD3(CRunningScript *thread)
	{
		char fmt[MAX_STR_LEN], *dst;

		if (*thread->GetBytePointer() >= 1 && *thread->GetBytePointer() <= 8) *thread >> dst;
		else dst = &GetScriptParamPointer(thread)->cParam;

		readString(thread, fmt, sizeof(fmt));
		format(thread, dst, -1, fmt);
		SkipUnusedParameters(thread);
		return OR_CONTINUE;
	}

	//0AD4=-1,%3d% = scan_string %1d% format %2d%  //IF and SET
	OpcodeResult __stdcall opcode_0AD4(CRunningScript *thread)
	{
		char fmt[MAX_STR_LEN], *format, *src;
		src = readString(thread);
		format = readString(thread, fmt, sizeof(fmt));

		size_t cExParams = 0;
		int *result = (int *)GetScriptParamPointer(thread);
		SCRIPT_VAR *ExParams[35];

		// read extra params
		for (int i = 0; i < 35; i++)
		{
			if (*thread->GetBytePointer())
			{
				ExParams[i] = GetScriptParamPointer(thread);
				cExParams++;
			}
			else ExParams[i] = nullptr;
		}
		thread->IncPtr();
		*result = sscanf(src, format,
						 /* extra parameters (will be aligned automatically, but the limit of 35 elements maximum exists) */
						 ExParams[0], ExParams[1], ExParams[2], ExParams[3], ExParams[4], ExParams[5],
						 ExParams[6], ExParams[7], ExParams[8], ExParams[9], ExParams[10], ExParams[11],
						 ExParams[12], ExParams[13], ExParams[14], ExParams[15], ExParams[16], ExParams[17],
						 ExParams[18], ExParams[19], ExParams[20], ExParams[21], ExParams[22], ExParams[23],
						 ExParams[24], ExParams[25], ExParams[26], ExParams[27], ExParams[28], ExParams[29],
						 ExParams[30], ExParams[31], ExParams[32], ExParams[33], ExParams[34]);

		SetScriptCondResult(thread, cExParams == *result);
		return OR_CONTINUE;
	}

	//0ADE=2,%2d% = text_by_GXT_entry %1d%
	OpcodeResult __stdcall opcode_0ADE(CRunningScript *thread)
	{
		const char *gxt = readString(thread);
		if (*thread->GetBytePointer() >= 1 && *thread->GetBytePointer() <= 8)
			*thread << GetInstance().TextManager.Get(gxt);
		else
			strcpy((char *)GetScriptParamPointer(thread), GetInstance().TextManager.Get(gxt));
		return OR_CONTINUE;
	}

	//0ADF=2,add_dynamic_GXT_entry %1d% text %2d%
	OpcodeResult __stdcall opcode_0ADF(CRunningScript *thread)
	{
		char gxtLabel[8]; // 7 + terminator character
		readString(thread, gxtLabel, sizeof(gxtLabel));

		char *text = readString(thread);

		GetInstance().TextManager.AddFxt(gxtLabel, text, true, thread);
		return OR_CONTINUE;
	}

	//0AE0=1,remove_dynamic_GXT_entry %1d%
	OpcodeResult __stdcall opcode_0AE0(CRunningScript *thread)
	{
		GetInstance().TextManager.RemoveFxt(readString(thread));
		return OR_CONTINUE;
	}

	//0AED=3,%3d% = float %1d% to_string_format %2d%
	OpcodeResult __stdcall opcode_0AED(CRunningScript *thread)
	{
		// this opcode is useless now
		float val;
		char *format, *result;
		*thread >> val;
		format = readString(thread);
		if (*thread->GetBytePointer() >= 1 && *thread->GetBytePointer() <= 8)
			*thread >> result;
		else
			result = &GetScriptParamPointer(thread)->cParam;
		if (!result)
			return OR_CONTINUE;

		if (!format)
		{
			result[0] = '\0';
			return OR_CONTINUE;
		}

		_snprintf_s(result, 16, _TRUNCATE, format, (double)val);
		result[15] = '\0';
		return OR_CONTINUE;
	}
}
