#include "instance.h"
#include "startup.h"
#include "tray.h"
#include "svg_icons.h"
#include "resource.h"
#include "version.h"
#include "theme_window.h"
#include "menu_theme.h"
#include "log_view.h"
#include "text_view.h"
#include "xray_process.h"
#include "settings_dialog.h"
#include "xray_tool_window.h"
#include "win32.h"
#include "ui_layout.h"

#include <commdlg.h>
#include <objbase.h>
#include <richedit.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <vsstyle.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <exception>
#include <iterator>
#include <memory>
#include <string_view>

#if !defined(_M_X64)
#error XTray supports Windows x64 only.
#endif

namespace {

using namespace xtray;

constexpr UINT kExitMessage = WM_APP + 43;
constexpr UINT kRuntimeFailureTestMessage = WM_APP + 49;
constexpr DWORD kWindowStyle = WS_OVERLAPPEDWINDOW;
enum ControlId { Run = 2001, Stop, Restart, SettingsButton, FileList, OpenFile,
    OpenFileDirectory, OpenDirectory, Filter, ClearLogs, ScrollToEnd, ShowGui, ExitApp, CheckConfig, PreviewConfig, ShowVersion, CopyLogs, WordWrap, ShowRunInfo };
enum IconId { StoppedTrayIcon, RunningTrayIcon, StoppedStatusIcon, RunningStatusIcon,
    SmallWindowIcon, LargeWindowIcon, IconCount };

NOTIFYICONDATAW smokeTrayData{};
bool smokeTrayAvailable = true;
BOOL WINAPI SmokeNotifyIcon(DWORD operation, PNOTIFYICONDATAW data)
{
    if (operation == NIM_ADD || operation == NIM_MODIFY) {
        smokeTrayData = *data;
    }
    return smokeTrayAvailable;
}

std::wstring MainWindowTitle(const std::filesystem::path& root)
{
    return std::wstring(kWindowTitle) + L" (" + root.native() + L")";
}

std::wstring XrayStatusText(ProcessState state, DWORD pid)
{
    std::wstring result = L"Xray";
    if (pid != 0) {
        result += L"(PID " + std::to_wstring(pid) + L")";
    }
    const auto* status = state == ProcessState::Starting ? Text(L"Starting...", L"启动中…")
        : state == ProcessState::Stopping ? Text(L"Stopping...", L"停止中…")
        : pid != 0 ? Text(L"Running", L"运行中") : Text(L"Stopped", L"已停止");
    return result + L" " + status;
}

std::wstring TrayTooltipText(const std::filesystem::path& root, ProcessState state, DWORD pid)
{
    const std::wstring first = std::wstring(kWindowTitle) + L"\n";
    const std::wstring last = L"\n" + XrayStatusText(state, pid);
    const size_t capacity = std::size(NOTIFYICONDATAW{}.szTip) - 1;
    const size_t pathLimit = capacity - first.size() - last.size();
    std::wstring directory = root.native();
    if (directory.size() > pathLimit) {
        // Keep the version and status intact within the native tooltip's limit.
        size_t prefix = (pathLimit - 1) / 2;
        size_t suffix = directory.size() - (pathLimit - 1 - prefix);
        if (prefix > 0 && directory[prefix - 1] >= 0xD800 && directory[prefix - 1] <= 0xDBFF) {
            --prefix;
        }
        if (suffix < directory.size() && directory[suffix] >= 0xDC00 && directory[suffix] <= 0xDFFF) {
            ++suffix;
        }
        directory = directory.substr(0, prefix) + L"…" + directory.substr(suffix);
    }
    return first + directory + last;
}

int ReportStartupError(bool smokeTest, const std::wstring& error)
{
    if (smokeTest) {
        std::fwprintf(stderr, L"%ls\n", error.c_str());
    } else {
        // Failed creation/teardown can leave WM_QUIT queued before the error dialog.
        MSG pending{};
        while (PeekMessageW(&pending, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE)) {}
        MessageBoxW(nullptr, error.c_str(), L"XTray", MB_OK | MB_ICONERROR);
    }
    return 1;
}

template<size_t Count>
struct IconBatch {
    std::array<HICON, Count> values{};
    ~IconBatch()
    {
        for (const auto icon : values) {
            if (icon != nullptr) {
                DestroyIcon(icon);
            }
        }
    }
    std::array<HICON, Count> Release()
    {
        const auto result = values;
        values.fill(nullptr);
        return result;
    }
};

bool IconMatches(HICON icon, COLORREF expected, int size)
{
    struct Bitmaps {
        ICONINFO info{};
        ~Bitmaps()
        {
            if (info.hbmColor != nullptr) { DeleteObject(info.hbmColor); }
            if (info.hbmMask != nullptr) { DeleteObject(info.hbmMask); }
        }
    } bitmaps;
    if (icon == nullptr || !GetIconInfo(icon, &bitmaps.info)) {
        return false;
    }
    BITMAP description{};
    if (GetObjectW(bitmaps.info.hbmColor, sizeof(description), &description) == 0 ||
        description.bmWidth != size || description.bmHeight != size) {
        return false;
    }
    BITMAPINFO format{};
    format.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    format.bmiHeader.biWidth = size;
    format.bmiHeader.biHeight = -size;
    format.bmiHeader.biPlanes = 1;
    format.bmiHeader.biBitCount = 32;
    std::vector<BYTE> pixels(static_cast<size_t>(size) * static_cast<size_t>(size) * 4);
    const HDC device = CreateCompatibleDC(nullptr);
    if (device == nullptr) {
        return false;
    }
    const int rows = GetDIBits(device, bitmaps.info.hbmColor, 0, static_cast<UINT>(size),
        pixels.data(), &format, DIB_RGB_COLORS);
    DeleteDC(device);
    if (rows != size || pixels[3] != 0) {
        return false;
    }
    size_t visible = 0;
    for (size_t index = 0; index < pixels.size(); index += 4) {
        if (pixels[index + 3] == 255) {
            if (pixels[index] != GetBValue(expected) || pixels[index + 1] != GetGValue(expected) ||
                pixels[index + 2] != GetRValue(expected)) {
                return false;
            }
            ++visible;
        }
    }
    return visible != 0;
}

class Application {
public:
    Application(std::filesystem::path root, bool smokeTest)
        : root_(std::move(root)), startup_(root_, ExecutablePath()),
          process_(root_, logs_), toolWindow_(root_), runInfoWindow_(root_), smokeTest_(smokeTest),
          tray_(smokeTest ? SmokeNotifyIcon : Shell_NotifyIconW) {}
    ~Application()
    {
        themeMonitor_.reset();
        toolWindow_.Close();
        runInfoWindow_.Close();
        process_.Shutdown();
        tray_.Remove();
        if (IsWindow(window_)) {
            DestroyWindow(window_);
        }
        if (font_ != nullptr) {
            DeleteObject(font_);
        }
        if (logFont_ != nullptr) {
            DeleteObject(logFont_);
        }
        for (const auto icon : icons_) {
            if (icon != nullptr) {
                DestroyIcon(icon);
            }
        }
        for (const auto icon : toolIcons_) {
            if (icon != nullptr) {
                DestroyIcon(icon);
            }
        }
    }

    bool Initialize(HWND window)
    {
        INITCOMMONCONTROLSEX controls{sizeof(INITCOMMONCONTROLSEX), ICC_WIN95_CLASSES};
        if (!InitCommonControlsEx(&controls)) {
            throw std::runtime_error(Text("Cannot initialize Windows controls.", "无法初始化 Windows 控件。"));
        }
        window_ = window;
        theme_.Attach(window_);
        taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
        if (taskbarCreated_ == 0) {
            throw std::runtime_error(Text("Cannot register the taskbar restart message.",
                "无法注册任务栏重建消息。"));
        }
        if (smokeTest_) {
            std::fwprintf(stderr, L"Smoke: creating controls.\n");
        }
        dpi_ = GetDpiForWindow(window);
        const auto child = [this](DWORD extended, const wchar_t* type, const wchar_t* title,
            DWORD style, int id = 0) {
            const HWND control = CreateWindowExW(extended, type, title, WS_CHILD | WS_VISIBLE | style,
                0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                GetModuleHandleW(nullptr), nullptr);
            if (control == nullptr) {
                throw std::runtime_error(Text("Cannot create a window control.", "无法创建窗口控件。"));
            }
            return control;
        };
        statusIcon_ = child(0, L"STATIC", L"", SS_ICON | SS_CENTERIMAGE);
        status_ = child(0, L"STATIC", L"", SS_CENTERIMAGE);
        run_ = child(0, L"BUTTON", L"", WS_TABSTOP, Run);
        stop_ = child(0, L"BUTTON", L"", WS_TABSTOP, Stop);
        restart_ = child(0, L"BUTTON", L"", WS_TABSTOP, Restart);
        fileLabel_ = child(0, L"STATIC", L"", SS_CENTERIMAGE);
        files_ = child(0, L"COMBOBOX", L"", CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP, FileList);
        openFile_ = child(0, L"BUTTON", L"", WS_TABSTOP, OpenFile);
        openFileDirectory_ = child(0, L"BUTTON", L"", WS_TABSTOP, OpenFileDirectory);
        directoryLabel_ = child(0, L"STATIC", L"", SS_CENTERIMAGE);
        directory_ = child(WS_EX_CLIENTEDGE, L"STATIC", L"",
            SS_LEFTNOWORDWRAP | SS_CENTERIMAGE | SS_NOPREFIX | SS_NOTIFY);
        openDirectory_ = child(0, L"BUTTON", L"", WS_TABSTOP, OpenDirectory);
        filterLabel_ = child(0, L"STATIC", L"", SS_CENTERIMAGE);
        filter_ = child(0, L"COMBOBOX", L"", CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP, Filter);
        copyLogs_ = child(0, L"BUTTON", L"", WS_TABSTOP | BS_ICON, CopyLogs);
        clearLogs_ = child(0, L"BUTTON", L"", WS_TABSTOP | BS_ICON, ClearLogs);
        scrollToEnd_ = child(0, L"BUTTON", L"", WS_TABSTOP | BS_ICON | BS_CHECKBOX | BS_PUSHLIKE, ScrollToEnd);
        wordWrap_ = child(0, L"BUTTON", L"", WS_TABSTOP | BS_ICON | BS_AUTOCHECKBOX | BS_PUSHLIKE, WordWrap);
        CreateTooltips();
        logWindow_ = child(WS_EX_CLIENTEDGE, MSFTEDIT_CLASS, L"",
            ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_NOHIDESEL | ES_DISABLENOSCROLL |
            WS_VSCROLL | WS_TABSTOP);
        view_ = std::make_unique<LogView>(logWindow_, logs_);
        if (smokeTest_) {
            std::fwprintf(stderr, L"Smoke: configuring fonts and icons.\n");
        }
        SetFontsAndIcons();
        CreateMenus();
        if (!smokeTest_) {
            const auto loaded = LoadSettings(root_);
            settings_ = loaded.settings;
            settingsValid_ = loaded.success;
            if (!loaded.success) {
                logs_.Tray(loaded.error);
            }
        }
        if (smokeTest_) {
            settings_.theme = ThemeMode::Light;
        }
        view_->SetWordWrap(settings_.logWordWrap);
        logWindow_ = view_->Window();
        ApplyLanguage(settings_.language);
        ApplyUiText();
        themeMonitor_ = std::make_unique<ThemeMonitor>(window_);
        UpdateTheme(themeMonitor_->SystemDark(), true);
        logs_.SetLimit(settings_.logLines);
        logs_.Tray(settingsValid_
            ? Text(L"XTray is ready. Configure paths in File > Settings, then click Run to start Xray.", L"XTray 已就绪；打开“文件 → 设置”填写路径，再点击“运行”启动 Xray。")
            : Text(L"Settings could not be loaded. Re-enter and save them in Settings before running Xray.", L"设置尚未成功加载，当前不能运行 Xray；请在“设置”中重新填写并保存。"));
        UpdatePaths();
        Layout();
        ready_ = true;
        Refresh();
        if (SetTimer(window_, 1, 100, nullptr) == 0) {
            throw std::runtime_error(Text("Cannot start the UI refresh timer.", "无法启动界面刷新计时器。"));
        }
        return true;
    }

    void Layout()
    {
        if (window_ == nullptr || IsIconic(window_)) {
            return;
        }
        RECT client{};
        GetClientRect(window_, &client);
        const int width = static_cast<int>(client.right);
        const int margin = Scale(16), gap = Scale(8);
        const auto move = [](HWND control, int x, int y, int widthValue, int height) {
            MoveWindow(control, x, y, (std::max)(1, widthValue), height, TRUE);
        };
        int actionX = width - margin;
        for (const auto control : {restart_, stop_, run_}) {
            const int buttonWidth = TextButtonWidth(control);
            actionX -= buttonWidth;
            move(control, actionX, Scale(16), buttonWidth, Scale(28));
            actionX -= gap;
        }
        move(statusIcon_, margin, Scale(16), Scale(24), Scale(28));
        move(status_, Scale(48), Scale(16), actionX - Scale(48), Scale(28));
        const int labelWidth = (std::max)({ControlTextSize(fileLabel_).cx,
            ControlTextSize(directoryLabel_).cx, ControlTextSize(filterLabel_).cx});
        const int inputX = margin + labelWidth + gap;
        const int folderWidth = TextButtonWidth(openFileDirectory_);
        const int fileWidth = TextButtonWidth(openFile_);
        const int folderX = width - margin - folderWidth;
        const int fileX = folderX - gap - fileWidth;
        move(fileLabel_, margin, Scale(60), labelWidth, Scale(26));
        move(files_, inputX, Scale(60) + (Scale(26) - ComboBoxHeight(files_)) / 2,
            fileX - gap - inputX, Scale(240));
        move(openFile_, fileX, Scale(60), fileWidth, Scale(26));
        move(openFileDirectory_, folderX, Scale(60), folderWidth, Scale(26));
        const int directoryWidth = TextButtonWidth(openDirectory_);
        const int directoryX = width - margin - directoryWidth;
        move(directoryLabel_, margin, Scale(98), labelWidth, Scale(26));
        const int directoryHeight = SingleLineControlHeight(directory_);
        move(directory_, inputX, Scale(98) + (Scale(26) - directoryHeight) / 2,
            directoryX - gap - inputX, directoryHeight);
        move(openDirectory_, directoryX, Scale(98), directoryWidth, Scale(26));
        const int filterY = Scale(134), filterHeight = Scale(32);
        move(filterLabel_, margin, filterY + (filterHeight - Scale(26)) / 2, labelWidth, Scale(26));
        move(filter_, inputX, filterY + (filterHeight - ComboBoxHeight(filter_)) / 2, Scale(136), Scale(180));
        const int iconButton = Scale(32);
        int toolX = width - margin - iconButton * 4 - gap * 3;
        for (const auto control : {copyLogs_, clearLogs_, scrollToEnd_, wordWrap_}) {
            move(control, toolX, filterY, iconButton, filterHeight);
            toolX += iconButton + gap;
        }
        move(logWindow_, margin, Scale(174), width - margin * 2,
            static_cast<int>(client.bottom) - Scale(190));
    }

    void DpiChanged(UINT dpi)
    {
        dpi_ = dpi;
        if (ready_) {
            SetFontsAndIcons();
        }
    }

    void Refresh()
    {
        if (!ready_ || updatingUi_) {
            return;
        }
        const auto state = process_.State();
        const DWORD pid = process_.Pid();
        const bool running = pid != 0;
        if (state != shownState_ || pid != shownPid_) {
            const auto status = XrayStatusText(state, pid);
            SetWindowTextW(status_, status.c_str());
            EnableWindow(run_, state == ProcessState::Stopped);
            EnableWindow(stop_, state == ProcessState::Running);
            EnableWindow(restart_, state == ProcessState::Running);
            SendMessageW(statusIcon_, STM_SETICON,
                reinterpret_cast<WPARAM>(icons_[running ? RunningStatusIcon : StoppedStatusIcon]), 0);
            shownState_ = state;
            shownPid_ = pid;
            UpdateTray();
        } else if (!tray_.IsAdded() && GetTickCount64() >= nextTrayAttempt_) {
            UpdateTray();
        }
        view_->Refresh();
        UpdateLogButtonState();
        if (failureSmokePending_ && process_.Pid() != 0) {
            failureSmokePending_ = false;
            std::fwprintf(stderr, L"Smoke: runtime failure child PID %lu.\n", process_.Pid());
            if (!PostMessageW(window_, kRuntimeFailureTestMessage, 0, 0)) {
                throw std::runtime_error("Cannot post the runtime-failure smoke message.");
            }
        }
    }

    void Command(int id, int notification)
    {
        const bool button = notification == BN_CLICKED &&
            (id == Run || id == Stop || id == Restart || id == SettingsButton ||
                id == OpenFile || id == OpenFileDirectory || id == OpenDirectory ||
                id == CopyLogs || id == WordWrap || id == ClearLogs || id == ScrollToEnd || id == ExitApp ||
                id == CheckConfig || id == PreviewConfig || id == ShowVersion || id == ShowRunInfo);
        const bool selection = notification == CBN_SELCHANGE && (id == FileList || id == Filter);
        // Native controls notify their parent during creation and programmatic updates.
        if (!ready_ || updatingUi_ || (!button && !selection)) {
            return;
        }
        if (id == ExitApp) {
            Exit();
            return;
        }
        if (id == ShowRunInfo) {
            try {
                runInfoWindow_.OpenRunInfo(window_, process_, theme_.Colors().dark, smokeTest_);
            } catch (const std::exception& exception) {
                runInfoWindow_.Close();
                if (smokeTest_) { throw; }
                const auto error = FromUtf8(exception.what());
                MessageBoxW(window_, error.c_str(), Text(L"Xray run information", L"Xray 运行信息"), MB_OK | MB_ICONERROR);
            }
        } else if (id == CheckConfig || id == PreviewConfig || id == ShowVersion) {
            const auto kind = id == CheckConfig ? XrayToolKind::Check
                : id == PreviewConfig ? XrayToolKind::Preview : XrayToolKind::Version;
            try {
                toolWindow_.Open(window_, kind, settings_, settingsValid_, theme_.Colors().dark, smokeTest_);
            } catch (const std::exception& exception) {
                toolWindow_.Close();
                if (smokeTest_) { throw; }
                const auto error = FromUtf8(exception.what());
                MessageBoxW(window_, error.c_str(), Text(L"Xray tool", L"Xray 工具"), MB_OK | MB_ICONERROR);
            }
        } else if (id == SettingsButton) {
            if (EditSettings(window_, root_, settings_, logs_, startup_, &toolWindow_)) {
                settingsValid_ = true;
                ApplyUiText();
                UpdateTheme(themeMonitor_->SystemDark());
                UpdatePaths();
            }
        } else if (id == CopyLogs) {
            try {
                CopyTextToClipboard(window_, view_->CopyText());
            } catch (const std::exception& exception) {
                if (smokeTest_) { throw; }
                const auto error = FromUtf8(exception.what());
                MessageBoxW(window_, error.c_str(), Text(L"Copy all", L"复制全部"), MB_OK | MB_ICONERROR);
            }
        } else if (id == WordWrap) {
            SetLogWordWrap(SendMessageW(wordWrap_, BM_GETCHECK, 0, 0) == BST_CHECKED);
        } else if (id == ClearLogs || id == ScrollToEnd) {
            SendMessageW(tooltip_, TTM_POP, 0, 0);
            if (id == ClearLogs) {
                view_->Clear();
            } else {
                view_->ScrollToEnd();
            }
        } else if (id == Filter && notification == CBN_SELCHANGE) {
            view_->SetFilter(static_cast<int>(SendMessageW(filter_, CB_GETCURSEL, 0, 0)));
        } else if (id == FileList && notification == CBN_SELCHANGE) {
            UpdateOpenButtons();
        } else if (id == OpenDirectory) {
            OpenPath(settings_.configDirectory, true);
        } else if (id == OpenFile || id == OpenFileDirectory) {
            const auto selected = SendMessageW(files_, CB_GETCURSEL, 0, 0);
            if (selected >= 0 && static_cast<size_t>(selected) < settings_.configFiles.size()) {
                auto path = ResolvePath(root_, settings_.configFiles[static_cast<size_t>(selected)]);
                if (id == OpenFileDirectory) {
                    path = path.parent_path();
                }
                OpenPath(path.native(), id == OpenFileDirectory);
            }
        } else if (id == Stop) {
            process_.Stop();
        } else if (id == Run || id == Restart) {
            if (!settingsValid_) {
                logs_.Tray(Text(L"Saved settings could not be read. Re-enter and save them in Settings.", L"尚未成功读取已保存设置，请打开“设置”重新填写并保存。"));
            } else if (id == Run) {
                process_.Run(settings_);
            } else {
                process_.Restart(settings_);
            }
        }
        Refresh();
    }

    bool ThemeMessage(UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result)
    {
        if (message == WM_ENTERMENULOOP) {
            menuActive_ = true;
        } else if (message == WM_EXITMENULOOP) {
            menuActive_ = false;
            if (std::exchange(menuThemePending_, false)) {
                PostMessageW(window_, kSystemThemeMessage, 0, 0);
            }
        }
        return menuTheme_.Handle(message, wParam, lParam, result) ||
            theme_.Handle(message, wParam, lParam, result);
    }

    void SystemThemeChanged()
    {
        if (ready_ && !smokeTest_ && themeMonitor_) {
            UpdateTheme(themeMonitor_->SystemDark());
        }
    }

    void ShowMain()
    {
        if (exiting_ || window_ == nullptr) {
            return;
        }
        ShowWindow(window_, IsIconic(window_) ? SW_RESTORE
            : smokeTest_ ? SW_SHOWNOACTIVATE : SW_SHOW);
        if (!smokeTest_) {
            HWND target = window_;
            for (;;) {
                const HWND popup = GetLastActivePopup(target);
                if (popup == target || popup == nullptr || !IsWindowVisible(popup)) {
                    break;
                }
                target = popup;
            }
            SetForegroundWindow(target);
        }
    }

    void StartFromLogon()
    {
        if (!ready_ || exiting_ || logonStarted_) {
            return;
        }
        logonStarted_ = true;
        HideMain();
        logs_.Tray(Text(L"Sign-in startup: starting Xray using the saved settings.",
            L"登录自启：按已保存的设置启动 Xray。"));
        Command(Run, BN_CLICKED);
    }

    void HideMain()
    {
        if (!ready_ || exiting_) {
            return;
        }
        if (!tray_.IsAdded()) {
            UpdateTray();
        }
        if (tray_.IsAdded()) {
            SendMessageW(tooltip_, TTM_POP, 0, 0);
            ShowWindow(window_, SW_HIDE);
        }
    }

    void Exit()
    {
        if (exiting_) {
            return;
        }
        exiting_ = true;
        KillTimer(window_, 1);
        tray_.Remove();
        toolWindow_.Close();
        runInfoWindow_.Close();
        process_.Shutdown();
        DestroyWindow(window_);
    }

    void Destroyed()
    {
        ready_ = false;
        if (IsWindow(tooltip_)) {
            DestroyWindow(tooltip_);
        }
        tooltip_ = nullptr;
        themeMonitor_.reset();
        menuTheme_.Detach();
        theme_.Detach();
        tray_.Remove();
        window_ = nullptr;
    }

    UINT TaskbarCreatedMessage() const { return taskbarCreated_; }

    void TaskbarCreated()
    {
        tray_.Lost();
        if (ready_ && !exiting_) {
            UpdateTray();
        }
    }

    void TrayEvent(WPARAM, LPARAM event)
    {
        if (!ready_ || exiting_ || HIWORD(event) != kTrayIconId) {
            return;
        }
        switch (LOWORD(event)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
            ShowMain();
            break;
        case WM_CONTEXTMENU:
            ShowTrayMenu();
            break;
        }
    }

    void TrayCommand(int id)
    {
        if (id == ShowGui) {
            ShowMain();
        } else if (IsWindowEnabled(window_) && !exiting_) {
            if (id == ExitApp) {
                Exit();
            } else if (id == Run || id == Stop || id == Restart) {
                Command(id, BN_CLICKED);
            }
        }
    }

    void StartFailureSmokeTest()
    {
        if (!smokeTest_) { return; }
        settings_.executable = (root_ / L"XTray.Tests.exe").native();
        settings_.configFiles = {L".\\stream.conf"};
        failureSmokePending_ = true;
        Command(Run, BN_CLICKED);
    }

    void RaiseFailureForSmokeTest() const
    {
        if (smokeTest_) {
            throw std::runtime_error("Injected window callback failure.");
        }
    }

    bool TranslateToolMessage(MSG& message) const
    {
        return toolWindow_.Translate(message) || runInfoWindow_.Translate(message);
    }

    bool IsExiting() const { return exiting_; }
    std::exception_ptr runtimeError;
    int exitCode = 0;
    std::exception_ptr startupError;

    bool SmokeTest()
    {
        std::fwprintf(stderr, L"Smoke: UTF-8 and incremental rendering.\n");
        logs_.Xray("test \xE4\xB8");
        view_->Refresh();
        logs_.Xray("\xAD\xE6\x96\x87\n");
        logs_.Tray(L"界面自检");
        view_->Refresh();
        auto text = WindowText(logWindow_);
        if (text.find(L"[XRAY] test 中文") == std::wstring::npos ||
            text.find(L"[XTRAY] 界面自检") == std::wstring::npos) {
            return false;
        }
        std::fwprintf(stderr, L"Smoke: source filters.\n");
        view_->SetFilter(1);
        text = WindowText(logWindow_);
        if (text.find(L"[XRAY]") != std::wstring::npos || text.find(L"[XTRAY]") == std::wstring::npos) {
            return false;
        }
        view_->SetFilter(2);
        text = WindowText(logWindow_);
        if (text.find(L"[XTRAY]") != std::wstring::npos || text.find(L"中文") == std::wstring::npos) {
            return false;
        }
        view_->SetFilter(0);
        std::fwprintf(stderr, L"Smoke: selection preservation.\n");
        CHARRANGE selected{0, 4};
        SendMessageW(logWindow_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&selected));
        logs_.Xray("next\n");
        view_->Refresh();
        CHARRANGE after{};
        SendMessageW(logWindow_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&after));
        if (after.cpMin != selected.cpMin || after.cpMax != selected.cpMax) {
            return false;
        }
        GETTEXTLENGTHEX length{GTL_PRECISE | GTL_NUMCHARS, 1200};
        const LONG oldLength = static_cast<LONG>(SendMessageW(logWindow_, EM_GETTEXTLENGTHEX,
            reinterpret_cast<WPARAM>(&length), 0));
        CHARRANGE throughEnd{0, oldLength};
        SendMessageW(logWindow_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&throughEnd));
        logs_.Xray("new line while selecting\n");
        view_->Refresh();
        SendMessageW(logWindow_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&after));
        if (after.cpMin != 0 || after.cpMax != oldLength) {
            return false;
        }
        std::fwprintf(stderr, L"Smoke: retained line limits.\n");
        logs_.SetLimit(1);
        logs_.Xray("last\n");
        logs_.Tray(L"最后一行");
        view_->Refresh();
        text = WindowText(logWindow_);
        if (text.find(L"[XRAY] last") == std::wstring::npos ||
            text.find(L"[XTRAY] 最后一行") == std::wstring::npos ||
            text.find(L"test 中文") != std::wstring::npos) {
            return false;
        }
        return SmokeTestIconResources() && SmokeTestInstanceText() && SmokeTestUiChanges() && SmokeTestLogButtons() &&
            SmokeTestThemes() && SmokeTestMenus() && SmokeTestXrayTools(window_, root_) &&
            SmokeTestLogon() && SmokeTestTray();
    }

private:
    void CreateMenus()
    {
        struct Menu {
            HMENU value;
            ~Menu() { if (value != nullptr) { DestroyMenu(value); } }
            HMENU Release() { return std::exchange(value, nullptr); }
        };
        Menu bar{CreateMenu()}, file{CreatePopupMenu()}, tools{CreatePopupMenu()};
        const auto require = [](bool success) {
            if (!success) { throw std::runtime_error("Cannot create the application menus."); }
        };
        require(bar.value != nullptr && file.value != nullptr && tools.value != nullptr);
        require(AppendMenuW(file.value, MF_STRING, SettingsButton, L"Settings") != FALSE);
        require(AppendMenuW(file.value, MF_SEPARATOR, 0, nullptr) != FALSE);
        require(AppendMenuW(file.value, MF_STRING, ExitApp, L"Exit") != FALSE);
        require(AppendMenuW(tools.value, MF_STRING, ShowRunInfo, L"Show run information") != FALSE);
        require(AppendMenuW(tools.value, MF_SEPARATOR, 0, nullptr) != FALSE);
        require(AppendMenuW(tools.value, MF_STRING, CheckConfig, L"Check saved configuration") != FALSE);
        require(AppendMenuW(tools.value, MF_STRING, PreviewConfig, L"Preview saved configuration") != FALSE);
        require(AppendMenuW(tools.value, MF_STRING, ShowVersion, L"Show Xray version") != FALSE);
        fileMenu_ = file.value;
        xrayMenu_ = tools.value;
        require(AppendMenuW(bar.value, MF_POPUP, reinterpret_cast<UINT_PTR>(file.value), L"File") != FALSE);
        file.Release();
        require(AppendMenuW(bar.value, MF_POPUP, reinterpret_cast<UINT_PTR>(tools.value), L"Xray Tools") != FALSE);
        tools.Release();
        require(SetMenu(window_, bar.value) != FALSE);
        Menu previous{std::exchange(menuBar_, bar.Release())};
        menuTheme_.Attach(window_, menuBar_);
        menuLanguage_ = CurrentLanguage();
        menuDark_ = theme_.Colors().dark;
        menuDpi_ = dpi_;
    }

    void UpdateMenus()
    {
        if (menuActive_) {
            menuThemePending_ = true;
            return;
        }
        // Windows also caches menu-wide widths and separator sizes after display.
        if (menuBar_ == nullptr || menuLanguage_ != CurrentLanguage() ||
            menuDark_ != theme_.Colors().dark || menuDpi_ != dpi_) {
            CreateMenus();
        }
        ModifyMenuW(menuBar_, 0, MF_BYPOSITION | MF_POPUP | MF_STRING,
            reinterpret_cast<UINT_PTR>(fileMenu_), Text(L"&File", L"文件"));
        ModifyMenuW(menuBar_, 1, MF_BYPOSITION | MF_POPUP | MF_STRING,
            reinterpret_cast<UINT_PTR>(xrayMenu_), Text(L"Xray &Tools", L"Xray 工具"));
        const std::array captions{
            std::pair{SettingsButton, Text(L"Settings", L"设置")},
            std::pair{ExitApp, Text(L"Exit", L"退出")},
            std::pair{ShowRunInfo, Text(L"Show run information", L"查看运行信息")},
            std::pair{CheckConfig, Text(L"Check saved configuration", L"检查已保存配置")},
            std::pair{PreviewConfig, Text(L"Preview saved configuration", L"预览已保存配置")},
            std::pair{ShowVersion, Text(L"Show Xray version", L"显示 Xray 版本")}
        };
        for (const auto& [id, caption] : captions) {
            MENUITEMINFOW item{sizeof(MENUITEMINFOW)};
            item.fMask = MIIM_STRING;
            item.dwTypeData = const_cast<wchar_t*>(caption);
            if (!SetMenuItemInfoW(menuBar_, id, FALSE, &item)) {
                throw std::runtime_error("Cannot update the application menu language.");
            }
        }
        menuTheme_.Apply(theme_.Colors());
    }

    bool SmokeTestMenus()
    {
        std::fwprintf(stderr, L"Smoke: File and Xray Tools menus, language refresh, light/dark transitions, and removed Settings button.\n");
        const auto language = CurrentLanguage();
        const auto appearance = settings_.theme;
        bool passed = GetMenu(window_) == menuBar_ && GetMenuItemCount(menuBar_) == 2 &&
            GetMenuItemCount(fileMenu_) == 3 && GetMenuItemCount(xrayMenu_) == 5 &&
            GetMenuItemID(xrayMenu_, 0) == ShowRunInfo &&
            (GetMenuState(xrayMenu_, 1, MF_BYPOSITION) & MF_SEPARATOR) != 0 &&
            GetMenuItemID(fileMenu_, 0) == SettingsButton && GetMenuItemID(fileMenu_, 2) == ExitApp &&
            GetDlgItem(window_, SettingsButton) == nullptr;
        for (const auto selected : {Language::English, Language::Chinese}) {
            ApplyLanguage(selected);
            for (const bool dark : {false, true, false}) {
                settings_.theme = dark ? ThemeMode::Dark : ThemeMode::Light;
                // Match saving Settings: refresh captions before applying the new appearance.
                ApplyUiText();
                UpdateTheme(false);
                wchar_t title[128]{};
                GetMenuStringW(xrayMenu_, PreviewConfig, title, static_cast<int>(std::size(title)), MF_BYCOMMAND);
                const bool caption = std::wstring_view(title) == Text(L"Preview saved configuration", L"预览已保存配置");
                if (!caption) {
                    std::fwprintf(stderr, L"Smoke: main menu language failed (dark=%d, caption=%ls).\n", dark, title);
                }
                passed = caption && passed;
                for (int position = 2; position < 5; ++position) {
                    MENUITEMINFOW item{sizeof(MENUITEMINFOW)};
                    item.fMask = MIIM_FTYPE | MIIM_ID;
                    passed = GetMenuItemInfoW(xrayMenu_, position, TRUE, &item) &&
                        ((item.fType & MFT_OWNERDRAW) != 0) == dark &&
                        item.wID == static_cast<UINT>(CheckConfig + position - 2) && passed;
                }
                const bool rendering = menuTheme_.SmokeTest();
                passed = rendering && passed;
            }
        }
        ApplyLanguage(language);
        settings_.theme = appearance;
        ApplyUiText();
        UpdateTheme(themeMonitor_->SystemDark());
        return passed;
    }

    void UpdateLogButtonState()
    {
        for (const auto& [control, checked] : std::array{
            std::pair{scrollToEnd_, view_->Following()},
            std::pair{wordWrap_, settings_.logWordWrap}}) {
            const auto state = checked ? BST_CHECKED : BST_UNCHECKED;
            if (SendMessageW(control, BM_GETCHECK, 0, 0) != state) {
                SendMessageW(control, BM_SETCHECK, state, 0);
            }
        }
        const bool hasText = GetWindowTextLengthW(logWindow_) != 0;
        if ((IsWindowEnabled(copyLogs_) != FALSE) != hasText) {
            EnableWindow(copyLogs_, hasText);
        }
    }

    bool SetLogWordWrap(bool enabled, bool persist = true)
    {
        if (persist) {
            auto next = settings_;
            next.logWordWrap = enabled;
            std::wstring error;
            if (!settingsValid_ || !SaveSettings(root_, next, error)) {
                if (!settingsValid_) {
                    error = Text(L"Cannot save word wrapping until the settings have been repaired in File > Settings.",
                        L"请先在“文件 → 设置”中修复并保存设置，然后再切换自动换行。");
                }
                UpdateLogButtonState();
                if (!smokeTest_) {
                    MessageBoxW(window_, error.c_str(), Text(L"Word wrap", L"自动换行"), MB_OK | MB_ICONERROR);
                }
                return false;
            }
        }
        try {
            view_->SetWordWrap(enabled);
        } catch (const std::exception& exception) {
            logWindow_ = view_->Window();
            if (persist) {
                std::wstring error;
                static_cast<void>(SaveSettings(root_, settings_, error));
            }
            UpdateLogButtonState();
            if (smokeTest_) { throw; }
            const auto error = FromUtf8(exception.what());
            MessageBoxW(window_, error.c_str(), Text(L"Word wrap", L"自动换行"), MB_OK | MB_ICONERROR);
            return false;
        }
        logWindow_ = view_->Window();
        settings_.logWordWrap = enabled;
        UpdateLogButtonState();
        UpdateTooltips();
        return true;
    }

    void CreateTooltips()
    {
        tooltip_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE, TOOLTIPS_CLASSW, nullptr,
            WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT,
            CW_USEDEFAULT, CW_USEDEFAULT, window_, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (tooltip_ == nullptr) {
            throw std::runtime_error(ToUtf8(WindowsError(
                Text(L"Cannot create log button tooltips", L"无法创建日志按钮提示"))));
        }
        // Without a Common Controls v6 manifest, TOOLINFO must exclude the v6-only field.
        for (const auto control : {copyLogs_, clearLogs_, scrollToEnd_, wordWrap_, files_, directory_}) {
            TOOLINFOW tool{};
            tool.cbSize = TTTOOLINFOW_V2_SIZE;
            tool.hwnd = window_;
            tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
            tool.uId = reinterpret_cast<UINT_PTR>(control);
            tool.lpszText = const_cast<wchar_t*>(L"");
            if (!SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool))) {
                throw std::runtime_error(Text("Cannot register log button tooltips.", "无法注册日志按钮提示。"));
            }
        }
    }

    void UpdateTooltips()
    {
        if (tooltip_ == nullptr) {
            return;
        }
        const auto selected = SendMessageW(files_, CB_GETCURSEL, 0, 0);
        fileTooltip_ = selected >= 0 && static_cast<size_t>(selected) < settings_.configFiles.size()
            ? settings_.configFiles[static_cast<size_t>(selected)] : L"";
        directoryTooltip_ = settings_.configDirectory;
        for (const auto& [control, label] : std::array{
            std::pair{scrollToEnd_, Text(L"Scroll to end", L"滚动至底部")},
            std::pair{clearLogs_, Text(L"Clear logs", L"清空日志")},
            std::pair{copyLogs_, Text(L"Copy all", L"复制全部")},
            std::pair{wordWrap_, WordWrapCaption(settings_.logWordWrap)},
            std::pair{files_, fileTooltip_.c_str()},
            std::pair{directory_, directoryTooltip_.c_str()}}) {
            TOOLINFOW tool{};
            tool.cbSize = TTTOOLINFOW_V2_SIZE;
            tool.hwnd = window_;
            tool.uId = reinterpret_cast<UINT_PTR>(control);
            tool.lpszText = const_cast<wchar_t*>(label);
            SendMessageW(tooltip_, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&tool));
        }
        SendMessageW(tooltip_, TTM_SETMAXTIPWIDTH, 0, Scale(400));
    }

    std::wstring TooltipText(HWND control) const
    {
        std::wstring text(32768, L'\0');
        TOOLINFOW tool{};
        tool.cbSize = TTTOOLINFOW_V2_SIZE;
        tool.hwnd = window_;
        tool.uId = reinterpret_cast<UINT_PTR>(control);
        tool.lpszText = text.data();
        SendMessageW(tooltip_, TTM_GETTEXTW, text.size(), reinterpret_cast<LPARAM>(&tool));
        text.resize(std::wcslen(text.c_str()));
        return text;
    }

    void UpdateToolIcons(SvgIconRenderer& renderer)
    {
        IconBatch<8> next;
        const auto& colors = theme_.Colors();
        const std::array kinds{SvgIcon::ClearLogs, SvgIcon::ScrollToEnd, SvgIcon::CopyAll, SvgIcon::WordWrap};
        for (size_t index = 0; index < kinds.size(); ++index) {
            next.values[index * 2] = renderer.CreateIcon(kinds[index], Scale(20), colors.text);
            next.values[index * 2 + 1] = renderer.CreateIcon(kinds[index], Scale(20), colors.disabled);
        }
        IconBatch<8> old;
        old.values = toolIcons_;
        toolIcons_ = next.Release();
        SetButtonIcons(clearLogs_, toolIcons_[0], toolIcons_[1], Scale(20));
        SetButtonIcons(scrollToEnd_, toolIcons_[2], toolIcons_[3], Scale(20));
        SetButtonIcons(copyLogs_, toolIcons_[4], toolIcons_[5], Scale(20));
        SetButtonIcons(wordWrap_, toolIcons_[6], toolIcons_[7], Scale(20));
        toolIconDpi_ = dpi_;
        toolIconColor_ = colors.text;
        toolDisabledColor_ = colors.disabled;
    }

    void UpdateTheme(bool systemDark, bool force = false)
    {
        if (menuActive_) {
            // Keep a tracked native menu alive until Windows leaves its menu loop.
            menuThemePending_ = true;
            return;
        }
        if (changingTheme_) {
            return;
        }
        changingTheme_ = true;
        struct Reset { bool& flag; ~Reset() { flag = false; } } reset{changingTheme_};
        const bool dark = ResolveDark(settings_.theme, systemDark);
        theme_.Apply(dark, force);
        if (clearLogs_ != nullptr && (toolIconDpi_ != dpi_ ||
            toolIconColor_ != theme_.Colors().text || toolDisabledColor_ != theme_.Colors().disabled)) {
            SvgIconRenderer renderer;
            UpdateToolIcons(renderer);
        }
        if (view_) {
            view_->SetTheme(theme_.Colors());
        }
        theme_.ApplyOwned(dark);
        toolWindow_.SetAppearance(dark);
        runInfoWindow_.SetAppearance(dark);
        UpdateMenus();
    }

    void UpdateTray()
    {
        if (exiting_ || !ready_) {
            return;
        }
        const auto state = shownState_;
        const bool running = shownPid_ != 0 && shownPid_ != MAXDWORD;
        const auto tooltip = TrayTooltipText(root_, state, running ? shownPid_ : 0);
        nextTrayAttempt_ = GetTickCount64() + 2000;
        if (tray_.Update(window_, icons_[running ? RunningTrayIcon : StoppedTrayIcon], tooltip)) {
            trayFailureReported_ = false;
        } else {
            if (!trayFailureReported_) {
                logs_.Tray(Text(
                    L"The tray icon is unavailable. The window will stay visible; XTray will retry automatically.",
                    L"托盘图标暂不可用；窗口将保持显示，XTray 会自动重试。"));
                trayFailureReported_ = true;
            }
            if (!IsWindowVisible(window_)) {
                ShowMain();
            }
        }
    }

    HMENU CreateTrayMenu() const
    {
        const HMENU menu = CreatePopupMenu();
        if (menu == nullptr) {
            throw std::runtime_error(Text("Cannot create the tray menu.", "无法创建托盘菜单。"));
        }
        const auto state = process_.State();
        const bool enabled = IsWindowEnabled(window_) && !exiting_;
        size_t index = 0;
        const auto append = [this, menu, &index](int id, const wchar_t* caption, bool available) {
            auto& accessible = menuCaptions_[index++];
            accessible = {MSAA_MENU_SIG, static_cast<DWORD>(std::wcslen(caption)),
                const_cast<wchar_t*>(caption)};
            MENUITEMINFOW item{};
            item.cbSize = sizeof(item);
            item.fMask = MIIM_FTYPE | MIIM_STRING | MIIM_STATE | MIIM_ID | MIIM_DATA;
            item.fType = theme_.Colors().dark ? MFT_OWNERDRAW : MFT_STRING;
            item.fState = available ? MFS_ENABLED : MFS_DISABLED;
            item.wID = static_cast<UINT>(id);
            item.dwTypeData = const_cast<wchar_t*>(caption);
            item.dwItemData = theme_.Colors().dark ? reinterpret_cast<ULONG_PTR>(&accessible) : 0;
            return InsertMenuItemW(menu, static_cast<UINT>(GetMenuItemCount(menu)), TRUE, &item) != FALSE;
        };
        if (!append(ShowGui, Text(L"Show GUI", L"显示 GUI"), true) ||
            !append(Run, Text(L"Run", L"运行"), enabled && state == ProcessState::Stopped) ||
            !append(Stop, Text(L"Stop", L"停止"), enabled && state == ProcessState::Running) ||
            !append(Restart, Text(L"Restart", L"重启"), enabled && state == ProcessState::Running) ||
            !append(ExitApp, Text(L"Exit", L"退出"), enabled)) {
            DestroyMenu(menu);
            throw std::runtime_error(Text("Cannot populate the tray menu.", "无法设置托盘菜单。"));
        }
        SetMenuDefaultItem(menu, ShowGui, FALSE);
        return menu;
    }

    void ShowTrayMenu()
    {
        const HMENU menu = CreateTrayMenu();
        POINT point{};
        GetCursorPos(&point);
        NOTIFYICONIDENTIFIER identity{};
        identity.cbSize = sizeof(identity);
        identity.hWnd = window_;
        identity.uID = kTrayIconId;
        RECT icon{};
        if (SUCCEEDED(Shell_NotifyIconGetRect(&identity, &icon)) && !PtInRect(&icon, point)) {
            point = {icon.left + (icon.right - icon.left) / 2, icon.bottom};
        }
        SetForegroundWindow(window_);
        const int selected = static_cast<int>(TrackPopupMenuEx(menu,
            TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, point.x, point.y, window_, nullptr));
        DestroyMenu(menu);
        PostMessageW(window_, WM_NULL, 0, 0);
        tray_.Focus();
        if (selected != 0) {
            TrayCommand(selected);
        }
    }

    void ApplyUiText()
    {
        updatingUi_ = true;
        const std::array captions{
            std::pair{run_, Text(L"Run", L"运行")},
            std::pair{stop_, Text(L"Stop", L"停止")},
            std::pair{restart_, Text(L"Restart", L"重启")},
            std::pair{fileLabel_, Text(L"Config files:", L"配置文件：")},
            std::pair{openFile_, Text(L"Open file", L"打开配置文件")},
            std::pair{openFileDirectory_, Text(L"Open file folder", L"打开配置文件目录")},
            std::pair{directoryLabel_, Text(L"Config folder:", L"配置目录：")},
            std::pair{openDirectory_, Text(L"Open folder", L"打开配置目录")},
            std::pair{filterLabel_, Text(L"Log source:", L"日志来源：")},
            std::pair{clearLogs_, Text(L"Clear logs", L"清空日志")},
            std::pair{scrollToEnd_, Text(L"Scroll to end", L"滚动至底部")},
            std::pair{copyLogs_, Text(L"Copy all", L"复制全部")},
            std::pair{wordWrap_, Text(L"Word wrap", L"自动换行")}
        };
        for (const auto& [control, text] : captions) {
            SetWindowTextW(control, text);
        }
        const auto selected = SendMessageW(filter_, CB_GETCURSEL, 0, 0);
        SendMessageW(filter_, CB_RESETCONTENT, 0, 0);
        for (const auto* item : {Text(L"All", L"全部"), Text(L"XTray only", L"仅 XTray"),
            Text(L"Xray only", L"仅 Xray")}) {
            SendMessageW(filter_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
        }
        SendMessageW(filter_, CB_SETCURSEL, selected == CB_ERR ? 0 : selected, 0);
        UpdateTooltips();
        UpdateMenus();
        toolWindow_.RefreshLanguage();
        runInfoWindow_.RefreshLanguage();
        shownPid_ = MAXDWORD;
        updatingUi_ = false;
        Layout();
    }

    bool SmokeTestWindowBranding(bool running)
    {
        const bool title = WindowText(window_) == MainWindowTitle(root_);
        const auto smallWindowIcon = reinterpret_cast<HICON>(SendMessageW(window_, WM_GETICON, ICON_SMALL, 0));
        const auto largeWindowIcon = reinterpret_cast<HICON>(SendMessageW(window_, WM_GETICON, ICON_BIG, 0));
        const bool smallIcon = smallWindowIcon == icons_[SmallWindowIcon] &&
            IconMatches(smallWindowIcon, kApplicationIconColor, Scale(16));
        const bool largeIcon = largeWindowIcon == icons_[LargeWindowIcon] &&
            IconMatches(largeWindowIcon, kApplicationIconColor, Scale(32));
        const bool statusIcon = reinterpret_cast<HICON>(SendMessageW(statusIcon_, STM_GETICON, 0, 0)) ==
            icons_[running ? RunningStatusIcon : StoppedStatusIcon];
        const bool trayIcon = smokeTrayData.hIcon == icons_[running ? RunningTrayIcon : StoppedTrayIcon];
        const auto pid = running ? process_.Pid() : 0;
        const auto state = running ? ProcessState::Running : ProcessState::Stopped;
        const bool statusText = WindowText(status_) == XrayStatusText(state, pid);
        const bool trayText = std::wstring_view(smokeTrayData.szTip) == TrayTooltipText(root_, state, pid);
        if (!title || !smallIcon || !largeIcon || !statusIcon || !trayIcon || !statusText || !trayText) {
            std::fwprintf(stderr,
                L"Smoke: window branding failed (title=%d, small=%d, large=%d, status=%d, tray=%d, status text=%d, tray text=%d).\n",
                title ? 1 : 0, smallIcon ? 1 : 0, largeIcon ? 1 : 0, statusIcon ? 1 : 0, trayIcon ? 1 : 0,
                statusText ? 1 : 0, trayText ? 1 : 0);
            return false;
        }
        return true;
    }

    bool SmokeTestInstanceText()
    {
        std::fwprintf(stderr, L"Smoke: directory title, three-line tray status, PID, and long-path limits.\n");
        const auto language = CurrentLanguage();
        const std::filesystem::path directory = L"D:\\Tools\\XRay\\XTray";
        const std::wstring heading = kWindowTitle;
        bool passed = MainWindowTitle(directory) == heading + L" (D:\\Tools\\XRay\\XTray)";
        for (const auto selected : {Language::English, Language::Chinese}) {
            ApplyLanguage(selected);
            const std::wstring stopped = selected == Language::Chinese ? L"Xray 已停止" : L"Xray Stopped";
            const std::wstring running = selected == Language::Chinese ? L"Xray(PID 12345) 运行中" : L"Xray(PID 12345) Running";
            const std::wstring starting = selected == Language::Chinese ? L"Xray 启动中…" : L"Xray Starting...";
            const std::wstring stopping = selected == Language::Chinese ? L"Xray(PID 12345) 停止中…" : L"Xray(PID 12345) Stopping...";
            passed = passed && XrayStatusText(ProcessState::Stopped, 0) == stopped &&
                XrayStatusText(ProcessState::Running, 12345) == running &&
                XrayStatusText(ProcessState::Starting, 0) == starting &&
                XrayStatusText(ProcessState::Stopping, 12345) == stopping &&
                TrayTooltipText(directory, ProcessState::Stopped, 0) == heading + L"\n" + directory.native() + L"\n" + stopped &&
                TrayTooltipText(directory, ProcessState::Running, 12345) == heading + L"\n" + directory.native() + L"\n" + running;
            for (const DWORD pid : {DWORD{0}, DWORD{12345}, DWORD{MAXDWORD}}) {
                const auto state = pid == 0 ? ProcessState::Stopped : ProcessState::Running;
                const auto status = XrayStatusText(state, pid);
                const size_t pathLimit = 127 - heading.size() - status.size() - 2;
                std::wstring path = L"D:\\目录 with spaces\\";
                path.append(pathLimit - path.size(), L'长');
                const auto exact = heading + L"\n" + path + L"\n" + status;
                passed = passed && TrayTooltipText(path, state, pid) == exact && exact.size() == 127;
                path += std::wstring(200, L'长') + L"\\XTray";
                const size_t prefix = (pathLimit - 1) / 2;
                const size_t suffix = path.size() - (pathLimit - 1 - prefix);
                path.replace(prefix - 1, 2, L"\U0001F600");
                path.replace(suffix - 1, 2, L"\U0001F600");
                const auto compact = TrayTooltipText(path, state, pid);
                passed = passed && compact.size() <= 127 && std::count(compact.begin(), compact.end(), L'\n') == 2 &&
                    compact.starts_with(heading + L"\nD:\\") && compact.ends_with(L"\\XTray\n" + status) &&
                    compact.find(L'…') != std::wstring::npos && FromUtf8(ToUtf8(compact)) == compact &&
                    MainWindowTitle(path) == heading + L" (" + path + L")";
            }
            if (!passed) {
                std::fwprintf(stderr, L"Smoke: instance text failed (language=%d).\n", static_cast<int>(selected));
                break;
            }
        }
        ApplyLanguage(language);
        return passed;
    }

    bool SmokeTestIconResources()
    {
        std::fwprintf(stderr, L"Smoke: fixed version title, yellow window icons, and SVG resources.\n");
        if (!SmokeTestWindowBranding(false)) {
            return false;
        }
        for (const int side : {16, 20, 24, 32, 48, 256}) {
            IconBatch<1> loaded;
            loaded.values[0] = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),
                MAKEINTRESOURCEW(IDI_XTRAY), IMAGE_ICON, side, side, 0));
            if (!IconMatches(loaded.values[0], kApplicationIconColor, side)) {
                std::fwprintf(stderr, L"Smoke: yellow icon validation failed at %d px.\n", side);
                return false;
            }
        }
        SvgIconRenderer renderer;
        for (const auto kind : {SvgIcon::Application, SvgIcon::ClearLogs, SvgIcon::ScrollToEnd,
            SvgIcon::CopyAll, SvgIcon::WordWrap, SvgIcon::TrayApplication}) {
            for (const int side : {16, 20, 24, 28, 32}) {
                const auto white = renderer.Render(kind, side, RGB(255, 255, 255));
                for (const COLORREF color : {kRunningIconColor, kStoppedIconColor}) {
                    const auto tinted = renderer.Render(kind, side, color);
                    size_t visible = 0;
                    for (size_t index = 0; index < white.bgra.size(); index += 4) {
                        const unsigned alpha = tinted.bgra[index + 3];
                        if (alpha != white.bgra[index + 3]) {
                            return false;
                        }
                        if (alpha != 0) {
                            const std::array<unsigned, 3> channels{
                                GetBValue(color), GetGValue(color), GetRValue(color)};
                            for (size_t channel = 0; channel < channels.size(); ++channel) {
                                const int expected = static_cast<int>((channels[channel] * alpha + 127U) / 255U);
                                if (std::abs(static_cast<int>(tinted.bgra[index + channel]) - expected) > 2) {
                                    return false;
                                }
                            }
                            ++visible;
                        }
                    }
                    if (visible == 0 || tinted.bgra[3] != 0) {
                        return false;
                    }
                }
            }
        }
        const auto logo = renderer.Render(SvgIcon::Application, 64, kRunningIconColor);
        if (logo.bgra[(32U * 64U + 32U) * 4U + 3U] != 0 ||
            logo.bgra[(57U * 64U + 32U) * 4U + 3U] < 250) {
            return false;
        }
        const auto scroll = renderer.Render(SvgIcon::ScrollToEnd, 24, RGB(255, 255, 255));
        for (int row = 4; row < 8; ++row) {
            for (int column = 1; column < 11; ++column) {
                const auto upper = static_cast<size_t>((row * 24 + column) * 4 + 3);
                const auto lower = static_cast<size_t>(((row + 6) * 24 + column) * 4 + 3);
                if (scroll.bgra[upper] != scroll.bgra[lower]) {
                    return false;
                }
            }
        }
        std::fwprintf(stderr, L"Smoke: native icon button state images.\n");
        const auto nativeImages = [this](HWND control) {
            BUTTON_IMAGELIST images{};
            const bool read = SendMessageW(control, BCM_GETIMAGELIST, 0,
                reinterpret_cast<LPARAM>(&images)) != 0;
            const int count = images.himl != nullptr ? ImageList_GetImageCount(images.himl) : 0;
            if (!read || images.uAlign != BUTTON_IMAGELIST_ALIGN_CENTER || count != 6) {
                std::fwprintf(stderr, L"Smoke: native button image list failed (id=%d, read=%d, count=%d, align=%u).\n",
                    GetDlgCtrlID(control), read ? 1 : 0, count, images.uAlign);
                return false;
            }
            const auto& colors = theme_.Colors();
            for (const int state : {0, 3}) {
                const HICON icon = ImageList_GetIcon(images.himl, state, ILD_TRANSPARENT);
                const bool matched = IconMatches(icon, state == 3 ? colors.disabled : colors.text, Scale(20));
                if (icon != nullptr) {
                    DestroyIcon(icon);
                }
                if (!matched) {
                    std::fwprintf(stderr, L"Smoke: native button image tint failed (id=%d, state=%d, size=%d).\n",
                        GetDlgCtrlID(control), state + 1, Scale(20));
                    return false;
                }
            }
            return true;
        };
        return nativeImages(copyLogs_) && nativeImages(clearLogs_) && nativeImages(scrollToEnd_) && nativeImages(wordWrap_);
    }

    bool SmokeTestLogButtons()
    {
        std::fwprintf(stderr, L"Smoke: word wrap, filtered complete copy, and native auto-follow state.\n");
        const auto fail = [](const wchar_t* reason) {
            std::fwprintf(stderr, L"Smoke: log toolbar failed: %ls.\n", reason);
            return false;
        };
        for (const auto control : {scrollToEnd_, wordWrap_}) {
            if ((GetWindowLongPtrW(control, GWL_STYLE) & BS_PUSHLIKE) == 0 ||
                (GetWindowLongPtrW(control, GWL_STYLE) & BS_ICON) == 0) {
                return fail(L"native stateful icon button style");
            }
        }
        logs_.Clear();
        logs_.Tray(L"word wrap 中文");
        logs_.Xray("long line " + std::string(1600, 'x') + "\n");
        view_->Refresh(true);
        const auto document = WindowText(logWindow_);
        CHARRANGE selectedWrap{0, 4}, afterWrap{};
        SendMessageW(logWindow_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&selectedWrap));
        SendMessageW(logWindow_, WM_VSCROLL, SB_TOP, 0);
        const auto wrappedLines = SendMessageW(logWindow_, EM_GETLINECOUNT, 0, 0);
        SetLogWordWrap(false, false);
        const auto unwrappedLines = SendMessageW(logWindow_, EM_GETLINECOUNT, 0, 0);
        SendMessageW(logWindow_, WM_HSCROLL, SB_RIGHT, 0);
        POINT horizontal{};
        SendMessageW(logWindow_, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&horizontal));
        SendMessageW(logWindow_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&afterWrap));
        if (wrappedLines <= unwrappedLines || horizontal.x <= 0 || WindowText(logWindow_) != document ||
            afterWrap.cpMin != 0 || afterWrap.cpMax != 4 ||
            SendMessageW(wordWrap_, BM_GETCHECK, 0, 0) != BST_UNCHECKED ||
            TooltipText(wordWrap_) != WordWrapCaption(false)) {
            return fail(L"unwrapped horizontal scrolling, selection, text, or tooltip");
        }
        SetLogWordWrap(true, false);
        if (WindowText(logWindow_) != document ||
            SendMessageW(logWindow_, EM_GETLINECOUNT, 0, 0) != wrappedLines ||
            SendMessageW(wordWrap_, BM_GETCHECK, 0, 0) != BST_CHECKED ||
            TooltipText(wordWrap_) != WordWrapCaption(true)) {
            return fail(L"wrapped display or text changed on a round trip");
        }
        if ((GetWindowLongPtrW(clearLogs_, GWL_STYLE) & BS_ICON) == 0 ||
            (GetWindowLongPtrW(scrollToEnd_, GWL_STYLE) & BS_ICON) == 0 ||
            TooltipText(clearLogs_) != Text(L"Clear logs", L"清空日志") ||
            TooltipText(scrollToEnd_) != Text(L"Scroll to end", L"滚动至底部")) {
            return false;
        }
        logs_.Clear();
        logs_.SetLimit(200);
        for (int index = 0; index < 90; ++index) {
            logs_.Tray(L"scroll tray " + std::to_wstring(index));
            logs_.Xray("scroll xray " + std::to_string(index) + "\n");
        }
        const auto tailSelected = [this] {
            CHARRANGE selection{};
            SendMessageW(logWindow_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&selection));
            GETTEXTLENGTHEX length{GTL_PRECISE | GTL_NUMCHARS, 1200};
            const LONG end = static_cast<LONG>(SendMessageW(logWindow_, EM_GETTEXTLENGTHEX,
                reinterpret_cast<WPARAM>(&length), 0));
            SCROLLINFO scroll{sizeof(SCROLLINFO), SIF_RANGE | SIF_PAGE | SIF_POS};
            return selection.cpMin == end && selection.cpMax == end &&
                GetScrollInfo(logWindow_, SB_VERT, &scroll) &&
                scroll.nPos + static_cast<int>(scroll.nPage) >= scroll.nMax - 1;
        };
        for (const int filter : {0, 1, 2}) {
            SendMessageW(filter_, CB_SETCURSEL, filter, 0);
            Command(Filter, CBN_SELCHANGE);
            view_->Refresh();
            CHARRANGE selected{0, 4};
            SendMessageW(logWindow_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&selected));
            SendMessageW(logWindow_, WM_VSCROLL, SB_TOP, 0);
            const auto text = WindowText(logWindow_);
            const auto revision = logs_.Revision();
            Refresh();
            if (SendMessageW(scrollToEnd_, BM_GETCHECK, 0, 0) != BST_UNCHECKED) {
                return fail(L"scrolling up did not pause auto-follow");
            }
            const auto expected = view_->CopyText();
            Command(CopyLogs, BN_CLICKED);
            if (!OpenClipboard(window_)) { return fail(L"clipboard unavailable"); }
            const HANDLE data = GetClipboardData(CF_UNICODETEXT);
            const auto copied = data != nullptr ? static_cast<const wchar_t*>(GlobalLock(data)) : nullptr;
            const bool complete = copied != nullptr && std::wstring_view(copied) == expected &&
                expected.find(L"\r\n") != std::wstring::npos &&
                (filter == 2 || expected.find(L"[XTRAY] scroll tray 89") != std::wstring::npos) &&
                (filter == 1 || expected.find(L"[XRAY] scroll xray 89") != std::wstring::npos) &&
                (filter != 1 || expected.find(L"[XRAY]") == std::wstring::npos) &&
                (filter != 2 || expected.find(L"[XTRAY]") == std::wstring::npos);
            if (copied != nullptr) { GlobalUnlock(data); }
            CloseClipboard();
            CHARRANGE afterCopy{};
            SendMessageW(logWindow_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&afterCopy));
            if (!complete || afterCopy.cpMin != selected.cpMin || afterCopy.cpMax != selected.cpMax ||
                SendMessageW(logWindow_, EM_GETFIRSTVISIBLELINE, 0, 0) != 0) {
                return fail(L"filtered full copy changed the selection or included hidden sources");
            }
            Command(ScrollToEnd, BN_CLICKED);
            SendMessageW(scrollToEnd_, BM_CLICK, 0, 0);
            if (SendMessageW(scrollToEnd_, BM_GETCHECK, 0, 0) != BST_CHECKED) {
                return fail(L"clicking the active follow button disabled following");
            }
            if (!tailSelected() || WindowText(logWindow_) != text || logs_.Revision() != revision ||
                SendMessageW(filter_, CB_GETCURSEL, 0, 0) != filter) {
                return false;
            }
            logs_.Tray(L"follow tray");
            logs_.Xray("follow xray\n");
            view_->Refresh();
            if (!tailSelected()) {
                return false;
            }
            SendMessageW(logWindow_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&selected));
            SendMessageW(logWindow_, WM_VSCROLL, SB_TOP, 0);
            logs_.Tray(L"reading tray");
            logs_.Xray("reading xray\n");
            Refresh();
            CHARRANGE after{};
            SendMessageW(logWindow_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&after));
            if (after.cpMin != selected.cpMin || after.cpMax != selected.cpMax ||
                SendMessageW(logWindow_, EM_GETFIRSTVISIBLELINE, 0, 0) != 0 ||
                SendMessageW(scrollToEnd_, BM_GETCHECK, 0, 0) != BST_UNCHECKED) {
                return fail(L"new output disturbed reading or resumed following");
            }
            CHARRANGE caret{selected.cpMax, selected.cpMax};
            SendMessageW(logWindow_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&caret));
            SendMessageW(logWindow_, WM_VSCROLL, SB_BOTTOM, 0);
            Refresh();
            if (SendMessageW(scrollToEnd_, BM_GETCHECK, 0, 0) != BST_CHECKED) {
                return fail(L"scrolling to the bottom did not resume following");
            }
            SendMessageW(logWindow_, WM_VSCROLL, SB_TOP, 0);
            logs_.Tray(L"paused without selection");
            Refresh();
            if (SendMessageW(scrollToEnd_, BM_GETCHECK, 0, 0) != BST_UNCHECKED ||
                SendMessageW(logWindow_, EM_GETFIRSTVISIBLELINE, 0, 0) != 0) {
                return fail(L"scrolling up without selecting text did not pause following");
            }
        }
        SendMessageW(filter_, CB_SETCURSEL, 0, 0);
        Command(Filter, CBN_SELCHANGE);
        Command(ClearLogs, BN_CLICKED);
        return true;
    }

    bool SmokeTestControlAlignment()
    {
        const auto rowAligned = [](const auto& controls, const wchar_t* name) {
            RECT reference{};
            if (!GetWindowRect(controls.front(), &reference)) { return false; }
            for (const HWND control : controls) {
                RECT bounds{};
                if (!GetWindowRect(control, &bounds)) { return false; }
                const LONG delta = bounds.top + bounds.bottom - reference.top - reference.bottom;
                if (std::abs(delta) > 1) {
                    std::fwprintf(stderr, L"Smoke: control row alignment failed (%ls, id=%d, delta=%ld).\n",
                        name, GetDlgCtrlID(control), delta);
                    return false;
                }
            }
            return true;
        };
        const auto columnAligned = [](const auto& controls, bool right) {
            RECT reference{};
            if (!GetWindowRect(controls.front(), &reference)) { return false; }
            for (const HWND control : controls) {
                RECT bounds{};
                if (!GetWindowRect(control, &bounds) ||
                    (right ? bounds.right != reference.right : bounds.left != reference.left)) {
                    std::fwprintf(stderr, L"Smoke: control column alignment failed (id=%d, right=%d).\n",
                        GetDlgCtrlID(control), right);
                    return false;
                }
            }
            return true;
        };
        return rowAligned(std::array{statusIcon_, status_, run_, stop_, restart_}, L"status") &&
            rowAligned(std::array{fileLabel_, files_, openFile_, openFileDirectory_}, L"config files") &&
            rowAligned(std::array{directoryLabel_, directory_, openDirectory_}, L"config directory") &&
            rowAligned(std::array{filterLabel_, filter_, copyLogs_, clearLogs_, scrollToEnd_, wordWrap_}, L"log source") &&
            columnAligned(std::array{statusIcon_, fileLabel_, directoryLabel_, filterLabel_, logWindow_}, false) &&
            columnAligned(std::array{files_, directory_, filter_}, false) &&
            columnAligned(std::array{restart_, openFileDirectory_, openDirectory_, wordWrap_, logWindow_}, true);
    }

    bool SmokeTestUiChanges()
    {
        std::fwprintf(stderr, L"Smoke: centered window and clear-button alignment.\n");
        RECT bounds{}, clear{}, scrollButton{}, logBounds{};
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        GetWindowRect(window_, &bounds);
        if (!GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor) ||
            bounds.left < monitor.rcWork.left || bounds.top < monitor.rcWork.top ||
            bounds.right > monitor.rcWork.right || bounds.bottom > monitor.rcWork.bottom ||
            std::abs((bounds.left + bounds.right) -
                (monitor.rcWork.left + monitor.rcWork.right)) > 1 ||
            std::abs((bounds.top + bounds.bottom) -
                (monitor.rcWork.top + monitor.rcWork.bottom)) > 1) {
            return false;
        }
        for (const int adjustment : {17, 0}) {
            SetWindowPos(window_, nullptr, 0, 0, bounds.right - bounds.left + adjustment,
                bounds.bottom - bounds.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            GetWindowRect(wordWrap_, &clear);
            GetWindowRect(logWindow_, &logBounds);
            GetWindowRect(scrollToEnd_, &scrollButton);
            RECT copy{}, clearButton{};
            GetWindowRect(copyLogs_, &copy);
            GetWindowRect(clearLogs_, &clearButton);
            if (copy.right + Scale(8) != clearButton.left || clearButton.right + Scale(8) != scrollButton.left ||
                copy.top != clear.top || clearButton.top != clear.top) {
                return false;
            }
            if (clear.right != logBounds.right || clear.right - clear.left != Scale(32) ||
                clear.bottom - clear.top != Scale(32) || scrollButton.bottom != clear.bottom ||
                scrollButton.right + Scale(8) != clear.left) {
                return false;
            }
        }

        std::fwprintf(stderr, L"Smoke: natural button widths, compact labels, and complete path tooltips.\n");
        const auto previousSettings = settings_;
        const auto previousLanguage = CurrentLanguage();
        settings_.configFiles = {L".\\very long 配置目录\\" + std::wstring(180, L'x') + L"\\one.json",
            L".\\other 配置目录\\two.json"};
        settings_.configDirectory = L".\\a long 配置目录 & spaces\\fragments";
        UpdatePaths();
        std::fwprintf(stderr, L"Smoke: main control rows and columns at normal and minimum widths.\n");
        RECT originalClient{};
        GetClientRect(window_, &originalClient);
        const LONG originalWidth = bounds.right - bounds.left;
        const LONG minimumWidth = originalWidth - originalClient.right + Scale(640);
        for (const auto language : {Language::English, Language::Chinese}) {
            ApplyLanguage(language);
            ApplyUiText();
            for (const LONG width : {(std::min)(originalWidth, minimumWidth), originalWidth}) {
                SetWindowPos(window_, nullptr, 0, 0, width, bounds.bottom - bounds.top,
                    SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                if (!SmokeTestControlAlignment()) { return false; }
            }
            for (const auto control : {run_, stop_, restart_, openFile_, openFileDirectory_, openDirectory_}) {
                RECT button{};
                GetWindowRect(control, &button);
                const LONG padding = button.right - button.left - ControlTextSize(control).cx;
                if (padding < Scale(23) || padding > Scale(25)) { return false; }
            }
            RECT label{}, input{};
            GetWindowRect(fileLabel_, &label);
            GetWindowRect(files_, &input);
            if (input.left - label.right != Scale(8) || input.right <= input.left ||
                TooltipText(files_) != settings_.configFiles.front() ||
                TooltipText(directory_) != settings_.configDirectory) {
                return false;
            }
        }
        std::fwprintf(stderr, L"Smoke: directory display alignment, click focus, Tab order, and literal path.\n");
        RECT directoryBounds{}, directoryButton{}, directoryClient{};
        GetWindowRect(directory_, &directoryBounds);
        GetWindowRect(openDirectory_, &directoryButton);
        GetClientRect(directory_, &directoryClient);
        wchar_t directoryClass[16]{};
        GetClassNameW(directory_, directoryClass, static_cast<int>(std::size(directoryClass)));
        const auto directoryStyle = GetWindowLongPtrW(directory_, GWL_STYLE);
        const auto previousFocus = GetFocus();
        SetFocus(openFileDirectory_);
        const auto clickFocus = GetFocus();
        SendMessageW(directory_, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(5, 5));
        SendMessageW(directory_, WM_LBUTTONUP, 0, MAKELPARAM(5, 5));
        const bool keepsFocus = GetFocus() == clickFocus && GetFocus() != directory_;
        SetFocus(previousFocus);
        if (_wcsicmp(directoryClass, L"Static") != 0 || (directoryStyle & WS_TABSTOP) != 0 ||
            (directoryStyle & SS_CENTERIMAGE) == 0 || (directoryStyle & SS_NOPREFIX) == 0 ||
            (SendMessageW(directory_, WM_GETDLGCODE, 0, 0) & DLGC_STATIC) == 0 || !keepsFocus ||
            GetNextDlgTabItem(window_, openFileDirectory_, FALSE) != openDirectory_ ||
            GetNextDlgTabItem(window_, openDirectory_, TRUE) != openFileDirectory_ ||
            WindowText(directory_) != settings_.configDirectory || TooltipText(directory_) != settings_.configDirectory ||
            directoryClient.bottom < ControlTextSize(directory_).cy ||
            std::abs(directoryBounds.top + directoryBounds.bottom - directoryButton.top - directoryButton.bottom) > 1) {
            std::fwprintf(stderr, L"Smoke: non-focusable directory display failed.\n");
            return false;
        }
        SendMessageW(files_, CB_SETCURSEL, 1, 0);
        Command(FileList, CBN_SELCHANGE);
        if (TooltipText(files_) != settings_.configFiles[1]) { return false; }
        settings_ = previousSettings;
        ApplyLanguage(previousLanguage);
        ApplyUiText();
        UpdatePaths();
        if (!SmokeTestSettingsLayout(window_, root_ / (L"layout smoke " + std::to_wstring(GetCurrentProcessId())))) {
            return false;
        }
        std::fwprintf(stderr, L"Smoke: selective clearing and continued output.\n");
        logs_.Clear();
        logs_.SetLimit(8);
        logs_.Tray(L"clear tray marker");
        logs_.Xray("clear xray marker\n");
        SendMessageW(filter_, CB_SETCURSEL, 1, 0);
        Command(Filter, CBN_SELCHANGE);
        Command(ClearLogs, BN_CLICKED);
        if (!WindowText(logWindow_).empty() || logs_.Snapshot().lines.size() != 1 ||
            logs_.Snapshot().lines.front()->source != LogSource::Xray) {
            return false;
        }
        SendMessageW(filter_, CB_SETCURSEL, 2, 0);
        Command(Filter, CBN_SELCHANGE);
        Command(ClearLogs, BN_CLICKED);
        if (!logs_.Snapshot().lines.empty() || !WindowText(logWindow_).empty()) {
            return false;
        }
        SendMessageW(filter_, CB_SETCURSEL, 0, 0);
        Command(Filter, CBN_SELCHANGE);
        logs_.Tray(L"retained 中文");
        const std::string longLine = "wrap-marker" + std::string(1600, 'x') + " end";
        logs_.Xray(longLine + "\n");
        view_->Refresh(true);
        if (WindowText(logWindow_).find(FromUtf8(longLine)) == std::wstring::npos) {
            return false;
        }

        std::fwprintf(stderr, L"Smoke: source colors, bold labels, and wrapped continuation.\n");
        const auto characterFormat = [this](LONG start, LONG end) {
            CHARRANGE range{start, end};
            SendMessageW(logWindow_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&range));
            CHARFORMAT2W format{};
            format.cbSize = sizeof(format);
            SendMessageW(logWindow_, EM_GETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&format));
            return format;
        };
        const auto tray = characterFormat(0, 7);
        FINDTEXTEXW find{{0, -1}, const_cast<wchar_t*>(L"wrap-marker"), {}};
        const LONG body = static_cast<LONG>(SendMessageW(logWindow_, EM_FINDTEXTEXW, FR_DOWN,
            reinterpret_cast<LPARAM>(&find)));
        if (body < 7 || (tray.dwEffects & CFE_BOLD) == 0 || tray.crTextColor != RGB(180, 83, 9)) {
            return false;
        }
        const auto label = characterFormat(body - 7, body - 1);
        const auto plain = characterFormat(body, body + 1);
        if ((label.dwEffects & CFE_BOLD) == 0 || (plain.dwEffects & CFE_BOLD) != 0 ||
            plain.crTextColor != RGB(32, 32, 32)) {
            return false;
        }
        PARAFORMAT paragraph{};
        paragraph.cbSize = sizeof(paragraph);
        SendMessageW(logWindow_, EM_GETPARAFORMAT, 0, reinterpret_cast<LPARAM>(&paragraph));
        const auto firstLine = SendMessageW(logWindow_, EM_LINEFROMCHAR, body, 0);
        const auto continuation = SendMessageW(logWindow_, EM_LINEINDEX, firstLine + 1, 0);
        POINT bodyPoint{}, continuationPoint{};
        SendMessageW(logWindow_, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&bodyPoint), body);
        SendMessageW(logWindow_, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&continuationPoint), continuation);
        if (paragraph.dxStartIndent != 0 || paragraph.dxOffset <= 0 || continuation <= body ||
            continuation >= body + static_cast<LONG>(longLine.size()) ||
            std::abs(bodyPoint.x - continuationPoint.x) > 1) {
            return false;
        }

        std::fwprintf(stderr, L"Smoke: language refresh preserves filters and existing logs.\n");
        SendMessageW(filter_, CB_SETCURSEL, 2, 0);
        Command(Filter, CBN_SELCHANGE);
        const auto revision = logs_.Revision();
        ApplyLanguage(Language::English);
        ApplyUiText();
        Refresh();
        if (WindowText(run_) != L"Run" || WindowText(clearLogs_) != L"Clear logs" ||
            TooltipText(clearLogs_) != L"Clear logs" || TooltipText(scrollToEnd_) != L"Scroll to end" ||
            WindowText(status_) != L"Xray Stopped" || logs_.Revision() != revision ||
            SendMessageW(filter_, CB_GETCURSEL, 0, 0) != 2 ||
            WindowText(logWindow_).find(L"[XTRAY]") != std::wstring::npos) {
            return false;
        }
        logs_.Tray(Text(L"English management marker", L"中文管理标记"));
        bool retained = false, translated = false;
        for (const auto& line : logs_.Snapshot().lines) {
            retained = retained || line->text.find(L"retained 中文") != std::wstring::npos;
            translated = translated || line->text.find(L"English management marker") != std::wstring::npos;
        }
        ApplyLanguage(Language::Chinese);
        ApplyUiText();
        Refresh();
        return retained && translated && WindowText(run_) == L"运行" &&
            WindowText(clearLogs_) == L"清空日志" && TooltipText(clearLogs_) == L"清空日志" &&
            TooltipText(scrollToEnd_) == L"滚动至底部";
    }

    bool SmokeTestThemes()
    {
        std::fwprintf(stderr, L"Smoke: theme palettes preserve log text, filters, selection, and scroll.\n");
        const auto fail = [](const wchar_t* check) {
            std::fwprintf(stderr, L"Smoke: theme check failed: %ls.\n", check);
            return false;
        };
        const auto original = settings_;
        const auto language = CurrentLanguage();
        logs_.Clear();
        logs_.SetLimit(200);
        logs_.Tray(L"theme management marker");
        for (int index = 0; index < 80; ++index) {
            logs_.Xray("theme output " + std::to_string(index) + "\n");
        }
        SendMessageW(filter_, CB_SETCURSEL, 0, 0);
        Command(Filter, CBN_SELCHANGE);
        std::fwprintf(stderr, L"Smoke: native log scrollbar page and wheel scrolling.\n");
        SendMessageW(logWindow_, WM_VSCROLL, SB_TOP, 0);
        const auto topLine = SendMessageW(logWindow_, EM_GETFIRSTVISIBLELINE, 0, 0);
        SendMessageW(logWindow_, WM_VSCROLL, SB_PAGEDOWN, 0);
        if (SendMessageW(logWindow_, EM_GETFIRSTVISIBLELINE, 0, 0) <= topLine) {
            return fail(L"native scrollbar page scrolling");
        }
        SendMessageW(logWindow_, WM_VSCROLL, SB_TOP, 0);
        UINT wheelLines = 0;
        if (SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &wheelLines, 0) && wheelLines != 0) {
            POINT wheelPoint{Scale(6), Scale(6)};
            if (!ClientToScreen(logWindow_, &wheelPoint)) {
                return fail(L"map log mouse wheel coordinates");
            }
            POINT previousScroll{};
            SendMessageW(logWindow_, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&previousScroll));
            ULONGLONG lastChange = GetTickCount64();
            const ULONGLONG deadline = lastChange + 1500;
            SendMessageW(logWindow_, WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA),
                MAKELPARAM(wheelPoint.x, wheelPoint.y));
            bool settled = false;
            // Rich Edit completes wheel scrolling through queued messages and timers.
            do {
                MSG message{};
                while (GetTickCount64() < deadline &&
                    PeekMessageW(&message, logWindow_, 0, 0, PM_REMOVE)) {
                    if (message.message == WM_QUIT) {
                        PostQuitMessage(static_cast<int>(message.wParam));
                        return fail(L"window quit during log wheel scrolling");
                    }
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
                POINT current{};
                SendMessageW(logWindow_, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&current));
                const ULONGLONG now = GetTickCount64();
                if (current.x != previousScroll.x || current.y != previousScroll.y) {
                    previousScroll = current;
                    lastChange = now;
                }
                if (SendMessageW(logWindow_, EM_GETFIRSTVISIBLELINE, 0, 0) > topLine &&
                    now - lastChange >= 100) {
                    settled = true;
                    break;
                }
                Sleep(5);
            } while (GetTickCount64() < deadline);
            if (!settled) {
                std::fwprintf(stderr, L"Smoke: wheel scrolling did not settle (x=%ld, y=%ld, lines=%u).\n",
                    previousScroll.x, previousScroll.y, wheelLines);
                return fail(L"native log mouse wheel scrolling");
            }
        }
        SendMessageW(logWindow_, WM_VSCROLL, SB_TOP, 0);
        CHARRANGE selected{1, 5};
        SendMessageW(logWindow_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&selected));
        const auto before = WindowText(logWindow_);
        const auto revision = logs_.Revision();
        POINT scroll{};
        SendMessageW(logWindow_, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll));
        const auto icon = SendMessageW(window_, WM_GETICON, ICON_SMALL, 0);

        struct Canvas {
            HDC device = CreateCompatibleDC(nullptr);
            HBITMAP bitmap = nullptr;
            HGDIOBJ previous = nullptr;
            Canvas()
            {
                BITMAPINFO info{};
                info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                info.bmiHeader.biWidth = 320;
                info.bmiHeader.biHeight = -64;
                info.bmiHeader.biPlanes = 1;
                info.bmiHeader.biBitCount = 32;
                void* pixels = nullptr;
                bitmap = CreateDIBSection(device, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
                if (device != nullptr && bitmap != nullptr) {
                    previous = SelectObject(device, bitmap);
                }
            }
            ~Canvas()
            {
                if (previous != nullptr) { SelectObject(device, previous); }
                if (bitmap != nullptr) { DeleteObject(bitmap); }
                if (device != nullptr) { DeleteDC(device); }
            }
        } canvas;
        if (canvas.device == nullptr || canvas.bitmap == nullptr) {
            return fail(L"create native painting canvas");
        }
        const auto labelColor = [this](const wchar_t* label, LONG length) {
            FINDTEXTEXW find{{0, -1}, const_cast<wchar_t*>(label), {}};
            const auto index = SendMessageW(logWindow_, EM_FINDTEXTEXW, FR_DOWN,
                reinterpret_cast<LPARAM>(&find));
            CHARFORMAT2W format{};
            format.cbSize = sizeof(format);
            if (index >= 0) {
                CHARRANGE range{static_cast<LONG>(index), static_cast<LONG>(index) + length};
                SendMessageW(logWindow_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&range));
                SendMessageW(logWindow_, EM_GETCHARFORMAT, SCF_SELECTION,
                    reinterpret_cast<LPARAM>(&format));
            }
            return format;
        };
        std::array<COLORREF, 2> scrollTrackColors{CLR_INVALID, CLR_INVALID};
        std::array<COLORREF, 2> buttonColors{CLR_INVALID, CLR_INVALID};
        std::array<COLORREF, 2> comboColors{CLR_INVALID, CLR_INVALID};
        for (const bool dark : {true, false, true, false}) {
            settings_.theme = dark ? ThemeMode::Dark : ThemeMode::Light;
            UpdateTheme(!dark);
            if (!SmokeTestControlAlignment()) { return fail(L"control alignment after theme changes"); }
            CHARRANGE after{};
            POINT afterScroll{};
            SendMessageW(logWindow_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&after));
            SendMessageW(logWindow_, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&afterScroll));
            const auto palette = PaletteFor(dark);
            if (!IconMatches(toolIcons_[0], palette.text, Scale(20)) ||
                !IconMatches(toolIcons_[1], palette.disabled, Scale(20))) {
                return fail(L"toolbar icon tint and disabled tint");
            }
            if (theme_.Colors() != palette) {
                return fail(L"apply requested palette");
            }
            const auto directoryBrush = SendMessageW(window_, WM_CTLCOLORSTATIC,
                reinterpret_cast<WPARAM>(canvas.device), reinterpret_cast<LPARAM>(directory_));
            if (directoryBrush == 0 || !IsWindowEnabled(directory_) ||
                GetBkColor(canvas.device) != palette.surface || GetTextColor(canvas.device) != palette.text) {
                return fail(L"directory display surface and readable text colors");
            }
            if (WindowText(logWindow_) != before || logs_.Revision() != revision ||
                SendMessageW(filter_, CB_GETCURSEL, 0, 0) != 0) {
                return fail(L"preserve log text, revision, and filter");
            }
            if (after.cpMin != selected.cpMin || after.cpMax != selected.cpMax) {
                return fail(L"preserve selected text range");
            }
            if (afterScroll.x != scroll.x || afterScroll.y != scroll.y) {
                return fail(L"preserve log scroll position");
            }
            if (SendMessageW(window_, WM_GETICON, ICON_SMALL, 0) != icon) {
                return fail(L"preserve status icon");
            }
            if (IsAppThemed() && IsThemeActive()) {
                const HTHEME scrollTheme = OpenThemeData(logWindow_, L"ScrollBar");
                if (scrollTheme == nullptr) {
                    return fail(L"resolve native scrollbar visual style");
                }
                const RECT track{0, 0, 24, 24};
                const HRESULT drawn = DrawThemeBackground(scrollTheme, canvas.device,
                    SBP_LOWERTRACKVERT, SCRBS_NORMAL, &track, nullptr);
                CloseThemeData(scrollTheme);
                const COLORREF color = GetPixel(canvas.device, 12, 12);
                if (FAILED(drawn) || color == CLR_INVALID) {
                    return fail(L"render native scrollbar theme swatch");
                }
                scrollTrackColors[dark ? 1 : 0] = color;
            }
            const auto tray = labelColor(L"[XTRAY]", 7);
            const auto xray = labelColor(L"[XRAY]", 6);
            if (tray.crTextColor != palette.trayLog || (tray.dwEffects & CFE_BOLD) == 0) {
                return fail(L"XTray log label color and boldness");
            }
            if (xray.crTextColor != palette.xrayLog || (xray.dwEffects & CFE_BOLD) == 0) {
                return fail(L"Xray log label color and boldness");
            }
            if (SendMessageW(logWindow_, EM_SETBKGNDCOLOR, 0, palette.logBackground) != palette.logBackground) {
                return fail(L"Rich Edit log background");
            }
            SendMessageW(logWindow_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&selected));
            SendMessageW(logWindow_, EM_SETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll));
            std::fwprintf(stderr, L"Smoke: native background, button, combo, and menu painting (%ls).\n",
                dark ? L"dark" : L"light");
            SendMessageW(window_, WM_ERASEBKGND, reinterpret_cast<WPARAM>(canvas.device), 0);
            if (GetPixel(canvas.device, 2, 2) != palette.window) {
                return fail(L"native window background");
            }
            if (!SmokeTestTextWordWrap(logWindow_)) { return fail(L"native log scrollbar repaint after wrapping"); }
            for (const auto control : {run_, copyLogs_, clearLogs_, scrollToEnd_, wordWrap_, filter_, files_}) {
                if (!SmokeTestNativeControl(control)) {
                    return fail(L"system-rendered button and combo controls");
                }
            }
            SendMessageW(run_, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(canvas.device), PRF_CLIENT);
            buttonColors[dark ? 1 : 0] = GetPixel(canvas.device, Scale(8), Scale(5));
            SendMessageW(filter_, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(canvas.device), PRF_CLIENT);
            comboColors[dark ? 1 : 0] = GetPixel(canvas.device, Scale(8), Scale(5));
            const HMENU menu = CreateTrayMenu();
            wchar_t caption[64]{};
            MENUITEMINFOW item{};
            item.cbSize = sizeof(item);
            item.fMask = MIIM_DATA | MIIM_FTYPE | MIIM_STRING | MIIM_ID;
            item.dwTypeData = caption;
            item.cch = static_cast<UINT>(std::size(caption));
            const bool read = GetMenuItemInfoW(menu, 0, TRUE, &item) != FALSE;
            const bool ownerDraw = (item.fType & MFT_OWNERDRAW) != 0;
            if (!read || ownerDraw != dark || item.wID != ShowGui ||
                (!dark && std::wstring_view(caption) != Text(L"Show GUI", L"显示 GUI"))) {
                DestroyMenu(menu);
                return fail(L"native light menu and dark menu fallback");
            }
            if (dark) {
                const auto* accessible = reinterpret_cast<const MSAAMENUINFO*>(item.dwItemData);
                if (accessible == nullptr || accessible->dwMSAASignature != MSAA_MENU_SIG ||
                    std::wstring_view(accessible->pszWText) != Text(L"Show GUI", L"显示 GUI")) {
                    DestroyMenu(menu);
                    return fail(L"dark menu accessible name");
                }
                DRAWITEMSTRUCT draw{};
                draw.CtlType = ODT_MENU;
                draw.hDC = canvas.device;
                draw.rcItem = {0, 0, 200, 30};
                draw.itemData = item.dwItemData;
                SendMessageW(window_, WM_DRAWITEM, 0, reinterpret_cast<LPARAM>(&draw));
                const auto background = GetPixel(canvas.device, 2, 2);
                draw.itemState = ODS_SELECTED;
                SendMessageW(window_, WM_DRAWITEM, 0, reinterpret_cast<LPARAM>(&draw));
                const auto highlighted = GetPixel(canvas.device, 2, 2);
                if (background != palette.window || highlighted != palette.hot) {
                    DestroyMenu(menu);
                    return fail(L"dark menu background and selection colors");
                }
            } else if (item.dwItemData != 0) {
                DestroyMenu(menu);
                return fail(L"light menu uses system text and accessibility");
            }
            DestroyMenu(menu);
        }
        if (scrollTrackColors[0] != CLR_INVALID && scrollTrackColors[1] != CLR_INVALID) {
            if (scrollTrackColors[0] == scrollTrackColors[1]) {
                return fail(L"native scrollbar colors follow light and dark modes");
            }
            if (buttonColors[0] == CLR_INVALID || buttonColors[1] == CLR_INVALID ||
                comboColors[0] == CLR_INVALID || comboColors[1] == CLR_INVALID ||
                buttonColors[0] == buttonColors[1] || comboColors[0] == comboColors[1]) {
                return fail(L"native button and combo light/dark colors");
            }
        } else {
            std::fwprintf(stderr, L"Smoke: native scrollbar pixel check requires active Windows visual styles.\n");
        }
        std::fwprintf(stderr, L"Smoke: follow-system updates and explicit mode overrides.\n");
        settings_.theme = ThemeMode::System;
        UpdateTheme(true);
        if (!theme_.Colors().dark) {
            return fail(L"follow dark system mode");
        }
        UpdateTheme(false);
        if (theme_.Colors().dark) {
            return fail(L"follow light system mode");
        }
        settings_.theme = ThemeMode::Dark;
        UpdateTheme(false);
        if (!theme_.Colors().dark) {
            return fail(L"explicit dark mode overrides light system mode");
        }
        std::fwprintf(stderr, L"Smoke: settings appearance draft, save, cancel, Escape, and close.\n");
        const auto fixture = root_ / (L"theme smoke " + std::to_wstring(GetCurrentProcessId()) +
            L"-" + std::to_wstring(GetTickCount64()));
        std::filesystem::create_directories(fixture);
        if (!SmokeTestSettingsTheme(window_, fixture)) {
            return fail(L"settings appearance save and cancel");
        }
        settings_ = original;
        ApplyLanguage(language);
        ApplyUiText();
        UpdateTheme(false);
        Refresh();
        if (WindowText(logWindow_) != before || logs_.Revision() != revision) {
            return fail(L"preserve logs after restoring appearance settings");
        }
        return true;
    }

    bool SmokeTestLogon()
    {
        std::fwprintf(stderr, L"Smoke: sign-in startup failure stays in the tray.\n");
        const auto original = settings_;
        const bool valid = settingsValid_;
        const auto language = CurrentLanguage();
        const auto wait = [this](const auto& condition) {
            const auto deadline = GetTickCount64() + 4000;
            do {
                Refresh();
                if (condition()) {
                    return true;
                }
                Sleep(10);
            } while (GetTickCount64() < deadline);
            return false;
        };
        settingsValid_ = false;
        StartFromLogon();
        if (IsWindowVisible(window_) || !tray_.IsAdded() ||
            process_.State() != ProcessState::Stopped || smokeTrayData.hIcon != icons_[StoppedTrayIcon]) {
            return false;
        }
        bool hasError = false;
        for (const auto& line : logs_.Snapshot().lines) {
            hasError = hasError || (line->source == LogSource::Tray &&
                line->text.find(L"尚未成功读取") != std::wstring::npos);
        }
        if (!hasError) {
            return false;
        }
        std::fwprintf(stderr, L"Smoke: invalid sign-in configuration reports errors without a window.\n");
        logonStarted_ = false;
        settingsValid_ = true;
        settings_.configFiles.clear();
        settings_.configDirectory.clear();
        StartFromLogon();
        if (!wait([&] {
            for (const auto& line : logs_.Snapshot().lines) {
                if (line->source == LogSource::Tray &&
                    line->text.find(L"请至少设置一个") != std::wstring::npos) {
                    return process_.State() == ProcessState::Stopped;
                }
            }
            return false;
        }) || IsWindowVisible(window_) || smokeTrayData.hIcon != icons_[StoppedTrayIcon]) {
            return false;
        }
        logonStarted_ = false;
        settings_.executable = (root_ / L"XTray.Tests.exe").native();
        settings_.configFiles = {L".\\stream.conf"};
        std::fwprintf(stderr, L"Smoke: sign-in starts one hidden child and captures output.\n");
        StartFromLogon();
        if (!wait([&] {
            for (const auto& line : logs_.Snapshot().lines) {
                if (line->source == LogSource::Xray &&
                    line->text.find(L"stream 1") != std::wstring::npos) {
                    return process_.State() == ProcessState::Running;
                }
            }
            return false;
        }) || IsWindowVisible(window_) || smokeTrayData.hIcon != icons_[RunningTrayIcon]) {
            return false;
        }
        const DWORD pid = process_.Pid();
        StartFromLogon();
        if (process_.Pid() != pid || IsWindowVisible(window_)) {
            return false;
        }
        Command(Stop, BN_CLICKED);
        if (!wait([&] { return process_.State() == ProcessState::Stopped; })) {
            return false;
        }
        settings_ = original;
        settingsValid_ = valid;
        logonStarted_ = false;
        const auto fixture = root_ / (L"startup smoke " + std::to_wstring(GetCurrentProcessId()) +
            L"-" + std::to_wstring(GetTickCount64()));
        std::filesystem::create_directories(fixture);
        std::fwprintf(stderr, L"Smoke: sign-in settings save, cancel, and actual registration state.\n");
        if (!SmokeTestSettingsStartup(window_, fixture)) {
            return false;
        }
        ApplyLanguage(language);
        ApplyUiText();
        Refresh();
        return true;
    }

    bool SmokeTestTray()
    {
        std::fwprintf(stderr, L"Smoke: tray registration and stopped menu state.\n");
        if (!tray_.IsAdded() || smokeTrayData.hIcon != icons_[StoppedTrayIcon] ||
            std::wstring_view(smokeTrayData.szTip).find(L"已停止") == std::wstring_view::npos) {
            return false;
        }
        const auto checkMenu = [this](bool running) {
            const HMENU menu = CreateTrayMenu();
            const bool valid = GetMenuItemCount(menu) == 5 &&
                GetMenuItemID(menu, 0) == ShowGui && GetMenuItemID(menu, 4) == ExitApp &&
                ((GetMenuState(menu, Run, MF_BYCOMMAND) & MF_GRAYED) == 0) != running &&
                ((GetMenuState(menu, Stop, MF_BYCOMMAND) & MF_GRAYED) == 0) == running &&
                ((GetMenuState(menu, Restart, MF_BYCOMMAND) & MF_GRAYED) == 0) == running;
            DestroyMenu(menu);
            return valid;
        };
        if (!checkMenu(false) || !SmokeTestWindowBranding(false)) {
            return false;
        }
        const auto original = settings_;
        settings_.executable = (root_ / L"XTray.Tests.exe").native();
        settings_.configFiles = {L".\\stream.conf"};
        const auto wait = [this](const auto& condition) {
            const auto deadline = GetTickCount64() + 4000;
            do {
                Refresh();
                if (condition()) {
                    return true;
                }
                Sleep(10);
            } while (GetTickCount64() < deadline);
            return false;
        };
        const auto hasOutput = [this](const wchar_t* marker) {
            for (const auto& line : logs_.Snapshot().lines) {
                if (line->source == LogSource::Xray && line->text.find(marker) != std::wstring::npos) {
                    return true;
                }
            }
            return false;
        };
        std::fwprintf(stderr, L"Smoke: tray run and restart.\n");
        TrayCommand(Run);
        if (!wait([&] { return hasOutput(L"stream 1"); }) || !checkMenu(true) ||
            smokeTrayData.hIcon != icons_[RunningTrayIcon] || !SmokeTestWindowBranding(true)) {
            return false;
        }
        const DWORD themePid = process_.Pid();
        settings_.theme = ThemeMode::Dark;
        UpdateTheme(false);
        if (!wait([&] { return hasOutput(L"stream 2"); }) || process_.Pid() != themePid ||
            !theme_.Colors().dark || !SmokeTestWindowBranding(true)) {
            return false;
        }
        settings_.theme = original.theme;
        UpdateTheme(false);
        const DWORD previousPid = process_.Pid();
        Handle previous(OpenProcess(SYNCHRONIZE, FALSE, previousPid));
        TrayCommand(Restart);
        if (!wait([&] { return process_.State() == ProcessState::Running &&
            process_.Pid() != previousPid && hasOutput(L"stream 1"); }) ||
            !previous || WaitForSingleObject(previous.Get(), 0) != WAIT_OBJECT_0) {
            return false;
        }
        std::fwprintf(stderr, L"Smoke: close hides, output continues, and click restores.\n");
        const DWORD pid = process_.Pid();
        RECT before{}, after{};
        GetWindowRect(window_, &before);
        TrayCommand(ShowGui);
        SendMessageW(window_, WM_CLOSE, 0, 0);
        if (IsWindowVisible(window_) || !wait([&] { return hasOutput(L"stream 3"); }) ||
            process_.Pid() != pid) {
            return false;
        }
        TrayEvent(0, MAKELPARAM(NIN_SELECT, kTrayIconId));
        GetWindowRect(window_, &after);
        if (!IsWindowVisible(window_) || !EqualRect(&before, &after) ||
            process_.Pid() != pid) {
            return false;
        }
        TaskbarCreated();
        if (!tray_.IsAdded() || smokeTrayData.hIcon != icons_[RunningTrayIcon] || !SmokeTestWindowBranding(true)) {
            return false;
        }
        std::fwprintf(stderr, L"Smoke: tray outage recovery and language refresh.\n");
        smokeTrayAvailable = false;
        TaskbarCreated();
        const auto trayLines = [this] {
            const auto snapshot = logs_.Snapshot();
            return std::count_if(snapshot.lines.begin(), snapshot.lines.end(),
                [](const auto& line) { return line->source == LogSource::Tray; });
        };
        const auto failureLines = trayLines();
        SendMessageW(window_, WM_CLOSE, 0, 0);
        if (!IsWindowVisible(window_) || tray_.IsAdded() || trayLines() != failureLines) {
            return false;
        }
        smokeTrayAvailable = true;
        TaskbarCreated();
        if (!tray_.IsAdded()) {
            return false;
        }
        ApplyLanguage(Language::English);
        ApplyUiText();
        Refresh();
        if (std::wstring_view(smokeTrayData.szTip).find(L"Running") == std::wstring_view::npos ||
            !SmokeTestWindowBranding(true)) {
            return false;
        }
        EnableWindow(window_, FALSE);
        const HMENU modalMenu = CreateTrayMenu();
        const bool disabled = (GetMenuState(modalMenu, Run, MF_BYCOMMAND) & MF_GRAYED) != 0 &&
            (GetMenuState(modalMenu, ExitApp, MF_BYCOMMAND) & MF_GRAYED) != 0;
        DestroyMenu(modalMenu);
        EnableWindow(window_, TRUE);
        if (!disabled) {
            return false;
        }
        std::fwprintf(stderr, L"Smoke: tray stop and failed run.\n");
        TrayCommand(Stop);
        if (!wait([&] { return process_.State() == ProcessState::Stopped; }) ||
            smokeTrayData.hIcon != icons_[StoppedTrayIcon] || !checkMenu(false) || !SmokeTestWindowBranding(false)) {
            return false;
        }
        settings_ = original;
        TrayCommand(Run);
        if (!wait([&] { return process_.State() == ProcessState::Stopped; }) ||
            smokeTrayData.hIcon != icons_[StoppedTrayIcon] || !SmokeTestWindowBranding(false)) {
            return false;
        }
        settings_.executable = (root_ / L"XTray.Tests.exe").native();
        settings_.configFiles = {L".\\stream.conf"};
        TrayCommand(Run);
        if (!wait([&] { return process_.State() == ProcessState::Running; })) {
            return false;
        }
        Handle child(OpenProcess(SYNCHRONIZE, FALSE, process_.Pid()));
        std::fwprintf(stderr, L"Smoke: explicit exit stops the child and removes the tray icon.\n");
        TrayCommand(ExitApp);
        return child && WaitForSingleObject(child.Get(), 0) == WAIT_OBJECT_0 &&
            !tray_.IsAdded() && !IsWindow(window_);
    }

    int Scale(int value) const { return MulDiv(value, static_cast<int>(dpi_), 96); }

    void SetFontsAndIcons()
    {
        NONCLIENTMETRICSW metrics{};
        metrics.cbSize = sizeof(metrics);
        if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi_)) {
            throw std::runtime_error(Text("Cannot read the system UI font.", "无法读取系统界面字体。"));
        }
        HFONT font = CreateFontIndirectW(&metrics.lfMessageFont);
        LOGFONTW log = metrics.lfMessageFont;
        log.lfHeight = -MulDiv(10, static_cast<int>(dpi_), 72);
        log.lfWeight = FW_NORMAL;
        wcscpy_s(log.lfFaceName, L"Consolas");
        HFONT logFont = CreateFontIndirectW(&log);
        if (font == nullptr || logFont == nullptr) {
            if (font != nullptr) {
                DeleteObject(font);
            }
            if (logFont != nullptr) {
                DeleteObject(logFont);
            }
            throw std::runtime_error(Text("Cannot create window fonts.", "无法创建窗口字体。"));
        }
        const std::array controls{status_, run_, stop_, restart_, fileLabel_,
            files_, openFile_, openFileDirectory_, directoryLabel_, directory_, openDirectory_,
            filterLabel_, filter_, copyLogs_, clearLogs_, scrollToEnd_, wordWrap_};
        for (const auto control : controls) {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        }
        SendMessageW(logWindow_, WM_SETFONT, reinterpret_cast<WPARAM>(logFont), TRUE);
        if (font_ != nullptr) {
            DeleteObject(font_);
        }
        if (logFont_ != nullptr) {
            DeleteObject(logFont_);
        }
        font_ = font;
        logFont_ = logFont;
        theme_.SetFont(font_);
        view_->InvalidateFormat(logFont_);
        UpdateTooltips();

        SvgIconRenderer renderer;
        IconBatch<IconCount> next;
        for (size_t state = 0; state < 2; ++state) {
            const COLORREF color = state != 0 ? kRunningIconColor : kStoppedIconColor;
            next.values[StoppedTrayIcon + state] = renderer.CreateIcon(SvgIcon::TrayApplication, Scale(16), color);
            next.values[StoppedStatusIcon + state] = renderer.CreateIcon(SvgIcon::Application, Scale(24), color);
        }
        next.values[SmallWindowIcon] = renderer.CreateIcon(SvgIcon::Application, Scale(16), kApplicationIconColor);
        next.values[LargeWindowIcon] = renderer.CreateIcon(SvgIcon::Application, Scale(32), kApplicationIconColor);
        UpdateToolIcons(renderer);
        IconBatch<IconCount> old;
        old.values = icons_;
        icons_ = next.Release();
        const bool running = process_.Pid() != 0;
        SendMessageW(statusIcon_, STM_SETICON,
            reinterpret_cast<WPARAM>(icons_[running ? RunningStatusIcon : StoppedStatusIcon]), 0);
        SendMessageW(window_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icons_[SmallWindowIcon]));
        SendMessageW(window_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icons_[LargeWindowIcon]));
        if (ready_) {
            UpdateMenus();
            theme_.Apply(theme_.Colors().dark, true);
            shownPid_ = MAXDWORD;
            Refresh();
        }
    }

    void UpdatePaths()
    {
        SendMessageW(files_, CB_RESETCONTENT, 0, 0);
        for (const auto& file : settings_.configFiles) {
            SendMessageW(files_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(file.c_str()));
        }
        if (!settings_.configFiles.empty()) {
            SendMessageW(files_, CB_SETCURSEL, 0, 0);
        }
        SetWindowTextW(directory_, settings_.configDirectory.c_str());
        EnableWindow(openDirectory_, !settings_.configDirectory.empty());
        UpdateOpenButtons();
    }

    void UpdateOpenButtons()
    {
        const bool selected = SendMessageW(files_, CB_GETCURSEL, 0, 0) != CB_ERR;
        EnableWindow(openFile_, selected);
        EnableWindow(openFileDirectory_, selected);
        UpdateTooltips();
    }

    void OpenPath(const std::wstring& value, bool directory)
    {
        const auto path = ResolvePath(root_, value);
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            logs_.Tray(WindowsError(Text(L"Cannot open path: ", L"无法打开路径：") + path.native()));
            return;
        }
        if (directory != ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)) {
            logs_.Tray(Text(L"Incorrect path type: ", L"路径类型不正确：") + path.native());
            return;
        }
        SHELLEXECUTEINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
        info.hwnd = window_;
        info.lpVerb = L"open";
        info.lpFile = path.c_str();
        info.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&info)) {
            logs_.Tray(WindowsError(Text(L"Cannot open path: ", L"打开路径失败：") + path.native()));
        }
    }

    const std::filesystem::path root_;
    StartupRegistration startup_;
    Logs logs_;
    XrayProcess process_;
    XrayToolWindow toolWindow_;
    XrayToolWindow runInfoWindow_;
    bool smokeTest_;
    TrayIcon tray_;
    ThemeWindow theme_;
    MenuTheme menuTheme_;
    std::unique_ptr<ThemeMonitor> themeMonitor_;
    bool changingTheme_ = false;
    bool exiting_ = false;
    bool logonStarted_ = false;
    bool failureSmokePending_ = false;
    bool trayFailureReported_ = false;
    UINT taskbarCreated_ = 0;
    ULONGLONG nextTrayAttempt_ = 0;
    Settings settings_;
    bool settingsValid_ = true;
    bool ready_ = false;
    bool updatingUi_ = false;
    UINT dpi_ = 96;
    HWND window_ = nullptr;
    HWND statusIcon_ = nullptr, status_ = nullptr;
    HWND run_ = nullptr, stop_ = nullptr, restart_ = nullptr;
    HMENU menuBar_ = nullptr, fileMenu_ = nullptr, xrayMenu_ = nullptr;
    Language menuLanguage_ = Language::Automatic;
    bool menuDark_ = false, menuActive_ = false, menuThemePending_ = false;
    UINT menuDpi_ = 0;
    HWND fileLabel_ = nullptr, files_ = nullptr, openFile_ = nullptr, openFileDirectory_ = nullptr;
    HWND directoryLabel_ = nullptr, directory_ = nullptr, openDirectory_ = nullptr;
    HWND filterLabel_ = nullptr, filter_ = nullptr, scrollToEnd_ = nullptr, clearLogs_ = nullptr;
    HWND copyLogs_ = nullptr, wordWrap_ = nullptr;
    HWND logWindow_ = nullptr, tooltip_ = nullptr;
    std::wstring fileTooltip_, directoryTooltip_;
    HFONT font_ = nullptr, logFont_ = nullptr;
    std::array<HICON, IconCount> icons_{};
    std::array<HICON, 8> toolIcons_{};
    UINT toolIconDpi_ = 0;
    COLORREF toolIconColor_ = CLR_INVALID, toolDisabledColor_ = CLR_INVALID;
    mutable std::array<MSAAMENUINFO, 5> menuCaptions_{};
    std::unique_ptr<LogView> view_;
    ProcessState shownState_ = ProcessState::Stopped;
    DWORD shownPid_ = MAXDWORD;
};

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* app = reinterpret_cast<Application*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* creation = reinterpret_cast<CREATESTRUCTW*>(lParam);
        app = static_cast<Application*>(creation->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    try {
        LRESULT themed = 0;
        if (app != nullptr && app->ThemeMessage(message, wParam, lParam, themed)) {
            return themed;
        }
        if (message == WM_CREATE) {
            return app->Initialize(window) ? 0 : -1;
        }
        if (message == WM_GETMINMAXINFO) {
            const UINT dpi = GetDpiForWindow(window);
            RECT bounds{0, 0, MulDiv(640, static_cast<int>(dpi), 96),
                MulDiv(400, static_cast<int>(dpi), 96)};
            if (AdjustWindowRectExForDpi(&bounds, kWindowStyle, TRUE, 0, dpi)) {
                auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
                limits->ptMinTrackSize = {bounds.right - bounds.left, bounds.bottom - bounds.top};
                MONITORINFO monitor{};
                monitor.cbSize = sizeof(monitor);
                if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) {
                    limits->ptMinTrackSize.x = (std::min)(limits->ptMinTrackSize.x,
                        monitor.rcWork.right - monitor.rcWork.left);
                    limits->ptMinTrackSize.y = (std::min)(limits->ptMinTrackSize.y,
                        monitor.rcWork.bottom - monitor.rcWork.top);
                }
            }
            return 0;
        }
        if (app != nullptr) {
            if (message == app->TaskbarCreatedMessage()) {
                app->TaskbarCreated();
                return 0;
            }
            switch (message) {
            case kSystemThemeMessage:
            case WM_SETTINGCHANGE:
            case WM_THEMECHANGED:
                app->SystemThemeChanged();
                return 0;
            case kShowInstanceMessage:
                app->ShowMain();
                return kInstanceShown;
            case kTrayCallbackMessage:
                app->TrayEvent(wParam, lParam);
                return 0;
            case kRuntimeFailureTestMessage:
                app->RaiseFailureForSmokeTest();
                return 0;
            case kExitMessage:
                app->Exit();
                return 0;
            case WM_SIZE:
                app->Layout();
                return 0;
            case WM_DPICHANGED: {
                app->DpiChanged(HIWORD(wParam));
                const auto* bounds = reinterpret_cast<const RECT*>(lParam);
                SetWindowPos(window, nullptr, bounds->left, bounds->top,
                    bounds->right - bounds->left, bounds->bottom - bounds->top,
                    SWP_NOZORDER | SWP_NOACTIVATE);
                return 0;
            }
            case WM_COMMAND:
                app->Command(LOWORD(wParam), HIWORD(wParam));
                return 0;
            case WM_TIMER:
                app->Refresh();
                return 0;
            case WM_QUERYENDSESSION:
                return TRUE;
            case WM_ENDSESSION:
                if (!wParam) {
                    return 0;
                }
                app->Exit();
                return 0;
            case WM_CLOSE:
                app->HideMain();
                return 0;
            case WM_DESTROY:
                KillTimer(window, 1);
                app->Destroyed();
                PostQuitMessage(app->exitCode);
                return 0;
            }
        }
    } catch (...) {
        if (message == WM_CREATE) {
            if (app != nullptr) {
                app->startupError = std::current_exception();
            }
            return -1;
        }
        if (app != nullptr) {
            if (!app->runtimeError) {
                app->runtimeError = std::current_exception();
            }
            app->exitCode = 1;
        }
        if (app == nullptr || app->IsExiting() || !PostMessageW(window, kExitMessage, 0, 0)) {
            PostQuitMessage(1);
        }
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int showCommand)
{
    const bool failureSmokeTest = std::wstring_view(commandLine) == L"--smoke-test-failure";
    const bool smokeTest = failureSmokeTest || std::wstring_view(commandLine) == L"--smoke-test";
    const bool logon = IsLogonCommandLine(commandLine);
    ApplyLanguage(Language::Automatic);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(com)) {
        return ReportStartupError(smokeTest, Text(L"Cannot initialize Windows COM.", L"无法初始化 Windows COM。"));
    }
    struct ComLifetime { ~ComLifetime() { CoUninitialize(); } } comLifetime;
    const HMODULE richEdit = LoadLibraryExW(L"Msftedit.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (richEdit == nullptr) {
        return ReportStartupError(smokeTest, WindowsError(Text(L"Cannot load Windows Rich Edit", L"无法加载 Windows Rich Edit")));
    }
    struct LibraryLifetime {
        HMODULE module;
        ~LibraryLifetime() { FreeLibrary(module); }
    } library{richEdit};

    bool initialized = false;
    try {
        const auto root = ExecutablePath().parent_path();
        std::unique_ptr<DirectoryInstance> directory;
        std::wstring className = L"XTray.Smoke." + std::to_wstring(GetCurrentProcessId());
        if (!smokeTest) {
            directory = std::make_unique<DirectoryInstance>(root);
            if (!directory->IsPrimary()) {
                // Repeated sign-in startup leaves the existing instance and its window alone.
                if (logon) {
                    return 0;
                }
                if (directory->ActivateExisting()) {
                    return 0;
                }
                return ReportStartupError(false, Text(
                    L"This XTray directory is already in use. Its window cannot be activated; it may be starting, unresponsive, or in another user session. Use a separate directory for an independent instance.",
                    L"此目录的 XTray 已被占用，无法唤回其窗口；它可能正在启动、未响应，或位于其他用户或登录会话。独立运行请使用其他目录。"));
            }
            className = directory->WindowClass();
        }
        Application app(root, smokeTest);
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = WindowProcedure;
        windowClass.hInstance = instance;
        windowClass.hCursor = static_cast<HCURSOR>(LoadImageW(
            nullptr, IDC_ARROW, IMAGE_CURSOR, 0, 0, LR_DEFAULTSIZE | LR_SHARED));
        windowClass.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        windowClass.lpszClassName = className.c_str();
        if (!RegisterClassExW(&windowClass)) {
            return ReportStartupError(smokeTest, WindowsError(Text(L"Cannot register the window", L"注册窗口失败")));
        }

        POINT cursor{};
        GetCursorPos(&cursor);
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        if (!GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &monitor)) {
            return ReportStartupError(smokeTest, WindowsError(
                Text(L"Cannot read the display work area", L"无法读取显示器工作区")));
        }
        const RECT workArea = monitor.rcWork;
        const UINT dpi = GetDpiForSystem();
        RECT bounds{0, 0, MulDiv(800, static_cast<int>(dpi), 96), MulDiv(600, static_cast<int>(dpi), 96)};
        if (!AdjustWindowRectExForDpi(&bounds, kWindowStyle, TRUE, 0, dpi)) {
            return ReportStartupError(smokeTest, WindowsError(Text(L"Cannot calculate the window size", L"计算窗口尺寸失败")));
        }
        LONG width = bounds.right - bounds.left;
        LONG height = bounds.bottom - bounds.top;
        width = (std::min)(width, workArea.right - workArea.left);
        height = (std::min)(height, workArea.bottom - workArea.top);
        const auto title = MainWindowTitle(root);
        const HWND window = CreateWindowExW(0, className.c_str(), title.c_str(), kWindowStyle,
            workArea.left, workArea.top, width, height, nullptr, nullptr, instance, &app);
        if (window == nullptr) {
            if (app.startupError) {
                std::rethrow_exception(app.startupError);
            }
            return ReportStartupError(smokeTest, WindowsError(Text(L"Cannot create the window or its controls", L"创建窗口或控件失败")));
        }
        initialized = true;
        // The hidden window is already on the target monitor, so this DPI is its actual DPI.
        const UINT windowDpi = GetDpiForWindow(window);
        bounds = {0, 0, MulDiv(800, static_cast<int>(windowDpi), 96),
            MulDiv(600, static_cast<int>(windowDpi), 96)};
        if (!AdjustWindowRectExForDpi(&bounds, kWindowStyle, TRUE, 0, windowDpi)) {
            return ReportStartupError(smokeTest, WindowsError(
                Text(L"Cannot calculate the window size", L"计算窗口尺寸失败")));
        }
        width = (std::min)(bounds.right - bounds.left, workArea.right - workArea.left);
        height = (std::min)(bounds.bottom - bounds.top, workArea.bottom - workArea.top);
        if (!SetWindowPos(window, nullptr, workArea.left + (workArea.right - workArea.left - width) / 2,
            workArea.top + (workArea.bottom - workArea.top - height) / 2, width, height,
            SWP_NOZORDER | SWP_NOACTIVATE)) {
            return ReportStartupError(smokeTest, WindowsError(
                Text(L"Cannot center the window", L"无法将窗口居中")));
        }
        if (failureSmokeTest) {
            app.StartFailureSmokeTest();
        } else if (smokeTest) {
            if (!app.SmokeTest()) {
                app.exitCode = ReportStartupError(true, L"The Rich Edit log UI smoke test failed.");
            }
            if (IsWindow(window) && !PostMessageW(window, kExitMessage, 0, 0)) {
                app.Exit();
                return ReportStartupError(true, WindowsError(Text(L"Cannot close the test window", L"关闭测试窗口失败")));
            }
        } else if (logon) {
            app.StartFromLogon();
        } else {
            ShowWindow(window, showCommand == SW_HIDE ? SW_SHOWNORMAL : showCommand);
            UpdateWindow(window);
        }

        MSG message{};
        BOOL result;
        while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
            if (!app.TranslateToolMessage(message) && !IsDialogMessageW(window, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        if (result == -1) {
            return ReportStartupError(smokeTest, WindowsError(
                Text(L"The window message loop failed", L"窗口消息循环失败")));
        }
        if (app.runtimeError) {
            std::rethrow_exception(app.runtimeError);
        }
        return app.exitCode != 0 ? app.exitCode : static_cast<int>(message.wParam);
    } catch (const std::exception& exception) {
        Utf8Stream decoder;
        const auto reason = decoder.Append(exception.what(), true);
        return ReportStartupError(smokeTest, (initialized
            ? smokeTest ? L"XTray runtime failure: " : Text(L"XTray stopped because of an internal error: ", L"XTray 因内部错误退出：")
            : Text(L"XTray could not start: ", L"XTray 启动失败：")) + reason);
    } catch (...) {
        return ReportStartupError(smokeTest, smokeTest ? L"Unknown XTray runtime failure."
            : Text(L"XTray stopped because of an unknown internal error.", L"XTray 因未知内部错误退出。"));
    }
}
