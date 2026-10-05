#include "CLEO.h"
#include "plugin.h"
#include <windows.h>

namespace
{
    constexpr DWORD STREAM_TYPE_NONE = 0;
    constexpr DWORD STREAM_TYPE_SFX = 1;
    constexpr DWORD STREAM_TYPE_MUSIC = 2;
    constexpr DWORD STREAM_TYPE_UI = 3;

    DWORD ReadStream(CScriptThread* thread)
    {
        return CLEO_GetIntOpcodeParam(thread);
    }

    bool ValidStream(DWORD stream)
    {
        return stream != 0 && CLEO_Audio_IsValidStream(stream) != FALSE;
    }

    class Audio
    {
    public:
        Audio()
        {
            if (CLEO_GetVersion() < CLEO_VERSION)
                return;

            // Keep the audio directory owned by the Audio plugin.
            // Do not depend on core CLEO TRACE/logging APIs here.
            CreateDirectoryA("cleo", nullptr);
            CreateDirectoryA("cleo\\audio", nullptr);

            CLEO_RegisterOpcode(0x2500, opcode_2500);
            CLEO_RegisterOpcode(0x2501, opcode_2501);
            CLEO_RegisterOpcode(0x2502, opcode_2502);
            CLEO_RegisterOpcode(0x2503, opcode_2503);
            CLEO_RegisterOpcode(0x2504, opcode_2504);
            CLEO_RegisterOpcode(0x2505, opcode_2505);
            CLEO_RegisterOpcode(0x2506, opcode_2506);
            CLEO_RegisterOpcode(0x2507, opcode_2507);
            CLEO_RegisterOpcode(0x2508, opcode_2508);
            CLEO_RegisterOpcode(0x2509, opcode_2509);
            CLEO_RegisterOpcode(0x250A, opcode_250A);
            CLEO_RegisterOpcode(0x250B, opcode_250B);
            CLEO_RegisterOpcode(0x250C, opcode_250C);
        }

    private:
        static OpcodeResult WINAPI opcode_2500(CScriptThread* thread)
        {
            const DWORD stream = ReadStream(thread);
            const bool playing = ValidStream(stream) && CLEO_Audio_GetState(stream) == 1;
            CLEO_SetThreadCondResult(thread, playing ? TRUE : FALSE);
            return OR_CONTINUE;
        }

        static OpcodeResult WINAPI opcode_2501(CScriptThread* thread)
        {
            const DWORD stream = ReadStream(thread);
            const float duration = ValidStream(stream) ? CLEO_Audio_GetDuration(stream) : 0.0f;
            CLEO_SetFloatOpcodeParam(thread, duration);
            return OR_CONTINUE;
        }

        static OpcodeResult WINAPI opcode_2502(CScriptThread* thread)
        {
            const DWORD stream = ReadStream(thread);
            const float speed = ValidStream(stream) ? CLEO_Audio_GetSpeed(stream) : 0.0f;
            CLEO_SetFloatOpcodeParam(thread, speed);
            return OR_CONTINUE;
        }

        static OpcodeResult WINAPI opcode_2503(CScriptThread* thread)
        {
            const DWORD stream = ReadStream(thread);
            const float speed = CLEO_GetFloatOpcodeParam(thread);
            if (ValidStream(stream))
                CLEO_Audio_SetSpeed(stream, speed, 0.0f);
            return OR_CONTINUE;
        }

        static OpcodeResult WINAPI opcode_2504(CScriptThread* thread)
        {
            const DWORD stream = ReadStream(thread);
            const float volume = CLEO_GetFloatOpcodeParam(thread);
            const DWORD timeMs = CLEO_GetIntOpcodeParam(thread);
            if (ValidStream(stream))
                CLEO_Audio_SetVolumeTransition(stream, volume, static_cast<float>(timeMs) * 0.001f);
            return OR_CONTINUE;
        }

        static OpcodeResult WINAPI opcode_2505(CScriptThread* thread)
        {
            const DWORD stream = ReadStream(thread);
            const float speed = CLEO_GetFloatOpcodeParam(thread);
            const DWORD timeMs = CLEO_GetIntOpcodeParam(thread);
            if (ValidStream(stream))
                CLEO_Audio_SetSpeed(stream, speed, static_cast<float>(timeMs) * 0.001f);
            return OR_CONTINUE;
        }

        static OpcodeResult WINAPI opcode_2506(CScriptThread* thread)
        {
            const DWORD stream = ReadStream(thread);
            const float radius = CLEO_GetFloatOpcodeParam(thread);
            if (ValidStream(stream))
                CLEO_Audio_Set3dSourceSize(stream, radius);
            return OR_CONTINUE;
        }

        static OpcodeResult WINAPI opcode_2507(CScriptThread* thread)
        {
            const DWORD stream = ReadStream(thread);
            const float progress = ValidStream(stream) ? CLEO_Audio_GetProgress(stream) : 0.0f;
            CLEO_SetFloatOpcodeParam(thread, progress);
            return OR_CONTINUE;
        }

        static OpcodeResult WINAPI opcode_2508(CScriptThread* thread)
        {
            const DWORD stream = ReadStream(thread);
            const float progress = CLEO_GetFloatOpcodeParam(thread);
            if (ValidStream(stream))
                CLEO_Audio_SetProgress(stream, progress);
            return OR_CONTINUE;
        }

        static OpcodeResult WINAPI opcode_2509(CScriptThread* thread)
        {
            const DWORD stream = ReadStream(thread);
            const DWORD type = ValidStream(stream) ? CLEO_Audio_GetType(stream) : STREAM_TYPE_NONE;
            CLEO_SetIntOpcodeParam(thread, type);
            return OR_CONTINUE;
        }

        static OpcodeResult WINAPI opcode_250A(CScriptThread* thread)
        {
            const DWORD stream = ReadStream(thread);
            const DWORD type = CLEO_GetIntOpcodeParam(thread);
            if (ValidStream(stream))
                CLEO_Audio_SetType(stream, type);
            return OR_CONTINUE;
        }

        static OpcodeResult WINAPI opcode_250B(CScriptThread* thread)
        {
            const DWORD stream = ReadStream(thread);
            float seconds = 0.0f;
            if (ValidStream(stream))
            {
                const float length = CLEO_Audio_GetLength(stream);
                seconds = CLEO_Audio_GetProgress(stream) * length;
            }
            CLEO_SetFloatOpcodeParam(thread, seconds);
            return OR_CONTINUE;
        }

        static OpcodeResult WINAPI opcode_250C(CScriptThread* thread)
        {
            const DWORD stream = ReadStream(thread);
            const float seconds = CLEO_GetFloatOpcodeParam(thread);
            if (ValidStream(stream))
            {
                const float length = CLEO_Audio_GetLength(stream);
                if (length > 0.0f)
                    CLEO_Audio_SetProgress(stream, seconds / length);
            }
            return OR_CONTINUE;
        }
    };

    Audio g_audio;
}
