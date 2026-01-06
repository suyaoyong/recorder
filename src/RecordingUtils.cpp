#include "RecordingUtils.h"

#include "SegmentNaming.h"

#include <chrono>
#include <cwctype>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

std::wstring ToWide(const std::string& text) {
    return std::wstring(text.begin(), text.end());
}

}

std::filesystem::path DefaultOutputPath() {
    auto now = std::chrono::system_clock::now();
    auto now_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &now_t);
    wchar_t buffer[64];
    wcsftime(buffer, std::size(buffer), L"loopback_%Y%m%d_%H%M%S.mp3", &tm);
    return buffer;
}

std::filesystem::path EnsureExtension(std::filesystem::path path, const std::wstring& desiredExtension) {
    std::wstring current = path.extension().wstring();
    auto normalize = [](std::wstring value) {
        for (auto& ch : value) {
            ch = static_cast<wchar_t>(towlower(ch));
        }
        return value;
    };
    if (normalize(current) != normalize(desiredExtension)) {
        path.replace_extension(desiredExtension);
    }
    return path;
}

void ConvertRecordedSegmentsToMp3(const std::filesystem::path& wavBasePath,
                                  const std::filesystem::path& mp3BasePath,
                                  uint32_t segmentCount,
                                  const Mp3ConversionOptions& options,
                                  Logger& logger) {
    if (segmentCount == 0) {
        return;
    }
    logger.Info(L"Converting " + std::to_wstring(segmentCount) + L" segment(s) to MP3...");
    for (uint32_t i = 0; i < segmentCount; ++i) {
        const auto wavSegment = BuildSegmentPath(wavBasePath, i);
        const auto mp3Segment = BuildSegmentPath(mp3BasePath, i);
        if (!std::filesystem::exists(wavSegment)) {
            throw std::runtime_error("Missing WAV segment for conversion: " + wavSegment.string());
        }
        logger.Info(L"[MP3] Encoding segment #" + std::to_wstring(i + 1) + L": " + mp3Segment.wstring());
        Mp3Converter::ConvertWavToMp3(wavSegment, mp3Segment, options, logger);
    }
    logger.Info(L"MP3 conversion finished.");
}
