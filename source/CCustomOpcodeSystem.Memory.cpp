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
	//0A8C=4,write_memory %1d% size %2d% value %3d% virtual_protect %4d%
	OpcodeResult __stdcall opcode_0A8C(CRunningScript *thread)
	{
		GetScriptParams(thread, 4);
		void *Address = opcodeParams[0].pParam;
		DWORD size = opcodeParams[1].dwParam;
		DWORD value = opcodeParams[2].dwParam;
		bool vp = opcodeParams[3].bParam;
		switch (size)
		{
		default:
			GetInstance().CodeInjector.MemoryWrite<BYTE>(Address, static_cast<BYTE>(value), vp, size);
			break;
		case 2:
			GetInstance().CodeInjector.MemoryWrite<WORD>(Address, static_cast<WORD>(value), vp);
			break;
		case 4:
			GetInstance().CodeInjector.MemoryWrite<DWORD>(Address, value, vp);
			break;
		}
		return OR_CONTINUE;
	}

	//0A8D=4,%4d% = read_memory %1d% size %2d% virtual_protect %3d%
	OpcodeResult __stdcall opcode_0A8D(CRunningScript *thread)
	{
		GetScriptParams(thread, 3);
		//DWORD value;
		void *Address = opcodeParams[0].pParam;
		DWORD size = opcodeParams[1].dwParam;
		bool vp = opcodeParams[2].bParam;

		opcodeParams[0].dwParam = 0;

		switch (size)
		{
		case 1:
			GetInstance().CodeInjector.MemoryRead(Address, opcodeParams[0].ucParam, vp);
			break;
		case 2:
			GetInstance().CodeInjector.MemoryRead(Address, opcodeParams[0].usParam, vp);
			break;
		case 4:
			GetInstance().CodeInjector.MemoryRead(Address, opcodeParams[0].dwParam, vp);
			break;
		default:
			TRACE("[0A8D] Unallowed size %u", size);
		}

		SetScriptParams(thread, 1);
		return OR_CONTINUE;
	}

	//0A8E=3,%3d% = %1d% + %2d% ; int
	OpcodeResult __stdcall opcode_0A8E(CRunningScript *thread)
	{
		GetScriptParams(thread, 2);
		opcodeParams[0].nParam += opcodeParams[1].nParam;
		SetScriptParams(thread, 1);
		return OR_CONTINUE;
	}

	//0A8F=3,%3d% = %1d% - %2d% ; int
	OpcodeResult __stdcall opcode_0A8F(CRunningScript *thread)
	{
		GetScriptParams(thread, 2);
		opcodeParams[0].nParam -= opcodeParams[1].nParam;
		SetScriptParams(thread, 1);
		return OR_CONTINUE;
	}

	//0A90=3,%3d% = %1d% * %2d% ; int
	OpcodeResult __stdcall opcode_0A90(CRunningScript *thread)
	{
		GetScriptParams(thread, 2);
		opcodeParams[0].nParam *= opcodeParams[1].nParam;
		SetScriptParams(thread, 1);
		return OR_CONTINUE;
	}

	//0A91=3,%3d% = %1d% / %2d% ; int
	OpcodeResult __stdcall opcode_0A91(CRunningScript *thread)
	{
		GetScriptParams(thread, 2);
		opcodeParams[0].nParam /= opcodeParams[1].nParam;
		SetScriptParams(thread, 1);
		return OR_CONTINUE;
	}

	//0AC8=2,%2d% = allocate_memory_size %1d%
	OpcodeResult __stdcall opcode_0AC8(CRunningScript *thread)
	{
		DWORD size;
		*thread >> size;

		CCustomScript *cs = reinterpret_cast<CCustomScript *>(thread);
		void *mem = GetSmartMemoryEngine().Memory().Allocate(cs, static_cast<size_t>(size));

		*thread << mem;
		SetScriptCondResult(thread, mem != nullptr);
		return OR_CONTINUE;
	}

	//0AC9=1,free_allocated_memory %1d%
	OpcodeResult __stdcall opcode_0AC9(CRunningScript *thread)
	{
		void *mem;
		*thread >> mem;

		GetSmartMemoryEngine().Memory().Free(thread, mem);
		return OR_CONTINUE;
	}

	//0AEE=3,%3d% = %1d% exp %2d% //all floats
	OpcodeResult __stdcall opcode_0AEE(CRunningScript *thread)
	{
		float base, arg;
		*thread >> base >> arg;
		*thread << (float)pow(base, arg);
		return OR_CONTINUE;
	}

	//0AEF=3,%3d% = log %1d% base %2d% //all floats
	OpcodeResult __stdcall opcode_0AEF(CRunningScript *thread)
	{
		float base, arg;
		*thread >> arg >> base;
		*thread << (float)(log(arg) / log(base));
		return OR_CONTINUE;
	}
}
