#include "DeviceEnumerator.h"
#include "LoopbackRecorder.h"
#include "Logger.h"
#include "HResultUtils.h"
#include "RecordingUtils.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwctype>
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

std::wstring Trim(const std::wstring& text) {
    const size_t first = text.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) {
        return L"";
    }
    const size_t last = text.find_last_not_of(L" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::wstring ToLower(const std::wstring& text) {
    std::wstring result = text;
    std::transform(result.begin(), result.end(), result.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return result;
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
    std::optional<int> segmentSeconds;
    std::optional<uint64_t> segmentBytes;
    bool convertToMp3 = false;
    std::optional<int> mp3BitrateKbps;
};

void PrintUsage() {
    std::wcout << L"Loopback Recorder\n"
               << L"Usage: loopback_recorder [--list-devices] [--device-index N] [--seconds N] [--out path]\n"
               << L"                        [--latency-ms N] [--watchdog-ms N] [--buffer-ms N]\n"
               << L"                        [--segment-seconds N] [--segment-bytes N]\n"
               << L"                        [--mp3] [--mp3-bitrate K]\n"
               << L"                        [--fail-on-glitch] [--mix-mic] [--log-file path] [--quiet]\n"
               << L"Notes:\n"
               << L"  - Output format is inferred from --out extension (.mp3 or .wav). Default is MP3.\n"
               << L"  - --mp3 is a legacy flag that forces .mp3 if no extension is provided.\n"
               << L"Examples:\n"
               << L"  loopback_recorder --seconds 30 --out demo.mp3\n"
               << L"  loopback_recorder --segment-seconds 300 --out session.wav\n"
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

bool ParseUint64(const std::wstring& text, uint64_t& value) {
    try {
        size_t idx = 0;
        unsigned long long parsed = std::stoull(text, &idx);
        if (idx != text.size()) {
            return false;
        }
        value = static_cast<uint64_t>(parsed);
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
        } else if (arg == L"--segment-seconds") {
            if (i + 1 >= argc) {
                throw std::runtime_error("--segment-seconds requires a value");
            }
            int value = 0;
            if (!ParseInt(argv[++i], value) || value <= 0) {
                throw std::runtime_error("--segment-seconds must be a positive integer");
            }
            opts.segmentSeconds = value;
        } else if (arg == L"--segment-bytes") {
            if (i + 1 >= argc) {
                throw std::runtime_error("--segment-bytes requires a value");
            }
            uint64_t value = 0;
            if (!ParseUint64(argv[++i], value) || value == 0) {
                throw std::runtime_error("--segment-bytes must be a positive integer");
            }
            opts.segmentBytes = value;
        } else if (arg == L"--log-file") {
            if (i + 1 >= argc) {
                throw std::runtime_error("--log-file requires a path");
            }
            opts.logFile = std::filesystem::path(argv[++i]);
        } else if (arg == L"--quiet") {
            opts.quiet = true;
        } else if (arg == L"--mp3") {
            opts.convertToMp3 = true;
        } else if (arg == L"--mp3-bitrate") {
            if (i + 1 >= argc) {
                throw std::runtime_error("--mp3-bitrate requires a value");
            }
            int value = 0;
            if (!ParseInt(argv[++i], value) || value < 32 || value > 320) {
                throw std::runtime_error("--mp3-bitrate must be between 32 and 320 kbps");
            }
            opts.mp3BitrateKbps = value;
        } else {
            throw std::runtime_error("Unknown argument: " + std::string(arg.begin(), arg.end()));
        }
    }
    return opts;
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

        RecorderConfig config;
        config.outputPath = options.outputPath.value_or(DefaultOutputPath());
        if (options.convertToMp3) {
            config.outputPath = EnsureExtension(config.outputPath, L".mp3");
        } else if (config.outputPath.extension().empty()) {
            config.outputPath = EnsureExtension(config.outputPath, L".mp3");
        }
        if (options.mp3BitrateKbps) {
            config.mp3BitrateKbps = static_cast<uint32_t>(*options.mp3BitrateKbps);
        }
        if (options.mp3BitrateKbps && ToLower(config.outputPath.extension().wstring()) != L".mp3") {
            logger.Warn(L"--mp3-bitrate is ignored when output is not MP3.");
        }
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
        if (options.segmentSeconds) {
            config.segmentDuration = std::chrono::seconds(*options.segmentSeconds);
        }
        if (options.segmentBytes) {
            config.segmentBytes = options.segmentBytes;
        }
        std::atomic<bool> stopRequested = false;
        std::atomic<bool> pauseRequested = false;
        std::atomic<bool> segmentRequested = false;

        const std::filesystem::path baseOutputPath = config.outputPath;
        constexpr int kMaxReconnectAttempts = 3;
        constexpr int kReconnectDelayMs = 1500;
        int reconnectAttempts = 0;

        if (config.maxDuration) {
            std::wcout << L"Target duration: " << config.maxDuration->count() << L" seconds" << std::endl;
        }
        std::wcout << L"Press ENTER to stop." << std::endl;
        std::wcout << L"Type 'P' + ENTER to toggle pause/resume, 'S' + ENTER to roll to a new file." << std::endl;

        std::thread commandThread([&]() {
            while (true) {
                std::wstring line;
                if (!std::getline(std::wcin, line)) {
                    stopRequested = true;
                    break;
                }
                std::wstring command = ToLower(Trim(line));
                if (command.empty()) {
                    stopRequested = true;
                    break;
                }
                if (command == L"p") {
                    bool newState = !pauseRequested.load();
                    pauseRequested.store(newState);
                    std::wcout << (newState ? L"[Command] Paused." : L"[Command] Resumed.") << std::endl;
                    continue;
                }
                if (command == L"s") {
                    segmentRequested.store(true);
                    std::wcout << L"[Command] Segment rotation requested." << std::endl;
                    continue;
                }
                std::wcout << L"Unknown command. ENTER=Stop, P=Pause/Resume, S=New segment." << std::endl;
            }
        });
        commandThread.detach();

        RecorderControls controls;
        controls.shouldStop = [&stopRequested]() {
            return stopRequested.load();
        };
        controls.isPaused = [&pauseRequested]() {
            return pauseRequested.load();
        };
        controls.requestNewSegment = [&segmentRequested]() -> bool {
            bool expected = true;
            return segmentRequested.compare_exchange_strong(expected, false);
        };

        auto ensureParentDirectory = [](const std::filesystem::path& path) {
            if (path.has_parent_path() && !path.parent_path().empty()) {
                std::filesystem::create_directories(path.parent_path());
            }
        };

        while (true) {
            DeviceEnumerator attemptEnumerator;
            Microsoft::WRL::ComPtr<IMMDevice> device;
            if (options.deviceIndex) {
                device = attemptEnumerator.GetDeviceByIndex(*options.deviceIndex);
            } else {
                device = attemptEnumerator.GetDefaultRenderDevice();
            }
            if (!device) {
                throw std::runtime_error("Unable to acquire playback device");
            }
            std::wstring friendlyName = DeviceEnumerator::GetFriendlyName(device.Get());
            logger.Info(L"Selected playback device: " + friendlyName);

            config.outputPath = EnsureUniquePath(baseOutputPath);
            ensureParentDirectory(config.outputPath);
            logger.Info(L"Output file: " + config.outputPath.wstring());

            std::wcout << L"Recording system audio to " << config.outputPath.wstring() << std::endl;
            if (reconnectAttempts > 0) {
                std::wcout << L"[Reconnect] Attempt " << reconnectAttempts << L"/" << kMaxReconnectAttempts << std::endl;
            }

            LoopbackRecorder recorder(device, logger);
            RecorderStats stats = recorder.Record(config, controls);

            const bool userRequestedStop = stopRequested.load();
            std::wcout << L"Recording finished." << std::endl;
            std::wcout << L"Captured frames: " << stats.framesCaptured
                       << L", silent frames: " << stats.silentFrames
                       << L", paused frames: " << stats.framesWhilePaused
                       << L", glitches: " << stats.glitchCount
                       << L", capture timeouts: " << stats.watchdogTimeouts
                       << L", ring waits: " << stats.ringBufferWaits
                       << L", ring timeouts: " << stats.ringBufferTimeouts
                       << L", writer waits: " << stats.writerWaitTimeouts
                       << L", dropped frames: " << stats.framesDropped
                       << L", segments: " << stats.segmentsWritten << std::endl;
            if (stats.deviceInvalidated) {
                std::wcout << L"Recording stopped because the playback device changed or disconnected." << std::endl;
            }
            if (stats.glitchCount > 0 || stats.watchdogTimeouts > 0) {
                std::wcout << L"Tip: increase --latency-ms or --watchdog-ms for noisier systems." << std::endl;
            }

            if (stats.deviceInvalidated && !userRequestedStop) {
                if (reconnectAttempts >= kMaxReconnectAttempts) {
                    logger.Warn(L"Playback device disconnected too many times; stopping.");
                    break;
                }
                ++reconnectAttempts;
                logger.Warn(L"Playback device disconnected; retrying in " +
                            std::to_wstring(kReconnectDelayMs) + L" ms (attempt " +
                            std::to_wstring(reconnectAttempts) + L"/" + std::to_wstring(kMaxReconnectAttempts) + L").");
                std::this_thread::sleep_for(std::chrono::milliseconds(kReconnectDelayMs));
                stopRequested = false;
                continue;
            }
            break;
        }
        stopRequested = true;
        return 0;
    } catch (const std::exception& ex) {
        std::string message = ex.what();
        logger.Error(L"Fatal error: " + ToWide(message));
        std::cerr << "Error: " << message << std::endl;
        return 1;
    }
}
