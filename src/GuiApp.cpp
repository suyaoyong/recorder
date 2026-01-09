#include "LoopbackRecorder.h"
#include "DeviceEnumerator.h"
#include "RecordingUtils.h"
#include "SegmentNaming.h"

#include <windows.h>
#include <commdlg.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwchar>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

namespace {

constexpr UINT WM_APP_LOG_MESSAGE = WM_APP + 1;
constexpr UINT WM_APP_RECORDER_DONE = WM_APP + 2;
constexpr UINT WM_APP_OUTPUT_PATH = WM_APP + 3;
constexpr UINT WM_APP_STATE_UPDATE = WM_APP + 4;

enum ControlId : int {
    IDC_OUTPUT_EDIT = 1001,
    IDC_BROWSE_BUTTON,
    IDC_BROWSE_FOLDER,
    IDC_OPEN_FOLDER,
    IDC_MP3_CHECK,
    IDC_BITRATE_EDIT,
    IDC_START_BUTTON,
    IDC_STOP_BUTTON,
    IDC_PAUSE_BUTTON,
    IDC_LOG_EDIT
};

struct AppState {
    HWND hwnd = nullptr;
    HWND outputEdit = nullptr;
    HWND mp3Checkbox = nullptr;
    HWND bitrateEdit = nullptr;
    HWND startButton = nullptr;
    HWND stopButton = nullptr;
    HWND pauseButton = nullptr;
    HWND logEdit = nullptr;
    HWND statusLabel = nullptr;
    std::thread worker;
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> pauseRequested{false};
    int defaultBitrate = 192;
    enum class RecorderState { Idle, Starting, Recording, Stopping, Recovering };
    RecorderState state = RecorderState::Idle;
    std::filesystem::path currentOutputPath;
    std::chrono::steady_clock::time_point startTime{};
    std::chrono::steady_clock::time_point pauseStart{};
    std::chrono::milliseconds pausedTotal{0};
    bool paused = false;
};

class ComGuard {
public:
    ComGuard() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr)) {
            throw std::runtime_error("COM initialization failed");
        }
    }
    ~ComGuard() {
        CoUninitialize();
    }
};

std::wstring ToWide(const std::string& text) {
    return std::wstring(text.begin(), text.end());
}

std::wstring GetWindowTextString(HWND hwnd) {
    const int length = GetWindowTextLengthW(hwnd);
    if (length == 0) {
        return L"";
    }
    std::wstring buffer(length, L'\0');
    GetWindowTextW(hwnd, buffer.data(), length + 1);
    return buffer;
}

void AppendLog(HWND edit, const std::wstring& message) {
    const int len = GetWindowTextLengthW(edit);
    SendMessageW(edit, EM_SETSEL, len, len);
    std::wstring text = message;
    text += L"\r\n";
    SendMessageW(edit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
}

std::wstring FormatBytes(uintmax_t bytes) {
    const double kb = 1024.0;
    const double mb = kb * 1024.0;
    const double gb = mb * 1024.0;
    std::wstringstream ss;
    if (bytes >= static_cast<uintmax_t>(gb)) {
        ss << std::fixed << std::setprecision(1);
        ss << (bytes / gb) << L" GB";
    } else if (bytes >= static_cast<uintmax_t>(mb)) {
        ss << std::fixed << std::setprecision(1);
        ss << (bytes / mb) << L" MB";
    } else if (bytes >= static_cast<uintmax_t>(kb)) {
        ss << std::fixed << std::setprecision(1);
        ss << (bytes / kb) << L" KB";
    } else {
        ss << bytes << L" B";
    }
    return ss.str();
}

void UpdateStatusText(AppState* state) {
    if (!state || !state->statusLabel) {
        return;
    }
    std::wstring text = L"Status: ";
    switch (state->state) {
    case AppState::RecorderState::Idle:
        text += L"Idle";
        break;
    case AppState::RecorderState::Starting:
        text += L"Starting...";
        break;
    case AppState::RecorderState::Recovering:
        text += L"Reconnecting...";
        break;
    case AppState::RecorderState::Stopping:
        text += L"Stopping...";
        break;
    case AppState::RecorderState::Recording: {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - state->startTime - state->pausedTotal);
        if (state->paused) {
            elapsed -= std::chrono::duration_cast<std::chrono::seconds>(now - state->pauseStart);
        }
        if (elapsed.count() < 0) {
            elapsed = std::chrono::seconds(0);
        }
        const int hours = static_cast<int>(elapsed.count() / 3600);
        const int mins = static_cast<int>((elapsed.count() % 3600) / 60);
        const int secs = static_cast<int>(elapsed.count() % 60);
        wchar_t timeBuf[32];
        swprintf_s(timeBuf, L"%02d:%02d:%02d", hours, mins, secs);
        text += L"Recording ";
        text += timeBuf;
        if (state->paused) {
            text += L" (Paused)";
        }
        std::filesystem::path sizePath = state->currentOutputPath;
        if (!sizePath.empty()) {
            sizePath = BuildSegmentPath(sizePath, 0);
        }
        uintmax_t bytes = 0;
        std::error_code ec;
        if (!sizePath.empty() && std::filesystem::exists(sizePath, ec)) {
            bytes = std::filesystem::file_size(sizePath, ec);
        }
        text += L" | ";
        text += FormatBytes(bytes);
        break;
    }
    default:
        text += L"Unknown";
        break;
    }
    SetWindowTextW(state->statusLabel, text.c_str());
}

void SetControlFont(HWND control, HFONT font) {
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

void UpdateControlsForState(AppState* state) {
    const bool canStart = state->state == AppState::RecorderState::Idle;
    const bool canStop = state->state == AppState::RecorderState::Starting
        || state->state == AppState::RecorderState::Recording
        || state->state == AppState::RecorderState::Recovering
        || state->state == AppState::RecorderState::Stopping;
    const bool canEdit = state->state == AppState::RecorderState::Idle;
    EnableWindow(state->startButton, canStart ? TRUE : FALSE);
    EnableWindow(state->stopButton, canStop ? TRUE : FALSE);
    EnableWindow(state->outputEdit, canEdit ? TRUE : FALSE);
    EnableWindow(state->mp3Checkbox, canEdit ? TRUE : FALSE);
    EnableWindow(state->bitrateEdit, canEdit ? TRUE : FALSE);
    EnableWindow(state->pauseButton, (state->state == AppState::RecorderState::Recording ||
                                      state->state == AppState::RecorderState::Recovering) ? TRUE : FALSE);
}

void PostStateUpdate(HWND hwnd, AppState::RecorderState newState) {
    PostMessageW(hwnd, WM_APP_STATE_UPDATE, static_cast<WPARAM>(newState), 0);
}

std::filesystem::path BrowseForFolder(HWND owner) {
    BROWSEINFOW bi{};
    bi.hwndOwner = owner;
    bi.lpszTitle = L"Select output folder";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) {
        return {};
    }
    wchar_t pathBuffer[MAX_PATH] = {};
    if (!SHGetPathFromIDListW(pidl, pathBuffer)) {
        CoTaskMemFree(pidl);
        return {};
    }
    CoTaskMemFree(pidl);
    return std::filesystem::path(pathBuffer);
}

void BrowseForOutputPath(AppState* state) {
    wchar_t buffer[MAX_PATH] = {};
    GetWindowTextW(state->outputEdit, buffer, std::size(buffer));
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = state->hwnd;
    ofn.lpstrFilter = L"MP3 Files\0*.mp3\0WAV Files\0*.wav\0All Files\0*.*\0";
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = static_cast<DWORD>(std::size(buffer));
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    const bool mp3Preferred = Button_GetCheck(state->mp3Checkbox) == BST_CHECKED;
    ofn.lpstrDefExt = mp3Preferred ? L"mp3" : L"wav";
    if (GetSaveFileNameW(&ofn)) {
        SetWindowTextW(state->outputEdit, buffer);
    }
}

void BrowseForOutputFolder(AppState* state) {
    auto folder = BrowseForFolder(state->hwnd);
    if (folder.empty()) {
        return;
    }
    std::filesystem::path current = GetWindowTextString(state->outputEdit);
    std::wstring filename = current.filename().wstring();
    if (filename.empty() || filename == L"." || filename == L"..") {
        filename = DefaultOutputPath().wstring();
    }
    std::filesystem::path combined = folder / filename;
    SetWindowTextW(state->outputEdit, combined.wstring().c_str());
}

void OpenOutputFolder(AppState* state) {
    std::filesystem::path current = GetWindowTextString(state->outputEdit);
    if (current.empty()) {
        current = DefaultOutputPath();
    }
    std::filesystem::path target = current;
    std::error_code ec;
    if (!std::filesystem::exists(target, ec) || !std::filesystem::is_directory(target, ec)) {
        target = current.parent_path();
    }
    if (target.empty()) {
        return;
    }
    ShellExecuteW(nullptr, L"open", target.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
void PostLogMessage(HWND hwnd, const std::wstring& line, LogLevel level) {
    auto payload = new std::wstring(line);
    PostMessageW(hwnd, WM_APP_LOG_MESSAGE, static_cast<WPARAM>(level),
                 reinterpret_cast<LPARAM>(payload));
}

void PostOutputPathUpdate(HWND hwnd, const std::filesystem::path& path) {
    auto payload = new std::wstring(path.wstring());
    PostMessageW(hwnd, WM_APP_OUTPUT_PATH, 0, reinterpret_cast<LPARAM>(payload));
}

void RunRecorder(AppState* state,
                 std::filesystem::path outputPath,
                 bool mp3Enabled,
                 uint32_t bitrateKbps) {
    Logger threadLogger;
    threadLogger.SetSink([hwnd = state->hwnd](LogLevel level, const std::wstring& line) {
        PostLogMessage(hwnd, line, level);
    });
    try {
        threadLogger.Info(L"Loopback Recorder starting.");
        ComGuard com;
        constexpr int kMaxReconnectAttempts = 3;
        constexpr int kReconnectDelayMs = 1500;
        const std::filesystem::path baseOutputPath = outputPath;
        int attempts = 0;
        bool finished = false;

        while (!finished) {
            PostStateUpdate(state->hwnd, AppState::RecorderState::Recording);
            DeviceEnumerator enumerator;
            auto device = enumerator.GetDefaultRenderDevice();
            if (!device) {
                throw std::runtime_error("Unable to acquire playback device");
            }
            std::wstring friendly = DeviceEnumerator::GetFriendlyName(device.Get());
            threadLogger.Info(L"Selected playback device: " + friendly);

            RecorderConfig config;
            config.outputPath = mp3Enabled
                ? EnsureExtension(baseOutputPath, L".mp3")
                : EnsureExtension(baseOutputPath, L".wav");
            config.outputPath = EnsureUniquePath(config.outputPath);
            if (mp3Enabled) {
                config.mp3BitrateKbps = bitrateKbps;
            }
            auto ensureDir = [](const std::filesystem::path& path) {
                if (path.has_parent_path() && !path.parent_path().empty()) {
                    std::filesystem::create_directories(path.parent_path());
                }
            };
            ensureDir(config.outputPath);
            PostOutputPathUpdate(state->hwnd, config.outputPath);

            LoopbackRecorder recorder(device, threadLogger);
            RecorderControls controls;
            controls.shouldStop = [state]() {
                return state->stopRequested.load();
            };
            controls.isPaused = [state]() {
                return state->pauseRequested.load();
            };

            threadLogger.Info(L"Recording system audio to " + config.outputPath.wstring());
            RecorderStats stats = recorder.Record(config, controls);
            threadLogger.Info(L"Recording finished. Segments: " + std::to_wstring(stats.segmentsWritten));

            if (stats.deviceInvalidated && !state->stopRequested.load()) {
                if (attempts >= kMaxReconnectAttempts) {
                    threadLogger.Warn(L"Playback device disconnected too many times; stopping.");
                    break;
                }
                ++attempts;
                PostStateUpdate(state->hwnd, AppState::RecorderState::Recovering);
                threadLogger.Warn(L"Playback device disconnected; retrying in " +
                                  std::to_wstring(kReconnectDelayMs) + L" ms (attempt " +
                                  std::to_wstring(attempts) + L"/" + std::to_wstring(kMaxReconnectAttempts) + L").");
                std::this_thread::sleep_for(std::chrono::milliseconds(kReconnectDelayMs));
                continue;
            }

            finished = true;
            threadLogger.Info(L"Recorder session ended.");
        }
    } catch (const std::exception& ex) {
        threadLogger.Error(L"Fatal error: " + ToWide(ex.what()));
    }
    PostMessageW(state->hwnd, WM_APP_RECORDER_DONE, 0, 0);
}

int GetBitrateFromEdit(HWND edit, int fallback) {
    std::wstring text = GetWindowTextString(edit);
    if (text.empty()) {
        return fallback;
    }
    try {
        int value = std::stoi(text);
        if (value < 32) value = 32;
        if (value > 320) value = 320;
        return value;
    } catch (...) {
        return fallback;
    }
}

void StartRecording(AppState* state) {
    if (state->state != AppState::RecorderState::Idle) {
        return;
    }
    std::wstring pathText = GetWindowTextString(state->outputEdit);
    if (pathText.empty()) {
        pathText = DefaultOutputPath().wstring();
        SetWindowTextW(state->outputEdit, pathText.c_str());
    }
    std::filesystem::path outputPath = pathText;
    const bool mp3Enabled = Button_GetCheck(state->mp3Checkbox) == BST_CHECKED;
    const int bitrate = GetBitrateFromEdit(state->bitrateEdit, state->defaultBitrate);
    outputPath = mp3Enabled
        ? EnsureExtension(outputPath, L".mp3")
        : EnsureExtension(outputPath, L".wav");
    outputPath = EnsureUniquePath(outputPath);
    SetWindowTextW(state->outputEdit, outputPath.wstring().c_str());

    state->stopRequested.store(false);
    state->pauseRequested.store(false);
    state->paused = false;
    state->pausedTotal = std::chrono::milliseconds(0);
    state->startTime = std::chrono::steady_clock::now();
    state->state = AppState::RecorderState::Starting;
    UpdateControlsForState(state);
    AppendLog(state->logEdit, L"[UI] Recording started.");

    state->worker = std::thread([state, outputPath, mp3Enabled, bitrate]() {
        RunRecorder(state, outputPath, mp3Enabled, static_cast<uint32_t>(bitrate));
    });
}

void StopRecording(AppState* state) {
    if (state->state == AppState::RecorderState::Idle) {
        return;
    }
    state->stopRequested.store(true);
    if (state->state != AppState::RecorderState::Stopping) {
        state->state = AppState::RecorderState::Stopping;
        UpdateControlsForState(state);
    }
    AppendLog(state->logEdit, L"[UI] Stop requested.");
}

void TogglePause(AppState* state) {
    if (state->state != AppState::RecorderState::Recording &&
        state->state != AppState::RecorderState::Recovering) {
        return;
    }
    const bool newPaused = !state->pauseRequested.load();
    state->pauseRequested.store(newPaused);
    state->paused = newPaused;
    if (newPaused) {
        state->pauseStart = std::chrono::steady_clock::now();
        SetWindowTextW(state->pauseButton, L"Resume");
        AppendLog(state->logEdit, L"[UI] Paused.");
    } else {
        state->pausedTotal += std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - state->pauseStart);
        SetWindowTextW(state->pauseButton, L"Pause");
        AppendLog(state->logEdit, L"[UI] Resumed.");
    }
    UpdateStatusText(state);
}

void CleanupWorker(AppState* state) {
    if (state->worker.joinable()) {
        state->worker.join();
    }
    state->stopRequested.store(false);
    state->pauseRequested.store(false);
    state->paused = false;
    state->pausedTotal = std::chrono::milliseconds(0);
    SetWindowTextW(state->pauseButton, L"Pause");
    state->state = AppState::RecorderState::Idle;
    UpdateControlsForState(state);
    AppendLog(state->logEdit, L"[UI] Recording stopped.");
    UpdateStatusText(state);
}

void CreateChildControls(HWND hwnd, AppState* state) {
    const int padding = 12;
    const int labelHeight = 20;
    const int editHeight = 24;
    const int buttonWidth = 80;
    HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

    CreateWindowW(L"STATIC", L"Output file:", WS_VISIBLE | WS_CHILD,
                  padding, padding, 120, labelHeight, hwnd, nullptr, nullptr, nullptr);
    state->outputEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", DefaultOutputPath().wstring().c_str(),
                                        WS_VISIBLE | WS_CHILD | ES_AUTOHSCROLL,
                                        padding, padding + labelHeight + 2, 300, editHeight,
                                        hwnd, reinterpret_cast<HMENU>(IDC_OUTPUT_EDIT), nullptr, nullptr);
    SetControlFont(state->outputEdit, font);

    HWND browseButton = CreateWindowW(L"BUTTON", L"File...",
                                      WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                                      padding + 300 + 8, padding + labelHeight + 2, buttonWidth, editHeight,
                                      hwnd, reinterpret_cast<HMENU>(IDC_BROWSE_BUTTON), nullptr, nullptr);
    SetControlFont(browseButton, font);

    HWND browseFolderButton = CreateWindowW(L"BUTTON", L"Folder...",
                                      WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                                      padding + 300 + 8 + buttonWidth + 6, padding + labelHeight + 2, buttonWidth, editHeight,
                                      hwnd, reinterpret_cast<HMENU>(IDC_BROWSE_FOLDER), nullptr, nullptr);
    SetControlFont(browseFolderButton, font);

    HWND openFolderButton = CreateWindowW(L"BUTTON", L"Open",
                                      WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                                      padding + 300 + 8 + (buttonWidth + 6) * 2, padding + labelHeight + 2, buttonWidth, editHeight,
                                      hwnd, reinterpret_cast<HMENU>(IDC_OPEN_FOLDER), nullptr, nullptr);
    SetControlFont(openFolderButton, font);

    state->mp3Checkbox = CreateWindowW(L"BUTTON", L"Save as MP3", WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX,
                                       padding, padding + labelHeight + editHeight + 12, 120, labelHeight,
                                       hwnd, reinterpret_cast<HMENU>(IDC_MP3_CHECK), nullptr, nullptr);
    SetControlFont(state->mp3Checkbox, font);
    Button_SetCheck(state->mp3Checkbox, BST_CHECKED);

    CreateWindowW(L"STATIC", L"MP3 Bitrate (kbps):", WS_VISIBLE | WS_CHILD,
                  padding + 150, padding + labelHeight + editHeight + 12, 150, labelHeight,
                  hwnd, nullptr, nullptr, nullptr);
    state->bitrateEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"192",
                                         WS_VISIBLE | WS_CHILD | ES_NUMBER,
                                         padding + 300, padding + labelHeight + editHeight + 8,
                                         80, editHeight,
                                         hwnd, reinterpret_cast<HMENU>(IDC_BITRATE_EDIT), nullptr, nullptr);
    SetControlFont(state->bitrateEdit, font);

    state->startButton = CreateWindowW(L"BUTTON", L"Start Recording",
                                       WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                                       padding, padding + labelHeight + editHeight + 50,
                                       buttonWidth + 20, editHeight,
                                       hwnd, reinterpret_cast<HMENU>(IDC_START_BUTTON), nullptr, nullptr);
    SetControlFont(state->startButton, font);

    state->stopButton = CreateWindowW(L"BUTTON", L"Stop",
                                      WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                                      padding + buttonWidth + 40, padding + labelHeight + editHeight + 50,
                                      buttonWidth, editHeight,
                                      hwnd, reinterpret_cast<HMENU>(IDC_STOP_BUTTON), nullptr, nullptr);
    SetControlFont(state->stopButton, font);
    EnableWindow(state->stopButton, FALSE);

    state->pauseButton = CreateWindowW(L"BUTTON", L"Pause",
                                      WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                                      padding + buttonWidth + 40 + buttonWidth + 20, padding + labelHeight + editHeight + 50,
                                      buttonWidth, editHeight,
                                      hwnd, reinterpret_cast<HMENU>(IDC_PAUSE_BUTTON), nullptr, nullptr);
    SetControlFont(state->pauseButton, font);
    EnableWindow(state->pauseButton, FALSE);

    state->statusLabel = CreateWindowW(L"STATIC", L"Status: Idle",
                                       WS_VISIBLE | WS_CHILD,
                                       padding, padding + labelHeight + editHeight + 86,
                                       520, labelHeight,
                                       hwnd, nullptr, nullptr, nullptr);
    SetControlFont(state->statusLabel, font);

    state->logEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                     WS_VISIBLE | WS_CHILD | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
                                     padding, padding + labelHeight + editHeight + 110,
                                     520, 220,
                                     hwnd, reinterpret_cast<HMENU>(IDC_LOG_EDIT), nullptr, nullptr);
    SetControlFont(state->logEdit, font);
}

LRESULT CALLBACK MainWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    AppState* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto newState = std::make_unique<AppState>();
        newState->hwnd = hwnd;
        CreateChildControls(hwnd, newState.get());
        UpdateStatusText(newState.get());
        SetTimer(hwnd, 1, 1000, nullptr);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(newState.get()));
        newState.release();
        return 0;
    }
    case WM_COMMAND:
        if (!state) {
            break;
        }
        switch (LOWORD(wParam)) {
        case IDC_BROWSE_BUTTON:
            BrowseForOutputPath(state);
            return 0;
        case IDC_BROWSE_FOLDER:
            BrowseForOutputFolder(state);
            return 0;
        case IDC_OPEN_FOLDER:
            OpenOutputFolder(state);
            return 0;
        case IDC_START_BUTTON:
            StartRecording(state);
            return 0;
        case IDC_STOP_BUTTON:
            StopRecording(state);
            return 0;
        case IDC_PAUSE_BUTTON:
            TogglePause(state);
            return 0;
        default:
            break;
        }
        break;
    case WM_APP_LOG_MESSAGE:
        if (state) {
            auto payload = reinterpret_cast<std::wstring*>(lParam);
            if (payload) {
                AppendLog(state->logEdit, *payload);
                delete payload;
            }
        }
        return 0;
    case WM_APP_STATE_UPDATE:
        if (state) {
            state->state = static_cast<AppState::RecorderState>(wParam);
            UpdateControlsForState(state);
            UpdateStatusText(state);
        }
        return 0;
    case WM_APP_OUTPUT_PATH:
        if (state) {
            auto payload = reinterpret_cast<std::wstring*>(lParam);
            if (payload) {
                SetWindowTextW(state->outputEdit, payload->c_str());
                state->currentOutputPath = *payload;
                delete payload;
            }
        }
        UpdateStatusText(state);
        return 0;
    case WM_TIMER:
        if (state) {
            UpdateStatusText(state);
        }
        return 0;
    case WM_APP_RECORDER_DONE:
        if (state) {
            CleanupWorker(state);
        }
        return 0;
    case WM_DESTROY:
        if (state) {
            state->stopRequested.store(true);
            if (state->worker.joinable()) {
                state->worker.join();
            }
            delete state;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        KillTimer(hwnd, 1);
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
    const wchar_t kClassName[] = L"LoopbackRecorderGui";
    WNDCLASSW wc{};
    wc.lpfnWndProc = MainWindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPWSTR>(IDC_ARROW));
    wc.hIcon = LoadIconW(nullptr, reinterpret_cast<LPWSTR>(IDI_APPLICATION));
    wc.lpszClassName = kClassName;
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, kClassName, L"Loopback Recorder GUI",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, 580, 520,
                                nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) {
        return 0;
    }
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}
