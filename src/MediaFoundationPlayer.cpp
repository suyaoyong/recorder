#include "MediaFoundationPlayer.h"

#include <mfreadwrite.h>
#include <mferror.h>

#include <algorithm>

class MediaFoundationPlayer::SessionCallback : public IMFAsyncCallback {
public:
    explicit SessionCallback(MediaFoundationPlayer* owner) : owner_(owner) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) {
            return E_POINTER;
        }
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFAsyncCallback)) {
            *ppv = static_cast<IMFAsyncCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&refCount_));
    }

    STDMETHODIMP_(ULONG) Release() override {
        ULONG value = static_cast<ULONG>(InterlockedDecrement(&refCount_));
        if (value == 0) {
            delete this;
        }
        return value;
    }

    STDMETHODIMP GetParameters(DWORD*, DWORD*) override {
        return E_NOTIMPL;
    }

    STDMETHODIMP Invoke(IMFAsyncResult* result) override {
        if (owner_) {
            owner_->HandleSessionEvent(result);
        }
        return S_OK;
    }

private:
    ~SessionCallback() = default;

    LONG refCount_ = 1;
    MediaFoundationPlayer* owner_ = nullptr;
};

MediaFoundationPlayer::MediaFoundationPlayer() = default;

MediaFoundationPlayer::~MediaFoundationPlayer() {
    Shutdown();
}

bool MediaFoundationPlayer::Initialize() {
    if (mfInitialized_) {
        return true;
    }
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        NotifyError(L"COM 初始化失败：" + DescribeHRESULTW(hr));
        return false;
    }
    comInitialized_ = (hr == S_OK || hr == S_FALSE);

    hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        NotifyError(L"Media Foundation 初始化失败：" + DescribeHRESULTW(hr));
        return false;
    }
    mfInitialized_ = true;
    return true;
}

void MediaFoundationPlayer::Shutdown() {
    CloseSession();
    if (mfInitialized_) {
        MFShutdown();
        mfInitialized_ = false;
    }
    if (comInitialized_) {
        CoUninitialize();
        comInitialized_ = false;
    }
}

void MediaFoundationPlayer::SafeRelease(IUnknown** unknown) {
    if (unknown && *unknown) {
        (*unknown)->Release();
        *unknown = nullptr;
    }
}

void MediaFoundationPlayer::SetListener(IPlaybackListener* listener) {
    listener_ = listener;
}

PlaybackState MediaFoundationPlayer::GetState() const {
    return state_;
}

int64_t MediaFoundationPlayer::GetDuration100ns() const {
    return duration100ns_;
}

int64_t MediaFoundationPlayer::GetPosition100ns() const {
    if (!presentationClock_) {
        return 0;
    }
    MFTIME time = 0;
    if (SUCCEEDED(presentationClock_->GetTime(&time))) {
        return static_cast<int64_t>(time);
    }
    return 0;
}

float MediaFoundationPlayer::GetVolume() const {
    return volume_;
}

bool MediaFoundationPlayer::SetVolume(float volume01) {
    volume01 = std::clamp(volume01, 0.0f, 1.0f);
    volume_ = volume01;
    if (!simpleVolume_) {
        return false;
    }
    return SUCCEEDED(simpleVolume_->SetMasterVolume(volume01));
}

void MediaFoundationPlayer::NotifyState(PlaybackState state) {
    state_ = state;
    if (listener_) {
        listener_->OnPlaybackStateChanged(state);
    }
}

void MediaFoundationPlayer::NotifyError(const std::wstring& message) {
    state_ = PlaybackState::Error;
    if (listener_) {
        listener_->OnPlaybackError(message);
    }
}

void MediaFoundationPlayer::CloseSession() {
    if (session_) {
        session_->Close();
        session_->Shutdown();
    }
    SafeRelease(reinterpret_cast<IUnknown**>(&simpleVolume_));
    SafeRelease(reinterpret_cast<IUnknown**>(&presentationClock_));
    SafeRelease(reinterpret_cast<IUnknown**>(&source_));
    SafeRelease(reinterpret_cast<IUnknown**>(&session_));
    topologyReady_ = false;
    pendingPlay_ = false;
    pendingPauseAfterStart_ = false;
    duration100ns_ = 0;
    currentPath_.clear();
    NotifyState(PlaybackState::Idle);
}

HRESULT MediaFoundationPlayer::CreateMediaSource(const std::filesystem::path& path, IMFMediaSource** source) {
    if (!source) {
        return E_POINTER;
    }
    *source = nullptr;
    IMFSourceResolver* resolver = nullptr;
    IUnknown* sourceUnk = nullptr;
    MF_OBJECT_TYPE objectType = MF_OBJECT_INVALID;

    HRESULT hr = MFCreateSourceResolver(&resolver);
    if (FAILED(hr)) {
        SafeRelease(reinterpret_cast<IUnknown**>(&resolver));
        return hr;
    }

    hr = resolver->CreateObjectFromURL(path.wstring().c_str(),
                                       MF_RESOLUTION_MEDIASOURCE,
                                       nullptr,
                                       &objectType,
                                       &sourceUnk);
    if (SUCCEEDED(hr)) {
        hr = sourceUnk->QueryInterface(IID_PPV_ARGS(source));
    }

    SafeRelease(reinterpret_cast<IUnknown**>(&sourceUnk));
    SafeRelease(reinterpret_cast<IUnknown**>(&resolver));
    return hr;
}

HRESULT MediaFoundationPlayer::CreateTopologyFromSource(IMFMediaSource* source, IMFTopology** topology) {
    if (!source || !topology) {
        return E_POINTER;
    }
    *topology = nullptr;

    IMFPresentationDescriptor* presentation = nullptr;
    HRESULT hr = source->CreatePresentationDescriptor(&presentation);
    if (FAILED(hr)) {
        SafeRelease(reinterpret_cast<IUnknown**>(&presentation));
        return hr;
    }

    IMFTopology* topo = nullptr;
    hr = MFCreateTopology(&topo);
    if (FAILED(hr)) {
        SafeRelease(reinterpret_cast<IUnknown**>(&presentation));
        return hr;
    }

    DWORD streamCount = 0;
    hr = presentation->GetStreamDescriptorCount(&streamCount);
    if (FAILED(hr)) {
        SafeRelease(reinterpret_cast<IUnknown**>(&presentation));
        SafeRelease(reinterpret_cast<IUnknown**>(&topo));
        return hr;
    }

    bool audioFound = false;
    for (DWORD i = 0; i < streamCount; ++i) {
        BOOL selected = FALSE;
        IMFStreamDescriptor* streamDesc = nullptr;
        hr = presentation->GetStreamDescriptorByIndex(i, &selected, &streamDesc);
        if (FAILED(hr)) {
            SafeRelease(reinterpret_cast<IUnknown**>(&streamDesc));
            continue;
        }

        IMFMediaTypeHandler* handler = nullptr;
        IMFMediaType* mediaType = nullptr;
        GUID majorType = GUID_NULL;
        if (SUCCEEDED(streamDesc->GetMediaTypeHandler(&handler)) &&
            SUCCEEDED(handler->GetMajorType(&majorType)) &&
            majorType == MFMediaType_Audio) {
            audioFound = true;
        } else {
            presentation->DeselectStream(i);
            SafeRelease(reinterpret_cast<IUnknown**>(&handler));
            SafeRelease(reinterpret_cast<IUnknown**>(&streamDesc));
            continue;
        }
        SafeRelease(reinterpret_cast<IUnknown**>(&handler));

        IMFTopologyNode* sourceNode = nullptr;
        IMFTopologyNode* outputNode = nullptr;
        IMFActivate* audioActivate = nullptr;

        hr = MFCreateTopologyNode(MF_TOPOLOGY_SOURCESTREAM_NODE, &sourceNode);
        if (FAILED(hr)) {
            SafeRelease(reinterpret_cast<IUnknown**>(&streamDesc));
            break;
        }

        hr = sourceNode->SetUnknown(MF_TOPONODE_SOURCE, source);
        if (SUCCEEDED(hr)) {
            hr = sourceNode->SetUnknown(MF_TOPONODE_PRESENTATION_DESCRIPTOR, presentation);
        }
        if (SUCCEEDED(hr)) {
            hr = sourceNode->SetUnknown(MF_TOPONODE_STREAM_DESCRIPTOR, streamDesc);
        }
        if (FAILED(hr)) {
            SafeRelease(reinterpret_cast<IUnknown**>(&sourceNode));
            SafeRelease(reinterpret_cast<IUnknown**>(&streamDesc));
            break;
        }

        hr = MFCreateAudioRendererActivate(&audioActivate);
        if (FAILED(hr)) {
            SafeRelease(reinterpret_cast<IUnknown**>(&sourceNode));
            SafeRelease(reinterpret_cast<IUnknown**>(&streamDesc));
            break;
        }

        hr = MFCreateTopologyNode(MF_TOPOLOGY_OUTPUT_NODE, &outputNode);
        if (SUCCEEDED(hr)) {
            hr = outputNode->SetObject(audioActivate);
        }
        if (SUCCEEDED(hr)) {
            hr = outputNode->SetUINT32(MF_TOPONODE_STREAMID, 0);
        }
        if (SUCCEEDED(hr)) {
            hr = outputNode->SetUINT32(MF_TOPONODE_NOSHUTDOWN_ON_REMOVE, FALSE);
        }
        if (FAILED(hr)) {
            SafeRelease(reinterpret_cast<IUnknown**>(&audioActivate));
            SafeRelease(reinterpret_cast<IUnknown**>(&sourceNode));
            SafeRelease(reinterpret_cast<IUnknown**>(&outputNode));
            SafeRelease(reinterpret_cast<IUnknown**>(&streamDesc));
            break;
        }

        hr = topo->AddNode(sourceNode);
        if (SUCCEEDED(hr)) {
            hr = topo->AddNode(outputNode);
        }
        if (SUCCEEDED(hr)) {
            hr = sourceNode->ConnectOutput(0, outputNode, 0);
        }

        SafeRelease(reinterpret_cast<IUnknown**>(&audioActivate));
        SafeRelease(reinterpret_cast<IUnknown**>(&sourceNode));
        SafeRelease(reinterpret_cast<IUnknown**>(&outputNode));
        SafeRelease(reinterpret_cast<IUnknown**>(&streamDesc));
        if (FAILED(hr)) {
            break;
        }
    }

    if (SUCCEEDED(hr) && !audioFound) {
        hr = MF_E_INVALIDMEDIATYPE;
    }

    if (SUCCEEDED(hr)) {
        *topology = topo;
    } else {
        SafeRelease(reinterpret_cast<IUnknown**>(&topo));
    }

    SafeRelease(reinterpret_cast<IUnknown**>(&presentation));
    return hr;
}

bool MediaFoundationPlayer::OpenFile(const std::filesystem::path& path) {
    if (!Initialize()) {
        return false;
    }
    CloseSession();
    NotifyState(PlaybackState::Opening);

    IMFMediaSource* source = nullptr;
    HRESULT hr = CreateMediaSource(path, &source);
    if (FAILED(hr)) {
        NotifyError(L"打开文件失败：" + DescribeHRESULTW(hr));
        SafeRelease(reinterpret_cast<IUnknown**>(&source));
        return false;
    }

    IMFMediaSession* session = nullptr;
    hr = MFCreateMediaSession(nullptr, &session);
    if (FAILED(hr)) {
        NotifyError(L"创建播放会话失败：" + DescribeHRESULTW(hr));
        SafeRelease(reinterpret_cast<IUnknown**>(&source));
        SafeRelease(reinterpret_cast<IUnknown**>(&session));
        return false;
    }

    IMFTopology* topology = nullptr;
    hr = CreateTopologyFromSource(source, &topology);
    if (FAILED(hr)) {
        NotifyError(L"创建播放拓扑失败：" + DescribeHRESULTW(hr));
        SafeRelease(reinterpret_cast<IUnknown**>(&topology));
        SafeRelease(reinterpret_cast<IUnknown**>(&source));
        SafeRelease(reinterpret_cast<IUnknown**>(&session));
        return false;
    }

    session_ = session;
    source_ = source;
    currentPath_ = path;

    if (!callback_) {
        callback_ = new SessionCallback(this);
    }
    EnsureEventPump();

    hr = session_->SetTopology(0, topology);
    SafeRelease(reinterpret_cast<IUnknown**>(&topology));
    if (FAILED(hr)) {
        NotifyError(L"设置播放拓扑失败：" + DescribeHRESULTW(hr));
        return false;
    }
    return true;
}

void MediaFoundationPlayer::EnsureEventPump() {
    if (session_) {
        session_->BeginGetEvent(callback_, nullptr);
    }
}

void MediaFoundationPlayer::Play() {
    if (!session_) {
        return;
    }
    if (!topologyReady_) {
        pendingPlay_ = true;
        return;
    }

    PROPVARIANT varStart;
    PropVariantInit(&varStart);
    if (state_ == PlaybackState::Paused) {
        varStart.vt = VT_EMPTY;
    } else {
        varStart.vt = VT_I8;
        varStart.hVal.QuadPart = 0;
    }
    HRESULT hr = session_->Start(&GUID_NULL, &varStart);
    PropVariantClear(&varStart);
    if (FAILED(hr)) {
        NotifyError(L"播放失败：" + DescribeHRESULTW(hr));
    }
}

void MediaFoundationPlayer::Pause() {
    if (!session_) {
        return;
    }
    HRESULT hr = session_->Pause();
    if (FAILED(hr)) {
        NotifyError(L"暂停失败：" + DescribeHRESULTW(hr));
    }
}

void MediaFoundationPlayer::Stop() {
    if (!session_) {
        return;
    }
    HRESULT hr = session_->Stop();
    if (FAILED(hr)) {
        NotifyError(L"停止失败：" + DescribeHRESULTW(hr));
    }
}

bool MediaFoundationPlayer::SeekTo(int64_t position100ns) {
    if (!session_) {
        return false;
    }
    if (!topologyReady_) {
        return false;
    }

    PROPVARIANT varStart;
    PropVariantInit(&varStart);
    varStart.vt = VT_I8;
    varStart.hVal.QuadPart = position100ns;
    if (state_ == PlaybackState::Paused) {
        pendingPauseAfterStart_ = true;
    }
    HRESULT hr = session_->Start(&GUID_NULL, &varStart);
    PropVariantClear(&varStart);
    if (FAILED(hr)) {
        NotifyError(L"定位失败：" + DescribeHRESULTW(hr));
        return false;
    }
    return true;
}

void MediaFoundationPlayer::HandleTopologyReady() {
    topologyReady_ = true;
    IMFPresentationClock* clock = nullptr;
    if (SUCCEEDED(session_->GetClock(reinterpret_cast<IMFClock**>(&clock))) && clock) {
        SafeRelease(reinterpret_cast<IUnknown**>(&presentationClock_));
        presentationClock_ = clock;
    } else {
        SafeRelease(reinterpret_cast<IUnknown**>(&clock));
    }

    SafeRelease(reinterpret_cast<IUnknown**>(&simpleVolume_));
    MFGetService(session_, MR_POLICY_VOLUME_SERVICE, IID_PPV_ARGS(&simpleVolume_));
    if (simpleVolume_) {
        simpleVolume_->SetMasterVolume(volume_);
    }

    if (source_) {
        IMFPresentationDescriptor* pd = nullptr;
        if (SUCCEEDED(source_->CreatePresentationDescriptor(&pd)) && pd) {
            UINT64 duration = 0;
            if (SUCCEEDED(pd->GetUINT64(MF_PD_DURATION, &duration))) {
                duration100ns_ = static_cast<int64_t>(duration);
            }
            SafeRelease(reinterpret_cast<IUnknown**>(&pd));
        }
    }
    if (listener_) {
        listener_->OnMediaOpened(duration100ns_);
    }

    if (pendingPlay_) {
        pendingPlay_ = false;
        Play();
    }
}

void MediaFoundationPlayer::HandleSessionEvent(IMFAsyncResult* result) {
    IMFMediaEvent* event = nullptr;
    HRESULT hr = session_->EndGetEvent(result, &event);
    if (FAILED(hr)) {
        NotifyError(L"读取播放事件失败：" + DescribeHRESULTW(hr));
        return;
    }

    MediaEventType type = MEUnknown;
    if (SUCCEEDED(event->GetType(&type))) {
        HRESULT status = S_OK;
        event->GetStatus(&status);
        if (FAILED(status)) {
            NotifyError(L"播放错误：" + DescribeHRESULTW(status));
        } else {
            switch (type) {
            case MESessionTopologyStatus: {
                UINT32 topoStatus = 0;
                if (SUCCEEDED(event->GetUINT32(MF_EVENT_TOPOLOGY_STATUS, &topoStatus)) &&
                    topoStatus == MF_TOPOSTATUS_READY) {
                    HandleTopologyReady();
                }
                break;
            }
            case MESessionStarted:
                NotifyState(PlaybackState::Playing);
                if (pendingPauseAfterStart_) {
                    pendingPauseAfterStart_ = false;
                    Pause();
                }
                break;
            case MESessionPaused:
                NotifyState(PlaybackState::Paused);
                break;
            case MESessionStopped:
                NotifyState(PlaybackState::Stopped);
                break;
            case MESessionEnded:
                NotifyState(PlaybackState::Ended);
                if (listener_) {
                    listener_->OnPlaybackEnded();
                }
                break;
            case MESessionClosed:
                NotifyState(PlaybackState::Idle);
                break;
            default:
                break;
            }
        }
    }

    SafeRelease(reinterpret_cast<IUnknown**>(&event));
    EnsureEventPump();
}
