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
	//0A92=-1,create_custom_thread %1d%
	OpcodeResult __stdcall opcode_0A92(CRunningScript *thread)
	{
		const char *script_name = readString(thread);
		TRACE("[0A92] Starting new custom script %s from thread named %s", script_name, thread->GetName());
		char cwd[MAX_PATH];
		_getcwd(cwd, sizeof(cwd));
		_chdir(cleo_dir);
		auto cs = new CCustomScript(script_name);
		SetScriptCondResult(thread, cs && cs->IsOK());
		if (cs && cs->IsOK())
		{
			GetInstance().ScriptEngine.AddCustomScript(cs);
			TransmitScriptParams(thread, cs);
		}
		else
		{
			if (cs) delete cs;
			SkipUnusedParameters(thread);
			TRACE("[0A92] Failed to load script '%s' from script '%s'.", script_name, thread->GetName());
		}
		_chdir(cwd);
		return OR_CONTINUE;
	}

	//0A93=0,end_custom_thread
	OpcodeResult __stdcall opcode_0A93(CRunningScript *thread)
	{
		CCustomScript *cs = reinterpret_cast<CCustomScript *>(thread);
		if (thread->IsMission() || !cs->IsCustom())
		{
			TRACE("[0A93] Incorrect usage of opcode in script '%s'", thread->GetName());
			return OR_CONTINUE;
		}
		GetInstance().ScriptEngine.RemoveCustomScript(cs);
		return OR_INTERRUPT;
	}

	//0A94=-1,create_custom_mission %1d%
	OpcodeResult __stdcall opcode_0A94(CRunningScript *thread)
	{
		char script_name[MAX_PATH];
		readString(thread, script_name);
		strcat(script_name, ".cm");		// add custom mission extension
		TRACE("[0A94] Starting new custom mission %s from thread named %s", script_name, thread->GetName());
		char cwd[MAX_PATH];
		_getcwd(cwd, sizeof(cwd));
		_chdir(cleo_dir);
		auto cs = new CCustomScript(script_name, true);
		SetScriptCondResult(thread, cs && cs->IsOK());
		if (cs && cs->IsOK())
		{
			auto csscript = reinterpret_cast<CCustomScript*>(thread);
			if (csscript->IsCustom())
				cs->SetCompatibility(csscript->GetCompatibility());
			GetInstance().ScriptEngine.AddCustomScript(cs);
			TransmitScriptParams(thread, (CRunningScript*)((BYTE*)missionLocals - 0x3C));
		}
		else
		{
			if (cs) delete cs;
			SkipUnusedParameters(thread);
			TRACE("[0A94] Failed to load mission '%s' from script '%s'.", script_name, thread->GetName());
		}
		_chdir(cwd);
		return OR_CONTINUE;
	}

	//0A95=0,enable_thread_saving
	OpcodeResult __stdcall opcode_0A95(CRunningScript *thread)
	{
		reinterpret_cast<CCustomScript *>(thread)->enable_saving();
		return OR_CONTINUE;
	}

	//0A96=2,%2d% = actor %1d% struct
	OpcodeResult __stdcall opcode_0A96(CRunningScript *thread)
	{
		DWORD handle;
		*thread >> handle;
		*thread << GetPedPool().GetAtRef(handle);
		return OR_CONTINUE;
	}

	//0A97=2,%2d% = car %1d% struct
	OpcodeResult __stdcall opcode_0A97(CRunningScript *thread)
	{
		DWORD handle;
		*thread >> handle;
		*thread << GetVehiclePool().GetAtRef(handle);
		return OR_CONTINUE;
	}

	//0A98=2,%2d% = object %1d% struct
	OpcodeResult __stdcall opcode_0A98(CRunningScript *thread)
	{
		DWORD handle;
		*thread >> handle;
		*thread << GetObjectPool().GetAtRef(handle);
		return OR_CONTINUE;
	}

	//0A99=1,chdir %1b:userdir/rootdir%
	OpcodeResult __stdcall opcode_0A99(CRunningScript *thread)
	{
		auto paramType = *thread->GetBytePointer();
		if (paramType >= 1 && paramType <= 8)
		{
			// integer param
			DWORD param;
			*thread >> param;
			//_chdir(param ? GetUserDirectory() : "");
			if (param) ChangeToUserDir();
			else ChangeToProgramDir("");
		}
		else
		{
			// string param
			char buf[MAX_PATH];
			std::fill(buf, buf + sizeof(buf), '\0');
			GetScriptStringParam(thread, buf, (BYTE)sizeof(buf));
			_chdir(buf);
		}
		return OR_CONTINUE;
	}

	//0A9F=1,%1d% = current_thread_pointer
	OpcodeResult __stdcall opcode_0A9F(CRunningScript *thread)
	{
		*thread << thread;
		return OR_CONTINUE;
	}

	//0AA0=1,gosub_if_false %1p%
	OpcodeResult __stdcall opcode_0AA0(CRunningScript *thread)
	{
		int off;
		*thread >> off;
		if (thread->GetConditionResult()) return OR_CONTINUE;
		thread->PushStack(thread->GetBytePointer());
		ThreadJump(thread, off);
		return OR_CONTINUE;
	}

	//0AA1=0,return_if_false
	OpcodeResult __stdcall opcode_0AA1(CRunningScript *thread)
	{
		if (thread->GetConditionResult()) return OR_CONTINUE;
		thread->SetIp(thread->PopStack());
		return OR_CONTINUE;
	}

	//0AAA=2,  %2d% = thread %1d% pointer  // IF and SET
	OpcodeResult __stdcall opcode_0AAA(CRunningScript *thread)
	{
		char *threadName = readString(thread);
		threadName[7] = '\0';
		CRunningScript *cs = GetInstance().ScriptEngine.FindCustomScriptNamed(threadName);
		if (!cs) cs = GetInstance().ScriptEngine.FindScriptNamed(threadName);
		*thread << cs;
		SetScriptCondResult(thread, cs != nullptr);
		return OR_CONTINUE;
	}

	inline bool IsScmStringType(BYTE type)
	{
		return type == DT_STRING || type == DT_TEXTLABEL || type == DT_VARLEN_STRING ||
			type == DT_VAR_STRING || type == DT_LVAR_STRING ||
			type == DT_VAR_STRING_ARRAY || type == DT_LVAR_STRING_ARRAY ||
			type == DT_VAR_TEXTLABEL || type == DT_LVAR_TEXTLABEL ||
			type == DT_VAR_TEXTLABEL_ARRAY || type == DT_LVAR_TEXTLABEL_ARRAY;
	}

	inline bool IsScmStringDestinationType(BYTE type)
	{
		return type == DT_VAR_STRING || type == DT_LVAR_STRING ||
			type == DT_VAR_STRING_ARRAY || type == DT_LVAR_STRING_ARRAY;
	}

	inline bool SkipOneScmParam(CRunningScript *thread)
	{
		switch (thread->ReadDataType())
		{
		case DT_VAR:
		case DT_LVAR:
		case DT_VAR_STRING:
		case DT_LVAR_STRING:
		case DT_VAR_TEXTLABEL:
		case DT_LVAR_TEXTLABEL:
			thread->IncPtr(2);
			return true;
		case DT_VAR_ARRAY:
		case DT_LVAR_ARRAY:
		case DT_VAR_STRING_ARRAY:
		case DT_LVAR_STRING_ARRAY:
		case DT_VAR_TEXTLABEL_ARRAY:
		case DT_LVAR_TEXTLABEL_ARRAY:
			thread->IncPtr(6);
			return true;
		case DT_BYTE:
			thread->IncPtr();
			return true;
		case DT_WORD:
			thread->IncPtr(2);
			return true;
		case DT_DWORD:
		case DT_FLOAT:
			thread->IncPtr(4);
			return true;
		case DT_VARLEN_STRING:
		{
			const BYTE length = thread->ReadDataByte();
			thread->IncPtr(length);
			return true;
		}
		case DT_TEXTLABEL:
			thread->IncPtr(8);
			return true;
		case DT_STRING:
			thread->IncPtr(16);
			return true;
		default:
			return false;
		}
	}

	inline DWORD CountScmVarArgs(CRunningScript *thread)
	{
		BYTE *savedIp = thread->GetBytePointer();
		DWORD count = 0;
		while (*thread->GetBytePointer() != DT_END)
		{
			if (!SkipOneScmParam(thread))
				break;
			++count;
		}
		thread->SetIp(savedIp);
		return count;
	}

	struct ScmReturnValue
	{
		SCRIPT_VAR value;
		bool isString;
		std::string stringValue;

		ScmReturnValue() : value(), isString(false) {}
	};

	inline ScmFunction *GetActiveScmFunction(CCustomScript *cs)
	{
		if (!cs)
			return nullptr;

		const WORD id = cs->GetScmFunction();
		if (id >= ScmFunction::store_size)
			return nullptr;

		return ScmFunction::Store[id];
	}


	//0AB1=-1,cleo_call %1p%
	OpcodeResult __stdcall opcode_0AB1(CRunningScript *thread)
	{
		int label;
		DWORD nParams;

		*thread >> label >> nParams;
		auto debugCs = reinterpret_cast<CCustomScript *>(thread);
		

		if (nParams > 32)
		{
			DIAG("[CLEO][ERROR][0AB1] Argument count %u exceeds supported limit of 32 in script '%.8s'; call skipped",
				nParams, thread->GetName());

			// Do not throw a C++ exception into the game. On GTA SA 1.0 US
			// an uncaught MSVC exception becomes 0xE06D7363 in KERNELBASE.dll.
			// Consume the complete vararg list and continue after the opcode.
		SkipUnusedParameters(thread);
			return OR_CONTINUE;
		}

		// Make sure the declared input list actually exists before changing
		// the current function scope. The remaining parameters belong to the
		// caller's return slots.
		if (CountScmVarArgs(thread) < nParams)
			throw "Not enough parameters in opcode 0AB1";

		ScmFunction *scmFunc = new ScmFunction(thread);
		scmFunc->callArgCount = static_cast<BYTE>(nParams);

		SCRIPT_VAR arguments[32] = {};
		SCRIPT_VAR* locals = thread->IsMission() ? missionLocals : thread->GetVarPtr();
		SCRIPT_VAR* localsEnd = locals + 32;
		SCRIPT_VAR* storedLocals = scmFunc->savedTls;

		for (DWORD i = 0; i < nParams; i++)
		{
			SCRIPT_VAR* arg = arguments + i;

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
				if (arg->pParam >= locals && arg->pParam < localsEnd)
				{
					arg->dwParam -= (DWORD)locals;
					arg->dwParam += (DWORD)storedLocals;
				}
				break;

			case DT_STRING:
			case DT_TEXTLABEL:
			case DT_VARLEN_STRING:
				scmFunc->stringParams.emplace_back(readString(thread));
				arg->pcParam = (char*)scmFunc->stringParams.back().c_str();
				break;

			default:
				delete scmFunc;
				throw "Invalid parameter type in opcode 0AB1";
			}
		}

		// Return execution to the caller's return-slot list. The child custom
		// stream itself is unchanged: BaseIP, parentThread, childThreads and
		// save metadata remain owned by CCustomScript/CScriptEngine.
		scmFunc->retnAddress = thread->GetBytePointer();
		SCRIPT_TRACE("[function][0AB1] %.8s args=%u %d %d",
			thread->GetName(), nParams,
			nParams > 0 ? arguments[0].nParam : 0, nParams > 1 ? arguments[1].nParam : 0);

		memcpy(locals, arguments, nParams * sizeof(SCRIPT_VAR));

		auto cs = reinterpret_cast<CCustomScript*>(thread);
		if (cs->IsCustom() && cs->GetCompatibility() >= CLEO_VER_4_MIN)
		{
			for (DWORD i = nParams; i < 32; i++)
				cs->SetIntVar(i, 0);
		}

		ThreadJump(thread, label);
		
		return OR_CONTINUE;
	}

	//0AB2=-1,cleo_return
	OpcodeResult __stdcall opcode_0AB2(CRunningScript *thread)
	{
		auto cs = reinterpret_cast<CCustomScript *>(thread);
		ScmFunction *scmFunc = GetActiveScmFunction(cs);
		

		if (!scmFunc)
		{
			TRACE("[0AB2] No active 0AB1 function for thread %.8s ptr=%p; skipping malformed return",
				thread->GetName(), thread);
			SkipUnusedParameters(thread);
			return OR_CONTINUE;
		}

		// CLEO 5-style validation: inspect the complete return vararg list first.
		DWORD returnVarArgCount = CountScmVarArgs(thread);
		DWORD nRetParams = 0;

		if (returnVarArgCount)
		{
			*thread >> nRetParams;

			if (returnVarArgCount - 1 < nRetParams)
			{
				TRACE("[0AB2] Declared %u return args, but only %u were provided in %.8s; skipping return",
					nRetParams, returnVarArgCount - 1, thread->GetName());
				SkipUnusedParameters(thread);
				return OR_CONTINUE;
			}
		}

		if (nRetParams > 32)
		{
			TRACE("[0AB2] Return argument count %u exceeds supported limit of 32 in %.8s; skipping return",
				nRetParams, thread->GetName());
			SkipUnusedParameters(thread);
			return OR_CONTINUE;
		}

		// Keep return values in a private buffer while function-local scope is active.
		// The actual write-back uses GTA SA's native SetScriptParams() after Return(),
		// which is the safe parameter API used by our CLEO 4 runtime.
		SCRIPT_VAR returnValues[32] = {};

		if (nRetParams)
		{
			GetScriptParams(thread, nRetParams);
			memcpy(returnValues, opcodeParams, nRetParams * sizeof(SCRIPT_VAR));
		}

		SCRIPT_TRACE("[function][0AB2] %.8s ret=%u %d",
			thread->GetName(), nRetParams, nRetParams ? returnValues[0].nParam : 0);

		// Restore caller scope and jump to its return-slot list.
		scmFunc->Return(thread);
		
		delete scmFunc;

		// Write results through the game's own parameter writer.
		if (nRetParams)
		{
			memcpy(opcodeParams, returnValues, nRetParams * sizeof(SCRIPT_VAR));
			SetScriptParams(thread, nRetParams);
		}

		SkipUnusedParameters(thread);
		return OR_CONTINUE;
	}

	//0AB3=2,var %1d% = %2d%
	OpcodeResult __stdcall opcode_0AB3(CRunningScript *thread)
	{
		DWORD	varId,
			value;
		*thread >> varId >> value;
		// Reject out-of-range shared-variable writes instead of corrupting memory.
		if (varId >= _countof(GetInstance().ScriptEngine.CleoVariables))
			return OR_CONTINUE;
		GetInstance().ScriptEngine.CleoVariables[varId].dwParam = value;
		return OR_CONTINUE;
	}

	//0AB4=2,%2d% = var %1d%
	OpcodeResult __stdcall opcode_0AB4(CRunningScript *thread)
	{
		DWORD varId;
		*thread >> varId;
		// Keep reads in bounds; unsupported indices evaluate to zero.
		if (varId >= _countof(GetInstance().ScriptEngine.CleoVariables))
			*thread << (DWORD)0;
		else
			*thread << GetInstance().ScriptEngine.CleoVariables[varId].dwParam;
		return OR_CONTINUE;
	}

	//0ABA=1,end_custom_thread_named %1d%
	OpcodeResult __stdcall opcode_0ABA(CRunningScript *thread)
	{
		char *threadName = readString(thread);
		CCustomScript *deleted_thread = nullptr;

		// With multiple custom streams sharing one name (parent + 0E6F children),
		// prefer terminating the current custom thread when its name matches.
		// The legacy fallback remains for explicitly targeting another name.
		CCustomScript *current_custom_thread = reinterpret_cast<CCustomScript *>(thread);
		if (current_custom_thread->IsCustom() && _stricmp(current_custom_thread->GetName(), threadName) == 0)
		{
			deleted_thread = current_custom_thread;
		}
		else
		{
			deleted_thread = GetInstance().ScriptEngine.FindCustomScriptNamed(threadName);
		}

		if (deleted_thread)
		{
			GetInstance().ScriptEngine.RemoveCustomScript(deleted_thread);
		}
		return deleted_thread == thread ? OR_INTERRUPT : OR_CONTINUE;
	}

	//0AC6=2,%2d% = label %1p% offset
	OpcodeResult __stdcall opcode_0AC6(CRunningScript *thread)
	{
		int label;
		*thread >> label;
		*thread << (label < 0 ? thread->GetBasePointer() - label : scmBlock + label);
		return OR_CONTINUE;
	}

	//0AC7=2,%2d% = var %1d% offset
	OpcodeResult __stdcall opcode_0AC7(CRunningScript *thread)
	{
		*thread << GetScriptParamPointer(thread);
		return OR_CONTINUE;
	}
}
