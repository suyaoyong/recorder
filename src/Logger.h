#pragma once

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

enum class LogLevel {
    Info,
    Warning,
    Error
};

class Logger {
public:
    Logger() = default;

    void EnableFileLogging(const std::filesystem::path& path);

    void Log(LogLevel level, const std::wstring& message);
    void Info(const std::wstring& message) { Log(LogLevel::Info, message); }
    void Warn(const std::wstring& message) { Log(LogLevel::Warning, message); }
    void Error(const std::wstring& message) { Log(LogLevel::Error, message); }

private:
    std::wstring Timestamp() const;
    std::wstring LevelLabel(LogLevel level) const;

    std::wofstream file_;
    bool fileEnabled_ = false;
    std::filesystem::path filePath_;
    mutable std::mutex mutex_;
};