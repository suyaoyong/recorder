#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "LoopbackRecorder.h"
#include "SpscByteRing.h"
#include "HResultUtils.h"

#include <Audioclient.h>
#include <avrt.h>
#include <windows.h>
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <limits>
#include <memory>
#include <functional>
#include <thread>
#include <cstring>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {
class AvrtScope {
public:
    AvrtScope() {
        DWORD taskIndex = 0;
        handle_ = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
        if (!handle_) {
            std::wcerr << L"Warning: failed to enter MMCSS 'Pro Audio' profile; continuing with normal priority." << std::endl;
        }
    }
    ~AvrtScope() {
        if (handle_) {
            AvRevertMmThreadCharacteristics(handle_);
        }
    }
private:
    HANDLE handle_ = nullptr;
};

class HandleGuard {
public:
    HandleGuard() = default;
    explicit HandleGuard(HANDLE h) : handle_(h) {}
    ~HandleGuard() { reset(); }
    HANDLE get() const { return handle_; }
    HANDLE release() {
        HANDLE tmp = handle_;
        handle_ = nullptr;
        return tmp;
    }
    void reset(HANDLE newHandle = nullptr) {
        if (handle_) {
            CloseHandle(handle_);
        }
        handle_ = newHandle;
    }
private:
    HANDLE handle_ = nullptr;
};

class ThreadGuard {
public:
    ThreadGuard(std::thread& thread, std::atomic<bool>& runningFlag, HANDLE wakeEvent)
        : thread_(thread), runningFlag_(runningFlag), wakeEvent_(wakeEvent) {}
    ~ThreadGuard() {
        runningFlag_.store(false, std::memory_order_release);
        if (wakeEvent_) {
            SetEvent(wakeEvent_);
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }
private:
    std::thread& thread_;
    std::atomic<bool>& runningFlag_;
    HANDLE wakeEvent_;
};

bool IsSupportedFormat(const WAVEFORMATEX* format) {
    if (!format) {
        return false;
    }
    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && format->wBitsPerSample == 32) {
        return true;
    }
    if (format->wFormatTag == WAVE_FORMAT_PCM && format->wBitsPerSample == 16) {
        return true;
    }
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_PCM && format->wBitsPerSample == 16) {
            return true;
        }
        if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT && format->wBitsPerSample == 32) {
            return true;
        }
    }
    return false;
}

}

LoopbackRecorder::LoopbackRecorder(ComPtr<IMMDevice> renderDevice, Logger& logger)
    : device_(std::move(renderDevice)), logger_(logger) {}

RecorderStats LoopbackRecorder::Record(const RecorderConfig& config, const std::function<bool()>& shouldStop) {
    RecorderStats stats;
    if (!device_) {
        throw std::runtime_error("Render device is null");
    }

    ComPtr<IAudioClient> audioClient;
    HRESULT hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &audioClient);
    if (FAILED(hr)) {
        std::wstring message = DescribeHRESULTW(hr);
        logger_.Error(std::wstring(L"IAudioClient activation failed: ") + message);
        throw std::runtime_error("IAudioClient activation failed: " + DescribeHRESULTA(hr));
    }

    WAVEFORMATEX* format = nullptr;
    hr = audioClient->GetMixFormat(&format);
    if (FAILED(hr)) {
        std::wstring message = DescribeHRESULTW(hr);
        logger_.Error(std::wstring(L"GetMixFormat failed: ") + message);
        throw std::runtime_error("GetMixFormat failed: " + DescribeHRESULTA(hr));
    }
    std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)> mixFormat(format, CoTaskMemFree);

    ValidateFormat(mixFormat.get());

    RecorderConfig localConfig = config;
    const std::wstring outputPathText = localConfig.outputPath.wstring();
    logger_.Info(L"Recording system audio to " + outputPathText);
    WavWriter writer(localConfig.outputPath, *mixFormat);

    const auto latency = std::clamp(localConfig.latencyHint, std::chrono::milliseconds(10), std::chrono::milliseconds(500));
    const REFERENCE_TIME bufferDuration = static_cast<REFERENCE_TIME>(latency.count()) * 10000; // 100ns units

    hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                 bufferDuration,
                                 0,
                                 mixFormat.get(),
                                 nullptr);
    if (FAILED(hr)) {
        std::wstring message = DescribeHRESULTW(hr);
        logger_.Error(std::wstring(L"IAudioClient Initialize failed: ") + message);
        throw std::runtime_error("IAudioClient Initialize failed: " + DescribeHRESULTA(hr));
    }

    HandleGuard samplesReadyEvent(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!samplesReadyEvent.get()) {
        throw std::runtime_error("Failed to create event handle");
    }

    hr = audioClient->SetEventHandle(samplesReadyEvent.get());
    if (FAILED(hr)) {
        std::wstring message = DescribeHRESULTW(hr);
        logger_.Error(std::wstring(L"SetEventHandle failed: ") + message);
        throw std::runtime_error("SetEventHandle failed: " + DescribeHRESULTA(hr));
    }

    ComPtr<IAudioCaptureClient> captureClient;
    hr = audioClient->GetService(IID_PPV_ARGS(&captureClient));
    if (FAILED(hr)) {
        std::wstring message = DescribeHRESULTW(hr);
        logger_.Error(std::wstring(L"Failed to acquire IAudioCaptureClient: ") + message);
        throw std::runtime_error("Failed to acquire IAudioCaptureClient: " + DescribeHRESULTA(hr));
    }

    HandleGuard dataReadyEvent(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    HandleGuard spaceAvailableEvent(CreateEventW(nullptr, FALSE, TRUE, nullptr));
    if (!dataReadyEvent.get() || !spaceAvailableEvent.get()) {
        throw std::runtime_error("Failed to create writer synchronization events");
    }
    HandleGuard userStopEvent;
    const bool hasStopCallback = static_cast<bool>(shouldStop);
    if (hasStopCallback) {
        userStopEvent.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!userStopEvent.get()) {
            throw std::runtime_error("Failed to create user stop event");
        }
    }

    AvrtScope avrtScope;
    hr = audioClient->Start();
    if (FAILED(hr)) {
        std::wstring message = DescribeHRESULTW(hr);
        logger_.Error(std::wstring(L"Failed to start audio client: ") + message);
        throw std::runtime_error("Failed to start audio client: " + DescribeHRESULTA(hr));
    }
    logger_.Info(L"WASAPI loopback capture started.");

    const uint32_t bytesPerFrame = mixFormat->nBlockAlign;
    const uint32_t sampleRate = mixFormat->nSamplesPerSec;
    const std::optional<uint64_t> frameLimit = localConfig.maxDuration
        ? std::optional<uint64_t>(static_cast<uint64_t>(sampleRate) * localConfig.maxDuration->count())
        : std::nullopt;

    const auto ringMs = std::clamp(localConfig.ringBufferSize, std::chrono::milliseconds(200), std::chrono::milliseconds(10000));
    const uint64_t ringFrames = std::max<uint64_t>(static_cast<uint64_t>(sampleRate) * ringMs.count() / 1000, 1);
    const uint64_t desiredCapacity = std::max<uint64_t>(ringFrames * bytesPerFrame, static_cast<uint64_t>(bytesPerFrame) * 2);
    const size_t ringCapacityBytes = static_cast<size_t>(std::min<uint64_t>(desiredCapacity, static_cast<uint64_t>(std::numeric_limits<size_t>::max())));
    logger_.Info(L"Capture latency " + std::to_wstring(latency.count()) + L" ms, ring buffer " + std::to_wstring(ringMs.count()) + L" ms (" + std::to_wstring(ringCapacityBytes / 1024) + L" KiB).");
    SpscByteRingBuffer ring(ringCapacityBytes);

    std::atomic<bool> writerActive{true};
    std::atomic<uint32_t> writerWaitTimeouts{0};
    std::atomic<bool> writerFailed{false};
    std::string writerErrorMessage;
    std::atomic<bool> fatalError{false};
    std::atomic<bool> stopWatcherTerminate{false};
    std::thread stopWatcher;
    if (hasStopCallback) {
        stopWatcher = std::thread([&]() {
            while (!stopWatcherTerminate.load(std::memory_order_acquire)) {
                if (fatalError.load(std::memory_order_acquire)) {
                    if (userStopEvent.get()) {
                        SetEvent(userStopEvent.get());
                    }
                    break;
                }
                if (shouldStop() || stopWatcherTerminate.load(std::memory_order_relaxed)) {
                    if (userStopEvent.get()) {
                        SetEvent(userStopEvent.get());
                    }
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });
    }

    std::thread writerThread([&]() {
        const size_t chunkBytes = std::min<size_t>(ring.Capacity(), std::max<size_t>(bytesPerFrame * 512, 16384));
        std::vector<BYTE> chunk(chunkBytes);
        const DWORD writerWaitMs = static_cast<DWORD>(std::clamp<int>(static_cast<int>(localConfig.watchdogTimeout.count() / 2), 5, 500));
        size_t bytesPendingFlush = 0;
        const size_t flushThreshold = static_cast<size_t>(bytesPerFrame) * sampleRate; // roughly one second
        try {
            while (writerActive.load(std::memory_order_acquire) || ring.AvailableToRead() > 0) {
                size_t bytes = ring.Read(chunk.data(), chunk.size());
                if (bytes == 0) {
                    DWORD waitRes = WaitForSingleObject(dataReadyEvent.get(), writerWaitMs);
                    if (waitRes == WAIT_TIMEOUT) {
                        ++writerWaitTimeouts;
                        continue;
                    }
                    if (waitRes == WAIT_FAILED) {
                        throw std::runtime_error("Writer thread wait failed");
                    }
                    continue;
                }
                writer.Write(chunk.data(), bytes);
                bytesPendingFlush += bytes;
                if (bytesPendingFlush >= flushThreshold) {
                    writer.Flush();
                    bytesPendingFlush = 0;
                }
                SetEvent(spaceAvailableEvent.get());
            }
            if (bytesPendingFlush > 0) {
                writer.Flush();
            }
        } catch (const std::exception& ex) {
            writerFailed.store(true, std::memory_order_release);
            writerErrorMessage = ex.what();
            fatalError.store(true, std::memory_order_release);
            SetEvent(spaceAvailableEvent.get());
            SetEvent(dataReadyEvent.get());
            if (userStopEvent.get()) {
                SetEvent(userStopEvent.get());
            }
        }
    });

    ThreadGuard writerGuard(writerThread, writerActive, dataReadyEvent.get());

    uint64_t framesRecorded = 0;
    uint64_t framesPerSecond = 0;
    uint64_t lastReportedDropped = 0;
    bool done = false;
    std::vector<BYTE> staging;
    staging.reserve(std::min<size_t>(ring.Capacity(), static_cast<size_t>(bytesPerFrame) * 4096));
    const DWORD waitMs = static_cast<DWORD>(std::clamp<int>(static_cast<int>(localConfig.watchdogTimeout.count()), 50, 60000));
    bool dropWarningIssued = false;
    auto lastStatusReport = std::chrono::steady_clock::now();

    auto maybeReportStatus = [&](bool force) {
        if (localConfig.quietStatusUpdates) {
            return;
        }
        auto now = std::chrono::steady_clock::now();
        if (!force && now - lastStatusReport < std::chrono::seconds(1)) {
            return;
        }
        size_t bytesInRing = ring.AvailableToRead();
        size_t framesInRing = bytesInRing / bytesPerFrame;
        uint64_t queueMs = framesInRing > 0 ? (framesInRing * 1000ull) / sampleRate : 0;
        uint64_t droppedSince = stats.framesDropped - lastReportedDropped;
        std::wstring message = L"[Status] fps=" + std::to_wstring(framesPerSecond) +
            L"/s, queue=" + std::to_wstring(queueMs) + L" ms, dropped=" + std::to_wstring(droppedSince);
        logger_.Info(message);
        framesPerSecond = 0;
        lastReportedDropped = stats.framesDropped;
        lastStatusReport = now;
    };

    auto handleAudioError = [&](HRESULT error, const wchar_t* context) {
        const std::wstring description = DescribeHRESULTW(error);
        if (error == AUDCLNT_E_DEVICE_INVALIDATED) {
            stats.deviceInvalidated = true;
            logger_.Error(std::wstring(context) + L": playback device became unavailable (" + description + L")");
        } else {
            logger_.Error(std::wstring(context) + L" failed: " + description);
        }
        return true;
    };

    auto pushToRing = [&](const BYTE* src, size_t bytes, size_t& acceptedBytes) -> bool {
        acceptedBytes = 0;
        while (acceptedBytes < bytes) {
            size_t wrote = ring.Write(src + acceptedBytes, bytes - acceptedBytes);
            if (wrote == 0) {
                ++stats.ringBufferWaits;
                if (fatalError.load(std::memory_order_acquire)) {
                    return false;
                }
                DWORD waitResult = WaitForSingleObject(spaceAvailableEvent.get(), waitMs);
                if (waitResult == WAIT_OBJECT_0) {
                    continue;
                }
                ++stats.ringBufferTimeouts;
                const size_t remaining = bytes - acceptedBytes;
                const uint64_t droppedFrames = remaining / bytesPerFrame;
                if (droppedFrames > 0) {
                    stats.framesDropped += droppedFrames;
                    if (!dropWarningIssued) {
                        logger_.Warn(L"Writer thread is slower than capture; dropping frames to stay real-time.");
                        dropWarningIssued = true;
                    }
                }
                if (localConfig.failOnGlitch) {
                    logger_.Error(L"Ring buffer overrun while --fail-on-glitch is enabled; aborting capture.");
                    return false;
                }
                break;
            }
            acceptedBytes += wrote;
            SetEvent(dataReadyEvent.get());
        }
        return true;
    };

    while (!done) {
        if (fatalError.load(std::memory_order_acquire)) {
            logger_.Error(L"Writer thread reported a fatal error; aborting capture.");
            break;
        }
        if (shouldStop && shouldStop()) {
            if (userStopEvent.get()) {
                SetEvent(userStopEvent.get());
            }
            break;
        }
        DWORD wait = WAIT_FAILED;
        if (hasStopCallback) {
            HANDLE waitHandles[2] = { samplesReadyEvent.get(), userStopEvent.get() };
            wait = WaitForMultipleObjects(2, waitHandles, FALSE, waitMs);
            if (wait == WAIT_OBJECT_0 + 1) {
                break;
            }
        } else {
            wait = WaitForSingleObject(samplesReadyEvent.get(), waitMs);
        }
        if (wait == WAIT_TIMEOUT) {
            ++stats.watchdogTimeouts;
            if (localConfig.failOnGlitch) {
                logger_.Error(L"Watchdog timeout reached; aborting capture.");
                break;
            }
            logger_.Warn(L"Capture watchdog timeout; attempting to continue.");
            continue;
        }
        if (wait != WAIT_OBJECT_0) {
            logger_.Error(L"Audio event wait returned an unexpected code.");
            break;
        }

        UINT32 packetLength = 0;
        hr = captureClient->GetNextPacketSize(&packetLength);
        if (FAILED(hr)) {
            handleAudioError(hr, L"GetNextPacketSize");
            break;
        }

        while (packetLength > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            hr = captureClient->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (FAILED(hr)) {
                handleAudioError(hr, L"GetBuffer");
                done = true;
                break;
            }

            const size_t bytesToWrite = static_cast<size_t>(frames) * bytesPerFrame;
            if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
                ++stats.glitchCount;
                if (localConfig.failOnGlitch) {
                    logger_.Error(L"Data discontinuity reported by audio engine; aborting capture.");
                    captureClient->ReleaseBuffer(frames);
                    done = true;
                    break;
                }
                logger_.Warn(L"Data discontinuity reported by audio engine.");
            }
            staging.resize(bytesToWrite);
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                std::fill(staging.begin(), staging.end(), 0);
                stats.silentFrames += frames;
            } else {
                std::memcpy(staging.data(), data, bytesToWrite);
                if (localConfig.enableMicMix) {
                    MixMicrophoneIfEnabled(staging.data(), frames, mixFormat.get());
                }
            }

            captureClient->ReleaseBuffer(frames);

            size_t acceptedBytes = 0;
            if (!pushToRing(staging.data(), bytesToWrite, acceptedBytes)) {
                done = true;
                break;
            }

            const uint64_t acceptedFrames = acceptedBytes / bytesPerFrame;
            framesRecorded += acceptedFrames;
            framesPerSecond += acceptedFrames;

            if (frameLimit && framesRecorded >= *frameLimit) {
                done = true;
                break;
            }
            hr = captureClient->GetNextPacketSize(&packetLength);
            if (FAILED(hr)) {
                handleAudioError(hr, L"GetNextPacketSize");
                done = true;
                break;
            }
        }
        maybeReportStatus(false);
    }

    writerActive.store(false, std::memory_order_release);
    SetEvent(dataReadyEvent.get());
    if (hasStopCallback) {
        stopWatcherTerminate.store(true, std::memory_order_release);
        if (userStopEvent.get()) {
            SetEvent(userStopEvent.get());
        }
        if (stopWatcher.joinable()) {
            stopWatcher.join();
        }
    }
    maybeReportStatus(true);

    audioClient->Stop();
    logger_.Info(L"WASAPI loopback capture stopped.");
    stats.framesCaptured = framesRecorded;
    logger_.Info(L"Frames captured: " + std::to_wstring(stats.framesCaptured) +
                 L", silent frames: " + std::to_wstring(stats.silentFrames) +
                 L", glitches: " + std::to_wstring(stats.glitchCount) +
                 L", dropped: " + std::to_wstring(stats.framesDropped));
    if (stats.framesCaptured > 0 && stats.framesCaptured == stats.silentFrames) {
        logger_.Warn(L"All captured frames were reported as silence. Verify the selected playback device is actively outputting audio (try --list-devices / --device-index).");
    }
    if (stats.deviceInvalidated) {
        logger_.Warn(L"Session ended because the playback device was disconnected or changed.");
    }
    stats.writerWaitTimeouts = writerWaitTimeouts.load();
    if (writerFailed.load()) {
        throw std::runtime_error("Writer thread failed: " + writerErrorMessage);
    }
    return stats;
}

void LoopbackRecorder::ValidateFormat(const WAVEFORMATEX* format) {
    if (!IsSupportedFormat(format)) {
        throw std::runtime_error("Only 16-bit PCM or 32-bit float formats are supported");
    }
}

void LoopbackRecorder::MixMicrophoneIfEnabled(BYTE* buffer, UINT32 frames, const WAVEFORMATEX* format) {
    (void)buffer;
    (void)frames;
    (void)format;
    // Placeholder: future work will route microphone input and mix at a matching format.
}
