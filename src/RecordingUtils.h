#pragma once

#include "Mp3Converter.h"
#include "Logger.h"

#include <filesystem>

std::filesystem::path DefaultOutputPath();
std::filesystem::path EnsureExtension(std::filesystem::path path, const std::wstring& desiredExtension);
void ConvertRecordedSegmentsToMp3(const std::filesystem::path& wavBasePath,
                                  const std::filesystem::path& mp3BasePath,
                                  uint32_t segmentCount,
                                  const Mp3ConversionOptions& options,
                                  Logger& logger);
