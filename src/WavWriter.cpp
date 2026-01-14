#include "WavWriter.h"

#include <stdexcept>
#include <cstring>
#include <system_error>

namespace {
void WriteValue(std::ofstream& stream, uint32_t value) {
    stream.write(reinterpret_cast<const char*>(&value), sizeof(value));
}
}

WavWriter::WavWriter(const std::filesystem::path& path, const WAVEFORMATEX& format)
    : path_(path) {
    std::error_code removeEc;
    std::filesystem::remove(path_, removeEc);
    stream_.open(path, std::ios::binary | std::ios::trunc);
    if (!stream_) {
        throw std::runtime_error("打开输出文件失败");
    }
    formatBlob_.assign(reinterpret_cast<const std::byte*>(&format),
                       reinterpret_cast<const std::byte*>(&format) + sizeof(WAVEFORMATEX) + format.cbSize);
    WriteHeader();
}

WavWriter::~WavWriter() {
    Close();
}

void WavWriter::Write(const BYTE* data, size_t byteCount) {
    if (!stream_) {
        throw std::runtime_error("WAV 流未打开");
    }
    stream_.write(reinterpret_cast<const char*>(data), byteCount);
    if (!stream_) {
        throw std::runtime_error("写入 WAV 数据失败");
    }
    dataBytes_ += static_cast<uint32_t>(byteCount);
}

void WavWriter::Flush() {
    if (!stream_) {
        return;
    }
    stream_.flush();
    if (!stream_) {
        throw std::runtime_error("刷新 WAV 数据到磁盘失败");
    }
}

void WavWriter::Close() {
    if (finalized_) {
        return;
    }
    if (stream_) {
        FinalizeHeader();
        stream_.close();
    }
    finalized_ = true;
}

void WavWriter::WriteHeader() {
    stream_.write("RIFF", 4);
    WriteValue(stream_, 0); // Placeholder, fix-up later
    stream_.write("WAVE", 4);

    stream_.write("fmt ", 4);
    WriteValue(stream_, static_cast<uint32_t>(formatBlob_.size()));
    stream_.write(reinterpret_cast<const char*>(formatBlob_.data()), formatBlob_.size());

    stream_.write("data", 4);
    WriteValue(stream_, 0); // Placeholder for data chunk size
}

void WavWriter::FinalizeHeader() {
    const auto currentPos = stream_.tellp();
    stream_.seekp(4, std::ios::beg);
    const uint32_t riffSize = 4 + 8 + static_cast<uint32_t>(formatBlob_.size()) + 8 + dataBytes_;
    WriteValue(stream_, riffSize);

    // data chunk size lives after RIFF(4) + size(4) + WAVE(4) + fmt (4 + 4 + format) + data tag(4)
    constexpr std::streamoff riffTagSize = 4;
    constexpr std::streamoff riffSizeField = 4;
    constexpr std::streamoff waveTagSize = 4;
    constexpr std::streamoff fmtTagSize = 4;
    constexpr std::streamoff fmtSizeField = 4;
    constexpr std::streamoff dataTagSize = 4;
    const std::streamoff dataSizeOffset = riffTagSize + riffSizeField + waveTagSize +
        fmtTagSize + fmtSizeField + static_cast<std::streamoff>(formatBlob_.size()) + dataTagSize;
    stream_.seekp(dataSizeOffset, std::ios::beg);
    WriteValue(stream_, dataBytes_);

    stream_.seekp(currentPos, std::ios::beg);
}
