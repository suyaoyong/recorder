#include "Logger.h"

#include <chrono>
#include <codecvt>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <locale>
#include <iostream>
#include <utility>

void Logger::EnableFileLogging(const std::filesystem::path& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (path.has_parent_path() && !path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path());
    }
    file_.open(path, std::ios::app);
    if (!file_) {
        throw std::runtime_error("Failed to open log file: " + path.string());
    }
    // Ensure Unicode device names serialize reliably (UTF-8 on disk)
    file_.imbue(std::locale(std::locale::classic(), new std::codecvt_utf8_utf16<wchar_t>()));
    fileEnabled_ = true;
    filePath_ = path;
}

void Logger::SetSink(std::function<void(LogLevel, const std::wstring&)> sink) {
    std::lock_guard<std::mutex> lock(mutex_);
    sink_ = std::move(sink);
}

void Logger::Log(LogLevel level, const std::wstring& message) {
    const std::wstring line = Timestamp() + L" [" + LevelLabel(level) + L"] " + message;
    if (level == LogLevel::Error) {
        std::wcerr << line << std::endl;
    } else {
        std::wcout << line << std::endl;
    }
    std::function<void(LogLevel, const std::wstring&)> sinkCopy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (fileEnabled_ && file_) {
            file_ << line << std::endl;
        }
        sinkCopy = sink_;
    }
    if (sinkCopy) {
        sinkCopy(level, line);
    }
}

std::wstring Logger::Timestamp() const {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto timeT = system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &timeT);
#else
    localtime_r(&timeT, &tm);
#endif
    std::wstringstream ss;
    ss << std::put_time(&tm, L"%Y-%m-%d %H:%M:%S");
    return ss.str();
}

std::wstring Logger::LevelLabel(LogLevel level) const {
    switch (level) {
    case LogLevel::Info:
        return L"INFO";
    case LogLevel::Warning:
        return L"WARN";
    case LogLevel::Error:
        return L"ERROR";
    }
    return L"LOG";
}
