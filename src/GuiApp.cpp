#include "LoopbackRecorder.h"
#include "DeviceEnumerator.h"
#include "RecordingUtils.h"
#include "SegmentNaming.h"

#include <windows.h>
#include <commdlg.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commctrl.h>

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
    IDC_FORMAT_COMBO,
    IDC_BITRATE_EDIT,
    IDC_START_BUTTON,
    IDC_STOP_BUTTON,
    IDC_PAUSE_BUTTON,
    IDC_LOG_EDIT
};

struct AppState {
    HWND hwnd = nullptr;
    HWND headerLabel = nullptr;
    HWND outputEdit = nullptr;
    HWND formatCombo = nullptr;
    HWND bitrateEdit = nullptr;
    HWND startButton = nullptr;
    HWND stopButton = nullptr;
    HWND pauseButton = nullptr;
    HWND logEdit = nullptr;
    HWND statusBar = nullptr;
    HFONT uiFont = nullptr;
    HFONT uiFontBold = nullptr;
    HFONT uiFontTitle = nullptr;
    HBRUSH backgroundBrush = nullptr;
    HBRUSH headerBrush = nullptr;
    COLORREF backgroundColor = RGB(245, 247, 250);
    COLORREF headerColor = RGB(232, 240, 254);
    HICON fileIcon = nullptr;
    HICON folderIcon = nullptr;
    HICON openIcon = nullptr;
    HIMAGELIST fileImageList = nullptr;
    HIMAGELIST folderImageList = nullptr;
    HIMAGELIST openImageList = nullptr;
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
            throw std::runtime_error("COM 初始化失败");
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
    if (!state || !state->statusBar) {
        return;
    }
    std::wstring text = L"状态：";
    switch (state->state) {
    case AppState::RecorderState::Idle:
        text += L"空闲";
        break;
    case AppState::RecorderState::Starting:
        text += L"启动中...";
        break;
    case AppState::RecorderState::Recovering:
        text += L"重连中...";
        break;
    case AppState::RecorderState::Stopping:
        text += L"停止中...";
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
        text += L"录音中 ";
        text += timeBuf;
        if (state->paused) {
            text += L"（已暂停）";
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
        text += L"未知";
        break;
    }
    SendMessageW(state->statusBar, SB_SETTEXT, 0, reinterpret_cast<LPARAM>(text.c_str()));
}

void SetControlFont(HWND control, HFONT font) {
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

void AttachButtonIcon(HWND button, HICON icon, HIMAGELIST& imageList) {
    if (!button || !icon) {
        return;
    }
    imageList = ImageList_Create(16, 16, ILC_COLOR32 | ILC_MASK, 1, 0);
    if (!imageList) {
        return;
    }
    ImageList_AddIcon(imageList, icon);
    BUTTON_IMAGELIST info{};
    info.himl = imageList;
    info.uAlign = BUTTON_IMAGELIST_ALIGN_LEFT;
    info.margin.left = 6;
    SendMessageW(button, BCM_SETIMAGELIST, 0, reinterpret_cast<LPARAM>(&info));
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
    EnableWindow(state->formatCombo, canEdit ? TRUE : FALSE);
    const bool mp3Selected = state->formatCombo &&
        SendMessageW(state->formatCombo, CB_GETCURSEL, 0, 0) == 1;
    EnableWindow(state->bitrateEdit, (canEdit && mp3Selected) ? TRUE : FALSE);
    EnableWindow(state->pauseButton, (state->state == AppState::RecorderState::Recording ||
                                      state->state == AppState::RecorderState::Recovering) ? TRUE : FALSE);
}

void PostStateUpdate(HWND hwnd, AppState::RecorderState newState) {
    PostMessageW(hwnd, WM_APP_STATE_UPDATE, static_cast<WPARAM>(newState), 0);
}

std::filesystem::path BrowseForFolder(HWND owner) {
    BROWSEINFOW bi{};
    bi.hwndOwner = owner;
    bi.lpszTitle = L"选择输出文件夹";
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
    ofn.lpstrFilter = L"MP3 文件\0*.mp3\0WAV 文件\0*.wav\0所有文件\0*.*\0";
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = static_cast<DWORD>(std::size(buffer));
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    const bool mp3Preferred = state->formatCombo &&
        SendMessageW(state->formatCombo, CB_GETCURSEL, 0, 0) == 1;
    ofn.lpstrDefExt = mp3Preferred ? L"mp3" : L"wav";
    if (GetSaveFileNameW(&ofn)) {
        std::filesystem::path outputPath = buffer;
        outputPath = mp3Preferred
            ? EnsureExtension(outputPath, L".mp3")
            : EnsureExtension(outputPath, L".wav");
        SetWindowTextW(state->outputEdit, outputPath.wstring().c_str());
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

void UpdateOutputExtension(AppState* state) {
    if (!state || state->state != AppState::RecorderState::Idle) {
        return;
    }
    std::filesystem::path outputPath = GetWindowTextString(state->outputEdit);
    if (outputPath.empty()) {
        outputPath = DefaultOutputPath();
    }
    const bool mp3Selected = state->formatCombo &&
        SendMessageW(state->formatCombo, CB_GETCURSEL, 0, 0) == 1;
    outputPath = mp3Selected
        ? EnsureExtension(outputPath, L".mp3")
        : EnsureExtension(outputPath, L".wav");
    SetWindowTextW(state->outputEdit, outputPath.wstring().c_str());
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
        threadLogger.Info(L"录音器启动中。");
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
                throw std::runtime_error("无法获取播放设备");
            }
            std::wstring friendly = DeviceEnumerator::GetFriendlyName(device.Get());
            threadLogger.Info(L"已选择播放设备：" + friendly);

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

            threadLogger.Info(L"开始录制系统音频到 " + config.outputPath.wstring());
            RecorderStats stats = recorder.Record(config, controls);
            threadLogger.Info(L"录音结束。分段数：" + std::to_wstring(stats.segmentsWritten));

            if (stats.deviceInvalidated && !state->stopRequested.load()) {
                if (attempts >= kMaxReconnectAttempts) {
                    threadLogger.Warn(L"播放设备断开次数过多，已停止。");
                    break;
                }
                ++attempts;
                PostStateUpdate(state->hwnd, AppState::RecorderState::Recovering);
                threadLogger.Warn(L"播放设备断开，将在 " + std::to_wstring(kReconnectDelayMs) +
                                  L" ms 后重试（第 " + std::to_wstring(attempts) + L"/" +
                                  std::to_wstring(kMaxReconnectAttempts) + L" 次）。");
                std::this_thread::sleep_for(std::chrono::milliseconds(kReconnectDelayMs));
                continue;
            }

            finished = true;
            threadLogger.Info(L"录音会话已结束。");
        }
    } catch (const std::exception& ex) {
        threadLogger.Error(L"致命错误：" + ToWide(ex.what()));
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
    const bool mp3Enabled = state->formatCombo &&
        SendMessageW(state->formatCombo, CB_GETCURSEL, 0, 0) == 1;
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
    AppendLog(state->logEdit, L"[界面] 开始录音。");

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
    AppendLog(state->logEdit, L"[界面] 请求停止。");
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
        SetWindowTextW(state->pauseButton, L"继续");
        AppendLog(state->logEdit, L"[界面] 已暂停。");
    } else {
        state->pausedTotal += std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - state->pauseStart);
        SetWindowTextW(state->pauseButton, L"暂停");
        AppendLog(state->logEdit, L"[界面] 已继续。");
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
    SetWindowTextW(state->pauseButton, L"暂停");
    state->state = AppState::RecorderState::Idle;
    UpdateControlsForState(state);
    AppendLog(state->logEdit, L"[界面] 录音已停止。");
    UpdateStatusText(state);
}

void CreateChildControls(HWND hwnd, AppState* state) {
    const int padding = 14;
    const int headerHeight = 34;
    const int labelHeight = 20;
    const int editHeight = 26;
    const int buttonHeight = 28;
    const int buttonWidth = 90;
    const int wideButtonWidth = 110;
    const int windowWidth = 620;
    const int contentWidth = windowWidth - padding * 2 - 12;

    state->uiFont = CreateFontW(18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    state->uiFontBold = CreateFontW(18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                    DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    state->uiFontTitle = CreateFontW(22, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                     DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    state->backgroundBrush = CreateSolidBrush(state->backgroundColor);
    state->headerBrush = CreateSolidBrush(state->headerColor);
    HFONT font = state->uiFont;

    const int groupLeft = padding;
    int y = padding + headerHeight + 6;

    state->headerLabel = CreateWindowW(L"STATIC", L"系统录音工具",
                                       WS_VISIBLE | WS_CHILD,
                                       groupLeft, padding, contentWidth, headerHeight,
                                       hwnd, nullptr, nullptr, nullptr);
    SetControlFont(state->headerLabel, state->uiFontTitle);

    HWND outputGroup = CreateWindowW(L"BUTTON", L"输出", WS_VISIBLE | WS_CHILD | BS_GROUPBOX,
                                     groupLeft, y, contentWidth, 86, hwnd, nullptr, nullptr, nullptr);
    SetControlFont(outputGroup, font);

    const int outputLabelY = y + 22;
    HWND outputLabel = CreateWindowW(L"STATIC", L"文件：", WS_VISIBLE | WS_CHILD,
                                     groupLeft + 12, outputLabelY, 60, labelHeight, hwnd, nullptr, nullptr, nullptr);
    SetControlFont(outputLabel, font);
    state->outputEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", DefaultOutputPath().wstring().c_str(),
                                        WS_VISIBLE | WS_CHILD | ES_AUTOHSCROLL,
                                        groupLeft + 64, outputLabelY - 2, 290, editHeight,
                                        hwnd, reinterpret_cast<HMENU>(IDC_OUTPUT_EDIT), nullptr, nullptr);
    SetControlFont(state->outputEdit, font);

    const int buttonRowX = groupLeft + 364;
    HWND browseButton = CreateWindowW(L"BUTTON", L"选择文件",
                                      WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                                      buttonRowX, outputLabelY - 2, buttonWidth, buttonHeight,
                                      hwnd, reinterpret_cast<HMENU>(IDC_BROWSE_BUTTON), nullptr, nullptr);
    SetControlFont(browseButton, font);

    HWND browseFolderButton = CreateWindowW(L"BUTTON", L"选择文件夹",
                                            WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                                            buttonRowX + buttonWidth + 8, outputLabelY - 2, wideButtonWidth, buttonHeight,
                                            hwnd, reinterpret_cast<HMENU>(IDC_BROWSE_FOLDER), nullptr, nullptr);
    SetControlFont(browseFolderButton, font);

    HWND openFolderButton = CreateWindowW(L"BUTTON", L"打开目录",
                                          WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                                          buttonRowX, outputLabelY + 32, buttonWidth, buttonHeight,
                                          hwnd, reinterpret_cast<HMENU>(IDC_OPEN_FOLDER), nullptr, nullptr);
    SetControlFont(openFolderButton, font);

    y += 94;
    HWND formatGroup = CreateWindowW(L"BUTTON", L"格式", WS_VISIBLE | WS_CHILD | BS_GROUPBOX,
                                     groupLeft, y, contentWidth, 70, hwnd, nullptr, nullptr, nullptr);
    SetControlFont(formatGroup, font);

    const int formatLabelY = y + 26;
    HWND formatLabel = CreateWindowW(L"STATIC", L"输出格式：", WS_VISIBLE | WS_CHILD,
                                     groupLeft + 12, formatLabelY, 80, labelHeight, hwnd, nullptr, nullptr, nullptr);
    SetControlFont(formatLabel, font);
    state->formatCombo = CreateWindowW(L"COMBOBOX", L"", WS_VISIBLE | WS_CHILD | CBS_DROPDOWNLIST,
                                       groupLeft + 94, formatLabelY - 2, 120, 200,
                                       hwnd, reinterpret_cast<HMENU>(IDC_FORMAT_COMBO), nullptr, nullptr);
    SetControlFont(state->formatCombo, font);
    SendMessageW(state->formatCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"WAV"));
    SendMessageW(state->formatCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"MP3"));
    SendMessageW(state->formatCombo, CB_SETCURSEL, 1, 0);

    HWND bitrateLabel = CreateWindowW(L"STATIC", L"MP3 比特率 (kbps)：", WS_VISIBLE | WS_CHILD,
                                      groupLeft + 230, formatLabelY, 150, labelHeight, hwnd, nullptr, nullptr, nullptr);
    SetControlFont(bitrateLabel, font);
    state->bitrateEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"192",
                                         WS_VISIBLE | WS_CHILD | ES_NUMBER,
                                         groupLeft + 386, formatLabelY - 2,
                                         90, editHeight,
                                         hwnd, reinterpret_cast<HMENU>(IDC_BITRATE_EDIT), nullptr, nullptr);
    SetControlFont(state->bitrateEdit, font);

    y += 78;
    HWND controlGroup = CreateWindowW(L"BUTTON", L"控制", WS_VISIBLE | WS_CHILD | BS_GROUPBOX,
                                      groupLeft, y, contentWidth, 72, hwnd, nullptr, nullptr, nullptr);
    SetControlFont(controlGroup, font);

    const int controlY = y + 26;
    state->startButton = CreateWindowW(L"BUTTON", L"开始录音",
                                       WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON,
                                       groupLeft + 12, controlY - 2,
                                       buttonWidth + 16, buttonHeight,
                                       hwnd, reinterpret_cast<HMENU>(IDC_START_BUTTON), nullptr, nullptr);
    SetControlFont(state->startButton, state->uiFontBold);

    state->stopButton = CreateWindowW(L"BUTTON", L"停止",
                                      WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                                      groupLeft + 140, controlY - 2,
                                      buttonWidth, buttonHeight,
                                      hwnd, reinterpret_cast<HMENU>(IDC_STOP_BUTTON), nullptr, nullptr);
    SetControlFont(state->stopButton, font);
    EnableWindow(state->stopButton, FALSE);

    state->pauseButton = CreateWindowW(L"BUTTON", L"暂停",
                                       WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                                       groupLeft + 246, controlY - 2,
                                       buttonWidth, buttonHeight,
                                       hwnd, reinterpret_cast<HMENU>(IDC_PAUSE_BUTTON), nullptr, nullptr);
    SetControlFont(state->pauseButton, font);
    EnableWindow(state->pauseButton, FALSE);

    y += 82;
    HWND logGroup = CreateWindowW(L"BUTTON", L"日志", WS_VISIBLE | WS_CHILD | BS_GROUPBOX,
                                  groupLeft, y, contentWidth, 248, hwnd, nullptr, nullptr, nullptr);
    SetControlFont(logGroup, font);

    state->logEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                     WS_VISIBLE | WS_CHILD | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
                                     groupLeft + 12, y + 26,
                                     contentWidth - 24, 200,
                                     hwnd, reinterpret_cast<HMENU>(IDC_LOG_EDIT), nullptr, nullptr);
    SetControlFont(state->logEdit, font);

    SHFILEINFOW sfi{};
    if (SHGetFileInfoW(L".txt", FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
                       SHGFI_ICON | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES)) {
        state->fileIcon = sfi.hIcon;
        AttachButtonIcon(browseButton, state->fileIcon, state->fileImageList);
    }
    if (SHGetFileInfoW(L"C:\\", FILE_ATTRIBUTE_DIRECTORY, &sfi, sizeof(sfi),
                       SHGFI_ICON | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES)) {
        state->folderIcon = sfi.hIcon;
        AttachButtonIcon(browseFolderButton, state->folderIcon, state->folderImageList);
    }
    if (SHGetFileInfoW(L"C:\\", FILE_ATTRIBUTE_DIRECTORY, &sfi, sizeof(sfi),
                       SHGFI_ICON | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES)) {
        state->openIcon = sfi.hIcon;
        AttachButtonIcon(openFolderButton, state->openIcon, state->openImageList);
    }
}

LRESULT CALLBACK MainWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    AppState* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto newState = std::make_unique<AppState>();
        newState->hwnd = hwnd;
        CreateChildControls(hwnd, newState.get());
        newState->statusBar = CreateWindowExW(0, STATUSCLASSNAMEW, L"",
                                              WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                                              0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);
        if (newState->uiFont) {
            SetControlFont(newState->statusBar, newState->uiFont);
        }
        SendMessageW(newState->statusBar, SB_SIMPLE, TRUE, 0);
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
        case IDC_FORMAT_COMBO:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                UpdateControlsForState(state);
                UpdateOutputExtension(state);
            }
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
    case WM_CTLCOLORSTATIC:
        if (state) {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            HWND target = reinterpret_cast<HWND>(lParam);
            if (target == state->headerLabel && state->headerBrush) {
                SetTextColor(hdc, RGB(40, 60, 90));
                SetBkColor(hdc, state->headerColor);
                return reinterpret_cast<INT_PTR>(state->headerBrush);
            }
            SetTextColor(hdc, RGB(35, 35, 35));
            SetBkColor(hdc, state->backgroundColor);
            SetBkMode(hdc, TRANSPARENT);
            if (state->backgroundBrush) {
                return reinterpret_cast<INT_PTR>(state->backgroundBrush);
            }
        }
        break;
    case WM_CTLCOLOREDIT:
        if (state) {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetTextColor(hdc, RGB(20, 20, 20));
            SetBkColor(hdc, RGB(255, 255, 255));
            return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_WINDOW));
        }
        break;
    case WM_CTLCOLORBTN:
        if (state && state->backgroundBrush) {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetBkColor(hdc, state->backgroundColor);
            return reinterpret_cast<INT_PTR>(state->backgroundBrush);
        }
        break;
    case WM_SIZE:
        if (state && state->statusBar) {
            SendMessageW(state->statusBar, WM_SIZE, 0, 0);
        }
        return 0;
    case WM_ERASEBKGND:
        if (state && state->backgroundBrush) {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            RECT rect;
            GetClientRect(hwnd, &rect);
            FillRect(hdc, &rect, state->backgroundBrush);
            return 1;
        }
        break;
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
            if (state->fileIcon) {
                DestroyIcon(state->fileIcon);
                state->fileIcon = nullptr;
            }
            if (state->folderIcon) {
                DestroyIcon(state->folderIcon);
                state->folderIcon = nullptr;
            }
            if (state->openIcon) {
                DestroyIcon(state->openIcon);
                state->openIcon = nullptr;
            }
            if (state->fileImageList) {
                ImageList_Destroy(state->fileImageList);
                state->fileImageList = nullptr;
            }
            if (state->folderImageList) {
                ImageList_Destroy(state->folderImageList);
                state->folderImageList = nullptr;
            }
            if (state->openImageList) {
                ImageList_Destroy(state->openImageList);
                state->openImageList = nullptr;
            }
            if (state->backgroundBrush) {
                DeleteObject(state->backgroundBrush);
                state->backgroundBrush = nullptr;
            }
            if (state->headerBrush) {
                DeleteObject(state->headerBrush);
                state->headerBrush = nullptr;
            }
            if (state->uiFont) {
                DeleteObject(state->uiFont);
                state->uiFont = nullptr;
            }
            if (state->uiFontBold) {
                DeleteObject(state->uiFontBold);
                state->uiFontBold = nullptr;
            }
            if (state->uiFontTitle) {
                DeleteObject(state->uiFontTitle);
                state->uiFontTitle = nullptr;
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
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    WNDCLASSW wc{};
    wc.lpfnWndProc = MainWindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPWSTR>(IDC_ARROW));
    wc.hIcon = LoadIconW(nullptr, reinterpret_cast<LPWSTR>(IDI_APPLICATION));
    wc.lpszClassName = kClassName;
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, kClassName, L"系统录音工具",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, 640, 620,
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
