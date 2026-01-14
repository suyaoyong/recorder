#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "LoopbackRecorder.h"
#include "SpscByteRing.h"
#include "HResultUtils.h"
#include "SegmentNaming.h"
#include "Mp3Converter.h"

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
#include <sstream>
#include <iomanip>
#include <filesystem>
#include <cwctype>

using Microsoft::WRL::ComPtr;

namespace {
class AvrtScope {
public:
    AvrtScope() {
        DWORD taskIndex = 0;
        handle_ = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
        if (!handle_) {
            std::wcerr << L"警告：无法进入 MMCSS“Pro Audio”优先级配置，将使用普通优先级继续。" << std::endl;
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

std::wstring ToLower(std::wstring value) {
    for (auto& ch : value) {
        ch = static_cast<wchar_t>(towlower(ch));
    }
    return value;
}

bool IsMp3Path(const std::filesystem::path& path) {
    auto ext = ToLower(path.extension().wstring());
    return ext == L".mp3";
}

}

LoopbackRecorder::LoopbackRecorder(ComPtr<IMMDevice> renderDevice, Logger& logger)
    : device_(std::move(renderDevice)), logger_(logger) {}

RecorderStats LoopbackRecorder::Record(const RecorderConfig& config, const RecorderControls& controls) {
    RecorderStats stats;
    if (!device_) {
        throw std::runtime_error("渲染设备为空");
    }

    ComPtr<IAudioClient> audioClient;
    HRESULT hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &audioClient);
    if (FAILED(hr)) {
        std::wstring message = DescribeHRESULTW(hr);
        logger_.Error(std::wstring(L"IAudioClient 激活失败：") + message);
        throw std::runtime_error("IAudioClient 激活失败：" + DescribeHRESULTA(hr));
    }

    WAVEFORMATEX* format = nullptr;
    hr = audioClient->GetMixFormat(&format);
    if (FAILED(hr)) {
        std::wstring message = DescribeHRESULTW(hr);
        logger_.Error(std::wstring(L"GetMixFormat 失败：") + message);
        throw std::runtime_error("GetMixFormat 失败：" + DescribeHRESULTA(hr));
    }
    std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)> mixFormat(format, CoTaskMemFree);

    ValidateFormat(mixFormat.get());

    RecorderConfig localConfig = config;
    const std::wstring outputPathText = localConfig.outputPath.wstring();
    const std::wstring outputExt = localConfig.outputPath.extension().wstring();
    const std::wstring segmentSuffix = outputExt.empty() ? L"" : outputExt;
    logger_.Info(L"录音基路径：" + outputPathText + L"（分段文件使用 _001" + segmentSuffix + L" 编号）。");

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
        logger_.Error(std::wstring(L"IAudioClient Initialize 失败：") + message);
        throw std::runtime_error("IAudioClient Initialize 失败：" + DescribeHRESULTA(hr));
    }

    HandleGuard samplesReadyEvent(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!samplesReadyEvent.get()) {
        throw std::runtime_error("创建事件句柄失败");
    }

    hr = audioClient->SetEventHandle(samplesReadyEvent.get());
    if (FAILED(hr)) {
        std::wstring message = DescribeHRESULTW(hr);
        logger_.Error(std::wstring(L"SetEventHandle 失败：") + message);
        throw std::runtime_error("SetEventHandle 失败：" + DescribeHRESULTA(hr));
    }

    ComPtr<IAudioCaptureClient> captureClient;
    hr = audioClient->GetService(IID_PPV_ARGS(&captureClient));
    if (FAILED(hr)) {
        std::wstring message = DescribeHRESULTW(hr);
        logger_.Error(std::wstring(L"获取 IAudioCaptureClient 失败：") + message);
        throw std::runtime_error("获取 IAudioCaptureClient 失败：" + DescribeHRESULTA(hr));
    }

    HandleGuard dataReadyEvent(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    HandleGuard spaceAvailableEvent(CreateEventW(nullptr, FALSE, TRUE, nullptr));
    if (!dataReadyEvent.get() || !spaceAvailableEvent.get()) {
        throw std::runtime_error("创建写入线程同步事件失败");
    }
    HandleGuard userStopEvent;
    const bool hasStopCallback = static_cast<bool>(controls.shouldStop);
    if (hasStopCallback) {
        userStopEvent.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!userStopEvent.get()) {
            throw std::runtime_error("创建用户停止事件失败");
        }
    }

    AvrtScope avrtScope;
    hr = audioClient->Start();
    if (FAILED(hr)) {
        std::wstring message = DescribeHRESULTW(hr);
        logger_.Error(std::wstring(L"启动音频客户端失败：") + message);
        throw std::runtime_error("启动音频客户端失败：" + DescribeHRESULTA(hr));
    }
    logger_.Info(L"WASAPI 回环采集已启动。");

    const uint32_t bytesPerFrame = mixFormat->nBlockAlign;
    const uint32_t sampleRate = mixFormat->nSamplesPerSec;
    const std::optional<uint64_t> frameLimit = localConfig.maxDuration
        ? std::optional<uint64_t>(static_cast<uint64_t>(sampleRate) * localConfig.maxDuration->count())
        : std::nullopt;
    const std::optional<uint64_t> segmentFrameTarget = localConfig.segmentDuration
        ? std::optional<uint64_t>(static_cast<uint64_t>(sampleRate) * localConfig.segmentDuration->count())
        : std::nullopt;
    const std::optional<uint64_t> segmentByteTarget = localConfig.segmentBytes;
    const bool manualSegmentsEnabled = static_cast<bool>(controls.requestNewSegment);
    const bool segmentationEnabled = segmentFrameTarget.has_value() || segmentByteTarget.has_value() || manualSegmentsEnabled;

    const auto ringMs = std::clamp(localConfig.ringBufferSize, std::chrono::milliseconds(200), std::chrono::milliseconds(10000));
    const uint64_t ringFrames = std::max<uint64_t>(static_cast<uint64_t>(sampleRate) * ringMs.count() / 1000, 1);
    const uint64_t desiredCapacity = std::max<uint64_t>(ringFrames * bytesPerFrame, static_cast<uint64_t>(bytesPerFrame) * 2);
    const size_t ringCapacityBytes = static_cast<size_t>(std::min<uint64_t>(desiredCapacity, static_cast<uint64_t>(std::numeric_limits<size_t>::max())));
    logger_.Info(L"采集延迟 " + std::to_wstring(latency.count()) + L" ms，环形缓冲 " +
                 std::to_wstring(ringMs.count()) + L" ms（" + std::to_wstring(ringCapacityBytes / 1024) + L" KiB）。");
    SpscByteRingBuffer ring(ringCapacityBytes);

    std::atomic<bool> writerActive{true};
    std::atomic<uint32_t> writerWaitTimeouts{0};
    std::atomic<bool> writerFailed{false};
    std::string writerErrorMessage;
    std::atomic<bool> fatalError{false};
    std::atomic<uint32_t> segmentsOpened{1};
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
                if ((controls.shouldStop && controls.shouldStop()) || stopWatcherTerminate.load(std::memory_order_relaxed)) {
                    if (userStopEvent.get()) {
                        SetEvent(userStopEvent.get());
                    }
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });
    }

    const bool mp3Output = IsMp3Path(localConfig.outputPath);
    std::thread writerThread([&, manualSegmentCallback = controls.requestNewSegment, segmentationEnabled, mp3Output]() mutable {
        const size_t chunkBytes = std::min<size_t>(ring.Capacity(), std::max<size_t>(bytesPerFrame * 512, 16384));
        std::vector<BYTE> chunk(chunkBytes);
        const DWORD writerWaitMs = static_cast<DWORD>(std::clamp<int>(static_cast<int>(localConfig.watchdogTimeout.count() / 2), 5, 500));
        size_t bytesPendingFlush = 0;
        const size_t flushThreshold = static_cast<size_t>(bytesPerFrame) * sampleRate; // roughly one second
        size_t currentSegmentIndex = 0;
        uint64_t framesInSegment = 0;
        uint64_t bytesInSegment = 0;
        Mp3ConversionOptions mp3Options;
        if (localConfig.mp3BitrateKbps) {
            mp3Options.bitrateKbps = *localConfig.mp3BitrateKbps;
        }

        auto consumeManualSegment = [&]() -> bool {
            if (!manualSegmentCallback) {
                return false;
            }
            return manualSegmentCallback();
        };

        try {
            class IAudioWriter {
            public:
                virtual ~IAudioWriter() = default;
                virtual void Write(const BYTE* data, size_t byteCount) = 0;
                virtual void Flush() = 0;
                virtual void Close() = 0;
            };
            class WavWriterAdapter final : public IAudioWriter {
            public:
                explicit WavWriterAdapter(const std::filesystem::path& path, const WAVEFORMATEX& format)
                    : writer_(path, format) {}
                void Write(const BYTE* data, size_t byteCount) override { writer_.Write(data, byteCount); }
                void Flush() override { writer_.Flush(); }
                void Close() override { writer_.Close(); }
            private:
                WavWriter writer_;
            };
            class Mp3WriterAdapter final : public IAudioWriter {
            public:
                Mp3WriterAdapter(const std::filesystem::path& path,
                                 const WAVEFORMATEX& format,
                                 const Mp3ConversionOptions& options,
                                 Logger& logger)
                    : writer_(path, format, options, logger) {}
                void Write(const BYTE* data, size_t byteCount) override { writer_.Write(data, byteCount); }
                void Flush() override { writer_.Flush(); }
                void Close() override { writer_.Close(); }
            private:
                Mp3StreamWriter writer_;
            };

            auto openWriterForSegment = [&](size_t segmentIndex) -> std::unique_ptr<IAudioWriter> {
                const auto segmentPath = BuildSegmentPath(localConfig.outputPath, segmentIndex);
                if (segmentIndex == 0) {
                    logger_.Info(L"打开初始分段：" + segmentPath.wstring());
                } else {
                    logger_.Info(L"滚动到分段 #" + std::to_wstring(segmentIndex + 1) + L"：" + segmentPath.wstring());
                }
                if (mp3Output) {
                    return std::make_unique<Mp3WriterAdapter>(segmentPath, *mixFormat, mp3Options, logger_);
                }
                return std::make_unique<WavWriterAdapter>(segmentPath, *mixFormat);
            };

            std::unique_ptr<IAudioWriter> segmentWriter = openWriterForSegment(currentSegmentIndex);
            segmentsOpened.store(1, std::memory_order_release);
            auto rollSegment = [&](const wchar_t* reason) {
                if (!segmentationEnabled) {
                    return;
                }
                if (segmentWriter) {
                    if (bytesPendingFlush > 0) {
                        segmentWriter->Flush();
                        bytesPendingFlush = 0;
                    }
                    segmentWriter->Close();
                }
                ++currentSegmentIndex;
                const auto nextPath = BuildSegmentPath(localConfig.outputPath, currentSegmentIndex);
                std::wstring reasonText = reason ? std::wstring(reason) : std::wstring(L"滚动");
                logger_.Info(L"开始分段 #" + std::to_wstring(currentSegmentIndex + 1) +
                             L"（" + reasonText + L"）：" + nextPath.wstring());
                if (mp3Output) {
                    segmentWriter = std::make_unique<Mp3WriterAdapter>(nextPath, *mixFormat, mp3Options, logger_);
                } else {
                    segmentWriter = std::make_unique<WavWriterAdapter>(nextPath, *mixFormat);
                }
                framesInSegment = 0;
                bytesInSegment = 0;
                bytesPendingFlush = 0;
                segmentsOpened.store(static_cast<uint32_t>(currentSegmentIndex + 1), std::memory_order_release);
            };

            while (writerActive.load(std::memory_order_acquire) || ring.AvailableToRead() > 0) {
                if (consumeManualSegment()) {
                    rollSegment(L"手动切段");
                }
                size_t bytes = ring.Read(chunk.data(), chunk.size());
                if (bytes == 0) {
                    DWORD waitRes = WaitForSingleObject(dataReadyEvent.get(), writerWaitMs);
                    if (waitRes == WAIT_TIMEOUT) {
                        ++writerWaitTimeouts;
                        continue;
                    }
                    if (waitRes == WAIT_FAILED) {
                        throw std::runtime_error("写入线程等待失败");
                    }
                    continue;
                }
                segmentWriter->Write(chunk.data(), bytes);
                bytesPendingFlush += bytes;
                bytesInSegment += bytes;
                framesInSegment += bytes / bytesPerFrame;
                if (bytesPendingFlush >= flushThreshold) {
                    segmentWriter->Flush();
                    bytesPendingFlush = 0;
                }
                SetEvent(spaceAvailableEvent.get());

                bool rotate = false;
                const wchar_t* reason = nullptr;
                if (segmentFrameTarget && framesInSegment >= *segmentFrameTarget) {
                    rotate = true;
                    reason = L"分段时长";
                }
                if (!rotate && segmentByteTarget && bytesInSegment >= *segmentByteTarget) {
                    rotate = true;
                    reason = L"分段大小";
                }
                if (rotate) {
                    rollSegment(reason);
                }
            }
            if (bytesPendingFlush > 0 && segmentWriter) {
                segmentWriter->Flush();
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

    const auto pauseCallback = controls.isPaused;
    bool lastPauseState = false;
    if (pauseCallback) {
        lastPauseState = pauseCallback();
        if (lastPauseState) {
            logger_.Info(L"录音开始时为暂停状态；将跳过音频数据直到恢复。");
        }
    }
    auto queryPauseState = [&]() -> bool {
        if (!pauseCallback) {
            return false;
        }
        bool paused = pauseCallback();
        if (paused != lastPauseState) {
            lastPauseState = paused;
            logger_.Info(paused ? L"录音已暂停。" : L"录音已继续。");
        }
        return paused;
    };

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
        std::wstring message = L"[状态] fps=" + std::to_wstring(framesPerSecond) +
            L"/s, 队列=" + std::to_wstring(queueMs) + L" ms, 丢弃=" + std::to_wstring(droppedSince) +
            L", 分段=" + std::to_wstring(segmentsOpened.load(std::memory_order_acquire));
        if (lastPauseState) {
            message += L"（已暂停）";
        }
        logger_.Info(message);
        framesPerSecond = 0;
        lastReportedDropped = stats.framesDropped;
        lastStatusReport = now;
    };

    auto handleAudioError = [&](HRESULT error, const wchar_t* context) {
        const std::wstring description = DescribeHRESULTW(error);
        if (error == AUDCLNT_E_DEVICE_INVALIDATED) {
            stats.deviceInvalidated = true;
            logger_.Error(std::wstring(context) + L"：播放设备不可用（" + description + L"）");
        } else {
            logger_.Error(std::wstring(context) + L" 失败：" + description);
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
                        logger_.Warn(L"写入线程慢于采集；为保持实时性将丢弃帧。");
                        dropWarningIssued = true;
                    }
                }
                if (localConfig.failOnGlitch) {
                    logger_.Error(L"启用 --fail-on-glitch 时发生环形缓冲溢出；终止采集。");
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
            logger_.Error(L"写入线程报告致命错误；终止采集。");
            break;
        }
        if (controls.shouldStop && controls.shouldStop()) {
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
                logger_.Error(L"看门狗超时；终止采集。");
                break;
            }
            logger_.Warn(L"采集看门狗超时；尝试继续。");
            continue;
        }
        if (wait != WAIT_OBJECT_0) {
            logger_.Error(L"等待音频事件返回了异常代码。");
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
                    logger_.Error(L"音频引擎报告数据不连续；终止采集。");
                    captureClient->ReleaseBuffer(frames);
                    done = true;
                    break;
                }
                logger_.Warn(L"音频引擎报告数据不连续。");
            }
            const bool pausedNow = queryPauseState();
            if (pausedNow) {
                stats.framesWhilePaused += frames;
                captureClient->ReleaseBuffer(frames);
                hr = captureClient->GetNextPacketSize(&packetLength);
                if (FAILED(hr)) {
                    handleAudioError(hr, L"GetNextPacketSize");
                    done = true;
                    break;
                }
                continue;
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
    logger_.Info(L"WASAPI 回环采集已停止。");
    stats.framesCaptured = framesRecorded;
    stats.segmentsWritten = segmentsOpened.load(std::memory_order_acquire);
    logger_.Info(L"已采集帧数：" + std::to_wstring(stats.framesCaptured) +
                 L"，静音帧：" + std::to_wstring(stats.silentFrames) +
                 L"，暂停帧：" + std::to_wstring(stats.framesWhilePaused) +
                 L"，断续：" + std::to_wstring(stats.glitchCount) +
                 L"，丢弃：" + std::to_wstring(stats.framesDropped) +
                 L"，分段：" + std::to_wstring(stats.segmentsWritten));
    if (stats.framesCaptured > 0 && stats.framesCaptured == stats.silentFrames) {
        logger_.Warn(L"所有采集帧均为静音。请确认所选播放设备正在输出音频（尝试 --list-devices / --device-index）。");
    }
    if (stats.deviceInvalidated) {
        logger_.Warn(L"会话结束：播放设备断开或已更改。");
    }
    stats.writerWaitTimeouts = writerWaitTimeouts.load();
    if (writerFailed.load()) {
        throw std::runtime_error("写入线程失败：" + writerErrorMessage);
    }
    return stats;
}

void LoopbackRecorder::ValidateFormat(const WAVEFORMATEX* format) {
    if (!IsSupportedFormat(format)) {
        throw std::runtime_error("仅支持 16-bit PCM 或 32-bit float 格式");
    }
}

void LoopbackRecorder::MixMicrophoneIfEnabled(BYTE* buffer, UINT32 frames, const WAVEFORMATEX* format) {
    (void)buffer;
    (void)frames;
    (void)format;
    // Placeholder: future work will route microphone input and mix at a matching format.
}
