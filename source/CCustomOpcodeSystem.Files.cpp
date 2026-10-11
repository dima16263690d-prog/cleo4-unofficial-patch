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
	//0A9A=3,%3d% = openfile %1d% mode %2d% // IF and SET
	OpcodeResult __stdcall opcode_0A9A(CRunningScript *thread)
	{
		const char *fname = readString(thread);
		auto paramType = *thread->GetBytePointer();
		char mode[0x10];

		// either CLEO 3 or CLEO 4 made a big mistake! (they differ in one major unapparent preference)
		// lets try to resolve this with a legacy mode
		auto cs = (CCustomScript*)thread;
		bool bLegacyMode = cs->IsCustom() && cs->GetCompatibility() < CLEO_VER_4_3;

		if (paramType >= 1 && paramType <= 8)
		{
			// integer param (for backward compatibility with CLEO 3)
			union
			{
				DWORD uParam;
				char strParam[4];
			} param;
			*thread >> param.uParam;
			strcpy(mode, param.strParam);
		}
		else
		{
			// string param
			GetScriptStringParam(thread, mode, sizeof(mode));
		}

		if (auto hfile = open_file(fname, mode, bLegacyMode))
		{
			GetInstance().OpcodeSystem.m_hFiles.insert(hfile);

			*thread << hfile;
			SetScriptCondResult(thread, true);
		}
		else
		{
			*thread << NULL;
			SetScriptCondResult(thread, false);
		}

		char szBlah[MAX_PATH];
		_getcwd(szBlah, MAX_PATH);

		return OR_CONTINUE;
	}

	//0A9B=1,closefile %1d%
	OpcodeResult __stdcall opcode_0A9B(CRunningScript *thread)
	{
		DWORD hFile;
		*thread >> hFile;
		if (convert_handle_to_file(hFile))
		{
			close_file(hFile);
			GetInstance().OpcodeSystem.m_hFiles.erase(hFile);
		}
		return OR_CONTINUE;
	}

	//0A9C=2,%2d% = file %1d% size
	OpcodeResult __stdcall opcode_0A9C(CRunningScript *thread)
	{
		DWORD hFile;
		*thread >> hFile;
		if (convert_handle_to_file(hFile)) *thread << file_get_size(hFile);
		return OR_CONTINUE;
	}

	//0A9D=3,readfile %1d% size %2d% to %3d%
	OpcodeResult __stdcall opcode_0A9D(CRunningScript *thread)
	{
		DWORD hFile;
		DWORD size;
		void *buf;
		*thread >> hFile >> size;
		buf = GetScriptParamPointer(thread);
		if (convert_handle_to_file(hFile)) read_file(buf, size, 1, hFile);
		return OR_CONTINUE;
	}

	//0A9E=3,writefile %1d% size %2d% from %3d%
	OpcodeResult __stdcall opcode_0A9E(CRunningScript *thread)
	{
		DWORD hFile;
		DWORD size;
		const void *buf;
		*thread >> hFile >> size;
		buf = GetScriptParamPointer(thread);
		if (convert_handle_to_file(hFile))
		{
			write_file(buf, size, 1, hFile);
			flush_file(hFile);
		}
		return OR_CONTINUE;
	}

	//0AAB=1,  file_exists %1d%
	OpcodeResult __stdcall opcode_0AAB(CRunningScript *thread)
	{
		DWORD fAttr = GetFileAttributes(readString(thread));
		SetScriptCondResult(thread, (fAttr != INVALID_FILE_ATTRIBUTES) && !(fAttr & FILE_ATTRIBUTE_DIRECTORY));
		return OR_CONTINUE;
	}

	//0AD5=3,file %1d% seek %2d% from_origin %3d% //IF and SET
	OpcodeResult __stdcall opcode_0AD5(CRunningScript *thread)
	{
		DWORD hFile;
		int seek, origin;
		*thread >> hFile >> seek >> origin;
		if (convert_handle_to_file(hFile)) SetScriptCondResult(thread, fseek(convert_handle_to_file(hFile), seek, origin) == 0);
		else SetScriptCondResult(thread, false);
		return OR_CONTINUE;
	}

	//0AD6=1,end_of_file %1d% reached
	OpcodeResult __stdcall opcode_0AD6(CRunningScript *thread)
	{
		DWORD hFile;
		*thread >> hFile;
		if (FILE *file = convert_handle_to_file(hFile))
			SetScriptCondResult(thread, ferror(file) || feof(file) != 0);
		else
			SetScriptCondResult(thread, true);
		return OR_CONTINUE;
	}

	//0AD7=3,read_string_from_file %1d% to %2d% size %3d% //IF and SET
	OpcodeResult __stdcall opcode_0AD7(CRunningScript *thread)
	{
		DWORD hFile;
		char *buf;
		DWORD size;
		*thread >> hFile;
		if (*thread->GetBytePointer() >= 1 && *thread->GetBytePointer() <= 8) *thread >> buf;
		else buf = (char *)GetScriptParamPointer(thread);
		*thread >> size;
		if (convert_handle_to_file(hFile)) SetScriptCondResult(thread, fgets(buf, size, convert_handle_to_file(hFile)) == buf);
		else SetScriptCondResult(thread, false);
		return OR_CONTINUE;
	}

	//0AD8=2,write_string_to_file %1d% from %2d% //IF and SET
	OpcodeResult __stdcall opcode_0AD8(CRunningScript *thread)
	{
		DWORD hFile;
		*thread >> hFile;
		if (FILE * file = convert_handle_to_file(hFile))
		{
			SetScriptCondResult(thread, fputs(readString(thread), file) > 0);
			fflush(file);
		}
		else {
			SetScriptCondResult(thread, false);
		}
		return OR_CONTINUE;
	}

	//0AD9=-1,write_formated_text %2d% to_file %1d%
	OpcodeResult __stdcall opcode_0AD9(CRunningScript *thread)
	{
		char fmt[MAX_STR_LEN]; char text[MAX_STR_LEN];
		DWORD hFile;
		*thread >> hFile;
		readString(thread, fmt, sizeof(fmt));
		format(thread, text, sizeof(text), fmt);
		if (FILE * file = convert_handle_to_file(hFile))
		{
			fputs(text, file);
			fflush(file);
		}
		SkipUnusedParameters(thread);
		return OR_CONTINUE;
	}

	//0ADA=-1,%3d% = scan_file %1d% format %2d% //IF and SET
	OpcodeResult __stdcall opcode_0ADA(CRunningScript *thread)
	{
		DWORD hFile;
		*thread >> hFile;
		char *fmt = readString(thread);
		int *result = (int *)GetScriptParamPointer(thread);


		size_t cExParams = 0;
		SCRIPT_VAR *ExParams[35];
		// read extra params
		while (*thread->GetBytePointer()) ExParams[cExParams++] = GetScriptParamPointer(thread);
		thread->IncPtr();

		if (FILE *file = convert_handle_to_file(hFile))
		{
			*result = fscanf(file, fmt,
							 /* extra parameters (will be aligned automatically, but the limit of 35 elements maximum exists) */
							 ExParams[0], ExParams[1], ExParams[2], ExParams[3], ExParams[4], ExParams[5],
							 ExParams[6], ExParams[7], ExParams[8], ExParams[9], ExParams[10], ExParams[11],
							 ExParams[12], ExParams[13], ExParams[14], ExParams[15], ExParams[16], ExParams[17],
							 ExParams[18], ExParams[19], ExParams[20], ExParams[21], ExParams[22], ExParams[23],
							 ExParams[24], ExParams[25], ExParams[26], ExParams[27], ExParams[28], ExParams[29],
							 ExParams[30], ExParams[31], ExParams[32], ExParams[33], ExParams[34]);
		}
		SetScriptCondResult(thread, cExParams == *result);
		return OR_CONTINUE;
	}

	//0AE4=1,  directory_exist %1d%
	OpcodeResult __stdcall opcode_0AE4(CRunningScript *thread)
	{
		auto fAttr = GetFileAttributes(readString(thread));
		SetScriptCondResult(thread, (fAttr != INVALID_FILE_ATTRIBUTES) && (fAttr & FILE_ATTRIBUTE_DIRECTORY));
		return OR_CONTINUE;
	}

	//0AE5=1,create_directory %1d% //IF and SET
	OpcodeResult __stdcall opcode_0AE5(CRunningScript *thread)
	{
		bool condResult = CreateDirectory(readString(thread), NULL) != 0;
		SetScriptCondResult(thread, condResult);
		return OR_CONTINUE;
	}

	//0AE6=3,%2d% = find_first_file %1d% get_filename_to %3d% //IF and SET
	OpcodeResult __stdcall opcode_0AE6(CRunningScript *thread)
	{
		WIN32_FIND_DATA ffd;
		memset(&ffd, 0, sizeof(ffd));

		HANDLE handle = FindFirstFile(readString(thread), &ffd);
		*thread << handle;
		GetInstance().OpcodeSystem.m_hFileSearches.insert(handle);
		if (handle != INVALID_HANDLE_VALUE)
		{
			auto type = *thread->GetBytePointer();
			char* str;
			switch (type)
			{
			case DT_VAR_STRING:
			case DT_LVAR_STRING:
			case DT_VAR_STRING_ARRAY:
			case DT_LVAR_STRING_ARRAY:
				str = (char*)GetScriptParamPointer(thread);
				memcpy(str, ffd.cFileName, 16);
				str[15] = '\0';
				break;
			case DT_VAR_TEXTLABEL:
			case DT_LVAR_TEXTLABEL:
			case DT_VAR_TEXTLABEL_ARRAY:
			case DT_LVAR_TEXTLABEL_ARRAY:
				str = (char*)GetScriptParamPointer(thread);
				memcpy(str, ffd.cFileName, 8);
				str[7] = '\0';
				break;
			default:
				*thread >> str;
				if (str)
					strncpy_s(str, 16, ffd.cFileName, _TRUNCATE);
			}
			SetScriptCondResult(thread, true);
		}
		else
		{
			readString(thread);
			SetScriptCondResult(thread, false);
		}
		return OR_CONTINUE;
	}

	//0AE7=2,%2d% = find_next_file %1d% //IF and SET
	OpcodeResult __stdcall opcode_0AE7(CRunningScript *thread)
	{
		WIN32_FIND_DATA ffd;
		memset(&ffd, 0, sizeof(ffd));

		HANDLE handle;
		*thread >> handle;
		if (FindNextFile(handle, &ffd))
		{
			auto type = *thread->GetBytePointer();
			char* str;
			switch (type)
			{
			case DT_VAR_STRING:
			case DT_LVAR_STRING:
			case DT_VAR_STRING_ARRAY:
			case DT_LVAR_STRING_ARRAY:
				str = (char*)GetScriptParamPointer(thread);
				memcpy(str, ffd.cFileName, 16);
				str[15] = '\0';
				break;
			case DT_VAR_TEXTLABEL:
			case DT_LVAR_TEXTLABEL:
			case DT_VAR_TEXTLABEL_ARRAY:
			case DT_LVAR_TEXTLABEL_ARRAY:
				str = (char*)GetScriptParamPointer(thread);
				memcpy(str, ffd.cFileName, 8);
				str[7] = '\0';
				break;
			default:
				*thread >> str;
				if (str)
					strncpy_s(str, 16, ffd.cFileName, _TRUNCATE);
			}
			SetScriptCondResult(thread, true);
		}
		else
		{
			readString(thread);
			SetScriptCondResult(thread, false);
		}
		return OR_CONTINUE;
	}

	//0AE8=1,find_close %1d%
	OpcodeResult __stdcall opcode_0AE8(CRunningScript *thread)
	{
		HANDLE handle;
		*thread >> handle;
		FindClose(handle);
		GetInstance().OpcodeSystem.m_hFileSearches.erase(handle);
		return OR_CONTINUE;
	}
}
