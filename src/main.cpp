#include "DeviceEnumerator.h"
#include "LoopbackRecorder.h"
#include "Logger.h"
#include "HResultUtils.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <ctime>
#include <iterator>
#include <stdexcept>
#include <cstdint>

namespace {
std::wstring ToWide(const std::string& text) {
    return std::wstring(text.begin(), text.end());
}

struct CommandLineOptions {
    bool listDevices = false;
    std::optional<size_t> deviceIndex;
    std::optional<int> seconds;
    std::optional<std::filesystem::path> outputPath;
    bool mixMic = false;
    bool showHelp = false;
    std::optional<int> latencyMs;
    std::optional<int> watchdogMs;
    bool failOnGlitch = false;
    std::optional<int> bufferMs;
    std::optional<std::filesystem::path> logFile;
    bool quiet = false;
};

void PrintUsage() {
    std::wcout << L"Loopback Recorder\n"
               << L"Usage: loopback_recorder [--list-devices] [--device-index N] [--seconds N] [--out path]\n"
               << L"                        [--latency-ms N] [--watchdog-ms N] [--buffer-ms N]\n"
               << L"                        [--fail-on-glitch] [--mix-mic] [--log-file path] [--quiet]\n"
               << L"Examples:\n"
               << L"  loopback_recorder --seconds 30 --out demo.wav\n"
               << L"  loopback_recorder --device-index 1\n";
}

bool ParseInt(const std::wstring& text, int& value) {
    try {
        size_t idx = 0;
        int parsed = std::stoi(text, &idx);
        if (idx != text.size()) {
            return false;
        }
        value = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

CommandLineOptions ParseArgs(int argc, wchar_t** argv) {
    CommandLineOptions opts;
    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg == L"--help" || arg == L"-h") {
            opts.showHelp = true;
        } else if (arg == L"--list-devices") {
            opts.listDevices = true;
        } else if (arg == L"--device-index") {
            if (i + 1 >= argc) {
                throw std::runtime_error("--device-index requires a value");
            }
            int value = 0;
            if (!ParseInt(argv[++i], value) || value < 0) {
                throw std::runtime_error("Invalid device index");
            }
            opts.deviceIndex = static_cast<size_t>(value);
        } else if (arg == L"--seconds") {
            if (i + 1 >= argc) {
                throw std::runtime_error("--seconds requires a value");
            }
            int value = 0;
            if (!ParseInt(argv[++i], value) || value <= 0) {
                throw std::runtime_error("--seconds must be a positive integer");
            }
            opts.seconds = value;
        } else if (arg == L"--out") {
            if (i + 1 >= argc) {
                throw std::runtime_error("--out requires a path");
            }
            opts.outputPath = std::filesystem::path(argv[++i]);
        } else if (arg == L"--mix-mic") {
            opts.mixMic = true;
        } else if (arg == L"--latency-ms") {
            if (i + 1 >= argc) {
                throw std::runtime_error("--latency-ms requires a value");
            }
            int value = 0;
            if (!ParseInt(argv[++i], value) || value <= 0) {
                throw std::runtime_error("--latency-ms must be > 0");
            }
            opts.latencyMs = value;
        } else if (arg == L"--watchdog-ms") {
            if (i + 1 >= argc) {
                throw std::runtime_error("--watchdog-ms requires a value");
            }
            int value = 0;
            if (!ParseInt(argv[++i], value) || value < 100) {
                throw std::runtime_error("--watchdog-ms must be >= 100 ms");
            }
            opts.watchdogMs = value;
        } else if (arg == L"--fail-on-glitch") {
            opts.failOnGlitch = true;
        } else if (arg == L"--buffer-ms") {
            if (i + 1 >= argc) {
                throw std::runtime_error("--buffer-ms requires a value");
            }
            int value = 0;
            if (!ParseInt(argv[++i], value) || value < 200) {
                throw std::runtime_error("--buffer-ms must be >= 200 ms");
            }
            opts.bufferMs = value;
        } else if (arg == L"--log-file") {
            if (i + 1 >= argc) {
                throw std::runtime_error("--log-file requires a path");
            }
            opts.logFile = std::filesystem::path(argv[++i]);
        } else if (arg == L"--quiet") {
            opts.quiet = true;
        } else {
            throw std::runtime_error("Unknown argument: " + std::string(arg.begin(), arg.end()));
        }
    }
    return opts;
}

std::filesystem::path DefaultOutputPath() {
    auto now = std::chrono::system_clock::now();
    auto now_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &now_t);
    wchar_t buffer[64];
    wcsftime(buffer, std::size(buffer), L"loopback_%Y%m%d_%H%M%S.wav", &tm);
    return buffer;
}

class ComGuard {
public:
    ComGuard() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr)) {
            throw std::runtime_error("COM initialization failed: " + DescribeHRESULTA(hr));
        }
    }
    ~ComGuard() {
        CoUninitialize();
    }
};
}

int wmain(int argc, wchar_t** argv) {
    Logger logger;
    try {
        CommandLineOptions options = ParseArgs(argc, argv);
        if (options.showHelp) {
            PrintUsage();
            return 0;
        }

        if (options.logFile) {
            logger.EnableFileLogging(*options.logFile);
            logger.Info(L"File logging enabled: " + options.logFile->wstring());
        }
        logger.Info(L"Loopback Recorder starting.");

        ComGuard com;
        DeviceEnumerator enumerator;

        if (options.listDevices) {
            logger.Info(L"Listing playback devices...");
            auto devices = enumerator.ListRenderDevices();
            std::wcout << L"Playback devices:" << std::endl;
            for (size_t i = 0; i < devices.size(); ++i) {
                std::wcout << L"  [" << i << L"] " << devices[i].name;
                if (devices[i].isDefault) {
                    std::wcout << L" (default)";
                }
                std::wcout << std::endl;
            }
            return 0;
        }

        Microsoft::WRL::ComPtr<IMMDevice> device;
        if (options.deviceIndex) {
            device = enumerator.GetDeviceByIndex(*options.deviceIndex);
        } else {
            device = enumerator.GetDefaultRenderDevice();
        }
        if (!device) {
            throw std::runtime_error("Unable to acquire playback device");
        }
        std::wstring friendlyName = DeviceEnumerator::GetFriendlyName(device.Get());
        logger.Info(L"Selected playback device: " + friendlyName);

        RecorderConfig config;
        config.outputPath = options.outputPath.value_or(DefaultOutputPath());
        config.enableMicMix = options.mixMic; // currently placeholder
        if (options.seconds) {
            config.maxDuration = std::chrono::seconds(*options.seconds);
        }
        if (options.latencyMs) {
            config.latencyHint = std::chrono::milliseconds(*options.latencyMs);
        }
        if (options.watchdogMs) {
            config.watchdogTimeout = std::chrono::milliseconds(*options.watchdogMs);
        }
        config.failOnGlitch = options.failOnGlitch;
        if (options.bufferMs) {
            config.ringBufferSize = std::chrono::milliseconds(*options.bufferMs);
        }
        config.quietStatusUpdates = options.quiet;
        if (config.outputPath.has_parent_path() && !config.outputPath.parent_path().empty()) {
            std::filesystem::create_directories(config.outputPath.parent_path());
        }

        logger.Info(L"Output file: " + config.outputPath.wstring());

        LoopbackRecorder recorder(device, logger);
        std::atomic<bool> stopRequested = false;

        std::wcout << L"Recording system audio to " << config.outputPath.wstring() << std::endl;
        if (config.maxDuration) {
            std::wcout << L"Target duration: " << config.maxDuration->count() << L" seconds" << std::endl;
        }
        std::wcout << L"Press ENTER to stop early..." << std::endl;

        std::thread stopper([&stopRequested]() {
            std::wstring line;
            std::getline(std::wcin, line);
            stopRequested = true;
        });
        stopper.detach();

        RecorderStats stats = recorder.Record(config, [&stopRequested]() {
            return stopRequested.load();
        });

        stopRequested = true;
        std::wcout << L"Recording finished." << std::endl;
        std::wcout << L"Captured frames: " << stats.framesCaptured
                   << L", silent frames: " << stats.silentFrames
                   << L", glitches: " << stats.glitchCount
                   << L", capture timeouts: " << stats.watchdogTimeouts
                   << L", ring waits: " << stats.ringBufferWaits
                   << L", ring timeouts: " << stats.ringBufferTimeouts
                   << L", writer waits: " << stats.writerWaitTimeouts
                   << L", dropped frames: " << stats.framesDropped << std::endl;
        if (stats.deviceInvalidated) {
            std::wcout << L"Recording stopped because the playback device changed or disconnected. Re-run after selecting an active device." << std::endl;
        }
        if (stats.glitchCount > 0 || stats.watchdogTimeouts > 0) {
            std::wcout << L"Tip: increase --latency-ms or --watchdog-ms for noisier systems." << std::endl;
        }
        return 0;
    } catch (const std::exception& ex) {
        std::string message = ex.what();
        logger.Error(L"Fatal error: " + ToWide(message));
        std::cerr << "Error: " << message << std::endl;
        return 1;
    }
}
