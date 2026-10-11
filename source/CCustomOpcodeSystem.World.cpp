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
	//0AB5=3,store_actor %1d% closest_vehicle_to %2d% closest_ped_to %3d%
	OpcodeResult __stdcall opcode_0AB5(CRunningScript *thread)
	{
		DWORD actor;
		*thread >> actor;
		auto pPlayerPed = GetPedPool().GetAtRef(actor);
		CPedIntelligence * pedintel;
		if (pPlayerPed && (pedintel = pPlayerPed->m_pIntelligence))
		{
			CVehicle * pVehicle = nullptr;
			for (int i = 0; i < NUM_SCAN_ENTITIES; i++)
			{
				pVehicle = (CVehicle*)pedintel->m_vehicleScanner.m_apEntities[i];
				if (pVehicle && pVehicle->m_nCreatedBy != 2 && !pVehicle->bFadeOut)
					break;
				pVehicle = nullptr;
			}

			CPed * pPed = nullptr;
			for (int i = 0; i < NUM_SCAN_ENTITIES; i++)
			{
				pPed = (CPed*)pedintel->m_pedScanner.m_apEntities[i];
				if (pPed && pPed != pPlayerPed && (pPed->m_nCreatedBy & 0xFF) == 1 && !pPed->bFadeOut)
					break;
				pPed = nullptr;
			}

			*thread << (pVehicle ? GetVehiclePool().GetRef(pVehicle) : -1) << (pPed ? GetPedPool().GetRef(pPed) : -1);
		}
		else *thread << -1 << -1;
		return OR_CONTINUE;
	}

	//0AB6=3,store_target_marker_coords_to %1d% %2d% %3d% // IF and SET
	OpcodeResult __stdcall opcode_0AB6(CRunningScript *thread)
	{
		// steam offset is different, so get it manually for now
		CGameVersionManager& gvm = GetInstance().VersionManager;
		DWORD hMarker = gvm.GetGameVersion() != GV_STEAM ? MenuManager->m_nTargetBlipIndex : *((DWORD*)0xC3312C);
		CMarker *pMarker;
		if (hMarker && (pMarker = &RadarBlips[LOWORD(hMarker)]) && /*pMarker->m_nPoolIndex == HIWORD(hMarker) && */pMarker->m_nBlipDisplay)
		{
			CVector coords(pMarker->m_vecPos);
			coords.z = FindGroundZ(coords.x, coords.y);
			*thread << coords;
			SetScriptCondResult(thread, true);
		}
		else
		{
			GetScriptParams(thread, 3);
			SetScriptCondResult(thread, false);
		}

		return OR_CONTINUE;
	}

	//0AB7=2,get_vehicle %1d% number_of_gears_to %2d%
	OpcodeResult __stdcall opcode_0AB7(CRunningScript *thread)
	{
		DWORD hVehicle;
		*thread >> hVehicle;
		*thread << GetVehiclePool().GetAtRef(hVehicle)->m_pHandlingData->m_transmissionData.m_nNumberOfGears;
		return OR_CONTINUE;
	}

	//0AB8=2,get_vehicle %1d% current_gear_to %2d%
	OpcodeResult __stdcall opcode_0AB8(CRunningScript *thread)
	{
		DWORD hVehicle;
		*thread >> hVehicle;
		*thread << GetVehiclePool().GetAtRef(hVehicle)->m_nCurrentGear;
		return OR_CONTINUE;
	}

	//0ABD=1,  vehicle %1d% siren_on
	OpcodeResult __stdcall opcode_0ABD(CRunningScript *thread)
	{
		DWORD hVehicle;
		*thread >> hVehicle;
		SetScriptCondResult(thread, GetVehiclePool().GetAtRef(hVehicle)->bSirenOrAlarm);
		return OR_CONTINUE;
	}

	//0ABE=1,  vehicle %1d% engine_on
	OpcodeResult __stdcall opcode_0ABE(CRunningScript *thread)
	{
		DWORD hVehicle;
		*thread >> hVehicle;
		SetScriptCondResult(thread, GetVehiclePool().GetAtRef(hVehicle)->bEngineOn);
		return OR_CONTINUE;
	}

	//0ABF=2,set_vehicle %1d% engine_state_to %2d%
	OpcodeResult __stdcall opcode_0ABF(CRunningScript *thread)
	{
		DWORD	hVehicle,
			state;
		*thread >> hVehicle >> state;
		auto veh = GetVehiclePool().GetAtRef(hVehicle);
		veh->bEngineOn = state != false;
		return OR_CONTINUE;
	}

	//0AD2=2,  %2d% = player %1d% targeted_actor //IF and SET
	OpcodeResult __stdcall opcode_0AD2(CRunningScript *thread)
	{
		DWORD playerId;
		*thread >> playerId;
		auto pPlayerPed = GetPlayerPed(playerId);
		auto pTargetEntity = GetWeaponTarget(pPlayerPed);
		if (!pTargetEntity) pTargetEntity = (CEntity*)pPlayerPed->m_pPlayerTargettedPed;
		if (pTargetEntity && pTargetEntity->m_nType == ENTITY_TYPE_PED)
		{
			*thread << GetPedPool().GetRef(reinterpret_cast<CPed*>(pTargetEntity));
			SetScriptCondResult(thread, true);
		}
		else
		{
			*thread << -1;
			SetScriptCondResult(thread, false);
		}
		return OR_CONTINUE;
	}

	//0ADB=2,%2d% = car_model %1o% name
	OpcodeResult __stdcall opcode_0ADB(CRunningScript *thread)
	{
		DWORD mi;
		char *buf;
		*thread >> mi;

		CVehicleModelInfo* model;
		// if 1.0 US, prefer GetModelInfo function  makes it compatible with fastman92's limit adjuster
		if (CLEO::GetInstance().VersionManager.GetGameVersion() == CLEO::GV_US10) {
			model = plugin::CallAndReturn<CVehicleModelInfo *, 0x403DA0, int>(mi);
		}
		else {
			model = reinterpret_cast<CVehicleModelInfo*>(Models[mi]);
		}
		if (*thread->GetBytePointer() >= 1 && *thread->GetBytePointer() <= 8) *thread >> buf;
		else buf = (char *)GetScriptParamPointer(thread);
		memcpy(buf, model->m_szGameName, 8);
		return OR_CONTINUE;
	}

	//0ADC=1, test_cheat %1d%
	OpcodeResult __stdcall opcode_0ADC(CRunningScript *thread)
	{
		SetScriptCondResult(thread, TestCheat(readString(thread)));
		return OR_CONTINUE;
	}

	//0ADD=1,spawn_car_with_model %1o% at_player_location 
	OpcodeResult __stdcall opcode_0ADD(CRunningScript *thread)
	{
		DWORD mi;
		*thread >> mi;

		CVehicleModelInfo* model;
		// if 1.0 US, prefer GetModelInfo function  makes it compatible with fastman92's limit adjuster
		if (CLEO::GetInstance().VersionManager.GetGameVersion() == CLEO::GV_US10) {
			model = plugin::CallAndReturn<CVehicleModelInfo *, 0x403DA0, int>(mi);
		}
		else {
			model = reinterpret_cast<CVehicleModelInfo*>(Models[mi]);
		}
		if (model->m_nVehicleType != VEHICLE_TYPE_TRAIN && model->m_nVehicleType != VEHICLE_TYPE_UNKNOWN) SpawnCar(mi);
		return OR_CONTINUE;
	}

	//0AE1=7,%7d% = find_actor_near_point %1d% %2d% %3d% in_radius %4d% find_next %5h% pass_deads %6h% //IF and SET
	OpcodeResult __stdcall opcode_0AE1(CRunningScript *thread)
	{
		CVector center;
		float radius;
		DWORD next, pass_deads;
		static DWORD stat_last_found = 0;
		auto& pool = GetPedPool();
		*thread >> center >> radius >> next >> pass_deads;

		DWORD& last_found = reinterpret_cast<CCustomScript *>(thread)->IsCustom() ?
			reinterpret_cast<CCustomScript *>(thread)->GetLastSearchPed() :
			stat_last_found;

		if (!next) last_found = 0;

		for (int index = last_found; index < pool.m_nSize; ++index)
		{
			if (auto obj = pool.GetAt(index))
			{
				if (pass_deads != -1 && (obj->IsPlayer() || (pass_deads && !IsAvailable(obj))/* || obj->GetOwner() == 2*/ || obj->bFadeOut))
					continue;

				if (radius >= 1000.0f || (VectorSqrMagnitude(obj->GetPosition() - center) <= radius * radius))
				{
					last_found = index + 1;	// on next opcode call start search from next index
											//if(last_found >= (unsigned)pool.GetSize()) last_found = 0;
											//obj->PedCreatedBy = 2; // add reference to found actor

					*thread << pool.GetRef(obj);
					SetScriptCondResult(thread, true);
					return OR_CONTINUE;
				}
			}
		}

		*thread << -1;
		last_found = 0;
		SetScriptCondResult(thread, false);
		return OR_CONTINUE;
	}

	//0AE2=7,%7d% = find_vehicle_near_point %1d% %2d% %3d% in_radius %4d% find_next %5h% pass_wrecked %6h% //IF and SET
	OpcodeResult __stdcall opcode_0AE2(CRunningScript *thread)
	{
		CVector center;
		float radius;
		DWORD next, pass_wrecked;
		static DWORD stat_last_found = 0;

		auto& pool = GetVehiclePool();
		*thread >> center >> radius >> next >> pass_wrecked;

		DWORD& last_found = reinterpret_cast<CCustomScript*>(thread)->IsCustom() ?
			reinterpret_cast<CCustomScript *>(thread)->GetLastSearchVehicle() :
			stat_last_found;

		if (!next) last_found = 0;

		for (int index = last_found; index < pool.m_nSize; ++index)
		{
			if (auto obj = pool.GetAt(index))
			{
				if ((pass_wrecked && IsWrecked(obj)) || (/*obj->GetOwner() == 2 ||*/ obj->bFadeOut))
					continue;

				if (radius >= 1000.0f || (VectorSqrMagnitude(obj->GetPosition() - center) <= radius * radius))
				{
					last_found = index + 1;	// on next opcode call start search from next index
											//if(last_found >= (unsigned)pool.GetSize()) last_found = 0;
											// obj.referenceType = 2; // add reference to found actor
					*thread << pool.GetRef(obj);
					SetScriptCondResult(thread, true);
					return OR_CONTINUE;
				}
			}
		}

		*thread << -1;
		last_found = 0;
		SetScriptCondResult(thread, false);
		return OR_CONTINUE;
	}

	//0AE3=6,%6d% = find_object_near_point %1d% %2d% %3d% in_radius %4d% find_next %5h% //IF and SET
	OpcodeResult __stdcall opcode_0AE3(CRunningScript *thread)
	{
		CVector center;
		float radius;
		DWORD next;
		static DWORD stat_last_found = 0;
		auto& pool = GetObjectPool();
		*thread >> center >> radius >> next;

		auto cs = reinterpret_cast<CCustomScript *>(thread);
		DWORD& last_found = cs->IsCustom() ? cs->GetLastSearchObject() : stat_last_found;

		if (!next) last_found = 0;

		for (int index = last_found; index < pool.m_nSize; ++index)
		{
			if (auto obj = pool.GetAt(index))
			{
				if (obj->m_nObjectFlags.bFadingIn) continue; // this is actually .bFadingOut (yet?)

				if (radius >= 1000.0f || (VectorSqrMagnitude(obj->GetPosition() - center) <= radius * radius))
				{
					last_found = index + 1;	// on next opcode call start search from next index
											//if(last_found >= (unsigned)pool.GetSize()) last_found = 0;
											// obj.referenceType = 2; // add reference to found actor
					*thread << pool.GetRef(obj);
					SetScriptCondResult(thread, true);
					return OR_CONTINUE;
				}
			}
		}

		last_found = 0;
		*thread << -1;
		SetScriptCondResult(thread, false);
		return OR_CONTINUE;
	}

	//0AEA=2,%2d% = actor_struct %1d% handle
	OpcodeResult __stdcall opcode_0AEA(CRunningScript *thread)
	{
		CPed *struc;
		*thread >> struc;
		*thread << GetPedPool().GetRef(struc);
		return OR_CONTINUE;
	}

	//0AEB=2,%2d% = car_struct %1d% handle
	OpcodeResult __stdcall opcode_0AEB(CRunningScript *thread)
	{
		CVehicle *struc;
		*thread >> struc;
		*thread << GetVehiclePool().GetRef(struc);
		return OR_CONTINUE;
	}

	//0AEC=2,%2d% = object_struct %1d% handle
	OpcodeResult __stdcall opcode_0AEC(CRunningScript *thread)
	{
		CObject *struc;
		*thread >> struc;
		*thread << GetObjectPool().GetRef(struc);
		return OR_CONTINUE;
	}
}
