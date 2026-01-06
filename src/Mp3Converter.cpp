#include "Mp3Converter.h"

#include <Windows.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using lame_t = void*;

struct LameApi {
    HMODULE module = nullptr;
    std::wstring modulePath;
    lame_t (__cdecl* init)() = nullptr;
    int (__cdecl* close)(lame_t) = nullptr;
    int (__cdecl* set_num_channels)(lame_t, int) = nullptr;
    int (__cdecl* set_in_samplerate)(lame_t, int) = nullptr;
    int (__cdecl* set_out_samplerate)(lame_t, int) = nullptr;
    int (__cdecl* set_brate)(lame_t, int) = nullptr;
    int (__cdecl* set_mode)(lame_t, int) = nullptr;
    int (__cdecl* set_quality)(lame_t, int) = nullptr;
    int (__cdecl* init_params)(lame_t) = nullptr;
    int (__cdecl* encode_buffer_interleaved)(lame_t, short int*, int, unsigned char*, int) = nullptr;
    int (__cdecl* flush)(lame_t, unsigned char*, int) = nullptr;
};

constexpr int kLameModeStereo = 1;
constexpr int kLameModeMono = 3;
constexpr size_t kFramesPerChunk = 4096;

std::wstring GetEnvVar(const wchar_t* name) {
    DWORD length = GetEnvironmentVariableW(name, nullptr, 0);
    if (length == 0) {
        return {};
    }
    std::wstring value;
    value.resize(length - 1);
    GetEnvironmentVariableW(name, value.data(), length);
    return value;
}

std::filesystem::path GetModuleDirectory() {
    std::array<wchar_t, MAX_PATH> buffer{};
    DWORD written = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (written == 0 || written == buffer.size()) {
        return std::filesystem::current_path();
    }
    std::filesystem::path exePath(buffer.data(), buffer.data() + written);
    return exePath.parent_path();
}

LameApi LoadLameApi() {
    std::vector<std::filesystem::path> candidates;
    const auto userPath = GetEnvVar(L"LAME_DLL_PATH");
    if (!userPath.empty()) {
        candidates.emplace_back(userPath);
    }
    const auto exeDir = GetModuleDirectory();
    const std::array<const wchar_t*, 2> defaultNames = { L"libmp3lame.dll", L"lame_enc.dll" };
    for (const auto* name : defaultNames) {
        candidates.push_back(exeDir / name);
    }
    for (const auto* name : defaultNames) {
        candidates.emplace_back(name);
    }

    HMODULE module = nullptr;
    for (const auto& candidate : candidates) {
        module = LoadLibraryW(candidate.c_str());
        if (module) {
            LameApi api;
            api.module = module;
            std::array<wchar_t, MAX_PATH> pathBuffer{};
            DWORD len = GetModuleFileNameW(module, pathBuffer.data(), static_cast<DWORD>(pathBuffer.size()));
            if (len > 0 && len < pathBuffer.size()) {
                api.modulePath.assign(pathBuffer.data(), len);
            }
            auto require = [&](const char* name) {
                FARPROC proc = GetProcAddress(module, name);
                if (!proc) {
                    FreeLibrary(module);
                    throw std::runtime_error(std::string("libmp3lame missing symbol: ") + name);
                }
                return proc;
            };
            api.init = reinterpret_cast<lame_t (__cdecl*)()>(require("lame_init"));
            api.close = reinterpret_cast<int (__cdecl*)(lame_t)>(require("lame_close"));
            api.set_num_channels = reinterpret_cast<int (__cdecl*)(lame_t, int)>(require("lame_set_num_channels"));
            api.set_in_samplerate = reinterpret_cast<int (__cdecl*)(lame_t, int)>(require("lame_set_in_samplerate"));
            api.set_out_samplerate = reinterpret_cast<int (__cdecl*)(lame_t, int)>(require("lame_set_out_samplerate"));
            api.set_brate = reinterpret_cast<int (__cdecl*)(lame_t, int)>(require("lame_set_brate"));
            api.set_mode = reinterpret_cast<int (__cdecl*)(lame_t, int)>(require("lame_set_mode"));
            api.set_quality = reinterpret_cast<int (__cdecl*)(lame_t, int)>(require("lame_set_quality"));
            api.init_params = reinterpret_cast<int (__cdecl*)(lame_t)>(require("lame_init_params"));
            api.encode_buffer_interleaved = reinterpret_cast<int (__cdecl*)(lame_t, short int*, int, unsigned char*, int)>(require("lame_encode_buffer_interleaved"));
            api.flush = reinterpret_cast<int (__cdecl*)(lame_t, unsigned char*, int)>(require("lame_encode_flush"));
            return api;
        }
    }
    throw std::runtime_error(
        "Unable to load libmp3lame.dll or lame_enc.dll. Place the DLL next to loopback_recorder.exe, set LAME_DLL_PATH, "
        "or install LAME for Windows.");
}

const LameApi& GetLameApi() {
    static std::once_flag flag;
    static LameApi api;
    static std::exception_ptr initError;
    std::call_once(flag, [&]() {
        try {
            api = LoadLameApi();
        } catch (...) {
            initError = std::current_exception();
        }
    });
    if (initError) {
        std::rethrow_exception(initError);
    }
    return api;
}

struct WavMetadata {
    WAVEFORMATEX format{};
    uint64_t dataOffset = 0;
    uint64_t dataSize = 0;
};

void ReadBytes(std::ifstream& stream, char* dest, size_t size) {
    stream.read(dest, static_cast<std::streamsize>(size));
    if (!stream) {
        throw std::runtime_error("Unexpected end of WAV file while reading chunk");
    }
}

WavMetadata ParseWav(std::ifstream& stream) {
    struct RiffHeader {
        char id[4];
        uint32_t size;
        char format[4];
    };
    struct ChunkHeader {
        char id[4];
        uint32_t size;
    };

    RiffHeader riff{};
    ReadBytes(stream, reinterpret_cast<char*>(&riff), sizeof(riff));
    if (std::string(riff.id, riff.id + 4) != "RIFF" || std::string(riff.format, riff.format + 4) != "WAVE") {
        throw std::runtime_error("Input file is not a RIFF/WAVE file");
    }

    WavMetadata metadata;
    bool fmtFound = false;
    bool dataFound = false;

    while (stream && (!fmtFound || !dataFound)) {
        ChunkHeader chunk{};
        stream.read(reinterpret_cast<char*>(&chunk), sizeof(chunk));
        if (!stream) {
            break;
        }
        const std::string chunkId(chunk.id, chunk.id + 4);
        if (chunkId == "fmt ") {
            std::vector<char> buffer(chunk.size);
            ReadBytes(stream, buffer.data(), buffer.size());
            if (chunk.size & 1u) {
                stream.seekg(1, std::ios::cur);
            }
            if (buffer.size() < sizeof(WAVEFORMATEX)) {
                throw std::runtime_error("fmt chunk too small");
            }
            std::memcpy(&metadata.format, buffer.data(), sizeof(WAVEFORMATEX));
            if (metadata.format.wFormatTag == WAVE_FORMAT_EXTENSIBLE && buffer.size() >= sizeof(WAVEFORMATEXTENSIBLE)) {
                auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(buffer.data());
                metadata.format = ext->Format;
                if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_PCM) {
                    metadata.format.wFormatTag = WAVE_FORMAT_PCM;
                } else if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
                    metadata.format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
                }
            }
            fmtFound = true;
        } else if (chunkId == "data") {
            const auto dataPos = stream.tellg();
            metadata.dataOffset = static_cast<uint64_t>(dataPos);
            metadata.dataSize = chunk.size;
            stream.seekg(static_cast<std::streamoff>(chunk.size), std::ios::cur);
            if (chunk.size & 1u) {
                stream.seekg(1, std::ios::cur);
            }
            dataFound = true;
        } else {
            stream.seekg(static_cast<std::streamoff>(chunk.size), std::ios::cur);
            if (chunk.size & 1u) {
                stream.seekg(1, std::ios::cur);
            }
        }
    }

    if (!fmtFound || !dataFound) {
        throw std::runtime_error("WAV file is missing fmt or data chunk");
    }
    if (metadata.format.nChannels == 0 || metadata.format.nSamplesPerSec == 0) {
        throw std::runtime_error("Unsupported WAV format");
    }
    if (metadata.dataSize == 0) {
        throw std::runtime_error("WAV file contains no audio data");
    }
    stream.clear();
    stream.seekg(static_cast<std::streamoff>(metadata.dataOffset), std::ios::beg);
    return metadata;
}

int16_t ClampToInt16(int32_t value) {
    if (value > 32767) {
        return 32767;
    }
    if (value < -32768) {
        return -32768;
    }
    return static_cast<int16_t>(value);
}

int16_t FloatToInt16(float value) {
    const float clamped = std::clamp(value, -1.0f, 1.0f);
    return static_cast<int16_t>(std::lround(clamped * 32767.0f));
}

void ConvertSamples(const uint8_t* source,
                    size_t frames,
                    const WAVEFORMATEX& format,
                    size_t targetChannels,
                    std::vector<int16_t>& destination) {
    destination.resize(frames * targetChannels);
    const size_t srcChannels = format.nChannels;
    if (format.wFormatTag == WAVE_FORMAT_PCM && format.wBitsPerSample == 16) {
        const auto* samples = reinterpret_cast<const int16_t*>(source);
        if (srcChannels == targetChannels) {
            std::copy(samples, samples + frames * targetChannels, destination.begin());
            return;
        }
        for (size_t frame = 0; frame < frames; ++frame) {
            const auto frameOffset = frame * srcChannels;
            if (targetChannels == 1) {
                int32_t acc = 0;
                for (size_t c = 0; c < srcChannels; ++c) {
                    acc += samples[frameOffset + c];
                }
                destination[frame] = ClampToInt16(acc / static_cast<int32_t>(srcChannels));
            } else {
                int32_t leftAcc = 0;
                int32_t rightAcc = 0;
                int leftCount = 0;
                int rightCount = 0;
                for (size_t c = 0; c < srcChannels; ++c) {
                    if ((c % 2) == 0) {
                        leftAcc += samples[frameOffset + c];
                        ++leftCount;
                    } else {
                        rightAcc += samples[frameOffset + c];
                        ++rightCount;
                    }
                }
                if (leftCount == 0) {
                    leftAcc = rightAcc;
                    leftCount = rightCount;
                }
                if (rightCount == 0) {
                    rightAcc = leftAcc;
                    rightCount = leftCount;
                }
                destination[frame * 2] = ClampToInt16(leftAcc / leftCount);
                destination[frame * 2 + 1] = ClampToInt16(rightAcc / rightCount);
            }
        }
    } else if (format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT && format.wBitsPerSample == 32) {
        const auto* samples = reinterpret_cast<const float*>(source);
        if (srcChannels == targetChannels) {
            for (size_t i = 0; i < frames * targetChannels; ++i) {
                destination[i] = FloatToInt16(samples[i]);
            }
            return;
        }
        for (size_t frame = 0; frame < frames; ++frame) {
            const auto frameOffset = frame * srcChannels;
            if (targetChannels == 1) {
                float acc = 0.0f;
                for (size_t c = 0; c < srcChannels; ++c) {
                    acc += samples[frameOffset + c];
                }
                destination[frame] = FloatToInt16(acc / static_cast<float>(srcChannels));
            } else {
                float leftAcc = 0.0f;
                float rightAcc = 0.0f;
                int leftCount = 0;
                int rightCount = 0;
                for (size_t c = 0; c < srcChannels; ++c) {
                    if ((c % 2) == 0) {
                        leftAcc += samples[frameOffset + c];
                        ++leftCount;
                    } else {
                        rightAcc += samples[frameOffset + c];
                        ++rightCount;
                    }
                }
                if (leftCount == 0) {
                    leftAcc = rightAcc;
                    leftCount = rightCount;
                }
                if (rightCount == 0) {
                    rightAcc = leftAcc;
                    rightCount = leftCount;
                }
                destination[frame * 2] = FloatToInt16(leftAcc / static_cast<float>(leftCount));
                destination[frame * 2 + 1] = FloatToInt16(rightAcc / static_cast<float>(rightCount));
            }
        }
    } else {
        throw std::runtime_error("Only 16-bit PCM or 32-bit float WAV files are supported");
    }
}

} // namespace

void Mp3Converter::ConvertWavToMp3(const std::filesystem::path& wavPath,
                                   const std::filesystem::path& mp3Path,
                                   const Mp3ConversionOptions& options,
                                   Logger& logger) {
    if (wavPath.empty()) {
        throw std::runtime_error("Input WAV path is empty");
    }
    if (!std::filesystem::exists(wavPath)) {
        throw std::runtime_error("Input WAV does not exist: " + wavPath.string());
    }

    std::ifstream wavStream(wavPath, std::ios::binary);
    if (!wavStream) {
        throw std::runtime_error("Failed to open WAV file for reading: " + wavPath.string());
    }

    WavMetadata metadata = ParseWav(wavStream);
    const size_t targetChannels = static_cast<size_t>(std::min<uint16_t>(metadata.format.nChannels, 2));
    if (metadata.format.nChannels > targetChannels) {
        logger.Warn(L"MP3 encoder only supports mono/stereo; down-mixing " +
                    std::to_wstring(metadata.format.nChannels) + L" channel(s) to " +
                    std::to_wstring(targetChannels) + L".");
    }

    const auto& lame = GetLameApi();
    struct LameHandle {
        const LameApi* api = nullptr;
        lame_t handle = nullptr;
        ~LameHandle() {
            if (handle && api && api->close) {
                api->close(handle);
            }
        }
    } encoder;
    encoder.api = &lame;
    encoder.handle = lame.init();
    if (!encoder.handle) {
        throw std::runtime_error("lame_init failed");
    }

    const auto bitrate = static_cast<int>(std::clamp<uint32_t>(options.bitrateKbps, 64, 320));
    lame.set_num_channels(encoder.handle, static_cast<int>(targetChannels));
    lame.set_in_samplerate(encoder.handle, static_cast<int>(metadata.format.nSamplesPerSec));
    lame.set_out_samplerate(encoder.handle, static_cast<int>(metadata.format.nSamplesPerSec));
    lame.set_brate(encoder.handle, bitrate);
    lame.set_mode(encoder.handle, targetChannels == 1 ? kLameModeMono : kLameModeStereo);
    lame.set_quality(encoder.handle, 2);
    if (lame.init_params(encoder.handle) < 0) {
        throw std::runtime_error("lame_init_params failed");
    }
    if (!lame.modulePath.empty()) {
        logger.Info(L"[MP3] Using libmp3lame from " + lame.modulePath);
    }
    logger.Info(L"[MP3] Input format: channels=" + std::to_wstring(metadata.format.nChannels) +
                L", rate=" + std::to_wstring(metadata.format.nSamplesPerSec) +
                L" Hz, bits=" + std::to_wstring(metadata.format.wBitsPerSample));

    std::ofstream mp3Stream(mp3Path, std::ios::binary | std::ios::trunc);
    if (!mp3Stream) {
        throw std::runtime_error("Failed to open MP3 file for writing: " + mp3Path.string());
    }

    const size_t frameBytes = metadata.format.nBlockAlign;
    if (frameBytes == 0) {
        throw std::runtime_error("Invalid WAV block alignment");
    }

    std::vector<uint8_t> rawBuffer(frameBytes * kFramesPerChunk);
    std::vector<int16_t> pcmBuffer;
    pcmBuffer.reserve(kFramesPerChunk * targetChannels);
    std::vector<unsigned char> mp3Buffer(static_cast<size_t>(1.25 * kFramesPerChunk) + 8192);

    uint64_t remaining = metadata.dataSize;
    wavStream.seekg(static_cast<std::streamoff>(metadata.dataOffset), std::ios::beg);
    while (remaining > 0) {
        const size_t toRead = static_cast<size_t>(std::min<uint64_t>(remaining, rawBuffer.size()));
        wavStream.read(reinterpret_cast<char*>(rawBuffer.data()), static_cast<std::streamsize>(toRead));
        const std::streamsize bytesRead = wavStream.gcount();
        if (bytesRead <= 0) {
            break;
        }
        remaining -= static_cast<uint64_t>(bytesRead);
        const size_t framesRead = static_cast<size_t>(bytesRead) / frameBytes;
        if (framesRead == 0) {
            break;
        }
        ConvertSamples(rawBuffer.data(), framesRead, metadata.format, targetChannels, pcmBuffer);
        const int encoded = lame.encode_buffer_interleaved(encoder.handle,
                                                           reinterpret_cast<short int*>(pcmBuffer.data()),
                                                           static_cast<int>(framesRead),
                                                           mp3Buffer.data(),
                                                           static_cast<int>(mp3Buffer.size()));
        if (encoded < 0) {
            throw std::runtime_error("lame_encode_buffer_interleaved failed with code " + std::to_string(encoded));
        }
        mp3Stream.write(reinterpret_cast<const char*>(mp3Buffer.data()), encoded);
    }

    const int flushBytes = lame.flush(encoder.handle, mp3Buffer.data(), static_cast<int>(mp3Buffer.size()));
    if (flushBytes < 0) {
        throw std::runtime_error("lame_encode_flush failed with code " + std::to_string(flushBytes));
    }
    if (flushBytes > 0) {
        mp3Stream.write(reinterpret_cast<const char*>(mp3Buffer.data()), flushBytes);
    }
    mp3Stream.flush();

    logger.Info(L"MP3 created: " + mp3Path.wstring());
}

Mp3StreamWriter::Mp3StreamWriter(const std::filesystem::path& path,
                                 const WAVEFORMATEX& format,
                                 const Mp3ConversionOptions& options,
                                 Logger& logger)
    : path_(path), logger_(&logger) {
    try {
        format_ = format;
        if (format_.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
            format_.cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
            const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(&format);
            if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_PCM) {
                format_.wFormatTag = WAVE_FORMAT_PCM;
            } else if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
                format_.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
            }
        }

        bytesPerFrame_ = format_.nBlockAlign;
        if (bytesPerFrame_ == 0) {
            throw std::runtime_error("Invalid audio block alignment");
        }

        targetChannels_ = static_cast<size_t>(std::min<uint16_t>(format_.nChannels, 2));
        if (format_.nChannels > targetChannels_) {
            logger.Info(L"[MP3] Down-mixing " + std::to_wstring(format_.nChannels) +
                        L" channel(s) to " + std::to_wstring(targetChannels_) + L".");
        }

        const auto& lame = GetLameApi();
        api_ = &lame;
        handle_ = lame.init();
        if (!handle_) {
            throw std::runtime_error("lame_init failed");
        }

        const auto bitrate = static_cast<int>(std::clamp<uint32_t>(options.bitrateKbps, 64, 320));
        lame.set_num_channels(handle_, static_cast<int>(targetChannels_));
        lame.set_in_samplerate(handle_, static_cast<int>(format_.nSamplesPerSec));
        lame.set_out_samplerate(handle_, static_cast<int>(format_.nSamplesPerSec));
        lame.set_brate(handle_, bitrate);
        lame.set_mode(handle_, targetChannels_ == 1 ? kLameModeMono : kLameModeStereo);
        lame.set_quality(handle_, 2);
        if (lame.init_params(handle_) < 0) {
            throw std::runtime_error("lame_init_params failed");
        }
        if (!lame.modulePath.empty()) {
            logger.Info(L"[MP3] Using libmp3lame from " + lame.modulePath);
        }
        logger.Info(L"[MP3] Live encoding: channels=" + std::to_wstring(format_.nChannels) +
                    L", rate=" + std::to_wstring(format_.nSamplesPerSec) +
                    L" Hz, bits=" + std::to_wstring(format_.wBitsPerSample) +
                    L", bitrate=" + std::to_wstring(bitrate) + L" kbps.");

        mp3Buffer_.resize(8192);

        stream_.open(path_, std::ios::binary | std::ios::trunc);
        if (!stream_) {
            throw std::runtime_error("Failed to open MP3 file for writing: " + path_.string());
        }
    } catch (...) {
        if (api_ && handle_) {
            const auto* lame = reinterpret_cast<const LameApi*>(api_);
            if (lame->close) {
                lame->close(handle_);
            }
            handle_ = nullptr;
        }
        throw;
    }
}

Mp3StreamWriter::~Mp3StreamWriter() {
    Close();
}

void Mp3StreamWriter::Write(const BYTE* data, size_t byteCount) {
    if (finalized_) {
        return;
    }
    if (!stream_) {
        throw std::runtime_error("MP3 stream is not open");
    }
    if (byteCount == 0) {
        return;
    }

    const size_t existing = pending_.size();
    pending_.resize(existing + byteCount);
    std::memcpy(pending_.data() + existing, data, byteCount);

    const size_t framesAvailable = pending_.size() / bytesPerFrame_;
    if (framesAvailable == 0) {
        return;
    }
    const size_t bytesToProcess = framesAvailable * bytesPerFrame_;

    ConvertSamples(pending_.data(), framesAvailable, format_, targetChannels_, pcmBuffer_);
    const size_t needed = static_cast<size_t>(1.25 * framesAvailable) + 7200;
    if (mp3Buffer_.size() < needed) {
        mp3Buffer_.resize(needed);
    }

    const auto* lame = reinterpret_cast<const LameApi*>(api_);
    const int encoded = lame->encode_buffer_interleaved(handle_,
                                                        reinterpret_cast<short int*>(pcmBuffer_.data()),
                                                        static_cast<int>(framesAvailable),
                                                        mp3Buffer_.data(),
                                                        static_cast<int>(mp3Buffer_.size()));
    if (encoded < 0) {
        throw std::runtime_error("lame_encode_buffer_interleaved failed with code " + std::to_string(encoded));
    }
    if (encoded > 0) {
        stream_.write(reinterpret_cast<const char*>(mp3Buffer_.data()), encoded);
    }

    const size_t remainder = pending_.size() - bytesToProcess;
    if (remainder > 0) {
        std::memmove(pending_.data(), pending_.data() + bytesToProcess, remainder);
    }
    pending_.resize(remainder);
}

void Mp3StreamWriter::Flush() {
    if (finalized_ || !stream_) {
        return;
    }
    stream_.flush();
    if (!stream_) {
        throw std::runtime_error("Failed to flush MP3 data to disk");
    }
}

void Mp3StreamWriter::Close() {
    if (finalized_) {
        return;
    }
    finalized_ = true;

    if (stream_) {
        if (!pending_.empty()) {
            const size_t remainder = pending_.size() % bytesPerFrame_;
            if (remainder != 0) {
                const size_t pad = bytesPerFrame_ - remainder;
                pending_.resize(pending_.size() + pad, 0);
            }
            const size_t framesAvailable = pending_.size() / bytesPerFrame_;
            if (framesAvailable > 0) {
                ConvertSamples(pending_.data(), framesAvailable, format_, targetChannels_, pcmBuffer_);
                const size_t needed = static_cast<size_t>(1.25 * framesAvailable) + 7200;
                if (mp3Buffer_.size() < needed) {
                    mp3Buffer_.resize(needed);
                }
                const auto* lame = reinterpret_cast<const LameApi*>(api_);
                const int encoded = lame->encode_buffer_interleaved(handle_,
                                                                    reinterpret_cast<short int*>(pcmBuffer_.data()),
                                                                    static_cast<int>(framesAvailable),
                                                                    mp3Buffer_.data(),
                                                                    static_cast<int>(mp3Buffer_.size()));
                if (encoded < 0) {
                    throw std::runtime_error("lame_encode_buffer_interleaved failed with code " + std::to_string(encoded));
                }
                if (encoded > 0) {
                    stream_.write(reinterpret_cast<const char*>(mp3Buffer_.data()), encoded);
                }
            }
            pending_.clear();
        }

        const auto* lame = reinterpret_cast<const LameApi*>(api_);
        if (lame && handle_) {
            if (mp3Buffer_.empty()) {
                mp3Buffer_.resize(8192);
            }
            const int flushBytes = lame->flush(handle_, mp3Buffer_.data(), static_cast<int>(mp3Buffer_.size()));
            if (flushBytes < 0) {
                throw std::runtime_error("lame_encode_flush failed with code " + std::to_string(flushBytes));
            }
            if (flushBytes > 0) {
                stream_.write(reinterpret_cast<const char*>(mp3Buffer_.data()), flushBytes);
            }
        }
        stream_.flush();
        stream_.close();
    }

    if (api_ && handle_) {
        const auto* lame = reinterpret_cast<const LameApi*>(api_);
        if (lame->close) {
            lame->close(handle_);
        }
        handle_ = nullptr;
    }

    if (logger_) {
        logger_->Info(L"MP3 stream finalized: " + path_.wstring());
    }
}
