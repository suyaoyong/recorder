#pragma once

#include "HResultUtils.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <filesystem>
#include <string>

enum class PlaybackState {
    Idle,
    Opening,
    Playing,
    Paused,
    Stopped,
    Ended,
    Error
};

class IPlaybackListener {
public:
    virtual ~IPlaybackListener() = default;
    virtual void OnPlaybackStateChanged(PlaybackState state) = 0;
    virtual void OnMediaOpened(int64_t duration100ns) = 0;
    virtual void OnPlaybackEnded() = 0;
    virtual void OnPlaybackError(const std::wstring& message) = 0;
};

class MediaFoundationPlayer {
public:
    MediaFoundationPlayer();
    ~MediaFoundationPlayer();

    bool Initialize();
    void Shutdown();

    bool OpenFile(const std::filesystem::path& path);
    void Play();
    void Pause();
    void Stop();
    bool SeekTo(int64_t position100ns);

    int64_t GetDuration100ns() const;
    int64_t GetPosition100ns() const;

    bool SetVolume(float volume01);
    float GetVolume() const;

    PlaybackState GetState() const;
    void SetListener(IPlaybackListener* listener);

private:
    class SessionCallback;

    void HandleSessionEvent(IMFAsyncResult* result);
    void HandleTopologyReady();
    void NotifyState(PlaybackState state);
    void NotifyError(const std::wstring& message);
    void CloseSession();

    HRESULT CreateMediaSource(const std::filesystem::path& path, IMFMediaSource** source);
    HRESULT CreateTopologyFromSource(IMFMediaSource* source, IMFTopology** topology);

    void EnsureEventPump();

    void SafeRelease(IUnknown** unknown);

    SessionCallback* callback_ = nullptr;
    IMFMediaSession* session_ = nullptr;
    IMFMediaSource* source_ = nullptr;
    IMFPresentationClock* presentationClock_ = nullptr;
    IMFSimpleAudioVolume* simpleVolume_ = nullptr;

    IPlaybackListener* listener_ = nullptr;
    std::filesystem::path currentPath_;

    bool mfInitialized_ = false;
    bool comInitialized_ = false;
    bool topologyReady_ = false;
    bool pendingPlay_ = false;
    bool pendingPauseAfterStart_ = false;

    PlaybackState state_ = PlaybackState::Idle;
    int64_t duration100ns_ = 0;
    float volume_ = 0.8f;
};
