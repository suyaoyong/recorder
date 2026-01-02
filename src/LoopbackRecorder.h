#pragma once

#include "WavWriter.h"
#include "Logger.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <wrl/client.h>
#include <Audioclient.h>
#include <mmdeviceapi.h>
#include <cstdint>

struct RecorderConfig {
    std::filesystem::path outputPath;
    std::optional<std::chrono::seconds> maxDuration;
    bool enableMicMix = false; // future extension
    std::chrono::milliseconds latencyHint{200};
    std::chrono::milliseconds watchdogTimeout{4000};
    bool failOnGlitch = false;
    std::chrono::milliseconds ringBufferSize{2000};
    bool quietStatusUpdates = false;
};

struct RecorderStats {
    uint64_t framesCaptured = 0;
    uint64_t silentFrames = 0;
    uint32_t glitchCount = 0;          // discontinuities / flags
    uint32_t watchdogTimeouts = 0;     // wait timeouts
    uint32_t ringBufferWaits = 0;
    uint32_t ringBufferTimeouts = 0;
    uint32_t writerWaitTimeouts = 0;
    uint64_t framesDropped = 0;
    bool deviceInvalidated = false;
};

class LoopbackRecorder {
public:
    LoopbackRecorder(Microsoft::WRL::ComPtr<IMMDevice> renderDevice, Logger& logger);
    RecorderStats Record(const RecorderConfig& config, const std::function<bool()>& shouldStop);
private:
    void ValidateFormat(const WAVEFORMATEX* format);
    void MixMicrophoneIfEnabled(BYTE* buffer, UINT32 frames, const WAVEFORMATEX* format);

    Microsoft::WRL::ComPtr<IMMDevice> device_;
    Logger& logger_;
};
