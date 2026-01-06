#include "LoopbackRecorder.h"
#include "DeviceEnumerator.h"
#include "RecordingUtils.h"

#include <windows.h>
#include <commdlg.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cwchar>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

namespace {

constexpr UINT WM_APP_LOG_MESSAGE = WM_APP + 1;
constexpr UINT WM_APP_RECORDER_DONE = WM_APP + 2;

enum ControlId : int {
    IDC_OUTPUT_EDIT = 1001,
    IDC_BROWSE_BUTTON,
    IDC_MP3_CHECK,
    IDC_BITRATE_EDIT,
    IDC_START_BUTTON,
    IDC_STOP_BUTTON,
    IDC_LOG_EDIT
};

struct AppState {
    HWND hwnd = nullptr;
    HWND outputEdit = nullptr;
    HWND mp3Checkbox = nullptr;
    HWND bitrateEdit = nullptr;
    HWND startButton = nullptr;
    HWND stopButton = nullptr;
    HWND logEdit = nullptr;
    std::thread worker;
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> running{false};
    int defaultBitrate = 192;
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

void SetControlFont(HWND control, HFONT font) {
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

void EnableControlsForRun(AppState* state, bool running) {
    EnableWindow(state->startButton, running ? FALSE : TRUE);
    EnableWindow(state->stopButton, running ? TRUE : FALSE);
    EnableWindow(state->outputEdit, running ? FALSE : TRUE);
    EnableWindow(state->mp3Checkbox, running ? FALSE : TRUE);
    EnableWindow(state->bitrateEdit, running ? FALSE : TRUE);
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

void PostLogMessage(HWND hwnd, const std::wstring& line, LogLevel level) {
    auto payload = new std::wstring(line);
    PostMessageW(hwnd, WM_APP_LOG_MESSAGE, static_cast<WPARAM>(level),
                 reinterpret_cast<LPARAM>(payload));
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
        DeviceEnumerator enumerator;
        auto device = enumerator.GetDefaultRenderDevice();
        if (!device) {
            throw std::runtime_error("Unable to acquire playback device");
        }
        std::wstring friendly = DeviceEnumerator::GetFriendlyName(device.Get());
        threadLogger.Info(L"Selected playback device: " + friendly);

        RecorderConfig config;
        config.outputPath = mp3Enabled
            ? EnsureExtension(outputPath, L".mp3")
            : EnsureExtension(outputPath, L".wav");
        if (mp3Enabled) {
            config.mp3BitrateKbps = bitrateKbps;
        }
        auto ensureDir = [](const std::filesystem::path& path) {
            if (path.has_parent_path() && !path.parent_path().empty()) {
                std::filesystem::create_directories(path.parent_path());
            }
        };
        ensureDir(config.outputPath);

        LoopbackRecorder recorder(device, threadLogger);
        RecorderControls controls;
        controls.shouldStop = [state]() {
            return state->stopRequested.load();
        };

        threadLogger.Info(L"Recording system audio to " + config.outputPath.wstring());
        RecorderStats stats = recorder.Record(config, controls);
        threadLogger.Info(L"Recording finished. Segments: " + std::to_wstring(stats.segmentsWritten));
        threadLogger.Info(L"Recorder session ended.");
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
    if (state->running.load()) {
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
    SetWindowTextW(state->outputEdit, outputPath.wstring().c_str());

    state->stopRequested.store(false);
    state->running.store(true);
    EnableControlsForRun(state, true);
    AppendLog(state->logEdit, L"[UI] Recording started.");

    state->worker = std::thread([state, outputPath, mp3Enabled, bitrate]() {
        RunRecorder(state, outputPath, mp3Enabled, static_cast<uint32_t>(bitrate));
    });
}

void StopRecording(AppState* state) {
    if (!state->running.load()) {
        return;
    }
    state->stopRequested.store(true);
    AppendLog(state->logEdit, L"[UI] Stop requested.");
}

void CleanupWorker(AppState* state) {
    if (state->worker.joinable()) {
        state->worker.join();
    }
    state->running.store(false);
    state->stopRequested.store(false);
    EnableControlsForRun(state, false);
    AppendLog(state->logEdit, L"[UI] Recording stopped.");
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
                                        padding, padding + labelHeight + 2, 360, editHeight,
                                        hwnd, reinterpret_cast<HMENU>(IDC_OUTPUT_EDIT), nullptr, nullptr);
    SetControlFont(state->outputEdit, font);

    HWND browseButton = CreateWindowW(L"BUTTON", L"Browse...",
                                      WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                                      padding + 360 + 8, padding + labelHeight + 2, buttonWidth, editHeight,
                                      hwnd, reinterpret_cast<HMENU>(IDC_BROWSE_BUTTON), nullptr, nullptr);
    SetControlFont(browseButton, font);

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

    state->logEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                     WS_VISIBLE | WS_CHILD | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
                                     padding, padding + labelHeight + editHeight + 90,
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
        case IDC_START_BUTTON:
            StartRecording(state);
            return 0;
        case IDC_STOP_BUTTON:
            StopRecording(state);
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
