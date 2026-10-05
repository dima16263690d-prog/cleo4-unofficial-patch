#include "stdafx.h"
#include "cleo.h"
#include "CSoundSystem.h"

namespace CLEO
{
    static CAudioStream* ResolveAudioStream(DWORD handle)
    {
        if (handle == 0)
            return nullptr;

        auto stream = reinterpret_cast<CAudioStream*>(handle);
        return GetInstance().SoundSystem.HasStream(stream) ? stream : nullptr;
    }
}

extern "C"
{
    BOOL WINAPI CLEO_Audio_IsValidStream(DWORD handle)
    {
        return CLEO::ResolveAudioStream(handle) ? TRUE : FALSE;
    }

    DWORD WINAPI CLEO_Audio_GetState(DWORD handle)
    {
        auto stream = CLEO::ResolveAudioStream(handle);
        return stream ? stream->GetState() : static_cast<DWORD>(-1);
    }

    void WINAPI CLEO_Audio_SetState(DWORD handle, DWORD action)
    {
        auto stream = CLEO::ResolveAudioStream(handle);
        if (!stream)
            return;

        switch (action)
        {
        case 0: stream->Stop();   break;
        case 1: stream->Play();   break;
        case 2: stream->Pause();  break;
        case 3: stream->Resume(); break;
        default:
            break;
        }
    }

    float WINAPI CLEO_Audio_GetLength(DWORD handle)
    {
        auto stream = CLEO::ResolveAudioStream(handle);
        return stream ? stream->GetLengthSeconds() : 0.0f;
    }

    float WINAPI CLEO_Audio_GetDuration(DWORD handle)
    {
        auto stream = CLEO::ResolveAudioStream(handle);
        if (!stream)
            return 0.0f;

        const float speed = stream->GetSpeed();
        if (speed <= 0.0f)
            return FLT_MAX;

        return stream->GetLengthSeconds() / speed;
    }

    float WINAPI CLEO_Audio_GetVolume(DWORD handle)
    {
        auto stream = CLEO::ResolveAudioStream(handle);
        return stream ? stream->GetVolume() : 0.0f;
    }

    void WINAPI CLEO_Audio_SetVolume(DWORD handle, float value, float transitionSeconds)
    {
        auto stream = CLEO::ResolveAudioStream(handle);
        if (stream)
            stream->SetVolume(value);
        (void)transitionSeconds;
    }

    void WINAPI CLEO_Audio_SetVolumeTransition(DWORD handle, float value, float transitionSeconds)
    {
        auto stream = CLEO::ResolveAudioStream(handle);
        if (stream)
        {
            // The legacy setter remains immediate. This API is the explicit
            // modern transition path used by Audio.cleo.
            stream->SetSpeed(stream->GetSpeed(), 0.0f);
            stream->SetVolume(value);
        }
        (void)transitionSeconds;
    }

    float WINAPI CLEO_Audio_GetSpeed(DWORD handle)
    {
        auto stream = CLEO::ResolveAudioStream(handle);
        return stream ? stream->GetSpeed() : 0.0f;
    }

    void WINAPI CLEO_Audio_SetSpeed(DWORD handle, float value, float transitionSeconds)
    {
        auto stream = CLEO::ResolveAudioStream(handle);
        if (stream)
            stream->SetSpeed(value, transitionSeconds);
    }

    float WINAPI CLEO_Audio_GetProgress(DWORD handle)
    {
        auto stream = CLEO::ResolveAudioStream(handle);
        return stream ? stream->GetProgress() : 0.0f;
    }

    void WINAPI CLEO_Audio_SetProgress(DWORD handle, float value)
    {
        auto stream = CLEO::ResolveAudioStream(handle);
        if (stream)
            stream->SetProgress(value);
    }

    BOOL WINAPI CLEO_Audio_GetLooping(DWORD handle)
    {
        auto stream = CLEO::ResolveAudioStream(handle);
        return stream && stream->GetLooping() ? TRUE : FALSE;
    }

    void WINAPI CLEO_Audio_SetLooping(DWORD handle, BOOL enable)
    {
        auto stream = CLEO::ResolveAudioStream(handle);
        if (stream)
            stream->SetLooping(enable != FALSE);
    }

    DWORD WINAPI CLEO_Audio_GetType(DWORD handle)
    {
        auto stream = CLEO::ResolveAudioStream(handle);
        return stream ? static_cast<DWORD>(stream->GetType()) : static_cast<DWORD>(StreamTypeNone);
    }

    void WINAPI CLEO_Audio_SetType(DWORD handle, DWORD type)
    {
        auto stream = CLEO::ResolveAudioStream(handle);
        if (stream)
            stream->SetType(static_cast<eStreamType>(type));
    }

    BOOL WINAPI CLEO_Audio_Set3dSourceSize(DWORD handle, float radius)
    {
        auto stream = CLEO::ResolveAudioStream(handle);
        if (!stream || !stream->Is3d())
            return FALSE;

        stream->Set3dSourceSize(radius);
        return TRUE;
    }
}
