#include "SegmentNaming.h"

#include <iomanip>
#include <sstream>

std::filesystem::path BuildSegmentPath(const std::filesystem::path& basePath, size_t segmentIndex) {
    auto directory = basePath.parent_path();
    std::wstring stem = basePath.stem().wstring();
    std::wstring extension = basePath.extension().wstring();
    if (stem.empty()) {
        stem = L"segment";
    }
    std::wstringstream builder;
    builder << stem << L"_" << std::setw(3) << std::setfill(L'0') << (segmentIndex + 1);
    std::filesystem::path filename = builder.str();
    if (!extension.empty()) {
        filename += extension;
    }
    return directory / filename;
}
