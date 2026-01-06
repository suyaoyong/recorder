#pragma once

#include <filesystem>

std::filesystem::path BuildSegmentPath(const std::filesystem::path& basePath, size_t segmentIndex);
