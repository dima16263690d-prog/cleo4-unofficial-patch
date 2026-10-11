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
	//0AAC=2,  %2d% = load_audiostream %1d%  // IF and SET
	OpcodeResult __stdcall opcode_0AAC(CRunningScript *thread)
	{
		auto stream = GetInstance().SoundSystem.LoadStream(readString(thread));
		*thread << stream;
		SetScriptCondResult(thread, stream != nullptr);
		return OR_CONTINUE;
	}

	//0AAD=2,set_audiostream %1d% perform_action %2d%
	OpcodeResult __stdcall opcode_0AAD(CRunningScript *thread)
	{
		CAudioStream *stream;
		int action;
		*thread >> stream >> action;
		if (stream)
		{
			switch (action)
			{
			case 0: stream->LegacyStop(); break;
			case 1: stream->LegacyPlay(); break;
			case 2: stream->Pause();  break;
			case 3: stream->Resume(); break;
			default:
				TRACE("[0AAD] Unknown audiostream's action: %d", action);
			}
		}
		return OR_CONTINUE;
	}

	//0AAE=1,release_audiostream %1d%
	OpcodeResult __stdcall opcode_0AAE(CRunningScript *thread)
	{
		CAudioStream *stream;
		*thread >> stream;
		if (stream) GetInstance().SoundSystem.UnloadStream(stream);
		return OR_CONTINUE;
	}

	//0AAF=2,%2d% = get_audiostream_length %1d%
	OpcodeResult __stdcall opcode_0AAF(CRunningScript *thread)
	{
		CAudioStream *stream;
		*thread >> stream;
		*thread << (stream ? stream->GetLength() : -1);
		return OR_CONTINUE;
	}

	//0AB9=2,get_audiostream %1d% state_to %2d%
	OpcodeResult __stdcall opcode_0AB9(CRunningScript *thread)
	{
		CAudioStream *stream;
		*thread >> stream;
		*thread << (stream ? stream->GetState() : -1);
		return OR_CONTINUE;
	}

	//0ABB=2,%2d% = audiostream %1d% volume
	OpcodeResult __stdcall opcode_0ABB(CRunningScript *thread)
	{
		CAudioStream *stream;
		*thread >> stream;
		*thread << (stream ? stream->GetVolume() : 0.0f);
		return OR_CONTINUE;
	}

	//0ABC=2,set_audiostream %1d% volume %2d%
	OpcodeResult __stdcall opcode_0ABC(CRunningScript *thread)
	{
		CAudioStream *stream;
		float volume;
		*thread >> stream >> volume;
		if (stream) stream->SetVolume(volume);
		return OR_CONTINUE;
	}

	//0AC0=2,loop_audiostream %1d% flag %2d%
	OpcodeResult __stdcall opcode_0AC0(CRunningScript *thread)
	{
		CAudioStream *stream;
		DWORD loop;
		*thread >> stream >> loop;
		if (stream) stream->Loop(loop != false);
		return OR_CONTINUE;
	}

	//0AC1=2,%2d% = load_audiostream_with_3d_support %1d% //IF and SET
	OpcodeResult __stdcall opcode_0AC1(CRunningScript *thread)
	{
		auto stream = GetInstance().SoundSystem.LoadStream(readString(thread), true);
		*thread << stream;
		SetScriptCondResult(thread, stream != nullptr);
		return OR_CONTINUE;
	}

	//0AC2=4,set_3d_audiostream %1d% position %2d% %3d% %4d%
	OpcodeResult __stdcall opcode_0AC2(CRunningScript *thread)
	{
		CAudioStream *stream;
		CVector pos;
		*thread >> stream >> pos;
		if (stream) stream->Set3dPosition(pos);
		return OR_CONTINUE;
	}

	//0AC3=2,link_3d_audiostream %1d% to_object %2d%
	OpcodeResult __stdcall opcode_0AC3(CRunningScript *thread)
	{
		CAudioStream *stream;
		DWORD handle;
		*thread >> stream >> handle;
		if (stream) stream->Link(GetObjectPool().GetAtRef(handle));
		return OR_CONTINUE;
	}

	//0AC4=2,link_3d_audiostream %1d% to_actor %2d%
	OpcodeResult __stdcall opcode_0AC4(CRunningScript *thread)
	{
		CAudioStream *stream;
		DWORD handle;
		*thread >> stream >> handle;
		if (stream) stream->Link(GetPedPool().GetAtRef(handle));
		return OR_CONTINUE;
	}

	//0AC5=2,link_3d_audiostream %1d% to_vehicle %2d%
	OpcodeResult __stdcall opcode_0AC5(CRunningScript *thread)
	{
		CAudioStream *stream;
		DWORD handle;
		*thread >> stream >> handle;
		if (stream) stream->Link(GetVehiclePool().GetAtRef(handle));
		return OR_CONTINUE;
	}
}
