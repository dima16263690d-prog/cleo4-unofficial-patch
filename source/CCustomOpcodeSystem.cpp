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
#include "CDebugBridge.h"
#include "CModLoaderCompat.h"
#include "CCustomOpcodeSystem.Internal.h"
#include "CScriptProfiler.h"
#include "CScriptPluginSdk.h"

namespace CLEO {
	DWORD FUNC_fopen;
	DWORD FUNC_fclose;
	DWORD FUNC_fwrite;
	DWORD FUNC_fread;
	DWORD FUNC_fgetc;
	DWORD FUNC_fgets;
	DWORD FUNC_fputs;
	DWORD FUNC_fseek;
	DWORD FUNC_fprintf;
	DWORD FUNC_ftell;
	DWORD FUNC_fflush;
	DWORD FUNC_feof;
	DWORD FUNC_ferror;

	OpcodeResult __stdcall opcode_0A8C(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0A8D(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0A8E(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0A8F(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0A90(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0A91(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0A92(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0A93(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0A94(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0A95(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0A96(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0A97(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0A98(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0A99(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0A9A(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0A9B(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0A9C(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0A9D(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0A9E(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0A9F(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AA0(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AA1(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AA2(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AA3(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AA4(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AA5(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AA6(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AA7(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AA8(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AA9(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AAA(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AAB(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AAC(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AAD(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AAE(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AAF(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AB0(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AB1(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AB2(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AB3(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AB4(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AB5(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AB6(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AB7(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AB8(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AB9(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0ABA(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0ABB(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0ABC(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0ABD(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0ABE(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0ABF(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AC0(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AC1(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AC2(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AC3(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AC4(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AC5(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AC6(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AC7(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AC8(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AC9(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0ACA(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0ACB(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0ACC(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0ACD(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0ACE(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0ACF(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AD0(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AD1(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AD2(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AD3(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AD4(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AD5(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AD6(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AD7(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AD8(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AD9(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0ADA(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0ADB(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0ADC(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0ADD(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0ADE(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0ADF(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AE0(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AE1(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AE2(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AE3(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AE4(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AE5(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AE6(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AE7(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AE8(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AE9(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AEA(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AEB(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AEC(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AED(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AEE(CRunningScript *thread);
	OpcodeResult __stdcall opcode_0AEF(CRunningScript *thread);

	CustomOpcodeHandler customOpcodeHandlersEx[100] =
	{
		opcode_0A8C, opcode_0A8D, opcode_0A8E, opcode_0A8F, opcode_0A90,
		opcode_0A91, opcode_0A92, opcode_0A93, opcode_0A94, opcode_0A95,
		opcode_0A96, opcode_0A97, opcode_0A98, opcode_0A99, opcode_0A9A,
		opcode_0A9B, opcode_0A9C, opcode_0A9D, opcode_0A9E, opcode_0A9F,
		opcode_0AA0, opcode_0AA1, opcode_0AA2, opcode_0AA3, opcode_0AA4,
		opcode_0AA5, opcode_0AA6, opcode_0AA7, opcode_0AA8, opcode_0AA9,
		opcode_0AAA, opcode_0AAB, opcode_0AAC, opcode_0AAD, opcode_0AAE,
		opcode_0AAF, opcode_0AB0, opcode_0AB1, opcode_0AB2, opcode_0AB3,
		opcode_0AB4, opcode_0AB5, opcode_0AB6, opcode_0AB7, opcode_0AB8,
		opcode_0AB9, opcode_0ABA, opcode_0ABB, opcode_0ABC, opcode_0ABD,
		opcode_0ABE, opcode_0ABF, opcode_0AC0, opcode_0AC1, opcode_0AC2,
		opcode_0AC3, opcode_0AC4, opcode_0AC5, opcode_0AC6, opcode_0AC7,
		opcode_0AC8, opcode_0AC9, opcode_0ACA, opcode_0ACB, opcode_0ACC,
		opcode_0ACD, opcode_0ACE, opcode_0ACF, opcode_0AD0, opcode_0AD1,
		opcode_0AD2, opcode_0AD3, opcode_0AD4, opcode_0AD5, opcode_0AD6,
		opcode_0AD7, opcode_0AD8, opcode_0AD9, opcode_0ADA, opcode_0ADB,
		opcode_0ADC, opcode_0ADD, opcode_0ADE, opcode_0ADF, opcode_0AE0,
		opcode_0AE1, opcode_0AE2, opcode_0AE3, opcode_0AE4, opcode_0AE5,
		opcode_0AE6, opcode_0AE7, opcode_0AE8, opcode_0AE9, opcode_0AEA,
		opcode_0AEB, opcode_0AEC, opcode_0AED, opcode_0AEE, opcode_0AEF,
	};

	typedef OpcodeResult(__thiscall *_OpcodeHandler)(CRunningScript *thread, unsigned short opcode);

	typedef void(*FuncScriptDeleteDelegateT) (CRunningScript *script);
	struct ScriptDeleteDelegate {
		std::vector<FuncScriptDeleteDelegateT> funcs;
		template<class FuncScriptDeleteDelegateT> void operator+=(FuncScriptDeleteDelegateT mFunc) { funcs.push_back(mFunc); }
		template<class FuncScriptDeleteDelegateT> void operator-=(FuncScriptDeleteDelegateT mFunc) { funcs.erase(std::remove(funcs.begin(), funcs.end(), mFunc), funcs.end()); }
		void operator()(CRunningScript *script) { for (auto& f : funcs) f(script); }
	};
	ScriptDeleteDelegate scriptDeleteDelegate;
	void RunScriptDeleteDelegate(CRunningScript *script)
	{
		// Let plugins and legacy delegates observe the script while its
		// per-script state is still available (they may read it to free
		// resources), then drop the SDK 2.0 state.
		NotifyScriptDeleted(script);
		scriptDeleteDelegate(script);
		PluginSdk::OnScriptDeleted(script);
	}

	// Opcode dispatch tables (defined further below; declared here so the
	// observation wrapper can be installed/uninstalled before they are defined).
	extern _OpcodeHandler *oldOpcodeHandlerTable;
	extern _OpcodeHandler newOpcodeHandlerTable[329];

	// ------------------------------------------------------------------
	// Active-path opcode observation
	// ------------------------------------------------------------------
	// GTA's script loop reads the opcode itself and calls
	// opcodeHandlerTable[opcode/100](thread, opcode) directly. The legacy CLEO
	// loop that used to host the per-opcode debug callbacks and the profiler is
	// NOT installed, so without this wrapper those observers would never fire.
	//
	// Every group in the active table is wrapped, so the observer sees every
	// opcode (00xx control flow, 0Axx/0Bxx CLEO opcodes, ...). A single indexed
	// table of the captured originals keeps the forward cheap: the wrapper looks
	// up the real handler by opcode/100.
	constexpr size_t kOpcodeGroupCount = 329;
	_OpcodeHandler g_originalHandlers[kOpcodeGroupCount];
	bool g_opcodeObservationInstalled = false;

	OpcodeResult __fastcall ObservedOpcodeHandler(CRunningScript *thread, int, unsigned short opcode)
	{
		// GTA passes the NOT-flag-cleared opcode to the table, so strip the high
		// bit before indexing and forwarding (group index must stay in range).
		const unsigned short base = opcode & 0x7FFF;

		if (g_scriptProfilerEnabled != 0)
			CScriptProfiler::Get().OnOpcode(thread, base);

		// No plugin observer registered: nothing can HANDLED/INTERRUPT the
		// opcode, so skip the callback dispatch entirely. When only the crash
		// snapshot is active, record the last opcode directly; that path is
		// mutex-free and much cheaper than the generic notifiers.
		if (!ScriptOpcodeCallbacksActive())
		{
			const bool crashSnapshot = g_crashSnapshotEnabled != 0;
			if (crashSnapshot)
				RecordCrashOpcodeBefore(thread, base);

			OpcodeResult result = g_originalHandlers[base / 100](thread, base);

			if (crashSnapshot)
				RecordCrashOpcodeResult(static_cast<LONG>(result));

			return result;
		}

		const int action = NotifyScriptOpcodeProcessBefore(thread, base);

		OpcodeResult result;
		if (action == CLEO_DEBUG_OPCODE_HANDLED)
			result = static_cast<OpcodeResult>(OR_CONTINUE);
		else if (action == CLEO_DEBUG_OPCODE_INTERRUPT)
			result = static_cast<OpcodeResult>(OR_INTERRUPT);
		else
			result = g_originalHandlers[base / 100](thread, base);

		if (result != OR_INTERRUPT || action == CLEO_DEBUG_OPCODE_PASSTHROUGH)
			result = NotifyScriptOpcodeProcessAfter(thread, base, result);

		return result;
	}

	void InstallOpcodeObservation()
	{
		if (oldOpcodeHandlerTable == nullptr || g_opcodeObservationInstalled)
			return;

		// Snapshot the real handlers from the active table (newOpcodeHandlerTable;
		// MA_OPCODE_HANDLER_REF points at it) and point every slot at the wrapper.
		// Every slot is non-null here: Inject() copies groups 0..27 from GTA's
		// table and fills 28..328 with extraOpcodeHandler before this runs, so
		// the wrapper never chains into a null handler.
		for (size_t group = 0; group < kOpcodeGroupCount; ++group)
		{
			g_originalHandlers[group] = newOpcodeHandlerTable[group];
			newOpcodeHandlerTable[group] = reinterpret_cast<_OpcodeHandler>(ObservedOpcodeHandler);
		}

		// Keep the source table (groups 0..27) in sync too, so a later handler
		// registration against it is not shadowed.
		for (size_t group = 0; group < 28; ++group)
			oldOpcodeHandlerTable[group] = reinterpret_cast<_OpcodeHandler>(ObservedOpcodeHandler);

		g_opcodeObservationInstalled = true;
	}

	void RemoveOpcodeObservation()
	{
		if (!g_opcodeObservationInstalled)
			return;

		for (size_t group = 0; group < kOpcodeGroupCount; ++group)
			newOpcodeHandlerTable[group] = g_originalHandlers[group];

		if (oldOpcodeHandlerTable != nullptr)
		{
			for (size_t group = 0; group < 28; ++group)
				oldOpcodeHandlerTable[group] = g_originalHandlers[group];
		}

		g_opcodeObservationInstalled = false;
	}

	void RefreshOpcodeObservation()
	{
		// Observation is needed exactly when some consumer is listening:
		// a plugin opcode observer, the per-script profiler, or the crash
		// snapshot recorder. Keeping the wrapper out of the hot path otherwise
		// means zero overhead for players who use none of them.
		const bool needed =
			ScriptOpcodeCallbacksActive() ||
			g_scriptProfilerEnabled != 0 ||
			g_crashSnapshotEnabled != 0;

		if (needed)
			InstallOpcodeObservation();
		else
			RemoveOpcodeObservation();
	}

	_OpcodeHandler *oldOpcodeHandlerTable;
	_OpcodeHandler newOpcodeHandlerTable[329];

	// Optional low opcode callbacks are owned by optional plugins.
	CustomOpcodeHandler lowOpcodeHandlers[0x0AF0] = {};
	CustomOpcodeHandler extraOpcodeHandlers[100][300];

	void UnregisterOpcodeSlot(WORD opcode)
	{
		if (opcode > 0x7FFF)
			return;

		if (opcode < 0x0AF0)
			lowOpcodeHandlers[opcode] = nullptr;
		else
			extraOpcodeHandlers[opcode % 100][opcode / 100 - 28] = nullptr;
	}

	CBuildingPool		**buildingPool = nullptr;			// add for future CLEO releases
	CVehiclePool			**vehiclePool = nullptr;
	CObjectPool			**objectPool = nullptr;
	CPedPool				**pedPool = nullptr;

	CPedPool& GetPedPool() { return **pedPool; }
	CVehiclePool& GetVehiclePool() { return **vehiclePool; }
	CObjectPool& GetObjectPool() { return **objectPool; }

	void(__thiscall * ProcessScript)(CRunningScript*);

	const char * (__cdecl * GetUserDirectory)();
	void(__cdecl * ChangeToUserDir)();
	void(__cdecl * ChangeToProgramDir)(const char *);

	float(__cdecl * FindGroundZ)(float x, float y);
	CMarker		* RadarBlips;

	CHandling	* Handling;

	CPlayerPed * (__cdecl * GetPlayerPed)(DWORD);
	CBaseModelInfo **Models;

	void(__cdecl * SpawnCar)(DWORD);

	WORD last_opcode = 0;
	WORD last_custom_opcode = 0;
	char last_thread[9] = "none";
	CRunningScript * last_script;
	ptrdiff_t last_off = -1;

	// opcode handler for opcodes, defined by user with cleo api
	OpcodeResult __fastcall extraOpcodeHandler(CRunningScript *thread, int dummy, unsigned short opcode)
	{
		last_custom_opcode = opcode;
		last_script = thread;
		return extraOpcodeHandlers[opcode % 100][opcode / 100 - 28](thread);
	}

	// opcode handler for custom opcodes
	OpcodeResult __fastcall customOpcodeHandler(CRunningScript *thread, int dummy, unsigned short opcode)
	{
		last_custom_opcode = opcode;
		last_script = thread;
		return customOpcodeHandlersEx[opcode - 0x0A8C](thread);
	}

	// Legacy CLEO4 script-execution loop, kept as the reference implementation.
	//
	// It is intentionally NOT installed: the 0x469FF6 hook was removed to
	// restore the v1.0-test path, so GTA's own script loop drives opcode
	// dispatch through newOpcodeHandlerTable. This loop is therefore dormant.
	// Per-opcode profiling and debug callbacks are observed by the
	// ObservedOpcodeHandler wrapper that InstallOpcodeObservation() places in
	// newOpcodeHandlerTable; per-script timing frames come from
	// HOOK_ProcessScript.
	char ExecuteLegacyScriptLoop(CRunningScript *thread)
	{
		OpcodeResult res;

		if (!thread)
			return 0;

		last_script = thread;

		try
		{
			do
			{
				CCustomScript *cs = reinterpret_cast<CCustomScript *>(thread);
				ptrdiff_t off = cs->IsCustom()
					? thread->GetBytePointer() - thread->GetBasePointer()
					: thread->GetBytePointer() - scmBlock;

				WORD opcode = thread->ReadDataWord();
				last_opcode = opcode;
				last_off = off;
				memcpy(last_thread, thread->GetName(), 8);
				last_thread[8] = '\0';

				cs->SetNotFlag((opcode & 0x8000) != 0);
				opcode &= 0x7FFF;

				res = static_cast<OpcodeResult>(OR_INTERRUPT);
				if (g_scriptProfilerEnabled != 0)
					CScriptProfiler::Get().OnOpcode(thread, opcode);
				if (opcode >= 0x0A8C && opcode <= 0x0AEF)
				{
					last_custom_opcode = opcode;
					last_script = thread;
					res = customOpcodeHandlersEx[opcode - 0x0A8C](thread);
				}
				else if (opcode >= 0x0AF0 &&
				         extraOpcodeHandlers[opcode % 100][opcode / 100 - 28] != nullptr)
				{
					last_custom_opcode = opcode;
					last_script = thread;
					res = extraOpcodeHandlers[opcode % 100][opcode / 100 - 28](thread);
				}
				else
				{
					// Legacy CLEO4 dispatch: use the prepared opcode table exactly as
					// v1.0-test did. Built-in and user opcodes above are entered
					// directly; the debug callbacks and profiler run only when a
					// plugin registered an opcode observer.
					if (g_crashSnapshotEnabled != 0 ||
					    ScriptOpcodeCallbacksActive() ||
					    g_scriptProfilerEnabled != 0)
					{
						const int action = NotifyScriptOpcodeProcessBefore(thread, opcode);
						if (action == CLEO_DEBUG_OPCODE_HANDLED)
							res = static_cast<OpcodeResult>(OR_CONTINUE);
						else if (action == CLEO_DEBUG_OPCODE_INTERRUPT)
							res = static_cast<OpcodeResult>(OR_INTERRUPT);
						else
							res = newOpcodeHandlerTable[opcode / 100](thread, opcode);

						if (res != OR_INTERRUPT || action == CLEO_DEBUG_OPCODE_PASSTHROUGH)
							res = NotifyScriptOpcodeProcessAfter(thread, opcode, res);
					}
					else
					{
						res = newOpcodeHandlerTable[opcode / 100](thread, opcode);
					}
				}
			} while (res == OR_CONTINUE);
		}
		catch (const char *e)
		{
			char str[128];
			sprintf(str, "%s encountered while parsing opcode '%04X' in script '%s'", e, last_opcode, last_thread);
			Error(str);
		}

		return 0;
	}

	char ScriptExecutionLoop()
	{
		CCustomScript *thread;
		_asm mov thread, esi
		return ExecuteLegacyScriptLoop(reinterpret_cast<CRunningScript *>(thread));
	}

	void CCustomOpcodeSystem::Inject(CCodeInjector& inj)
	{
		TRACE("Injecting CustomOpcodeSystem...");
		CGameVersionManager& gvm = GetInstance().VersionManager;
		oldOpcodeHandlerTable = gvm.TranslateMemoryAddress(MA_OPCODE_HANDLER);

		// add handler for custom opcodes
		oldOpcodeHandlerTable[27] = reinterpret_cast<_OpcodeHandler>(customOpcodeHandler);

		// replace old OpcodeHandlerTable with the new one
		//inj.MemoryWrite(gvm.TranslateMemoryAddress(MA_OPCODE_HANDLER_REF), reinterpret_cast<_OpcodeHandler>(&newOpcodeHandlerTable[0]));
		MemWrite(gvm.TranslateMemoryAddress(MA_OPCODE_HANDLER_REF), reinterpret_cast<_OpcodeHandler>(&newOpcodeHandlerTable[0]));

		// copy old table to the new                                                             
		//std::copy(oldOpcodeHandlerTable, oldOpcodeHandlerTable + 28, newOpcodeHandlerTable);
		MemCopy<_OpcodeHandler>(newOpcodeHandlerTable, oldOpcodeHandlerTable, (&oldOpcodeHandlerTable[28] - oldOpcodeHandlerTable) * 4);
		//MemCopy(newOpcodeHandlerTable, oldOpcodeHandlerTable, oldOpcodeHandlerTable - (oldOpcodeHandlerTable + 28));

		// fill the rest with default handler
		std::fill(newOpcodeHandlerTable + 28, newOpcodeHandlerTable + 329, reinterpret_cast<_OpcodeHandler>(extraOpcodeHandler));

		// Now that newOpcodeHandlerTable is fully built, arm the observer wrapper
		// if any consumer is already listening. Otherwise it stays off (zero
		// overhead) and gets installed on demand by RefreshOpcodeObservation().
		RefreshOpcodeObservation();

		// Keep the original CLEO4 opcode execution path.
		// The experimental fast dispatcher is not installed into the legacy
		// opcode table. This preserves the v1.0-test execution semantics.

		FUNC_fopen = gvm.TranslateMemoryAddress(MA_FOPEN_FUNCTION);
		FUNC_fclose = gvm.TranslateMemoryAddress(MA_FCLOSE_FUNCTION);
		FUNC_fread = gvm.TranslateMemoryAddress(MA_FREAD_FUNCTION);
		FUNC_fwrite = gvm.TranslateMemoryAddress(MA_FWRITE_FUNCTION);
		FUNC_fgetc = gvm.TranslateMemoryAddress(MA_FGETC_FUNCTION);
		FUNC_fgets = gvm.TranslateMemoryAddress(MA_FGETS_FUNCTION);
		FUNC_fputs = gvm.TranslateMemoryAddress(MA_FPUTS_FUNCTION);
		FUNC_fseek = gvm.TranslateMemoryAddress(MA_FSEEK_FUNCTION);
		FUNC_fprintf = gvm.TranslateMemoryAddress(MA_FPRINTF_FUNCTION);
		FUNC_ftell = gvm.TranslateMemoryAddress(MA_FTELL_FUNCTION);
		FUNC_fflush = gvm.TranslateMemoryAddress(MA_FFLUSH_FUNCTION);
		FUNC_feof = gvm.TranslateMemoryAddress(MA_FEOF_FUNCTION);
		FUNC_ferror = gvm.TranslateMemoryAddress(MA_FERROR_FUNCTION);

		pedPool = gvm.TranslateMemoryAddress(MA_PED_POOL);
		vehiclePool = gvm.TranslateMemoryAddress(MA_VEHICLE_POOL);
		objectPool = gvm.TranslateMemoryAddress(MA_OBJECT_POOL);
		GetUserDirectory = gvm.TranslateMemoryAddress(MA_GET_USER_DIR_FUNCTION);
		ChangeToUserDir = gvm.TranslateMemoryAddress(MA_CHANGE_TO_USER_DIR_FUNCTION);
		ChangeToProgramDir = gvm.TranslateMemoryAddress(MA_CHANGE_TO_PROGRAM_DIR_FUNCTION);
		FindGroundZ = gvm.TranslateMemoryAddress(MA_FIND_GROUND_Z_FUNCTION);
		GetPlayerPed = gvm.TranslateMemoryAddress(MA_GET_PLAYER_PED_FUNCTION);
		Handling = gvm.TranslateMemoryAddress(MA_HANDLING);
		Models = gvm.TranslateMemoryAddress(MA_MODELS);
		SpawnCar = gvm.TranslateMemoryAddress(MA_SPAWN_CAR_FUNCTION);

		// TODO: consider version-agnostic code
		if (gvm.GetGameVersion() == GV_US10) {
			// make it compatible with fastman92's limit adjuster (only required for 1.0 US)
			RadarBlips = injector::ReadMemory<CMarker*>(0x583A05 + 2, true);

		}
		else {
			RadarBlips = gvm.TranslateMemoryAddress(MA_RADAR_BLIPS);
		}
	}

	// read string parameter according to convention on strings
	char *readString(CRunningScript *thread, char* buf, BYTE size)
	{
		if (size == 0) size = MAX_STR_LEN;

		auto paramType = *thread->GetBytePointer();
		if (!paramType) return nullptr;

		// String variables are stored in the script-variable storage itself.
		// A SCRIPT_VAR is only one 32-bit slot, so cParam is a single byte and
		// cannot be used as a string pointer. For 0AB1/0AB2, GetScriptParamPointer()
		// resolves the current function-local slot (including savedTls), therefore
		// read the string from the slot memory directly.
		if (paramType == DT_VAR_STRING ||
			paramType == DT_LVAR_STRING ||
			paramType == DT_VAR_TEXTLABEL ||
			paramType == DT_LVAR_TEXTLABEL ||
			paramType == DT_VAR_STRING_ARRAY ||
			paramType == DT_LVAR_STRING_ARRAY ||
			paramType == DT_VAR_TEXTLABEL_ARRAY ||
			paramType == DT_LVAR_TEXTLABEL_ARRAY)
		{
			SCRIPT_VAR *var = GetScriptParamPointer(thread);
			if (!var)
				return nullptr;

			const char *src = reinterpret_cast<const char *>(var);
			const size_t storageSize =
				(paramType == DT_VAR_TEXTLABEL ||
				 paramType == DT_LVAR_TEXTLABEL ||
				 paramType == DT_VAR_TEXTLABEL_ARRAY ||
				 paramType == DT_LVAR_TEXTLABEL_ARRAY) ? 8 : 16;

			if (buf != nullptr)
			{
				const size_t copySize = std::min<size_t>(size - 1, storageSize - 1);
				if (copySize)
					memcpy(buf, src, copySize);
				buf[copySize] = '\0';
				return buf;
			}

			return const_cast<char *>(src);
		}

		if (paramType >= DT_DWORD && paramType <= DT_LVAR_ARRAY) // process parameter as a pointer to string
		{
			GetScriptParams(thread, 1);

			if (buf != nullptr)
			{
				strncpy(buf, opcodeParams[0].pcParam, size - 1);
				buf[size - 1] = '\0';
			}

			return opcodeParams[0].pcParam; // original string pointer
		}
		else // process as scm string
		{
			// no user output buffer provided
			if (buf == nullptr)
			{
				static char result[MAX_STR_LEN];
				buf = result;
				size = sizeof(result);
			}

			std::fill(buf, buf + size, '\0');

			if (paramType == DT_VARLEN_STRING)
			{
			// process here as GetScriptStringParam can not obtain strings with length greater than 128
				thread->IncPtr(1); // already read paramType

				BYTE length = *thread->GetBytePointer(); // as unsigned!
				thread->IncPtr(1); // length

				if (length > 0)
				{
					auto count = std::min<size_t>(size - 1, length);
					memcpy(buf, thread->GetBytePointer(), count);
					buf[count] = '\0';

					thread->IncPtr(length); // read text
				}
			}
			else
			{
				GetScriptStringParam(thread, buf, size);
			}

			return buf;
		}
	}

	// perform 'sprintf'-operation for parameters, passed through SCM
	int format(CRunningScript *thread, char *str, size_t len, const char *format)
	{
		unsigned int written = 0;
		const char *iter = format;
		char bufa[256], fmtbufa[64], *fmta;

		while (*iter)
		{
			while (*iter && *iter != '%')
			{
				if (written++ >= len)
					return -1;
				*str++ = *iter++;
			}
			if (*iter == '%')
			{
				if (iter[1] == '%')
				{
					if (written++ >= len)
						return -1;
					*str++ = '%'; /* "%%"->'%' */
					iter += 2;
					continue;
				}

				//get flags and width specifier
				fmta = fmtbufa;
				*fmta++ = *iter++;
				while (*iter == '0' ||
					   *iter == '+' ||
					   *iter == '-' ||
					   *iter == ' ' ||
					   *iter == '*' ||
					   *iter == '#')
				{
					if (*iter == '*')
					{
						char *buffiter = bufa;
						//get width
						GetScriptParams(thread, 1);
						_itoa(opcodeParams[0].dwParam, buffiter, 10);
						while (*buffiter)
							*fmta++ = *buffiter++;
					}
					else
						*fmta++ = *iter;
					iter++;
				}

				//get immidiate width value
				while (isdigit(*iter))
					*fmta++ = *iter++;

				//get precision
				if (*iter == '.')
				{
					*fmta++ = *iter++;
					if (*iter == '*')
					{
						char *buffiter = bufa;
						GetScriptParams(thread, 1);
						_itoa(opcodeParams[0].dwParam, buffiter, 10);
						while (*buffiter)
							*fmta++ = *buffiter++;
					}
					else
						while (isdigit(*iter))
							*fmta++ = *iter++;
				}
				//get size
				if (*iter == 'h' || *iter == 'l')
					*fmta++ = *iter++;

				switch (*iter)
				{
				case 's':
				{
					static const char none[] = "(null)";
					const char *astr = readString(thread);
					const char *striter = astr ? astr : none;
					while (*striter)
					{
						if (written++ >= len)
							return -1;
						*str++ = *striter++;
					}
					iter++;
					break;
				}

				case 'c':
					if (written++ >= len)
						return -1;
					GetScriptParams(thread, 1);
					*str++ = (char)opcodeParams[0].nParam;
					iter++;
					break;

				default:
				{
					/* For non wc types, use system sprintf and append to wide char output */
					/* FIXME: for unrecognised types, should ignore % when printing */
					char *bufaiter = bufa;
					if (*iter == 'p' || *iter == 'P')
					{
						GetScriptParams(thread, 1);
						sprintf(bufaiter, "%08X", opcodeParams[0].dwParam);
					}
					else
					{
						*fmta++ = *iter;
						*fmta = '\0';
						if (*iter == 'a' || *iter == 'A' ||
							*iter == 'e' || *iter == 'E' ||
							*iter == 'f' || *iter == 'F' ||
							*iter == 'g' || *iter == 'G')
						{
							GetScriptParams(thread, 1);
							sprintf(bufaiter, fmtbufa, opcodeParams[0].fParam);
						}
						else
						{
							GetScriptParams(thread, 1);
							sprintf(bufaiter, fmtbufa, opcodeParams[0].pParam);
						}
					}
					while (*bufaiter)
					{
						if (written++ >= len)
							return -1;
						*str++ = *bufaiter++;
					}
					iter++;
					break;
				}
				}
			}
		}
		if (written >= len)
			return -1;
		*str++ = 0;
		return (int)written;
	}

	// Legacy modes for CLEO 3
	FILE * legacy_fopen(const char * szPath, const char * szMode)
	{
		FILE * hFile;
		_asm
		{
			push szMode
			push szPath
			call FUNC_fopen
			add esp, 8
			mov hFile, eax
		}
		return hFile;
	}
	void legacy_fclose(FILE * hFile)
	{
		_asm
		{
			push hFile
			call FUNC_fclose
			add esp, 4
		}
	}
	size_t legacy_fread(void * buf, size_t len, size_t count, FILE * stream)
	{
		_asm
		{
			push stream
			push count
			push len
			push buf
			call FUNC_fread
			add esp, 0x10
		}
	}
	size_t legacy_fwrite(const void * buf, size_t len, size_t count, FILE * stream)
	{
		_asm
		{
			push stream
			push count
			push len
			push buf
			call FUNC_fwrite
			add esp, 0x10
		}
	}
	char legacy_fgetc(FILE * stream)
	{
		_asm
		{
			push stream
			call FUNC_fgetc
			add esp, 0x4
		}
	}
	char * legacy_fgets(char *pStr, int num, FILE * stream)
	{
		_asm
		{
			push stream
			push num
			push pStr
			call FUNC_fgets
			add esp, 0xC
		}
	}
	int legacy_fputs(const char *pStr, FILE * stream)
	{
		_asm
		{
			push stream
			push pStr
			call FUNC_fputs
			add esp, 0x8
		}
	}
	int legacy_fseek(FILE * stream, long int offs, int original)
	{
		_asm
		{
			push stream
			push offs
			push original
			call FUNC_fseek
			add esp, 0xC
		}
	}
	int legacy_ftell(FILE * stream)
	{
		_asm
		{
			push stream
			call FUNC_ftell
			add esp, 0x4
		}
	}
	int __declspec(naked) fprintf(FILE * stream, const char * format, ...)
	{
		_asm jmp FUNC_fprintf
	}
	int legacy_fflush(FILE * stream)
	{
		_asm
		{
			push stream
			call FUNC_fflush
			add esp, 0x4
		}
	}
	int legacy_feof(FILE * stream)
	{
		_asm
		{
			push stream
			call FUNC_feof
			add esp, 0x4
		}
	}
	int legacy_ferror(FILE * stream)
	{
		_asm
		{
			push stream
			call FUNC_ferror
			add esp, 0x4
		}
	}

	bool is_legacy_handle(DWORD dwHandle) { return (dwHandle & 0x1) == 0; }
	FILE * convert_handle_to_file(DWORD dwHandle) { return dwHandle ? reinterpret_cast<FILE*>(is_legacy_handle(dwHandle) ? dwHandle : dwHandle & ~(0x1)) : nullptr; }

	// Resolve a game-relative path into the Mod Loader tree. Returns false
	// when Mod Loader is not installed or the file is not present there, so
	// callers can fall back to the original path.
	bool ResolveModLoaderPath(const char *szPath, std::string& out)
	{
		return CModLoaderCompat::Get().ResolveFile(szPath, out);
	}

	DWORD open_file(const char * szPath, const char * szMode, bool bLegacy)
	{
		FILE * hFile = bLegacy ? legacy_fopen(szPath, szMode) : fopen(szPath, szMode);
		if (hFile) return bLegacy ? (DWORD)hFile : (DWORD)hFile | 0x1;
		return NULL;
	}
	void close_file(DWORD dwHandle)
	{
		if (is_legacy_handle(dwHandle)) legacy_fclose(convert_handle_to_file(dwHandle));
		else fclose(convert_handle_to_file(dwHandle));
	}
	DWORD file_get_size(DWORD file_handle)
	{
		FILE * hFile = convert_handle_to_file(file_handle);
		if (hFile)
		{
			auto savedPos = ftell(hFile);
			fseek(hFile, 0, SEEK_END);
			DWORD dwSize = static_cast<DWORD>(ftell(hFile));
			fseek(hFile, savedPos, SEEK_SET);
			return dwSize;
		}
		return 0;
	}
	DWORD read_file(void *buf, DWORD size, DWORD count, DWORD hFile)
	{
		return is_legacy_handle(hFile) ? legacy_fread(buf, size, 1, convert_handle_to_file(hFile)) : fread(buf, size, 1, convert_handle_to_file(hFile));
	}
	DWORD write_file(const void *buf, DWORD size, DWORD count, DWORD hFile)
	{
		return is_legacy_handle(hFile) ? legacy_fwrite(buf, size, 1, convert_handle_to_file(hFile)) : fwrite(buf, size, 1, convert_handle_to_file(hFile));
	}
	void flush_file(DWORD dwHandle)
	{
		if (is_legacy_handle(dwHandle)) legacy_fflush(convert_handle_to_file(dwHandle));
		else fflush(convert_handle_to_file(dwHandle));
	}

	/*inline void __impl_RetrieveScriptParam(SCRIPT_VAR*) { }
	template<typename ..Params> inline void __impl_RetrieveScriptParam(SCRIPT_VAR *, unsigned&, Params&...);
	template<typename Params> inline void __impl_RetrieveScriptParam(SCRIPT_VAR *, int&, Params&...);
	template<typename Params> inline void __impl_RetrieveScriptParam(SCRIPT_VAR *, float&, Params&...);
	template<typename ThisParam, typename Params> inline void __impl_RetrieveScriptParam(SCRIPT_VAR *, ThisParam *&, Params&...);

	template<typename Params>
	inline void __impl_RetrieveScriptParam(SCRIPT_VAR *var, unsigned& thisParam, Params&... restParams)
	{
	thisParam = var->dwParam;
	__impl_RetrieveScriptParam(var + 1, restParams...);
	}

	template<typename... Params>
	inline void __impl_RetrieveScriptParam(SCRIPT_VAR *var, int& thisParam, Params&... restParams)
	{
	thisParam = var->nParam;
	__impl_RetrieveScriptParam(var + 1, restParams...);
	}

	template<typename... Params>
	inline void __impl_RetrieveScriptParam(SCRIPT_VAR *var, float& thisParam, Params&... restParams)
	{
	thisParam = var->fParam;
	__impl_RetrieveScriptParam(var + 1, restParams...);
	}

	template<typename ThisParam, typename... Params>
	inline void __impl_RetrieveScriptParam(SCRIPT_VAR *var, ThisParam *& thisParam, Params&... restParams)
	{
	thisParam = reinterpret_cast<ThisParam *>(var->pParam);
	__impl_RetrieveScriptParam(var + 1, restParams...);
	}


	template<typename... Params>
	inline void RetrieveScriptParams(CCustomScript *thread, Params&... params)
	{
	GetScriptParams(thread, sizeof...(params));
	__impl_RetrieveScriptParam(opcodeParams, params...);
	}*/

	void ThreadJump(CRunningScript *thread, int off)
	{
		thread->SetIp(off < 0 ? thread->GetBasePointer() - off : scmBlock + off);
	}

	void SkipUnusedParameters(CRunningScript *thread)
	{
		while (*thread->GetBytePointer()) GetScriptParams(thread, 1);	// skip parameters
		thread->ReadDataByte();
	}

	// ========================================================================
	// PROJECT ARCHITECTURE NOTE — 0E6F and ScmFunction
	//
	// 0E6F is the project's custom-stream mechanism. Do NOT replace or
	// redesign 0E6F/CCustomScript/parentThread/childThreads with the CLEO 5
	// architecture. The 0E6F lifecycle owns custom-script creation, buffers,
	// parent/child links and child Save/Load state.
	//
	// ScmFunction is a separate execution-scope layer used by 0AB1/0AB2.
	// It may borrow the useful scope ideas from CLEO 5, but it must remain
	// compatible with the existing CLEO 4 CRunningScript layout and legacy
	// .cs/.cs3/.cs4 behavior.
	//
	// Runtime checks completed on 27.09.2026:
	//   - 0AB1/0AB2: 0 args -> 0 returns
	//   - 1 arg -> 1 return (10 -> 15)
	//   - 2 args -> 2 returns (111,222 -> 121,242; screen test)
	//   - ConditionResult TRUE/FALSE checks through 004D jump_if_false
	//   - nested ScmFunction A -> B -> C (777 -> 797 -> 807)
	//   - GOSUB inside 0AB1 function (40 -> 42)
	//   - 0E6F parent/child/nested-child Save/Load sidecar read/write
	//
	// Important diagnostic limitation:
	// current [0AB2] trace prints ret count + first return value only.
	// ret=2 121 does not by itself prove the second return value.
	// ========================================================================

	/************************************************************************/
	/*						Opcode definitions								*/
	/************************************************************************/




































































































}



/********************************************************************/

// API
extern "C"
{
	using namespace CLEO;

	// Define external symbols with MSVC decorating schemes
	BOOL WINAPI CLEO_RegisterOpcode(WORD opcode, CustomOpcodeHandler callback);
	DWORD WINAPI CLEO_GetIntOpcodeParam(CRunningScript* thread);
	float WINAPI CLEO_GetFloatOpcodeParam(CRunningScript* thread);
	void WINAPI CLEO_SetIntOpcodeParam(CRunningScript* thread, DWORD value);
	void WINAPI CLEO_SetFloatOpcodeParam(CRunningScript* thread, float value);
	LPSTR WINAPI CLEO_ReadStringOpcodeParam(CRunningScript* thread, char *buf, int size);
	LPSTR WINAPI CLEO_ReadStringPointerOpcodeParam(CRunningScript* thread, char *buf, int size);
	void WINAPI CLEO_WriteStringOpcodeParam(CRunningScript* thread, LPCSTR str);
	void WINAPI CLEO_SetThreadCondResult(CRunningScript* thread, BOOL result);
	void WINAPI CLEO_SkipOpcodeParams(CRunningScript* thread, int count);
	void WINAPI CLEO_ThreadJumpAtLabelPtr(CRunningScript* thread, int labelPtr);
	int WINAPI CLEO_GetOperandType(CRunningScript* thread);
	void WINAPI CLEO_RetrieveOpcodeParams(CRunningScript *thread, int count)
	{
		if (count <= 0)
			return;

		const DWORD requested = static_cast<DWORD>(count);
		const DWORD stored = std::min<DWORD>(requested, 32);

		// Collect the representable portion in the native 32-entry buffer.
		if (stored)
			GetScriptParams(thread, stored);

		// Still advance the script over the remaining parameters, one at a time.
		for (DWORD i = stored; i < requested; ++i)
			GetScriptParams(thread, 1);
	}

	int WINAPI CLEO_FormatOpcodeString(CRunningScript* thread, char* buffer, int size)
	{
		if (thread == nullptr || buffer == nullptr || size <= 0)
			return -1;

		char formatString[MAX_STR_LEN] = {};
		CLEO_ReadStringOpcodeParam(thread, formatString, sizeof(formatString));

		return format(thread, buffer, static_cast<size_t>(size), formatString);
	}

	void WINAPI CLEO_RecordOpcodeParams(CRunningScript *thread, int count)
	{
		if (count <= 0)
			return;

		const DWORD requested = static_cast<DWORD>(count);
		const DWORD stored = std::min<DWORD>(requested, 32);

		// Never ask GTA to store more than the native 32-entry buffer.
		SetScriptParams(thread, stored);
	}

	SCRIPT_VAR * WINAPI CLEO_GetPointerToScriptVariable(CRunningScript* thread);
	RwTexture * WINAPI CLEO_GetScriptTextureById(CRunningScript* thread, int id);
	HSTREAM WINAPI CLEO_GetInternalAudioStream(CRunningScript* thread, CAudioStream *stream);
	CRunningScript* WINAPI CLEO_CreateCustomScript(CRunningScript* fromThread, const char *fileName, int label);

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable:4550)
#endif

	BOOL WINAPI CLEO_RegisterOpcode(WORD opcode, CustomOpcodeHandler callback)
	{
		if (opcode > 0x7FFF || callback == nullptr)
			return FALSE;

		// Legacy entry point. It keeps the historical behaviour (a rejected
		// duplicate only logs a warning) and additionally records ownership
		// metadata so the SDK 2.0 queries and unload cleanup can see it.
		if (opcode < 0x0AF0)
		{
			CustomOpcodeHandler& dst = lowOpcodeHandlers[opcode];

			if (dst != nullptr)
			{
				Error("Warning! CLEO couldn't register opcode handler.");
				return FALSE;
			}

			dst = callback;
			PluginSdk::RegisterOpcode(opcode, callback, nullptr, nullptr, nullptr,
				PluginSdk::ModuleForAddress(reinterpret_cast<const void*>(callback)), true);
			NotifyScriptOpcodeRegistered(opcode);
			return TRUE;
		}

		CustomOpcodeHandler& dst = extraOpcodeHandlers[opcode % 100][opcode / 100 - 28];

		if (*dst)
		{
			Error("Warning! CLEO couldn't register opcode handler.");
			return FALSE;
		}
		dst = callback;
		PluginSdk::RegisterOpcode(opcode, callback, nullptr, nullptr, nullptr,
			PluginSdk::ModuleForAddress(reinterpret_cast<const void*>(callback)), true);
		NotifyScriptOpcodeRegistered(opcode);
		return TRUE;
	}

	BOOL WINAPI CLEO_RegisterOpcodeEx(WORD opcode, CustomOpcodeHandler callback,
		const char* name, const char* params, const char* description)
	{
		if (opcode > 0x7FFF || callback == nullptr)
			return FALSE;

		const HMODULE module =
			PluginSdk::ModuleForAddress(reinterpret_cast<const void*>(callback));

		// Check both the dispatch table and the metadata index first, then
		// commit, so a rejected registration never leaves half the state set.
		if (opcode < 0x0AF0)
		{
			if (lowOpcodeHandlers[opcode] != nullptr)
				return FALSE;
		}
		else
		{
			if (extraOpcodeHandlers[opcode % 100][opcode / 100 - 28] != nullptr)
				return FALSE;
		}

		if (!PluginSdk::RegisterOpcode(opcode, callback, name, params, description, module))
			return FALSE;

		if (opcode < 0x0AF0)
			lowOpcodeHandlers[opcode] = callback;
		else
			extraOpcodeHandlers[opcode % 100][opcode / 100 - 28] = callback;

		NotifyScriptOpcodeRegistered(opcode);
		return TRUE;
	}

	BOOL WINAPI CLEO_GetOpcodeInfo(WORD opcode, CLEO_OpcodeInfo* info)
	{
		return PluginSdk::GetOpcodeInfo(opcode, info) ? TRUE : FALSE;
	}

	int WINAPI CLEO_ParseParamSignature(const char* signature,
		CLEO_OpcodeParamKind* out, int maxOut)
	{
		return PluginSdk::ParseSignature(signature, out, maxOut);
	}

	BOOL WINAPI CLEO_OpcodeValidate(CRunningScript* thread, const char* signature)
	{
		return PluginSdk::ValidateParams(thread, signature) ? TRUE : FALSE;
	}

	void WINAPI CLEO_ReportError(const char* message)
	{
		PluginSdk::ReportError(message);
	}

	BOOL WINAPI CLEO_RegisterPluginResource(const char* kind, void* resource,
		CLEO_PluginResourceReleaseFn release)
	{
		return PluginSdk::RegisterResource(
			PluginSdk::ModuleForAddress(_ReturnAddress()),
			kind, resource,
			reinterpret_cast<PluginSdk::PluginResourceReleaseFn>(release)) ? TRUE : FALSE;
	}

	BOOL WINAPI CLEO_UnregisterPluginResource(void* resource)
	{
		return PluginSdk::UnregisterResource(
			PluginSdk::ModuleForAddress(_ReturnAddress()), resource) ? TRUE : FALSE;
	}

	void* WINAPI CLEO_GetScriptContext(CRunningScript* thread, uintptr_t key)
	{
		return PluginSdk::GetScriptContext(thread, key, PluginSdk::ModuleForAddress(_ReturnAddress()));
	}

	BOOL WINAPI CLEO_SetScriptContext(CRunningScript* thread, uintptr_t key, void* value)
	{
		return PluginSdk::SetScriptContext(thread, key, value,
			PluginSdk::ModuleForAddress(_ReturnAddress())) ? TRUE : FALSE;
	}

	BOOL WINAPI CLEO_SetProfilingEnabled(BOOL enabled)
	{
		const BOOL previous = CScriptProfiler::Get().SetEnabled(enabled != FALSE) ? TRUE : FALSE;
		// The profiler counts opcodes through the active-path observer wrapper.
		RefreshOpcodeObservation();
		return previous;
	}

	DWORD WINAPI CLEO_GetScriptProfileCount()
	{
		return static_cast<DWORD>(CScriptProfiler::Get().GetCount());
	}

	BOOL WINAPI CLEO_GetScriptProfile(DWORD index, CLEO_ScriptProfile* profile)
	{
		if (profile == nullptr)
			return FALSE;

		CScriptProfiler::Snapshot snapshot;
		if (!CScriptProfiler::Get().GetSnapshot(index, snapshot))
			return FALSE;

		profile->scriptPtr = snapshot.scriptPtr;
		memcpy(profile->scriptName, snapshot.name, 9);
		// Public surface reports milliseconds; internal resolution is us.
		profile->totalMs = snapshot.totalUs / 1000;
		profile->selfMs = snapshot.selfUs / 1000;
		profile->netMs = snapshot.netUs / 1000;
		profile->opcodes = snapshot.opcodes;
		profile->ticks = snapshot.ticks;
		return TRUE;
	}

	void WINAPI CLEO_ResetScriptProfiles()
	{
		CScriptProfiler::Get().Reset();
	}

	BOOL WINAPI CLEO_SetProfilerOverheadEnabled(BOOL enabled)
	{
		CScriptProfiler::Get().SetMeasureOverhead(enabled != FALSE);
		return TRUE;
	}

	DWORD WINAPI CLEO_GetTopOpcodes(CLEO_OpcodeCount* out, DWORD max)
	{
		if (out == nullptr || max == 0)
			return 0;

		std::vector<CScriptProfiler::OpcodeCount> counts(max);
		const size_t written =
			CScriptProfiler::Get().GetTopOpcodes(counts.data(), max);

		for (size_t i = 0; i < written; ++i)
		{
			out[i].opcode = counts[i].opcode;
			out[i].reserved = 0;
			out[i].count = counts[i].count;
		}
		return static_cast<DWORD>(written);
	}

#ifdef _MSC_VER
#pragma warning(pop)
#endif

	DWORD WINAPI CLEO_GetIntOpcodeParam(CRunningScript* thread)
	{
		DWORD result;
		*thread >> result;
		return result;
	}

	float WINAPI CLEO_GetFloatOpcodeParam(CRunningScript* thread)
	{
		float result;
		*thread >> result;
		return result;
	}

	void WINAPI CLEO_SetIntOpcodeParam(CRunningScript* thread, DWORD value)
	{
		*thread << value;
	}

	void WINAPI CLEO_SetFloatOpcodeParam(CRunningScript* thread, float value)
	{
		*thread << value;
	}

	LPSTR WINAPI CLEO_ReadStringOpcodeParam(CRunningScript* thread, char *buf, int size)
	{
		static char internal_buf[MAX_STR_LEN];
		if (!buf) { buf = internal_buf; size = MAX_STR_LEN; }
		if (!size) size = MAX_STR_LEN;
		std::fill(buf, buf + size, '\0');
		GetScriptStringParam(thread, buf, size);
		return buf;
	}

	LPSTR WINAPI CLEO_ReadStringPointerOpcodeParam(CRunningScript* thread, char *buf, int size)
	{
		static char internal_buf[MAX_STR_LEN];
		if (!buf) { buf = internal_buf; size = MAX_STR_LEN; }
		if (!size) size = MAX_STR_LEN;
		std::fill(buf, buf + size, '\0');
		return readString(thread, buf, size);
	}

	void WINAPI CLEO_WriteStringOpcodeParam(CRunningScript* thread, LPCSTR str)
	{
		auto dst = (char *)GetScriptParamPointer(thread);
		if (!dst)
			return;

		strncpy_s(dst, 16, str ? str : "", _TRUNCATE);
		dst[15] = '\0';
	}

	void WINAPI CLEO_SetThreadCondResult(CRunningScript* thread, BOOL result)
	{
		SetScriptCondResult(thread, result != FALSE);
	}

	void WINAPI CLEO_SkipOpcodeParams(CRunningScript* thread, int count)
	{
		int len;
		for (int i = 0; i < count; i++)
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
				break;
			case DT_VAR_ARRAY:
			case DT_LVAR_ARRAY:
				thread->IncPtr(6);
				break;
			case DT_BYTE:
				thread->IncPtr();
				break;
			case DT_WORD:
				thread->IncPtr(2);
				break;
			case DT_DWORD:
			case DT_FLOAT:
				thread->IncPtr(4);
				break;
			case DT_VARLEN_STRING:
				len = thread->ReadDataByte();
				thread->IncPtr(len);
				break;

			case DT_TEXTLABEL:
				thread->IncPtr(8);
				break;
			case DT_STRING:
				thread->IncPtr(16);
				break;
			}
		}
	}

	void WINAPI CLEO_ThreadJumpAtLabelPtr(CRunningScript* thread, int labelPtr)
	{
		ThreadJump(thread, labelPtr);
	}

	int WINAPI CLEO_GetOperandType(CRunningScript* thread)
	{
		return *thread->GetBytePointer();
	}

	SCRIPT_VAR * WINAPI CLEO_GetPointerToScriptVariable(CRunningScript* thread)
	{
		return GetScriptParamPointer(thread);
	}

	RwTexture * WINAPI CLEO_GetScriptTextureById(CRunningScript* thread, int id)
	{
		CCustomScript* customScript = reinterpret_cast<CCustomScript*>(thread);
		// We need to store-restore to update the texture list, not optimized, but this will not be used every frame anyway
		customScript->StoreScriptTextures();
		RwTexture *texture = customScript->GetScriptTextureById(id - 1);
		customScript->RestoreScriptTextures();
		return texture;
	}

	HSTREAM WINAPI CLEO_GetInternalAudioStream(CRunningScript* thread, CAudioStream *stream)
	{
		return stream->GetInternal();
	}

	CRunningScript* WINAPI CLEO_CreateCustomScript(CRunningScript* fromThread, const char *script_name, int label)
	{
		if (label != 0) // create from label
		{
			TRACE("Starting new custom script from thread named %s label %i", script_name, label);
		}
		else
		{
			TRACE("Starting new custom script %s", script_name);
		}

		// A label child must originate from a live CLEO custom script. Do not
		// read the CLEO marker byte from an arbitrary native CRunningScript.
		CCustomScript *parent = label != 0
			? GetInstance().ScriptEngine.FindCustomScriptByPtr(fromThread)
			: nullptr;
		if (label != 0 && (!parent || !parent->IsCustom()))
		{
			TRACE("Rejected custom child creation: parent is not a registered CLEO script");
			if (fromThread)
			{
				SetScriptCondResult(fromThread, false);
				SkipUnusedParameters(fromThread);
			}
			return nullptr;
		}

		char cwd[MAX_PATH];
		_getcwd(cwd, sizeof(cwd));
		_chdir(cleo_dir);
		// if "label == 0" then "script_name" need to be the file name
		auto cs = new CCustomScript(script_name, false, parent, label);
		if (fromThread) SetScriptCondResult(fromThread, cs && cs->IsOK());
		if (cs && cs->IsOK())
		{
			GetInstance().ScriptEngine.AddCustomScript(cs);
			if (fromThread) TransmitScriptParams(fromThread, cs);
			if (parent && label != 0)
				GetInstance().ScriptEngine.RestorePendingChildScript(parent, cs, label);
		}
		else
		{
			if (cs)
			{
				delete cs;
				cs = nullptr;
			}
			if (fromThread) SkipUnusedParameters(fromThread);
			TRACE("Failed to load script '%s'.", script_name);
		}
		_chdir(cwd);
		return cs;
	}

	CRunningScript* WINAPI CLEO_GetLastCreatedCustomScript()
	{
		return lastScriptCreated;
	}

	void WINAPI CLEO_AddScriptDeleteDelegate(FuncScriptDeleteDelegateT func)
	{
		scriptDeleteDelegate += func;
	}

	void WINAPI CLEO_RemoveScriptDeleteDelegate(FuncScriptDeleteDelegateT func)
	{
		scriptDeleteDelegate -= func;
	}

	BOOL WINAPI CLEO_RegisterCallback(int callbackId, uintptr_t callback)
	{
		return RegisterCallback(callbackId, callback);
	}

	BOOL WINAPI CLEO_UnregisterCallback(int callbackId, uintptr_t callback)
	{
		return UnregisterCallback(callbackId, callback);
	}

}