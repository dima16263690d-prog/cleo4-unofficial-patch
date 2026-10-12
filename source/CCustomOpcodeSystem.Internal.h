#pragma once

// Private shared declarations for the built-in opcode implementation.
// Include this after stdafx.h and the CLEO/game headers.

namespace CLEO
{
    extern const char * (__cdecl * GetUserDirectory)();
    extern void (__cdecl * ChangeToUserDir)();
    extern void (__cdecl * ChangeToProgramDir)(const char *);
    extern float (__cdecl * FindGroundZ)(float x, float y);
    extern CMarker * RadarBlips;
    extern CPlayerPed * (__cdecl * GetPlayerPed)(DWORD);
    extern CBaseModelInfo **Models;
    extern void (__cdecl * SpawnCar)(DWORD);

    CPedPool& GetPedPool();
    CVehiclePool& GetVehiclePool();
    CObjectPool& GetObjectPool();

    char *readString(CRunningScript *thread, char *buf = nullptr, BYTE size = 0);
    int format(CRunningScript *thread, char *str, size_t len, const char *format);

    DWORD open_file(const char *szPath, const char *szMode, bool bLegacy);
    void close_file(DWORD dwHandle);
    DWORD file_get_size(DWORD file_handle);
    DWORD read_file(void *buf, DWORD size, DWORD count, DWORD hFile);
    DWORD write_file(const void *buf, DWORD size, DWORD count, DWORD hFile);
    void flush_file(DWORD dwHandle);
    void ThreadJump(CRunningScript *thread, int off);
    void SkipUnusedParameters(CRunningScript *thread);

    // Per-opcode observation on the ACTIVE dispatch path. GTA's script loop
    // reads opcodes itself, so the legacy ExecuteLegacyScriptLoop (which used to
    // host the debug callbacks and profiler) is not installed. These helpers
    // install a light wrapper around GTA's opcode handler so DebugUtils opcode
    // tracing / hang-guard and per-script opcode counts actually observe
    // execution. They are no-ops when nothing is observing.
    void InstallOpcodeObservation();
    void RemoveOpcodeObservation();

    	inline CRunningScript& operator>>(CRunningScript& thread, DWORD& uval)
    	{
    		GetScriptParams(&thread, 1);
    		uval = opcodeParams[0].dwParam;
    		return thread;
    	}
    
    	inline CRunningScript& operator<<(CRunningScript& thread, DWORD uval)
    	{
    		opcodeParams[0].dwParam = uval;
    		SetScriptParams(&thread, 1);
    		return thread;
    	}
    
    	inline CRunningScript& operator>>(CRunningScript& thread, int& nval)
    	{
    		GetScriptParams(&thread, 1);
    		nval = opcodeParams[0].nParam;
    		return thread;
    	}
    
    	inline CRunningScript& operator<<(CRunningScript& thread, int nval)
    	{
    		opcodeParams[0].nParam = nval;
    		SetScriptParams(&thread, 1);
    		return thread;
    	}
    
    	inline CRunningScript& operator>>(CRunningScript& thread, float& fval)
    	{
    		GetScriptParams(&thread, 1);
    		fval = opcodeParams[0].fParam;
    		return thread;
    	}
    
    	inline CRunningScript& operator<<(CRunningScript& thread, float fval)
    	{
    		opcodeParams[0].fParam = fval;
    		SetScriptParams(&thread, 1);
    		return thread;
    	}
    
    	inline CRunningScript& operator>>(CRunningScript& thread, CVector& vec)
    	{
    		GetScriptParams(&thread, 3);
    		vec.x = opcodeParams[0].fParam;
    		vec.y = opcodeParams[1].fParam;
    		vec.z = opcodeParams[2].fParam;
    		return thread;
    	}
    
    	inline CRunningScript& operator<<(CRunningScript& thread, const CVector& vec)
    	{
    		opcodeParams[0].fParam = vec.x;
    		opcodeParams[1].fParam = vec.y;
    		opcodeParams[2].fParam = vec.z;
    		SetScriptParams(&thread, 3);
    		return thread;
    	}
    
    	template<typename T>
    	inline CRunningScript& operator>>(CRunningScript& thread, T *& pval)
    	{
    		GetScriptParams(&thread, 1);
    		pval = reinterpret_cast<T *>(opcodeParams[0].pParam);
    		return thread;
    	}
    
    	template<typename T>
    	inline CRunningScript& operator<<(CRunningScript& thread, T *pval)
    	{
    		opcodeParams[0].pParam = (void *)(pval);
    		SetScriptParams(&thread, 1);
    		return thread;
    	}
    
    	inline CRunningScript& operator>>(CRunningScript& thread, memory_pointer& pval)
    	{
    		GetScriptParams(&thread, 1);
    		pval = opcodeParams[0].pParam;
    		return thread;
    	}
    
    	template<typename T>
    	inline CRunningScript& operator<<(CRunningScript& thread, memory_pointer pval)
    	{
    		opcodeParams[0].pParam = pval;
    		SetScriptParams(&thread, 1);
    		return thread;
    	}
    
}
