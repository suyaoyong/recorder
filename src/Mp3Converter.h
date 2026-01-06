#pragma once

#include "Logger.h"

#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <cstddef>
#include <cstdint>
#include <mmreg.h>
#include <vector>

struct Mp3ConversionOptions {
    uint32_t bitrateKbps = 192;
};

class Mp3Converter {
public:
    static void ConvertWavToMp3(const std::filesystem::path& wavPath,
                                const std::filesystem::path& mp3Path,
                                const Mp3ConversionOptions& options,
                                Logger& logger);
};

class Mp3StreamWriter {
public:
    Mp3StreamWriter(const std::filesystem::path& path,
                    const WAVEFORMATEX& format,
                    const Mp3ConversionOptions& options,
                    Logger& logger);
    ~Mp3StreamWriter();

    Mp3StreamWriter(const Mp3StreamWriter&) = delete;
    Mp3StreamWriter& operator=(const Mp3StreamWriter&) = delete;

    void Write(const BYTE* data, size_t byteCount);
    void Flush();
    void Close();

private:
    std::filesystem::path path_;
    std::ofstream stream_;
    const void* api_ = nullptr;
    void* handle_ = nullptr;
    WAVEFORMATEX format_{};
    size_t bytesPerFrame_ = 0;
    size_t targetChannels_ = 0;
    std::vector<uint8_t> pending_;
    std::vector<int16_t> pcmBuffer_;
    std::vector<unsigned char> mp3Buffer_;
    bool finalized_ = false;
    Logger* logger_ = nullptr;
};
