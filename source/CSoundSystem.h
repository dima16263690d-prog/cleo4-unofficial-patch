#pragma once
#include "stdafx.h"
#include "CCodeInjector.h"
#include <set>
#include <algorithm>
#include "bass.h"

namespace CLEO
{
    class CAudioStream;
    class C3DAudioStream;

    class CInterpolatedValue
    {
        float current;
        float target;
        float remaining;
        float step;

    public:
        explicit CInterpolatedValue(float value = 0.0f)
            : current(value), target(value), remaining(0.0f), step(0.0f) {}

        float Get() const { return current; }

        void Set(float value, float transitionSeconds)
        {
            target = value;
            if (transitionSeconds <= 0.0f)
            {
                current = target;
                remaining = 0.0f;
                step = 0.0f;
                return;
            }

            remaining = transitionSeconds;
            step = (target - current) / transitionSeconds;
        }

        void Update(float deltaSeconds)
        {
            if (remaining <= 0.0f || deltaSeconds <= 0.0f)
                return;

            float dt = std::min(deltaSeconds, remaining);
            current += step * dt;
            remaining -= dt;

            if (remaining <= 0.0f)
            {
                current = target;
                remaining = 0.0f;
                step = 0.0f;
            }
        }

        void Finish()
        {
            current = target;
            remaining = 0.0f;
            step = 0.0f;
        }
    };

    enum eStreamType
    {
        StreamTypeNone = 0,
        StreamTypeSoundEffect = 1,
        StreamTypeMusic = 2,
        StreamTypeUserInterface = 3
    };

    class CSoundSystem : VInjectible
    {
        friend class CAudioStream;
        friend class C3DAudioStream;

        std::set<CAudioStream *> streams;
        BASS_INFO SoundDevice;
        bool initialized;
        int forceDevice;
        bool paused;
        bool bUseFPAudio;
        HWND hwnd;
        DWORD lastUpdateTick;
        float timeStep;

    public:
        virtual void Inject(CCodeInjector& inj);
        bool Init(HWND hwnd);
        inline bool Initialized() { return initialized; }

        CSoundSystem()
            : SoundDevice{},
              initialized(false),
              forceDevice(-1),
              paused(false),
              bUseFPAudio(false),
              hwnd(nullptr),
              lastUpdateTick(0),
              timeStep(0.02f)
        {
        }

        ~CSoundSystem()
        {
            TRACE("Closing SoundSystem...");
            UnloadAllStreams();
            if (initialized)
            {
                TRACE("Freeing BASS library");
                BASS_Free();
                initialized = false;
            }
            TRACE("SoundSystem closed!");
        }

        CAudioStream * LoadStream(const char *filename, bool in3d = false);
        void PauseStreams();
        void ResumeStreams();
        void UnloadStream(CAudioStream *stream);
        void UnloadAllStreams();
        void Update();

        bool HasStream(CAudioStream *stream) const;
    };

    class CAudioStream
    {
        friend class CSoundSystem;

        CAudioStream(const CAudioStream&) = delete;

    protected:
        HSTREAM streamInternal;
        enum eStreamState
        {
            no = 0,
            playing = 1,
            paused = 2,
            stopped = -1,
        } state;
        bool OK;
        bool is3d;
        float rate;
        CInterpolatedValue speed;
        CInterpolatedValue volume;
        eStreamType type;

        CAudioStream();

    public:
        CAudioStream(const char *src);
        virtual ~CAudioStream();

        // Legacy actions
        void Play();
        void Pause(bool change_state = true);
        void Stop();
        void Resume();
        DWORD GetLength();             // legacy-compatible integer seconds
        DWORD GetState();              // legacy-compatible: -1/1/2
        float GetVolume();
        void SetVolume(float val);
        void Loop(bool enable);
        HSTREAM GetInternal();

        // Extended audio API
        bool IsOk() const { return OK; }
        bool Is3d() const { return is3d; }
        float GetLengthSeconds() const;
        float GetSpeed() const;
        void SetSpeed(float value, float transitionTime = 0.0f);
        float GetProgress() const;
        void SetProgress(float value);
        bool GetLooping() const;
        void SetLooping(bool enable);
        eStreamType GetType() const;
        void SetType(eStreamType value);
        virtual void Set3dSourceSize(float radius);

        // overloadable actions
        virtual void Set3dPosition(const CVector& pos);
        virtual void Link(CPlaceable *placable = nullptr);
        virtual void Process();
    };

    class C3DAudioStream : public CAudioStream
    {
        friend class CSoundSystem;

        CPlaceable *link;
        BASS_3DVECTOR position;
        float sourceRadius;

    public:
        C3DAudioStream(const char *src);
        virtual ~C3DAudioStream();

        virtual void Set3dPosition(const CVector& pos);
        virtual void Link(CPlaceable *placable = nullptr);
        virtual void Set3dSourceSize(float radius);
        virtual void Process();
    };
}
