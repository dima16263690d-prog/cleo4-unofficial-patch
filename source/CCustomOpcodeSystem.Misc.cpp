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
#include "CFastOpcodeExecutor.h"
#include "CFastScriptExecutor.h"
#include "CCustomOpcodeSystem.Internal.h"

namespace CLEO
{
	//0AB0=1,  key_pressed %1d%
	OpcodeResult __stdcall opcode_0AB0(CRunningScript *thread)
	{
		DWORD key;
		*thread >> key;
		SHORT state = GetKeyState(key);
		SetScriptCondResult(thread, (GetKeyState(key) & 0x8000) != 0);
		return OR_CONTINUE;
	}

	//0AE9=0,pop_float
	OpcodeResult __stdcall opcode_0AE9(CRunningScript *thread)
	{
		float result;
		_asm fstp result
		opcodeParams[0].fParam = result;
		SetScriptParams(thread, 1);
		return OR_CONTINUE;
	}
}
