#include "LoopbackRecorder.h"
#include "DeviceEnumerator.h"
#include "RecordingUtils.h"
#include "SegmentNaming.h"
#include "MediaFoundationPlayer.h"

#include <windows.h>
#include <commdlg.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commctrl.h>
#include <gdiplus.h>
#include <objidl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwchar>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "resource.h"

namespace {

constexpr UINT WM_APP_LOG_MESSAGE = WM_APP + 1;
constexpr UINT WM_APP_RECORDER_DONE = WM_APP + 2;
constexpr UINT WM_APP_OUTPUT_PATH = WM_APP + 3;
constexpr UINT WM_APP_STATE_UPDATE = WM_APP + 4;
constexpr UINT WM_APP_DEVICE_NAME = WM_APP + 5;
constexpr UINT WM_APP_PLAYBACK_STATE = WM_APP + 6;
constexpr UINT WM_APP_PLAYBACK_OPENED = WM_APP + 7;
constexpr UINT WM_APP_PLAYBACK_ENDED = WM_APP + 8;
constexpr UINT WM_APP_PLAYBACK_ERROR = WM_APP + 9;

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
    IDC_PLAYBACK_PLAY,
    IDC_PLAYBACK_PAUSE,
    IDC_PLAYBACK_STOP,
    IDC_PLAYBACK_SEEK,
    IDC_PLAYBACK_TIME,
    IDC_PLAYBACK_VOLUME,
    IDC_LOG_EDIT
};

enum MenuId : int {
    IDM_FILE_NEW = 2001,
    IDM_FILE_OPEN_FOLDER,
    IDM_FILE_EXIT,
    IDM_RECORD_START_STOP,
    IDM_RECORD_PAUSE,
    IDM_PLAYBACK_TOGGLE,
    IDM_PLAYBACK_PLAY,
    IDM_PLAYBACK_PAUSE,
    IDM_PLAYBACK_STOP,
    IDM_SETTINGS_FORMAT_WAV,
    IDM_SETTINGS_FORMAT_MP3,
    IDM_SETTINGS_BITRATE_128,
    IDM_SETTINGS_BITRATE_192,
    IDM_SETTINGS_BITRATE_256,
    IDM_SETTINGS_BITRATE_320,
    IDM_VIEW_CLEAR_LOG,
    IDM_HELP_ABOUT
};

class PlaybackListener;

struct RecordingStatusParts {
    std::wstring status;
    std::wstring time;
    std::wstring size;
    std::wstring format;
};

struct AboutDialogState {
    HWND parent = nullptr;
    HBITMAP qrBitmap = nullptr;
};

struct AppState {
    HWND hwnd = nullptr;
    HWND headerLabel = nullptr;
    HWND statusGroup = nullptr;
    HWND actionGroup = nullptr;
    HWND statusStateLabel = nullptr;
    HWND statusTimeLabel = nullptr;
    HWND statusMetaLabel = nullptr;
    HWND outputEdit = nullptr;
    HWND outputLabel = nullptr;
    HWND browseButton = nullptr;
    HWND browseFolderButton = nullptr;
    HWND openFolderButton = nullptr;
    HWND formatCombo = nullptr;
    HWND formatLabel = nullptr;
    HWND bitrateEdit = nullptr;
    HWND bitrateLabel = nullptr;
    HWND startButton = nullptr;
    HWND stopButton = nullptr;
    HWND pauseButton = nullptr;
    HWND playbackPlayButton = nullptr;
    HWND playbackPauseButton = nullptr;
    HWND playbackStopButton = nullptr;
    HWND playbackSeek = nullptr;
    HWND playbackTimeLabel = nullptr;
    HWND playbackVolume = nullptr;
    HWND playbackGroup = nullptr;
    HWND playbackVolumeLabel = nullptr;
    HWND logEdit = nullptr;
    HWND logGroup = nullptr;
    HWND statusBar = nullptr;
    HWND settingsGroup = nullptr;
    HMENU mainMenu = nullptr;
    HMENU settingsMenu = nullptr;
    HMENU bitrateMenu = nullptr;
    HFONT uiFont = nullptr;
    HFONT uiFontBold = nullptr;
    HFONT uiFontTitle = nullptr;
    HFONT uiFontSecondary = nullptr;
    HFONT uiFontTimer = nullptr;
    HBRUSH backgroundBrush = nullptr;
    HBRUSH headerBrush = nullptr;
    HBRUSH panelBrush = nullptr;
    HBRUSH panelAltBrush = nullptr;
    COLORREF backgroundColor = RGB(0xEF, 0xF4, 0xF8);
    COLORREF headerColor = RGB(0xFF, 0xFF, 0xFF);
    COLORREF panelColor = RGB(0xFF, 0xFF, 0xFF);
    COLORREF panelAltColor = RGB(0xF6, 0xFA, 0xFD);
    COLORREF textPrimary = RGB(0x1F, 0x2A, 0x37);
    COLORREF textSecondary = RGB(0x5B, 0x6B, 0x7A);
    COLORREF textTertiary = RGB(0x8B, 0x99, 0xA8);
    COLORREF primaryColor = RGB(0x2D, 0x9C, 0xDB);
    COLORREF accentColor = RGB(0xF2, 0x99, 0x4A);
    COLORREF recordColor = RGB(0xEB, 0x57, 0x57);
    COLORREF pauseColor = RGB(0xF2, 0xC9, 0x4C);
    COLORREF borderColor = RGB(0xD7, 0xE3, 0xEE);
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
    std::filesystem::path currentPlaybackPath;
    std::wstring currentDeviceName;
    std::chrono::steady_clock::time_point startTime{};
    std::chrono::steady_clock::time_point pauseStart{};
    std::chrono::milliseconds pausedTotal{0};
    bool paused = false;
    MediaFoundationPlayer* player = nullptr;
    PlaybackListener* playbackListener = nullptr;
    PlaybackState playbackState = PlaybackState::Idle;
    int64_t playbackDuration100ns = 0;
    bool playbackSeeking = false;
    float playbackVolumeValue = 0.8f;
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

class PlaybackListener : public IPlaybackListener {
public:
    explicit PlaybackListener(HWND hwnd) : hwnd_(hwnd) {}

    void OnPlaybackStateChanged(PlaybackState state) override {
        PostMessageW(hwnd_, WM_APP_PLAYBACK_STATE, static_cast<WPARAM>(state), 0);
    }

    void OnMediaOpened(int64_t duration100ns) override {
        PostMessageW(hwnd_, WM_APP_PLAYBACK_OPENED, 0, static_cast<LPARAM>(duration100ns));
    }

    void OnPlaybackEnded() override {
        PostMessageW(hwnd_, WM_APP_PLAYBACK_ENDED, 0, 0);
    }

    void OnPlaybackError(const std::wstring& message) override {
        auto payload = new std::wstring(message);
        PostMessageW(hwnd_, WM_APP_PLAYBACK_ERROR, 0, reinterpret_cast<LPARAM>(payload));
    }

private:
    HWND hwnd_ = nullptr;
};

std::wstring ToWide(const std::string& text) {
    return std::wstring(text.begin(), text.end());
}

int GetBitrateFromEdit(HWND edit, int fallback);
void UpdateStatusDetails(AppState* state);
void UpdateMenuForState(AppState* state);
void UpdateOutputExtension(AppState* state);
std::filesystem::path ResolvePlayablePath(AppState* state);
void PlayRecording(AppState* state);
void PausePlayback(AppState* state);
void StopPlayback(AppState* state);
void TogglePlayback(AppState* state);
void UpdatePlaybackControls(AppState* state);
void UpdatePlaybackTime(AppState* state, int64_t position100ns);
std::wstring FormatPlaybackTime(int64_t position100ns, int64_t duration100ns);
std::wstring BuildRecordingSummary(AppState* state);
RecordingStatusParts BuildRecordingStatusParts(AppState* state);
LRESULT CALLBACK AboutWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

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

COLORREF AdjustColor(COLORREF color, int delta) {
    auto clamp = [](int value) {
        if (value < 0) {
            return 0;
        }
        if (value > 255) {
            return 255;
        }
        return value;
    };
    const int r = clamp(static_cast<int>(GetRValue(color)) + delta);
    const int g = clamp(static_cast<int>(GetGValue(color)) + delta);
    const int b = clamp(static_cast<int>(GetBValue(color)) + delta);
    return RGB(r, g, b);
}

void UpdateStatusText(AppState* state) {
    if (!state) {
        return;
    }
    RecordingStatusParts parts = BuildRecordingStatusParts(state);
    if (state->statusStateLabel) {
        SetWindowTextW(state->statusStateLabel, parts.status.c_str());
    }
    if (state->statusTimeLabel) {
        SetWindowTextW(state->statusTimeLabel, parts.time.c_str());
    }
    if (state->statusMetaLabel) {
        std::wstring meta = parts.size + L" | " + parts.format;
        SetWindowTextW(state->statusMetaLabel, meta.c_str());
    }
    if (state->statusBar) {
        std::wstring summary = BuildRecordingSummary(state);
        SendMessageW(state->statusBar, SB_SETTEXT, 0, reinterpret_cast<LPARAM>(summary.c_str()));
    }
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

void PostDeviceNameUpdate(HWND hwnd, const std::wstring& name) {
    auto payload = new std::wstring(name);
    PostMessageW(hwnd, WM_APP_DEVICE_NAME, 0, reinterpret_cast<LPARAM>(payload));
}

void UpdateStatusDetails(AppState* state) {
    UpdateStatusText(state);
}

void UpdateMenuForState(AppState* state) {
    if (!state || !state->mainMenu) {
        return;
    }
    const bool playbackActive = state->playbackState == PlaybackState::Playing ||
        state->playbackState == PlaybackState::Opening;
    const bool canEdit = state->state == AppState::RecorderState::Idle && !playbackActive;
    const bool canPlayFile = canEdit && !ResolvePlayablePath(state).empty();
    const bool canPlaybackPlay = canPlayFile && !playbackActive;
    const bool canPlaybackPause = state->playbackState == PlaybackState::Playing;
    const bool canPlaybackStop = state->playbackState == PlaybackState::Playing ||
        state->playbackState == PlaybackState::Paused;
    EnableMenuItem(state->mainMenu, IDM_FILE_NEW, MF_BYCOMMAND | (canEdit ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(state->mainMenu, IDM_SETTINGS_FORMAT_WAV, MF_BYCOMMAND | (canEdit ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(state->mainMenu, IDM_SETTINGS_FORMAT_MP3, MF_BYCOMMAND | (canEdit ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(state->mainMenu, IDM_SETTINGS_BITRATE_128, MF_BYCOMMAND | (canEdit ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(state->mainMenu, IDM_SETTINGS_BITRATE_192, MF_BYCOMMAND | (canEdit ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(state->mainMenu, IDM_SETTINGS_BITRATE_256, MF_BYCOMMAND | (canEdit ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(state->mainMenu, IDM_SETTINGS_BITRATE_320, MF_BYCOMMAND | (canEdit ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(state->mainMenu, IDM_PLAYBACK_PLAY, MF_BYCOMMAND | (canPlaybackPlay ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(state->mainMenu, IDM_PLAYBACK_PAUSE, MF_BYCOMMAND | (canPlaybackPause ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(state->mainMenu, IDM_PLAYBACK_STOP, MF_BYCOMMAND | (canPlaybackStop ? MF_ENABLED : MF_GRAYED));

    const bool mp3Selected = state->formatCombo &&
        SendMessageW(state->formatCombo, CB_GETCURSEL, 0, 0) == 1;
    CheckMenuRadioItem(state->mainMenu, IDM_SETTINGS_FORMAT_WAV, IDM_SETTINGS_FORMAT_MP3,
                       mp3Selected ? IDM_SETTINGS_FORMAT_MP3 : IDM_SETTINGS_FORMAT_WAV, MF_BYCOMMAND);
    const int bitrate = GetBitrateFromEdit(state->bitrateEdit, state->defaultBitrate);
    const int bitrateId = (bitrate <= 160 ? IDM_SETTINGS_BITRATE_128 :
                           bitrate <= 224 ? IDM_SETTINGS_BITRATE_192 :
                           bitrate <= 288 ? IDM_SETTINGS_BITRATE_256 : IDM_SETTINGS_BITRATE_320);
    CheckMenuRadioItem(state->mainMenu, IDM_SETTINGS_BITRATE_128, IDM_SETTINGS_BITRATE_320,
                       mp3Selected ? bitrateId : IDM_SETTINGS_BITRATE_192, MF_BYCOMMAND);
    DrawMenuBar(state->hwnd);
}
void UpdateControlsForState(AppState* state) {
    const bool playbackActive = state->playbackState == PlaybackState::Playing ||
        state->playbackState == PlaybackState::Opening;
    const bool canToggle = !playbackActive &&
        (state->state == AppState::RecorderState::Idle ||
         state->state == AppState::RecorderState::Recording ||
         state->state == AppState::RecorderState::Recovering);
    const bool canStop = state->state == AppState::RecorderState::Starting
        || state->state == AppState::RecorderState::Recording
        || state->state == AppState::RecorderState::Recovering
        || state->state == AppState::RecorderState::Stopping;
    const bool canEdit = state->state == AppState::RecorderState::Idle && !playbackActive;
    EnableWindow(state->startButton, canToggle ? TRUE : FALSE);
    if (state->stopButton) {
        EnableWindow(state->stopButton, canStop ? TRUE : FALSE);
    }
    EnableWindow(state->outputEdit, canEdit ? TRUE : FALSE);
    EnableWindow(state->formatCombo, canEdit ? TRUE : FALSE);
    EnableWindow(state->browseButton, canEdit ? TRUE : FALSE);
    EnableWindow(state->browseFolderButton, canEdit ? TRUE : FALSE);
    EnableWindow(state->openFolderButton, canEdit ? TRUE : FALSE);
    const bool mp3Selected = state->formatCombo &&
        SendMessageW(state->formatCombo, CB_GETCURSEL, 0, 0) == 1;
    EnableWindow(state->bitrateEdit, (canEdit && mp3Selected) ? TRUE : FALSE);
    EnableWindow(state->pauseButton, (state->state == AppState::RecorderState::Recording ||
                                      state->state == AppState::RecorderState::Recovering) ? TRUE : FALSE);
    const wchar_t* startLabel = L"开始录音";
    if (state->state == AppState::RecorderState::Recording ||
        state->state == AppState::RecorderState::Recovering) {
        startLabel = L"停止录音";
    } else if (state->state == AppState::RecorderState::Starting) {
        startLabel = L"启动中...";
    } else if (state->state == AppState::RecorderState::Stopping) {
        startLabel = L"停止中...";
    }
    SetWindowTextW(state->startButton, startLabel);
    InvalidateRect(state->startButton, nullptr, TRUE);
    UpdatePlaybackControls(state);
    UpdateMenuForState(state);
}

void PostStateUpdate(HWND hwnd, AppState::RecorderState newState) {
    PostMessageW(hwnd, WM_APP_STATE_UPDATE, static_cast<WPARAM>(newState), 0);
}

void ClearLog(AppState* state) {
    if (!state || !state->logEdit) {
        return;
    }
    SetWindowTextW(state->logEdit, L"");
}

void UpdateStatusBarLayout(AppState* state) {
    if (!state || !state->statusBar) {
        return;
    }
    RECT rect{};
    GetClientRect(state->hwnd, &rect);
    const int width = rect.right - rect.left;
    int parts[1] = { width };
    SendMessageW(state->statusBar, SB_SETPARTS, 1, reinterpret_cast<LPARAM>(parts));
}

void BuildMainMenu(AppState* state) {
    if (!state) {
        return;
    }
    HMENU menu = CreateMenu();
    HMENU fileMenu = CreatePopupMenu();
    HMENU recordMenu = CreatePopupMenu();
    HMENU playbackMenu = CreatePopupMenu();
    HMENU settingsMenu = CreatePopupMenu();
    HMENU formatMenu = CreatePopupMenu();
    HMENU bitrateMenu = CreatePopupMenu();
    HMENU viewMenu = CreatePopupMenu();
    HMENU helpMenu = CreatePopupMenu();

    AppendMenuW(fileMenu, MF_STRING, IDM_FILE_NEW, L"新建录音\tCtrl+N");
    AppendMenuW(fileMenu, MF_STRING, IDM_FILE_OPEN_FOLDER, L"打开音频保存目录");
    AppendMenuW(fileMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(fileMenu, MF_STRING, IDM_FILE_EXIT, L"退出");

    AppendMenuW(recordMenu, MF_STRING, IDM_RECORD_START_STOP, L"开始录音/停止录音\tCtrl+R");
    AppendMenuW(recordMenu, MF_STRING, IDM_RECORD_PAUSE, L"暂停/继续录音\tCtrl+P");

    AppendMenuW(playbackMenu, MF_STRING, IDM_PLAYBACK_PLAY, L"播放\tSpace");
    AppendMenuW(playbackMenu, MF_STRING, IDM_PLAYBACK_PAUSE, L"暂停\tCtrl+Alt+P");
    AppendMenuW(playbackMenu, MF_STRING, IDM_PLAYBACK_STOP, L"停止\tCtrl+Space");

    AppendMenuW(formatMenu, MF_STRING, IDM_SETTINGS_FORMAT_WAV, L"WAV");
    AppendMenuW(formatMenu, MF_STRING, IDM_SETTINGS_FORMAT_MP3, L"MP3");

    AppendMenuW(bitrateMenu, MF_STRING, IDM_SETTINGS_BITRATE_128, L"128 kbps");
    AppendMenuW(bitrateMenu, MF_STRING, IDM_SETTINGS_BITRATE_192, L"192 kbps");
    AppendMenuW(bitrateMenu, MF_STRING, IDM_SETTINGS_BITRATE_256, L"256 kbps");
    AppendMenuW(bitrateMenu, MF_STRING, IDM_SETTINGS_BITRATE_320, L"320 kbps");

    AppendMenuW(settingsMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(formatMenu), L"输出格式");
    AppendMenuW(settingsMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(bitrateMenu), L"MP3 比特率");

    AppendMenuW(viewMenu, MF_STRING, IDM_VIEW_CLEAR_LOG, L"清空日志\tCtrl+L");

    AppendMenuW(helpMenu, MF_STRING, IDM_HELP_ABOUT, L"关于\tF1");

    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(fileMenu), L"  文件  ");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(recordMenu), L"  录音  ");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(playbackMenu), L"  播放  ");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(settingsMenu), L"  设置  ");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(viewMenu), L"  查看  ");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(helpMenu), L"  帮助  ");

    state->mainMenu = menu;
    state->settingsMenu = settingsMenu;
    state->bitrateMenu = bitrateMenu;
    SetMenu(state->hwnd, menu);
    UpdateMenuForState(state);
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

std::filesystem::path GetDefaultOutputFolder() {
    std::error_code ec;
    auto current = std::filesystem::current_path(ec);
    if (!ec && !current.empty()) {
        return current;
    }
    wchar_t modulePath[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    if (len > 0) {
        std::filesystem::path exePath(modulePath);
        if (exePath.has_parent_path()) {
            return exePath.parent_path();
        }
    }
    return {};
}

std::filesystem::path GetExecutableDirectory() {
    wchar_t modulePath[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    if (len == 0) {
        return {};
    }
    std::filesystem::path exePath(modulePath);
    if (exePath.has_parent_path()) {
        return exePath.parent_path();
    }
    return {};
}

std::filesystem::path FindQrImagePath() {
    std::vector<std::filesystem::path> candidates;
    const std::filesystem::path exeDir = GetExecutableDirectory();
    if (!exeDir.empty()) {
        candidates.push_back(exeDir / L"wechat_qr.png");
        candidates.push_back(exeDir / L"wechat_qr.bmp");
        candidates.push_back(exeDir / L"assets" / L"wechat_qr.png");
        candidates.push_back(exeDir / L"assets" / L"wechat_qr.bmp");
        candidates.push_back(exeDir / L".." / L"assets" / L"wechat_qr.png");
        candidates.push_back(exeDir / L".." / L"assets" / L"wechat_qr.bmp");
        candidates.push_back(exeDir / L".." / L".." / L"assets" / L"wechat_qr.png");
        candidates.push_back(exeDir / L".." / L".." / L"assets" / L"wechat_qr.bmp");
    }
    std::error_code ec;
    auto cwd = std::filesystem::current_path(ec);
    if (!ec && !cwd.empty()) {
        candidates.push_back(cwd / L"wechat_qr.png");
        candidates.push_back(cwd / L"wechat_qr.bmp");
        candidates.push_back(cwd / L"assets" / L"wechat_qr.png");
        candidates.push_back(cwd / L"assets" / L"wechat_qr.bmp");
    }
    for (const auto& candidate : candidates) {
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
    return {};
}

HBITMAP LoadQrBitmapFromResource(HINSTANCE instance) {
    HRSRC resource = FindResourceW(instance, MAKEINTRESOURCEW(IDR_QR_PNG), MAKEINTRESOURCEW(10));
    if (!resource) {
        return nullptr;
    }
    HGLOBAL resourceData = LoadResource(instance, resource);
    if (!resourceData) {
        return nullptr;
    }
    DWORD resourceSize = SizeofResource(instance, resource);
    if (resourceSize == 0) {
        return nullptr;
    }
    const void* data = LockResource(resourceData);
    if (!data) {
        return nullptr;
    }
    HGLOBAL buffer = GlobalAlloc(GMEM_MOVEABLE, resourceSize);
    if (!buffer) {
        return nullptr;
    }
    void* dest = GlobalLock(buffer);
    if (!dest) {
        GlobalFree(buffer);
        return nullptr;
    }
    std::memcpy(dest, data, resourceSize);
    GlobalUnlock(buffer);

    IStream* stream = nullptr;
    if (CreateStreamOnHGlobal(buffer, TRUE, &stream) != S_OK) {
        GlobalFree(buffer);
        return nullptr;
    }

    std::unique_ptr<Gdiplus::Bitmap> bitmap(Gdiplus::Bitmap::FromStream(stream));
    stream->Release();
    if (!bitmap || bitmap->GetLastStatus() != Gdiplus::Ok) {
        return nullptr;
    }
    HBITMAP hbitmap = nullptr;
    bitmap->GetHBITMAP(Gdiplus::Color(0xFF, 0xFF, 0xFF, 0xFF), &hbitmap);
    return hbitmap;
}

HBITMAP LoadQrBitmap() {
    HBITMAP resourceBitmap = LoadQrBitmapFromResource(GetModuleHandleW(nullptr));
    if (resourceBitmap) {
        return resourceBitmap;
    }
    const std::filesystem::path path = FindQrImagePath();
    if (path.empty()) {
        return nullptr;
    }
    Gdiplus::Bitmap bitmap(path.c_str());
    if (bitmap.GetLastStatus() != Gdiplus::Ok) {
        return nullptr;
    }
    HBITMAP hbitmap = nullptr;
    bitmap.GetHBITMAP(Gdiplus::Color(0xFF, 0xFF, 0xFF, 0xFF), &hbitmap);
    return hbitmap;
}

bool IsMp3DllAvailable() {
    wchar_t envPath[MAX_PATH] = {};
    const DWORD envLen = GetEnvironmentVariableW(L"LAME_DLL_PATH", envPath, MAX_PATH);
    if (envLen > 0 && envLen < MAX_PATH) {
        if (std::filesystem::exists(envPath)) {
            return true;
        }
    }
    const std::filesystem::path exeDir = GetExecutableDirectory();
    if (exeDir.empty()) {
        return false;
    }
    return std::filesystem::exists(exeDir / L"libmp3lame.dll") ||
        std::filesystem::exists(exeDir / L"lame_enc.dll");
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
        UpdateControlsForState(state);
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
    UpdateControlsForState(state);
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
        target = GetDefaultOutputFolder();
    }
    if (target.empty()) {
        AppendLog(state->logEdit, L"[界面] 无法打开目录：路径为空。");
        return;
    }
    HINSTANCE res = ShellExecuteW(nullptr, L"open", target.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(res) <= 32) {
        AppendLog(state->logEdit, L"[界面] 打开目录失败：" + target.wstring());
    } else {
        AppendLog(state->logEdit, L"[界面] 已打开目录：" + target.wstring());
    }
}

std::filesystem::path ResolvePlayablePath(AppState* state) {
    if (!state) {
        return {};
    }
    std::filesystem::path basePath = state->currentOutputPath;
    if (basePath.empty()) {
        basePath = GetWindowTextString(state->outputEdit);
    }
    if (basePath.empty()) {
        basePath = DefaultOutputPath();
    }
    std::error_code ec;
    if (!basePath.empty() && std::filesystem::exists(basePath, ec) && std::filesystem::is_regular_file(basePath, ec)) {
        return basePath;
    }
    if (!basePath.empty()) {
        auto firstSegment = BuildSegmentPath(basePath, 0);
        if (std::filesystem::exists(firstSegment, ec) && std::filesystem::is_regular_file(firstSegment, ec)) {
            return firstSegment;
        }
    }
    return {};
}

void PlayRecording(AppState* state) {
    if (!state || state->state != AppState::RecorderState::Idle) {
        return;
    }
    auto playable = ResolvePlayablePath(state);
    if (playable.empty()) {
        AppendLog(state->logEdit, L"[界面] 未找到可播放的录音文件。");
        return;
    }
    if (!state->player) {
        AppendLog(state->logEdit, L"[界面] 播放器未初始化。");
        return;
    }
    const bool shouldOpen = state->currentPlaybackPath.empty() || state->currentPlaybackPath != playable;
    if (shouldOpen) {
        state->currentPlaybackPath = playable;
        if (!state->player->OpenFile(playable)) {
            AppendLog(state->logEdit, L"[界面] 打开播放文件失败。");
            return;
        }
    }
    state->player->Play();
    AppendLog(state->logEdit, L"[界面] 播放录音：" + playable.wstring());
}

void PausePlayback(AppState* state) {
    if (!state || !state->player) {
        return;
    }
    state->player->Pause();
}

void StopPlayback(AppState* state) {
    if (!state || !state->player) {
        return;
    }
    state->player->Stop();
}

void TogglePlayback(AppState* state) {
    if (!state) {
        return;
    }
    switch (state->playbackState) {
    case PlaybackState::Playing:
        PausePlayback(state);
        break;
    case PlaybackState::Paused:
    case PlaybackState::Stopped:
    case PlaybackState::Ended:
    case PlaybackState::Idle:
        PlayRecording(state);
        break;
    case PlaybackState::Opening:
    case PlaybackState::Error:
    default:
        break;
    }
}

std::wstring FormatPlaybackTime(int64_t position100ns, int64_t duration100ns) {
    auto formatSeconds = [](int64_t seconds) {
        if (seconds < 0) {
            seconds = 0;
        }
        const int64_t hours = seconds / 3600;
        const int64_t mins = (seconds % 3600) / 60;
        const int64_t secs = seconds % 60;
        wchar_t buffer[32];
        if (hours > 0) {
            swprintf_s(buffer, L"%02lld:%02lld:%02lld", hours, mins, secs);
        } else {
            swprintf_s(buffer, L"%02lld:%02lld", mins, secs);
        }
        return std::wstring(buffer);
    };

    const int64_t posSec = position100ns / 10000000;
    const int64_t durSec = duration100ns / 10000000;
    return formatSeconds(posSec) + L" / " + formatSeconds(durSec);
}

void UpdatePlaybackTime(AppState* state, int64_t position100ns) {
    if (!state || !state->playbackSeek || !state->playbackTimeLabel) {
        return;
    }
    const int seekRange = 1000;
    if (!state->playbackSeeking && state->playbackDuration100ns > 0) {
        const double ratio = static_cast<double>(position100ns) /
            static_cast<double>(state->playbackDuration100ns);
        const int pos = static_cast<int>(std::clamp(ratio, 0.0, 1.0) * seekRange);
        SendMessageW(state->playbackSeek, TBM_SETPOS, TRUE, pos);
    }
    std::wstring timeText = FormatPlaybackTime(position100ns, state->playbackDuration100ns);
    SetWindowTextW(state->playbackTimeLabel, timeText.c_str());
}

void UpdatePlaybackControls(AppState* state) {
    if (!state) {
        return;
    }
    const bool canUsePlayback = state->state == AppState::RecorderState::Idle;
    const bool hasPlayable = !ResolvePlayablePath(state).empty();
    const bool canPlay = canUsePlayback && hasPlayable &&
        state->playbackState != PlaybackState::Playing &&
        state->playbackState != PlaybackState::Opening;
    const bool canPause = canUsePlayback && state->playbackState == PlaybackState::Playing;
    const bool canStop = canUsePlayback && (state->playbackState == PlaybackState::Playing ||
                                            state->playbackState == PlaybackState::Paused);
    EnableWindow(state->playbackPlayButton, canPlay ? TRUE : FALSE);
    EnableWindow(state->playbackPauseButton, canPause ? TRUE : FALSE);
    EnableWindow(state->playbackStopButton, canStop ? TRUE : FALSE);
    EnableWindow(state->playbackSeek, (canUsePlayback && state->playbackDuration100ns > 0) ? TRUE : FALSE);
    EnableWindow(state->playbackVolume, canUsePlayback ? TRUE : FALSE);
}

RecordingStatusParts BuildRecordingStatusParts(AppState* state) {
    RecordingStatusParts parts{};
    if (!state) {
        return parts;
    }
    std::wstring status;
    switch (state->state) {
    case AppState::RecorderState::Idle:
        status = L"空闲";
        break;
    case AppState::RecorderState::Starting:
        status = L"启动中";
        break;
    case AppState::RecorderState::Recording:
        status = state->paused ? L"已暂停" : L"录音中";
        break;
    case AppState::RecorderState::Recovering:
        status = L"重连中";
        break;
    case AppState::RecorderState::Stopping:
        status = L"停止中";
        break;
    default:
        status = L"未知";
        break;
    }
    parts.status = status;

    auto now = std::chrono::steady_clock::now();
    std::chrono::seconds elapsed{0};
    if (state->state == AppState::RecorderState::Recording || state->state == AppState::RecorderState::Stopping ||
        state->state == AppState::RecorderState::Recovering) {
        elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - state->startTime - state->pausedTotal);
        if (state->paused) {
            elapsed -= std::chrono::duration_cast<std::chrono::seconds>(now - state->pauseStart);
        }
        if (elapsed.count() < 0) {
            elapsed = std::chrono::seconds(0);
        }
    }
    const int hours = static_cast<int>(elapsed.count() / 3600);
    const int mins = static_cast<int>((elapsed.count() % 3600) / 60);
    const int secs = static_cast<int>(elapsed.count() % 60);
    wchar_t timeBuf[16];
    swprintf_s(timeBuf, L"%02d:%02d:%02d", hours, mins, secs);
    parts.time = timeBuf;

    std::filesystem::path sizePath = state->currentOutputPath;
    if (!sizePath.empty()) {
        sizePath = BuildSegmentPath(sizePath, 0);
    }
    uintmax_t bytes = 0;
    std::error_code ec;
    if (!sizePath.empty() && std::filesystem::exists(sizePath, ec)) {
        bytes = std::filesystem::file_size(sizePath, ec);
    }
    parts.size = FormatBytes(bytes);

    const bool mp3Selected = state->formatCombo &&
        SendMessageW(state->formatCombo, CB_GETCURSEL, 0, 0) == 1;
    const int bitrate = GetBitrateFromEdit(state->bitrateEdit, state->defaultBitrate);
    std::wstring format = mp3Selected ? L"MP3" : L"WAV";
    if (mp3Selected) {
        format += L" ";
        format += std::to_wstring(bitrate);
        format += L" kbps";
    }
    parts.format = format;
    return parts;
}

std::wstring BuildRecordingSummary(AppState* state) {
    if (!state) {
        return L"";
    }
    RecordingStatusParts parts = BuildRecordingStatusParts(state);

    std::wstring summary = parts.status;
    summary += L" | ";
    summary += parts.time;
    summary += L" | ";
    summary += parts.size;
    summary += L" | ";
    summary += parts.format;
    return summary;
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
    UpdateControlsForState(state);
    UpdateStatusDetails(state);
}

void SetFormatSelection(AppState* state, bool mp3Selected) {
    if (!state || !state->formatCombo) {
        return;
    }
    SendMessageW(state->formatCombo, CB_SETCURSEL, mp3Selected ? 1 : 0, 0);
    UpdateControlsForState(state);
    UpdateOutputExtension(state);
}

void SetBitrateValue(AppState* state, int bitrate) {
    if (!state || !state->bitrateEdit) {
        return;
    }
    SetWindowTextW(state->bitrateEdit, std::to_wstring(bitrate).c_str());
    UpdateStatusDetails(state);
    UpdateMenuForState(state);
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
            PostDeviceNameUpdate(state->hwnd, friendly);

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
    bool mp3Enabled = state->formatCombo &&
        SendMessageW(state->formatCombo, CB_GETCURSEL, 0, 0) == 1;
    if (mp3Enabled && !IsMp3DllAvailable()) {
        MessageBoxW(state->hwnd,
                    L"未检测到 libmp3lame.dll（或 lame_enc.dll），只能保存为 WAV 文件。\n"
                    L"请将 DLL 放到程序同目录，或设置环境变量 LAME_DLL_PATH。",
                    L"缺少 MP3 编码库", MB_OK | MB_ICONWARNING);
        AppendLog(state->logEdit, L"[界面] 未检测到 MP3 编码库，已切换为 WAV 输出。");
        SetFormatSelection(state, false);
        mp3Enabled = false;
    }
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
        SetWindowTextW(state->pauseButton, L"继续录音");
        AppendLog(state->logEdit, L"[界面] 已暂停。");
    } else {
        state->pausedTotal += std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - state->pauseStart);
        SetWindowTextW(state->pauseButton, L"暂停录音");
        AppendLog(state->logEdit, L"[界面] 已继续。");
    }
    UpdateStatusText(state);
    if (state->startButton) {
        InvalidateRect(state->startButton, nullptr, TRUE);
    }
}

void CleanupWorker(AppState* state) {
    if (state->worker.joinable()) {
        state->worker.join();
    }
    state->stopRequested.store(false);
    state->pauseRequested.store(false);
    state->paused = false;
    state->pausedTotal = std::chrono::milliseconds(0);
    SetWindowTextW(state->pauseButton, L"暂停录音");
    state->state = AppState::RecorderState::Idle;
    UpdateControlsForState(state);
    AppendLog(state->logEdit, L"[界面] 录音已停止。");
    UpdateStatusText(state);
}

void CreateChildControls(HWND hwnd, AppState* state) {
    const int padding = 20;
    const int labelHeight = 16;
    const int editHeight = 26;
    const int buttonHeight = 30;
    const int actionButtonWidth = 96;
    const int actionButtonHeight = buttonHeight;
    RECT client{};
    GetClientRect(hwnd, &client);
    int windowWidth = static_cast<int>(client.right - client.left);
    if (windowWidth <= 0) {
        windowWidth = 900;
    }
    const int contentWidth = windowWidth - padding * 2;

    state->uiFont = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    state->uiFontBold = CreateFontW(13, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                    DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    state->uiFontTitle = CreateFontW(16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                     DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    state->uiFontSecondary = CreateFontW(11, 0, 0, 0, FW_LIGHT, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                         DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    state->uiFontTimer = CreateFontW(26, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                     DEFAULT_PITCH | FF_MODERN, L"Consolas");
    state->backgroundBrush = CreateSolidBrush(state->backgroundColor);
    state->headerBrush = CreateSolidBrush(state->headerColor);
    state->panelBrush = CreateSolidBrush(state->panelColor);
    state->panelAltBrush = CreateSolidBrush(state->panelAltColor);
    HFONT font = state->uiFont;

    const int groupLeft = padding;
    int y = padding;

    state->headerLabel = nullptr;

    const int statusGroupHeight = 80;
    state->statusGroup = CreateWindowW(L"BUTTON", L"录音状态", WS_VISIBLE | WS_CHILD | BS_GROUPBOX,
                                     groupLeft, y, contentWidth, statusGroupHeight, hwnd, nullptr, nullptr, nullptr);
    SetControlFont(state->statusGroup, state->uiFontSecondary);

    state->statusStateLabel = CreateWindowW(L"STATIC", L"空闲", WS_VISIBLE | WS_CHILD,
                                            groupLeft + 16, y + 20, contentWidth - 32, labelHeight,
                                            hwnd, nullptr, nullptr, nullptr);
    SetControlFont(state->statusStateLabel, state->uiFontTitle);

    state->statusTimeLabel = CreateWindowW(L"STATIC", L"00:00:00", WS_VISIBLE | WS_CHILD,
                                           groupLeft + 16, y + 36, contentWidth - 32, 30,
                                           hwnd, nullptr, nullptr, nullptr);
    SetControlFont(state->statusTimeLabel, state->uiFontTimer);

    state->statusMetaLabel = CreateWindowW(L"STATIC", L"0 B | MP3 192 kbps", WS_VISIBLE | WS_CHILD,
                                           groupLeft + 16, y + 62, contentWidth - 32, labelHeight,
                                           hwnd, nullptr, nullptr, nullptr);
    SetControlFont(state->statusMetaLabel, state->uiFontSecondary);

    y += statusGroupHeight + 10;
    const int actionGroupHeight = 92;
    state->actionGroup = CreateWindowW(L"BUTTON", L"主要操作", WS_VISIBLE | WS_CHILD | BS_GROUPBOX,
                                     groupLeft, y, contentWidth, actionGroupHeight, hwnd, nullptr, nullptr, nullptr);
    SetControlFont(state->actionGroup, state->uiFontSecondary);

    const int actionRowY = y + 32;
    const int actionSpacing = 12;
    const int actionRowWidth = actionButtonWidth * 2 + actionSpacing;
    const int actionX = groupLeft + (contentWidth - actionRowWidth) / 2;
    state->startButton = CreateWindowW(L"BUTTON", L"开始录音",
                                       WS_VISIBLE | WS_CHILD | BS_OWNERDRAW,
                                       actionX, actionRowY, actionButtonWidth, actionButtonHeight,
                                       hwnd, reinterpret_cast<HMENU>(IDC_START_BUTTON), nullptr, nullptr);
    SetControlFont(state->startButton, state->uiFontBold);

    state->pauseButton = CreateWindowW(L"BUTTON", L"暂停录音",
                                       WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                                       actionX + actionButtonWidth + actionSpacing, actionRowY,
                                       actionButtonWidth, actionButtonHeight,
                                       hwnd, reinterpret_cast<HMENU>(IDC_PAUSE_BUTTON), nullptr, nullptr);
    SetControlFont(state->pauseButton, font);
    EnableWindow(state->pauseButton, FALSE);

    state->stopButton = nullptr;

    y += actionGroupHeight + 10;
    const int settingsGroupHeight = 130;
    state->settingsGroup = CreateWindowW(L"BUTTON", L"录音设置", WS_VISIBLE | WS_CHILD | BS_GROUPBOX,
                                      groupLeft, y, contentWidth, settingsGroupHeight, hwnd, nullptr, nullptr, nullptr);
    SetControlFont(state->settingsGroup, state->uiFontSecondary);

    const int outputLabelY = y + 24;
    state->outputLabel = CreateWindowW(L"STATIC", L"输出文件：", WS_VISIBLE | WS_CHILD,
                                     groupLeft + 12, outputLabelY, 72, labelHeight, hwnd, nullptr, nullptr, nullptr);
    SetControlFont(state->outputLabel, state->uiFontSecondary);
    state->outputEdit = CreateWindowExW(0, L"EDIT", DefaultOutputPath().wstring().c_str(),
                                        WS_VISIBLE | WS_CHILD | ES_AUTOHSCROLL,
                                        groupLeft + 86, outputLabelY - 2, contentWidth - 98, editHeight,
                                        hwnd, reinterpret_cast<HMENU>(IDC_OUTPUT_EDIT), nullptr, nullptr);
    SetControlFont(state->outputEdit, font);

    const int buttonRowY = y + 54;
    const int smallButtonWidth = 110;
    state->browseButton = CreateWindowW(L"BUTTON", L"选择文件",
                                      WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                                      groupLeft + 12, buttonRowY, smallButtonWidth, buttonHeight,
                                      hwnd, reinterpret_cast<HMENU>(IDC_BROWSE_BUTTON), nullptr, nullptr);
    SetControlFont(state->browseButton, font);

    state->browseFolderButton = CreateWindowW(L"BUTTON", L"选择文件夹",
                                            WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                                            groupLeft + 12 + smallButtonWidth + 8, buttonRowY, smallButtonWidth, buttonHeight,
                                            hwnd, reinterpret_cast<HMENU>(IDC_BROWSE_FOLDER), nullptr, nullptr);
    SetControlFont(state->browseFolderButton, font);

    state->openFolderButton = CreateWindowW(L"BUTTON", L"打开目录",
                                          WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                                          groupLeft + 12 + (smallButtonWidth + 8) * 2, buttonRowY, smallButtonWidth, buttonHeight,
                                          hwnd, reinterpret_cast<HMENU>(IDC_OPEN_FOLDER), nullptr, nullptr);
    SetControlFont(state->openFolderButton, font);

    const int formatRowY = y + 86;
    state->formatLabel = CreateWindowW(L"STATIC", L"输出格式：", WS_VISIBLE | WS_CHILD,
                                     groupLeft + 12, formatRowY, 72, labelHeight, hwnd, nullptr, nullptr, nullptr);
    SetControlFont(state->formatLabel, state->uiFontSecondary);
    state->formatCombo = CreateWindowW(L"COMBOBOX", L"", WS_VISIBLE | WS_CHILD | CBS_DROPDOWNLIST,
                                       groupLeft + 86, formatRowY - 2, 120, 200,
                                       hwnd, reinterpret_cast<HMENU>(IDC_FORMAT_COMBO), nullptr, nullptr);
    SetControlFont(state->formatCombo, font);
    SendMessageW(state->formatCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"WAV"));
    SendMessageW(state->formatCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"MP3"));
    SendMessageW(state->formatCombo, CB_SETCURSEL, 1, 0);

    state->bitrateLabel = CreateWindowW(L"STATIC", L"音质 (kbps)：", WS_VISIBLE | WS_CHILD,
                                      groupLeft + 230, formatRowY, 90, labelHeight, hwnd, nullptr, nullptr, nullptr);
    SetControlFont(state->bitrateLabel, state->uiFontSecondary);
    state->bitrateEdit = CreateWindowExW(0, L"EDIT", L"192",
                                         WS_VISIBLE | WS_CHILD | ES_NUMBER,
                                         groupLeft + 324, formatRowY - 2,
                                         90, editHeight,
                                         hwnd, reinterpret_cast<HMENU>(IDC_BITRATE_EDIT), nullptr, nullptr);
    SetControlFont(state->bitrateEdit, font);

    y += settingsGroupHeight + 10;
    const int playbackGroupHeight = 90;
    state->playbackGroup = CreateWindowW(L"BUTTON", L"回放检查", WS_VISIBLE | WS_CHILD | BS_GROUPBOX,
                                       groupLeft, y, contentWidth, playbackGroupHeight, hwnd, nullptr, nullptr, nullptr);
    SetControlFont(state->playbackGroup, state->uiFontSecondary);

    const int playbackRowY = y + 24;
    state->playbackPlayButton = CreateWindowW(L"BUTTON", L"播放",
                                              WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                                              groupLeft + 12, playbackRowY,
                                              90, buttonHeight,
                                              hwnd, reinterpret_cast<HMENU>(IDC_PLAYBACK_PLAY), nullptr, nullptr);
    SetControlFont(state->playbackPlayButton, font);

    state->playbackPauseButton = CreateWindowW(L"BUTTON", L"暂停",
                                               WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                                               groupLeft + 110, playbackRowY,
                                               90, buttonHeight,
                                               hwnd, reinterpret_cast<HMENU>(IDC_PLAYBACK_PAUSE), nullptr, nullptr);
    SetControlFont(state->playbackPauseButton, font);

    state->playbackStopButton = CreateWindowW(L"BUTTON", L"停止",
                                              WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                                              groupLeft + 208, playbackRowY,
                                              90, buttonHeight,
                                              hwnd, reinterpret_cast<HMENU>(IDC_PLAYBACK_STOP), nullptr, nullptr);
    SetControlFont(state->playbackStopButton, font);

    state->playbackSeek = CreateWindowW(TRACKBAR_CLASSW, L"",
                                        WS_VISIBLE | WS_CHILD | TBS_AUTOTICKS,
                                        groupLeft + 310, playbackRowY,
                                        contentWidth - 420, buttonHeight,
                                        hwnd, reinterpret_cast<HMENU>(IDC_PLAYBACK_SEEK), nullptr, nullptr);
    SendMessageW(state->playbackSeek, TBM_SETRANGE, TRUE, MAKELONG(0, 1000));

    state->playbackTimeLabel = CreateWindowW(L"STATIC", L"00:00 / 00:00", WS_VISIBLE | WS_CHILD,
                                             groupLeft + contentWidth - 90, playbackRowY + 4,
                                             90, labelHeight,
                                             hwnd, reinterpret_cast<HMENU>(IDC_PLAYBACK_TIME), nullptr, nullptr);
    SetControlFont(state->playbackTimeLabel, state->uiFontSecondary);

    state->playbackVolumeLabel = CreateWindowW(L"STATIC", L"音量：", WS_VISIBLE | WS_CHILD,
                                     groupLeft + 12, playbackRowY + 34, 48, labelHeight,
                                     hwnd, nullptr, nullptr, nullptr);
    SetControlFont(state->playbackVolumeLabel, state->uiFontSecondary);

    state->playbackVolume = CreateWindowW(TRACKBAR_CLASSW, L"",
                                          WS_VISIBLE | WS_CHILD | TBS_AUTOTICKS,
                                          groupLeft + 60, playbackRowY + 32,
                                          200, buttonHeight,
                                          hwnd, reinterpret_cast<HMENU>(IDC_PLAYBACK_VOLUME), nullptr, nullptr);
    SendMessageW(state->playbackVolume, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
    SendMessageW(state->playbackVolume, TBM_SETPOS, TRUE, static_cast<LPARAM>(state->playbackVolumeValue * 100));

    y += playbackGroupHeight + 10;
    const int logGroupHeight = 84;
    const int logEditHeight = 60;
    state->logGroup = CreateWindowW(L"BUTTON", L"日志", WS_VISIBLE | WS_CHILD | BS_GROUPBOX,
                                  groupLeft, y, contentWidth, logGroupHeight, hwnd, nullptr, nullptr, nullptr);
    SetControlFont(state->logGroup, state->uiFontSecondary);

    state->logEdit = CreateWindowExW(0, L"EDIT", L"",
                                     WS_VISIBLE | WS_CHILD | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
                                     groupLeft + 12, y + 26,
                                     contentWidth - 24, logEditHeight,
                                     hwnd, reinterpret_cast<HMENU>(IDC_LOG_EDIT), nullptr, nullptr);
    SetControlFont(state->logEdit, font);

    SHFILEINFOW sfi{};
    if (SHGetFileInfoW(L".txt", FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
                       SHGFI_ICON | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES)) {
        state->fileIcon = sfi.hIcon;
        AttachButtonIcon(state->browseButton, state->fileIcon, state->fileImageList);
    }
    if (SHGetFileInfoW(L"C:\\", FILE_ATTRIBUTE_DIRECTORY, &sfi, sizeof(sfi),
                       SHGFI_ICON | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES)) {
        state->folderIcon = sfi.hIcon;
        AttachButtonIcon(state->browseFolderButton, state->folderIcon, state->folderImageList);
    }
    if (SHGetFileInfoW(L"C:\\", FILE_ATTRIBUTE_DIRECTORY, &sfi, sizeof(sfi),
                       SHGFI_ICON | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES)) {
        state->openIcon = sfi.hIcon;
        AttachButtonIcon(state->openFolderButton, state->openIcon, state->openImageList);
    }
}

void ShowAboutDialog(HWND parent) {
    static const wchar_t kAboutClassName[] = L"LoopbackRecorderAbout";
    static std::atomic<bool> registered{false};
    if (!registered.exchange(true)) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = AboutWindowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPWSTR>(IDC_ARROW));
        wc.hIcon = LoadIconW(nullptr, reinterpret_cast<LPWSTR>(IDI_APPLICATION));
        wc.lpszClassName = kAboutClassName;
        RegisterClassW(&wc);
    }

    AboutDialogState* aboutState = new AboutDialogState{};
    aboutState->parent = parent;

    const int width = 600;
    const int height = 420;
    RECT rect{ 0, 0, width, height };
    AdjustWindowRectEx(&rect, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    const int windowWidth = rect.right - rect.left;
    const int windowHeight = rect.bottom - rect.top;

    RECT parentRect{};
    GetWindowRect(parent, &parentRect);
    const int x = parentRect.left + ((parentRect.right - parentRect.left) - windowWidth) / 2;
    const int y = parentRect.top + ((parentRect.bottom - parentRect.top) - windowHeight) / 2;

    HWND aboutWindow = CreateWindowExW(WS_EX_DLGMODALFRAME, kAboutClassName, L"关于",
                                       WS_CAPTION | WS_SYSMENU | WS_POPUP,
                                       x, y, windowWidth, windowHeight,
                                       parent, nullptr, GetModuleHandleW(nullptr), aboutState);
    if (!aboutWindow) {
        delete aboutState;
        return;
    }
    EnableWindow(parent, FALSE);
    ShowWindow(aboutWindow, SW_SHOW);
    UpdateWindow(aboutWindow);
}

LRESULT CALLBACK MainWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    AppState* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto newState = std::make_unique<AppState>();
        newState->hwnd = hwnd;
        CreateChildControls(hwnd, newState.get());
        BuildMainMenu(newState.get());
        newState->player = new MediaFoundationPlayer();
        newState->playbackListener = new PlaybackListener(hwnd);
        newState->player->SetListener(newState->playbackListener);
        newState->player->Initialize();
        newState->player->SetVolume(newState->playbackVolumeValue);
        newState->statusBar = CreateWindowExW(0, STATUSCLASSNAMEW, L"",
                                              WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                                              0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);
        if (newState->uiFont) {
            SetControlFont(newState->statusBar, newState->uiFont);
        }
        SendMessageW(newState->statusBar, SB_SETBKCOLOR, 0, newState->panelColor);
        SendMessageW(newState->statusBar, SB_SIMPLE, TRUE, 0);
        UpdateStatusBarLayout(newState.get());
        UpdateStatusText(newState.get());
        SetTimer(hwnd, 1, 1000, nullptr);
        SetTimer(hwnd, 2, 250, nullptr);
        UpdateControlsForState(newState.get());
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
            if (state->state == AppState::RecorderState::Idle) {
                StartRecording(state);
            } else if (state->state == AppState::RecorderState::Recording ||
                       state->state == AppState::RecorderState::Recovering) {
                StopRecording(state);
            }
            return 0;
        case IDC_STOP_BUTTON:
            StopRecording(state);
            return 0;
        case IDC_PAUSE_BUTTON:
            TogglePause(state);
            return 0;
        case IDC_PLAYBACK_PLAY:
            PlayRecording(state);
            return 0;
        case IDC_PLAYBACK_PAUSE:
            PausePlayback(state);
            return 0;
        case IDC_PLAYBACK_STOP:
            StopPlayback(state);
            return 0;
        case IDC_FORMAT_COMBO:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                UpdateControlsForState(state);
                UpdateOutputExtension(state);
            }
            return 0;
        case IDC_BITRATE_EDIT:
            if (HIWORD(wParam) == EN_CHANGE) {
                UpdateStatusDetails(state);
                UpdateMenuForState(state);
            }
            return 0;
        case IDC_OUTPUT_EDIT:
            if (HIWORD(wParam) == EN_CHANGE) {
                UpdateControlsForState(state);
            }
            return 0;
        case IDM_FILE_NEW:
            if (state->state == AppState::RecorderState::Idle) {
                SetWindowTextW(state->outputEdit, DefaultOutputPath().wstring().c_str());
                SetFormatSelection(state, true);
                SetBitrateValue(state, state->defaultBitrate);
                ClearLog(state);
            }
            return 0;
        case IDM_FILE_OPEN_FOLDER:
            OpenOutputFolder(state);
            return 0;
        case IDM_FILE_EXIT:
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            return 0;
        case IDM_RECORD_START_STOP:
            if (state->state == AppState::RecorderState::Idle) {
                StartRecording(state);
            } else {
                StopRecording(state);
            }
            return 0;
        case IDM_RECORD_PAUSE:
            TogglePause(state);
            return 0;
        case IDM_PLAYBACK_PLAY:
            PlayRecording(state);
            return 0;
        case IDM_PLAYBACK_PAUSE:
            PausePlayback(state);
            return 0;
        case IDM_PLAYBACK_STOP:
            StopPlayback(state);
            return 0;
        case IDM_PLAYBACK_TOGGLE:
            TogglePlayback(state);
            return 0;
        case IDM_SETTINGS_FORMAT_WAV:
            if (state->state == AppState::RecorderState::Idle) {
                SetFormatSelection(state, false);
            }
            return 0;
        case IDM_SETTINGS_FORMAT_MP3:
            if (state->state == AppState::RecorderState::Idle) {
                SetFormatSelection(state, true);
            }
            return 0;
        case IDM_SETTINGS_BITRATE_128:
            if (state->state == AppState::RecorderState::Idle) {
                SetBitrateValue(state, 128);
            }
            return 0;
        case IDM_SETTINGS_BITRATE_192:
            if (state->state == AppState::RecorderState::Idle) {
                SetBitrateValue(state, 192);
            }
            return 0;
        case IDM_SETTINGS_BITRATE_256:
            if (state->state == AppState::RecorderState::Idle) {
                SetBitrateValue(state, 256);
            }
            return 0;
        case IDM_SETTINGS_BITRATE_320:
            if (state->state == AppState::RecorderState::Idle) {
                SetBitrateValue(state, 320);
            }
            return 0;
        case IDM_VIEW_CLEAR_LOG:
            ClearLog(state);
            return 0;
        case IDM_HELP_ABOUT:
            ShowAboutDialog(hwnd);
            return 0;
        default:
            break;
        }
        break;
    case WM_HSCROLL:
        if (state) {
            HWND target = reinterpret_cast<HWND>(lParam);
            if (target == state->playbackSeek && state->playbackDuration100ns > 0) {
                const int code = LOWORD(wParam);
                const int pos = static_cast<int>(SendMessageW(state->playbackSeek, TBM_GETPOS, 0, 0));
                const int64_t targetPos = static_cast<int64_t>(
                    (static_cast<double>(pos) / 1000.0) * state->playbackDuration100ns);
                if (code == TB_THUMBTRACK) {
                    state->playbackSeeking = true;
                    UpdatePlaybackTime(state, targetPos);
                } else if (code == TB_ENDTRACK || code == TB_THUMBPOSITION) {
                    state->playbackSeeking = false;
                    if (state->player) {
                        state->player->SeekTo(targetPos);
                    }
                }
                return 0;
            }
            if (target == state->playbackVolume) {
                const int pos = static_cast<int>(SendMessageW(state->playbackVolume, TBM_GETPOS, 0, 0));
                state->playbackVolumeValue = static_cast<float>(pos) / 100.0f;
                if (state->player) {
                    state->player->SetVolume(state->playbackVolumeValue);
                }
                return 0;
            }
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
        UpdateControlsForState(state);
        UpdateStatusText(state);
        return 0;
    case WM_APP_DEVICE_NAME:
        if (state) {
            auto payload = reinterpret_cast<std::wstring*>(lParam);
            if (payload) {
                state->currentDeviceName = *payload;
                delete payload;
            }
        }
        UpdateStatusDetails(state);
        return 0;
    case WM_APP_PLAYBACK_STATE:
        if (state) {
            state->playbackState = static_cast<PlaybackState>(wParam);
            if (state->playbackState == PlaybackState::Stopped ||
                state->playbackState == PlaybackState::Ended) {
                UpdatePlaybackTime(state, 0);
            }
            UpdateControlsForState(state);
        }
        return 0;
    case WM_APP_PLAYBACK_OPENED:
        if (state) {
            state->playbackDuration100ns = static_cast<int64_t>(lParam);
            UpdatePlaybackTime(state, 0);
            UpdateControlsForState(state);
        }
        return 0;
    case WM_APP_PLAYBACK_ENDED:
        if (state) {
            UpdatePlaybackControls(state);
        }
        return 0;
    case WM_APP_PLAYBACK_ERROR:
        if (state) {
            auto payload = reinterpret_cast<std::wstring*>(lParam);
            if (payload) {
                AppendLog(state->logEdit, L"[播放] " + *payload);
                delete payload;
            }
            UpdateControlsForState(state);
        }
        return 0;
    case WM_DRAWITEM:
        if (state && wParam == IDC_START_BUTTON) {
            auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (!dis) {
                return TRUE;
            }
            const bool disabled = (dis->itemState & ODS_DISABLED) != 0;
            const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
            COLORREF fill = state->recordColor;
            if (state->state == AppState::RecorderState::Recording ||
                state->state == AppState::RecorderState::Recovering) {
                fill = state->paused ? state->pauseColor : state->primaryColor;
            }
            if (disabled) {
                fill = RGB(0x2A, 0x30, 0x36);
            }
            if (pressed && !disabled) {
                fill = AdjustColor(fill, -24);
            }
            HBRUSH fillBrush = CreateSolidBrush(fill);
            FillRect(dis->hDC, &dis->rcItem, fillBrush);
            DeleteObject(fillBrush);

            HBRUSH frameBrush = CreateSolidBrush(state->borderColor);
            FrameRect(dis->hDC, &dis->rcItem, frameBrush);
            DeleteObject(frameBrush);

            if (pressed) {
                OffsetRect(&dis->rcItem, 1, 1);
            }

            SetBkMode(dis->hDC, TRANSPARENT);
            const bool useLightText = !disabled &&
                (fill == state->recordColor || fill == state->primaryColor || fill == state->pauseColor);
            SetTextColor(dis->hDC, useLightText ? RGB(0xFF, 0xFF, 0xFF) : state->textPrimary);
            HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(dis->hDC, state->uiFontBold));
            std::wstring label = GetWindowTextString(state->startButton);
            DrawTextW(dis->hDC, label.c_str(), static_cast<int>(label.size()),
                      &dis->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dis->hDC, oldFont);

            if (dis->itemState & ODS_FOCUS) {
                DrawFocusRect(dis->hDC, &dis->rcItem);
            }
            return TRUE;
        }
        break;
    case WM_CTLCOLORSTATIC:
        if (state) {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            HWND target = reinterpret_cast<HWND>(lParam);
            if (target == state->headerLabel && state->headerBrush) {
                SetTextColor(hdc, state->textPrimary);
                SetBkColor(hdc, state->headerColor);
                return reinterpret_cast<INT_PTR>(state->headerBrush);
            }
            if (target == state->statusStateLabel) {
                COLORREF statusColor = state->textSecondary;
                if (state->state == AppState::RecorderState::Recording ||
                    state->state == AppState::RecorderState::Recovering) {
                    statusColor = state->paused ? state->pauseColor : state->recordColor;
                }
                SetTextColor(hdc, statusColor);
            } else if (target == state->statusTimeLabel) {
                SetTextColor(hdc, state->textPrimary);
            } else if (target == state->statusMetaLabel || target == state->playbackTimeLabel) {
                SetTextColor(hdc, state->textSecondary);
            } else if (target == state->statusBar) {
                SetTextColor(hdc, state->textSecondary);
            } else {
                SetTextColor(hdc, state->textTertiary);
            }
            const bool usePanelBackground = (target == state->statusStateLabel ||
                                              target == state->statusTimeLabel ||
                                              target == state->statusMetaLabel ||
                                              target == state->outputLabel ||
                                              target == state->formatLabel ||
                                              target == state->bitrateLabel ||
                                              target == state->playbackTimeLabel ||
                                              target == state->playbackVolumeLabel);
            SetBkMode(hdc, TRANSPARENT);
            if (usePanelBackground && state->panelBrush) {
                SetBkColor(hdc, state->panelColor);
                return reinterpret_cast<INT_PTR>(state->panelBrush);
            }
            SetBkColor(hdc, state->backgroundColor);
            if (state->backgroundBrush) {
                return reinterpret_cast<INT_PTR>(state->backgroundBrush);
            }
        }
        break;
    case WM_CTLCOLOREDIT:
        if (state) {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            HWND target = reinterpret_cast<HWND>(lParam);
            if (target == state->logEdit) {
                SetTextColor(hdc, state->textSecondary);
            } else {
                SetTextColor(hdc, state->textPrimary);
            }
            SetBkColor(hdc, state->panelAltColor);
            return reinterpret_cast<INT_PTR>(state->panelAltBrush ? state->panelAltBrush : state->backgroundBrush);
        }
        break;
    case WM_CTLCOLORBTN:
        if (state) {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            HWND target = reinterpret_cast<HWND>(lParam);
            SetTextColor(hdc, state->textSecondary);
            if (target == state->statusGroup ||
                target == state->actionGroup ||
                target == state->settingsGroup ||
                target == state->playbackGroup ||
                target == state->logGroup) {
                SetBkColor(hdc, state->panelColor);
                return reinterpret_cast<INT_PTR>(state->panelBrush ? state->panelBrush : state->backgroundBrush);
            }
            SetBkColor(hdc, state->backgroundColor);
            return reinterpret_cast<INT_PTR>(state->backgroundBrush);
        }
        break;
    case WM_SIZE:
        if (state && state->statusBar) {
            SendMessageW(state->statusBar, WM_SIZE, 0, 0);
            UpdateStatusBarLayout(state);
            UpdateStatusDetails(state);
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
            if (wParam == 1) {
                UpdateStatusText(state);
            } else if (wParam == 2 && state->player) {
                const bool shouldUpdate = state->playbackState == PlaybackState::Playing ||
                    state->playbackState == PlaybackState::Paused;
                if (shouldUpdate) {
                    const int64_t position = state->player->GetPosition100ns();
                    UpdatePlaybackTime(state, position);
                }
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
            if (state->player) {
                state->player->Shutdown();
                delete state->player;
                state->player = nullptr;
            }
            if (state->playbackListener) {
                delete state->playbackListener;
                state->playbackListener = nullptr;
            }
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
            if (state->panelBrush) {
                DeleteObject(state->panelBrush);
                state->panelBrush = nullptr;
            }
            if (state->panelAltBrush) {
                DeleteObject(state->panelAltBrush);
                state->panelAltBrush = nullptr;
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
            if (state->uiFontSecondary) {
                DeleteObject(state->uiFontSecondary);
                state->uiFontSecondary = nullptr;
            }
            if (state->uiFontTimer) {
                DeleteObject(state->uiFontTimer);
                state->uiFontTimer = nullptr;
            }
            delete state;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        KillTimer(hwnd, 1);
        KillTimer(hwnd, 2);
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK AboutWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    AboutDialogState* aboutState = reinterpret_cast<AboutDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto* create = reinterpret_cast<LPCREATESTRUCTW>(lParam);
        aboutState = reinterpret_cast<AboutDialogState*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(aboutState));

        const int padding = 16;
        const int qrSize = 150;
        const int width = 600;
        const int height = 420;
        const int textWidth = 320;
        const int qrX = padding + textWidth + 8;

        std::wstring aboutText =
            L"系统录音工具（Loopback Recorder GUI）\r\n"
            L"  版本：v0.1.0\r\n"
            L"  作用：基于 WASAPI Loopback 录制系统正在播放的音频，支持 WAV/MP3 输出、暂停/继续与回放检查。\r\n"
            L"  作者：suspark\r\n"
            L"\r\n"
            L"交流与更新：\r\n"
            L"  微信公众号（问题反馈）：边跑步边读书\r\n"
            L"\r\n"
            L"隐私与安全：\r\n"
            L"  无驱动、无后台、不采集隐私数据；录音文件仅保存在本地。\r\n"
            L"\r\n"
            L"MP3 编码库：\r\n"
            L"  libmp3lame.dll（或 lame_enc.dll）请与程序同目录。\r\n"
            L"\r\n"
            L"项目主页：\r\n"
            L"  https://github.com/suyaoyong/recorder\r\n"
            L"\r\n"
            L"免责声明：\r\n"
            L"  本工具仅用于用户授权的音频录制，请遵守当地法律法规。";
        HWND text = CreateWindowW(L"STATIC", aboutText.c_str(),
                                  WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
                                  padding, padding, textWidth, height - padding * 3 - 36,
                                  hwnd, nullptr, nullptr, nullptr);

        HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        if (text && font) {
            SendMessageW(text, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        }

        aboutState->qrBitmap = LoadQrBitmap();
        if (aboutState->qrBitmap) {
            HWND qr = CreateWindowW(L"STATIC", nullptr,
                                    WS_CHILD | WS_VISIBLE | SS_BITMAP,
                                    qrX, padding + 10,
                                    qrSize, qrSize, hwnd, nullptr, nullptr, nullptr);
            SendMessageW(qr, STM_SETIMAGE, IMAGE_BITMAP, reinterpret_cast<LPARAM>(aboutState->qrBitmap));
        } else {
            std::wstring placeholder = L"二维码未找到\r\n请放置 wechat_qr.png\r\n到 assets 目录";
            HWND qrText = CreateWindowW(L"STATIC", placeholder.c_str(),
                                        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
                                        qrX, padding + 10,
                                        qrSize, qrSize, hwnd, nullptr, nullptr, nullptr);
            if (qrText && font) {
                SendMessageW(qrText, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            }
        }

        HWND okButton = CreateWindowW(L"BUTTON", L"确定",
                                      WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                                      (width - 88) / 2, height - padding - 30,
                                      88, 30, hwnd, reinterpret_cast<HMENU>(IDOK), nullptr, nullptr);
        if (okButton && font) {
            SendMessageW(okButton, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        }
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (aboutState) {
            if (aboutState->qrBitmap) {
                DeleteObject(aboutState->qrBitmap);
                aboutState->qrBitmap = nullptr;
            }
            if (aboutState->parent) {
                EnableWindow(aboutState->parent, TRUE);
                SetForegroundWindow(aboutState->parent);
            }
            delete aboutState;
        }
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

    Gdiplus::GdiplusStartupInput gdiplusInput{};
    ULONG_PTR gdiplusToken = 0;
    if (Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusInput, nullptr) != Gdiplus::Ok) {
        gdiplusToken = 0;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = MainWindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPWSTR>(IDC_ARROW));
    wc.hIcon = LoadIconW(nullptr, reinterpret_cast<LPWSTR>(IDI_APPLICATION));
    wc.lpszClassName = kClassName;
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, kClassName, L"系统录音工具",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, 860, 540,
                                nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) {
        return 0;
    }
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    ACCEL accels[] = {
        { FCONTROL | FVIRTKEY, 'N', IDM_FILE_NEW },
        { FCONTROL | FVIRTKEY, 'R', IDM_RECORD_START_STOP },
        { FCONTROL | FVIRTKEY, 'P', IDM_RECORD_PAUSE },
        { FVIRTKEY, VK_SPACE, IDM_PLAYBACK_TOGGLE },
        { FCONTROL | FALT | FVIRTKEY, 'P', IDM_PLAYBACK_PAUSE },
        { FCONTROL | FVIRTKEY, VK_SPACE, IDM_PLAYBACK_STOP },
        { FCONTROL | FVIRTKEY, 'L', IDM_VIEW_CLEAR_LOG },
        { FVIRTKEY, VK_F1, IDM_HELP_ABOUT }
    };
    HACCEL accelTable = CreateAcceleratorTableW(accels, static_cast<int>(sizeof(accels) / sizeof(accels[0])));

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (!TranslateAcceleratorW(hwnd, accelTable, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (accelTable) {
        DestroyAcceleratorTable(accelTable);
    }
    if (gdiplusToken != 0) {
        Gdiplus::GdiplusShutdown(gdiplusToken);
    }
    return static_cast<int>(msg.wParam);
}
