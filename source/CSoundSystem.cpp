#include "stdafx.h"
#include "CSoundSystem.h"
#include "bass.h"
#include "CDebugBridge.h"
#include "cleo.h"
#include <windows.h>

namespace CLEO
{
    HWND(__cdecl * CreateMainWindow)(HINSTANCE hinst);
    LRESULT(__stdcall * imp_DefWindowProc)(HWND wnd, UINT msg, WPARAM wparam, LPARAM lparam);

    HWND OnCreateMainWindow(HINSTANCE hinst)
    {
        if (HIWORD(BASS_GetVersion()) != BASSVERSION)
            Error("An incorrect version of bass.dll has been loaded");

        TRACE("Creating main window...");
        HWND wnd = CreateMainWindow(hinst);
        if (!GetInstance().SoundSystem.Init(wnd))
            TRACE("CSoundSystem::Init() failed. Error code: %d", BASS_ErrorGetCode());
        return wnd;
    }

    CPlaceable *camera;
    RwCamera **pRwCamera;
    bool *userPaused;
    bool *codePaused;

    LRESULT __stdcall HOOK_DefWindowProc(HWND wnd, UINT msg, WPARAM wparam, LPARAM lparam)
    {
        if (GetInstance().SoundSystem.Initialized())
        {
            if (*codePaused)
            {
                GetInstance().SoundSystem.PauseStreams();
            }
            else
            {
                switch (msg)
                {
                case WM_ACTIVATE:
                    if (wparam == 0)
                        GetInstance().SoundSystem.PauseStreams();
                    else if (wparam == 1)
                        GetInstance().SoundSystem.ResumeStreams();
                    break;

                case WM_KILLFOCUS:
                    GetInstance().SoundSystem.PauseStreams();
                    break;
                }
            }
        }

        return imp_DefWindowProc(wnd, msg, wparam, lparam);
    }

    void CSoundSystem::Inject(CCodeInjector& inj)
    {
        TRACE("Injecting SoundSystem...");

        CGameVersionManager& gvm = GetInstance().VersionManager;
        CreateMainWindow = gvm.TranslateMemoryAddress(MA_CREATE_MAIN_WINDOW_FUNCTION);

        GetInstance().HookSystem.InstallCall(
            inj,
            "CreateMainWindow",
            gvm.TranslateMemoryAddress(MA_CALL_CREATE_MAIN_WINDOW),
            (size_t)&OnCreateMainWindow
        );

        camera = gvm.TranslateMemoryAddress(MA_CAMERA);
        userPaused = gvm.TranslateMemoryAddress(MA_USER_PAUSE);
        codePaused = gvm.TranslateMemoryAddress(MA_CODE_PAUSE);
        pRwCamera = gvm.TranslateMemoryAddress(MA_RW_CAMERA_PP);

        auto addr = gvm.TranslateMemoryAddress(MA_DEF_WINDOW_PROC_PTR);
        static const auto pWindowProcHook = &HOOK_DefWindowProc;

        size_t originalWindowProc = 0;
        if (!GetInstance().HookSystem.InstallPointer(
                inj,
                "DefWindowProc",
                addr,
                (size_t)&pWindowProcHook,
                &originalWindowProc))
        {
            Error("CSoundSystem::Inject(): failed to install DefWindowProc pointer hook");
            return;
        }

        DWORD originalWindowProcTarget = 0;
        inj.MemoryRead((memory_pointer)originalWindowProc, originalWindowProcTarget);
        imp_DefWindowProc =
            reinterpret_cast<LRESULT(__stdcall *)(HWND, UINT, WPARAM, LPARAM)>(originalWindowProcTarget);
    }

    void EnumerateBassDevices(int& total, int& enabled, int& default_device)
    {
        TRACE("Listing audio devices:");

        BASS_DEVICEINFO info{};
        enabled = 0;
        default_device = -1;

        for (total = 0; BASS_GetDeviceInfo(total, &info); ++total)
        {
            const bool isEnabled = (info.flags & BASS_DEVICE_ENABLED) != 0;
            if (isEnabled)
                ++enabled;
            if (info.flags & BASS_DEVICE_DEFAULT)
                default_device = total;

            TRACE("  %d%s: %s", total, isEnabled ? "" : " (disabled)", info.name);
        }

        TRACE("Default audio device index: %d", default_device);
    }

    BASS_3DVECTOR pos(0, 0, 0);
    BASS_3DVECTOR vel(0, 0, 0);
    BASS_3DVECTOR front(0, -1.0f, 0);
    BASS_3DVECTOR top(0, 0, 1.0f);

    bool CSoundSystem::Init(HWND hwnd)
    {
        if (initialized)
            return true;

        int default_device, total_devices, enabled_devices;
        BASS_DEVICEINFO info{ nullptr, nullptr, 0 };

        EnumerateBassDevices(total_devices, enabled_devices, default_device);

        if (forceDevice != -1 &&
            BASS_GetDeviceInfo(forceDevice, &info) &&
            (info.flags & BASS_DEVICE_ENABLED))
        {
            default_device = forceDevice;
        }

        TRACE(
            "Found %d devices, %d enabled devices, selecting device %d (%s)",
            total_devices,
            enabled_devices,
            default_device,
            BASS_GetDeviceInfo(default_device, &info) ? info.name : "Unknown device"
        );

        // Keep the runtime on the updated BASS API while retaining CLEO 4's
        // existing single-device initialization model.
        BASS_SetConfig(BASS_CONFIG_FLOATDSP, TRUE);

        if (BASS_Init(default_device, 44100, BASS_DEVICE_3D, hwnd, nullptr) &&
            BASS_Set3DFactors(1.0f, 0.0f, 1.0f) &&
            BASS_Set3DPosition(&pos, &vel, &front, &top))
        {
            TRACE("SoundSystem initialized");

            DWORD floatable = BASS_StreamCreate(44100, 1, BASS_SAMPLE_FLOAT, NULL, NULL);
            if (floatable)
            {
                bUseFPAudio = true;
                TRACE("Floating-point audio supported");
                BASS_StreamFree(floatable);
            }
            else
            {
                bUseFPAudio = false;
                TRACE("Floating-point audio not supported");
            }

            if (BASS_GetInfo(&SoundDevice))
            {
                if (SoundDevice.flags & DSCAPS_EMULDRIVER)
                    TRACE("Audio drivers not installed - using DirectSound emulation");
                if (!SoundDevice.eax)
                    TRACE("Audio hardware acceleration disabled (no EAX)");
            }

            initialized = true;
            this->hwnd = hwnd;
            lastUpdateTick = GetTickCount();
            timeStep = 0.02f;
            BASS_Apply3D();
            return true;
        }

        Warning("Could not initialize BASS sound system. Error code: %d", BASS_ErrorGetCode());
        return false;
    }

    CAudioStream *CSoundSystem::LoadStream(const char *filename, bool in3d)
    {
        CAudioStream *result = in3d
            ? static_cast<CAudioStream*>(new C3DAudioStream(filename))
            : static_cast<CAudioStream*>(new CAudioStream(filename));

        if (result->OK)
        {
            streams.insert(result);
            return result;
        }

        delete result;
        return nullptr;
    }

    bool CSoundSystem::HasStream(CAudioStream *stream) const
    {
        return stream != nullptr && streams.find(stream) != streams.end();
    }

    void CSoundSystem::UnloadStream(CAudioStream *stream)
    {
        if (HasStream(stream))
        {
            streams.erase(stream);
            delete stream;
        }
        else
        {
            TRACE("Unloading of stream that is not in list of loaded streams");
        }
    }

    void CSoundSystem::UnloadAllStreams()
    {
        for (CAudioStream *stream : streams)
            delete stream;

        streams.clear();
    }

    void CSoundSystem::ResumeStreams()
    {
        paused = false;

        for (CAudioStream *stream : streams)
        {
            if (stream->state == CAudioStream::playing)
                stream->Resume();
        }
    }

    void CSoundSystem::PauseStreams()
    {
        paused = true;

        for (CAudioStream *stream : streams)
        {
            if (stream->state == CAudioStream::playing)
                stream->Pause(false);
        }
    }

    void CSoundSystem::Update()
    {
        if (*userPaused || *codePaused)
        {
            if (!paused)
                PauseStreams();
            return;
        }

        if (paused)
            ResumeStreams();

        const DWORD now = GetTickCount();
        if (lastUpdateTick != 0)
        {
            const DWORD elapsedMs = now - lastUpdateTick;
            timeStep = static_cast<float>(elapsedMs) * 0.001f;
            if (timeStep <= 0.0f || timeStep > 0.25f)
                timeStep = 0.02f;
        }
        else
        {
            timeStep = 0.02f;
        }
        lastUpdateTick = now;

        CMatrixLink *pMatrix = nullptr;
        CVector *pVec = nullptr;

        if (camera && camera->m_matrix)
        {
            pMatrix = camera->m_matrix;
            pVec = &pMatrix->pos;
        }
        else if (camera)
        {
            pVec = &camera->m_placement.m_vPosn;
        }

        if (pVec)
        {
            BASS_3DVECTOR cameraPos(pVec->y, pVec->z, pVec->x);
            BASS_3DVECTOR cameraFront(
                pMatrix ? pMatrix->at.y : 0.0f,
                pMatrix ? pMatrix->at.z : -1.0f,
                pMatrix ? pMatrix->at.x : 0.0f
            );
            BASS_3DVECTOR cameraTop(
                pMatrix ? pMatrix->up.y : 0.0f,
                pMatrix ? pMatrix->up.z : 0.0f,
                pMatrix ? pMatrix->up.x : 1.0f
            );

            BASS_Set3DPosition(
                &cameraPos,
                nullptr,
                pMatrix ? &cameraFront : nullptr,
                pMatrix ? &cameraTop : nullptr
            );
        }

        for (CAudioStream *stream : streams)
            stream->Process();

        BASS_Apply3D();
    }

    CAudioStream::CAudioStream()
        : streamInternal(0),
          state(paused),
          OK(false),
          is3d(false),
          rate(44100.0f),
          speed(1.0f),
          volume(1.0f),
          type(StreamTypeNone)
    {
    }

    CAudioStream::CAudioStream(const char *src)
        : CAudioStream()
    {
        unsigned flags = BASS_SAMPLE_SOFTWARE | BASS_STREAM_PRESCAN;
        if (GetInstance().SoundSystem.bUseFPAudio)
            flags |= BASS_SAMPLE_FLOAT;

        if (!(streamInternal = BASS_StreamCreateFile(FALSE, src, 0, 0, flags)) &&
            !(streamInternal = BASS_StreamCreateURL(src, 0, flags, 0, nullptr)))
        {
            TRACE("Loading audiostream %s failed. Error code: %d", src, BASS_ErrorGetCode());
            return;
        }

        BASS_ChannelGetAttribute(streamInternal, BASS_ATTRIB_FREQ, &rate);
        BASS_ChannelSetAttribute(streamInternal, BASS_ATTRIB_VOL, volume.Get());
        OK = true;
    }

    CAudioStream::~CAudioStream()
    {
        if (streamInternal)
        {
            BASS_StreamFree(streamInternal);
            streamInternal = 0;
        }
    }

    void CAudioStream::Play()
    {
        if (state == stopped)
            BASS_ChannelSetPosition(streamInternal, 0, BASS_POS_BYTE);

        BASS_ChannelPlay(streamInternal, FALSE);
        state = playing;
    }

    void CAudioStream::Pause(bool change_state)
    {
        if (BASS_ChannelIsActive(streamInternal) == BASS_ACTIVE_PLAYING ||
            BASS_ChannelIsActive(streamInternal) == BASS_ACTIVE_STALLED)
        {
            BASS_ChannelPause(streamInternal);
            if (change_state)
                state = paused;
        }
    }

    void CAudioStream::Stop()
    {
        BASS_ChannelPause(streamInternal);
        BASS_ChannelSetPosition(streamInternal, 0, BASS_POS_BYTE);
        state = stopped;
        speed.Finish();
        volume.Finish();
    }

    void CAudioStream::Resume()
    {
        if (state == stopped)
            BASS_ChannelSetPosition(streamInternal, 0, BASS_POS_BYTE);

        BASS_ChannelPlay(streamInternal, FALSE);
        state = playing;
    }

    DWORD CAudioStream::GetLength()
    {
        return static_cast<DWORD>(GetLengthSeconds());
    }

    float CAudioStream::GetLengthSeconds() const
    {
        if (!streamInternal)
            return 0.0f;

        const QWORD lengthBytes = BASS_ChannelGetLength(streamInternal, BASS_POS_BYTE);
        if (lengthBytes == static_cast<QWORD>(-1))
            return 0.0f;

        return static_cast<float>(BASS_ChannelBytes2Seconds(streamInternal, lengthBytes));
    }

    DWORD CAudioStream::GetState()
    {
        if (state == stopped)
            return static_cast<DWORD>(-1);

        switch (BASS_ChannelIsActive(streamInternal))
        {
        case BASS_ACTIVE_PLAYING:
        case BASS_ACTIVE_STALLED:
            return 1;
        case BASS_ACTIVE_PAUSED:
            return 2;
        case BASS_ACTIVE_STOPPED:
        default:
            return static_cast<DWORD>(-1);
        }
    }

    float CAudioStream::GetVolume()
    {
        return volume.Get();
    }

    void CAudioStream::SetVolume(float val)
    {
        volume.Finish();
        volume.Set(std::max(val, 0.0f), 0.0f);
        if (streamInternal)
            BASS_ChannelSetAttribute(streamInternal, BASS_ATTRIB_VOL, volume.Get());
    }

    void CAudioStream::Loop(bool enable)
    {
        SetLooping(enable);
    }

    HSTREAM CAudioStream::GetInternal()
    {
        return streamInternal;
    }

    float CAudioStream::GetSpeed() const
    {
        return speed.Get();
    }

    void CAudioStream::SetSpeed(float value, float transitionTime)
    {
        value = std::max(value, 0.0f);

        if (value > 0.0f && transitionTime > 0.0f && state != playing)
            Resume();

        speed.Set(value, transitionTime);

        if (transitionTime <= 0.0f && streamInternal)
        {
            const float effectiveSpeed = GetType() == StreamTypeNone ||
                                         GetType() == StreamTypeUserInterface
                ? speed.Get()
                : speed.Get() * std::max(CTimer::ms_fTimeScale, 0.0f);
            const float frequency = std::max(rate * effectiveSpeed, 0.000001f);
            BASS_ChannelSetAttribute(streamInternal, BASS_ATTRIB_FREQ, frequency);
        }
    }

    float CAudioStream::GetProgress() const
    {
        if (!streamInternal)
            return 0.0f;

        const QWORD positionBytes = BASS_ChannelGetPosition(streamInternal, BASS_POS_BYTE);
        const QWORD lengthBytes = BASS_ChannelGetLength(streamInternal, BASS_POS_BYTE);

        if (positionBytes == static_cast<QWORD>(-1) ||
            lengthBytes == static_cast<QWORD>(-1) ||
            lengthBytes == 0)
        {
            return 0.0f;
        }

        const double position = BASS_ChannelBytes2Seconds(streamInternal, positionBytes);
        const double total = BASS_ChannelBytes2Seconds(streamInternal, lengthBytes);
        if (total <= 0.0)
            return 0.0f;

        return std::clamp(static_cast<float>(position / total), 0.0f, 1.0f);
    }

    void CAudioStream::SetProgress(float value)
    {
        if (!streamInternal)
            return;

        value = std::clamp(value, 0.0f, 1.0f);
        const double seconds = GetLengthSeconds() * value;
        const QWORD bytePos = BASS_ChannelSeconds2Bytes(streamInternal, seconds);
        if (bytePos != static_cast<QWORD>(-1))
            BASS_ChannelSetPosition(streamInternal, bytePos, BASS_POS_BYTE);

        if (state == stopped)
            state = paused;
    }

    bool CAudioStream::GetLooping() const
    {
        if (!streamInternal)
            return false;

        return (BASS_ChannelFlags(streamInternal, 0, 0) & BASS_SAMPLE_LOOP) != 0;
    }

    void CAudioStream::SetLooping(bool enable)
    {
        if (streamInternal)
            BASS_ChannelFlags(
                streamInternal,
                enable ? BASS_SAMPLE_LOOP : 0,
                BASS_SAMPLE_LOOP
            );
    }

    eStreamType CAudioStream::GetType() const
    {
        return type;
    }

    void CAudioStream::SetType(eStreamType value)
    {
        switch (value)
        {
        case StreamTypeNone:
        case StreamTypeSoundEffect:
        case StreamTypeMusic:
        case StreamTypeUserInterface:
            type = value;
            break;

        default:
            type = StreamTypeNone;
            break;
        }
    }

    void CAudioStream::Set3dSourceSize(float radius)
    {
        // Not applicable to a normal 2D audio stream.
        (void)radius;
    }

    void CAudioStream::Process()
    {
        if (!streamInternal)
            return;

        if (state == playing &&
            BASS_ChannelIsActive(streamInternal) == BASS_ACTIVE_STOPPED)
        {
            state = stopped;
        }

        speed.Update(GetInstance().SoundSystem.timeStep);
        volume.Update(GetInstance().SoundSystem.timeStep);

        if (speed.Get() <= 0.0f && state == playing)
            Pause();

        const float masterSpeed =
            (type == StreamTypeSoundEffect || type == StreamTypeMusic)
                ? std::max(CTimer::ms_fTimeScale, 0.0f)
                : 1.0f;

        if (state == playing)
        {
            const float effectiveSpeed = std::max(speed.Get() * masterSpeed, 0.000001f);
            BASS_ChannelSetAttribute(
                streamInternal,
                BASS_ATTRIB_FREQ,
                std::max(rate * effectiveSpeed, 0.000001f)
            );
            BASS_ChannelSetAttribute(streamInternal, BASS_ATTRIB_VOL, volume.Get());
        }
    }

    void CAudioStream::Set3dPosition(const CVector& pos)
    {
        TRACE("Set3dPosition called on non-3D audio stream");
        (void)pos;
    }

    void CAudioStream::Link(CPlaceable *placable)
    {
        TRACE("Link called on non-3D audio stream");
        (void)placable;
    }

    C3DAudioStream::C3DAudioStream(const char *src)
        : CAudioStream(src),
          link(nullptr),
          position{0, 0, 0},
          sourceRadius(0.5f)
    {
        is3d = true;

        if (!streamInternal)
            return;

        // 3D streams must be mono and software-mixed, matching BASS's 3D requirements.
        BASS_ChannelSet3DAttributes(
            streamInternal,
            BASS_3DMODE_NORMAL,
            sourceRadius,
            -1.0f,
            -1,
            -1,
            -1.0f
        );
        BASS_ChannelSetAttribute(streamInternal, BASS_ATTRIB_VOL, 0.0f);

        // Recreate a 3D stream with the correct format flags if the generic
        // constructor could not create an appropriate channel.
        if (!BASS_ChannelSet3DAttributes(
                streamInternal,
                BASS_3DMODE_NORMAL,
                sourceRadius,
                -1.0f,
                -1,
                -1,
                -1.0f))
        {
            TRACE("Failed to configure 3D audio stream %p", streamInternal);
        }
    }

    C3DAudioStream::~C3DAudioStream()
    {
        // Base destructor owns and frees the BASS channel.
        link = nullptr;
    }

    void C3DAudioStream::Set3dPosition(const CVector& pos)
    {
        position.x = pos.x;
        position.y = pos.y;
        position.z = pos.z;
        link = nullptr;

        if (streamInternal)
        {
            BASS_3DVECTOR bassPos(pos.x, pos.z, pos.y);
            BASS_ChannelSet3DPosition(streamInternal, &bassPos, nullptr, nullptr);
        }
    }

    void C3DAudioStream::Link(CPlaceable *placable)
    {
        link = placable;
    }

    void C3DAudioStream::Set3dSourceSize(float radius)
    {
        sourceRadius = std::max(radius, 0.01f);

        if (streamInternal)
        {
            BASS_ChannelSet3DAttributes(
                streamInternal,
                BASS_3DMODE_NORMAL,
                sourceRadius,
                -1.0f,
                -1,
                -1,
                -1.0f
            );
        }
    }

    void C3DAudioStream::Process()
    {
        if (!streamInternal)
            return;

        if (state == playing &&
            BASS_ChannelIsActive(streamInternal) == BASS_ACTIVE_STOPPED)
        {
            state = stopped;
        }

        if (link && state == playing)
        {
            CVector *pVec = link->m_matrix
                ? &link->m_matrix->pos
                : &link->m_placement.m_vPosn;

            BASS_3DVECTOR linkPos(pVec->y, pVec->z, pVec->x);
            BASS_ChannelSet3DPosition(streamInternal, &linkPos, nullptr, nullptr);

            position.x = pVec->x;
            position.y = pVec->y;
            position.z = pVec->z;
        }
        else if (state == playing)
        {
            BASS_3DVECTOR bassPos(position.x, position.z, position.y);
            BASS_ChannelSet3DPosition(streamInternal, &bassPos, nullptr, nullptr);
        }

        // Use the common transition/speed/volume processing after the 3D update.
        CAudioStream::Process();
    }
}
