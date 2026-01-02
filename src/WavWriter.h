#pragma once

#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <mmreg.h>

class WavWriter {
public:
    WavWriter(const std::filesystem::path& path, const WAVEFORMATEX& format);
    ~WavWriter();

    void Write(const BYTE* data, size_t byteCount);
    void Flush();
    void Close();

    WavWriter(const WavWriter&) = delete;
    WavWriter& operator=(const WavWriter&) = delete;
private:
    void WriteHeader();
    void FinalizeHeader();

    std::filesystem::path path_;
    std::ofstream stream_;
    std::vector<std::byte> formatBlob_;
    uint32_t dataBytes_ = 0;
    bool finalized_ = false;
};
