#include "instance.h"
#include "startup.h"
#include "tray.h"
#include "svg_icons.h"
#include "resource.h"
#include "theme_window.h"
#include "log_view.h"
#include "xray_process.h"
#include "settings_dialog.h"
#include "win32.h"

#include <commdlg.h>
#include <objbase.h>
#include <richedit.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <exception>
#include <memory>
#include <string_view>

#if !defined(_M_X64)
#error XTray supports Windows x64 only.
#endif

namespace {

using namespace xtray;

constexpr UINT kExitMessage = WM_APP + 43;
constexpr DWORD kWindowStyle = WS_OVERLAPPEDWINDOW;
enum ControlId { Run = 2001, Stop, Restart, SettingsButton, FileList, OpenFile,
    OpenFileDirectory, OpenDirectory, Filter, ClearLogs, ScrollToEnd, ShowGui, ExitApp };

NOTIFYICONDATAW smokeTrayData{};
bool smokeTrayAvailable = true;
BOOL WINAPI SmokeNotifyIcon(DWORD operation, PNOTIFYICONDATAW data)
{
    if (operation == NIM_ADD || operation == NIM_MODIFY) {
        smokeTrayData = *data;
    }
    return smokeTrayAvailable;
}

int ReportStartupError(bool smokeTest, const std::wstring& error)
{
    if (smokeTest) {
        std::fwprintf(stderr, L"%ls\n", error.c_str());
    } else {
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
          process_(root_, logs_), smokeTest_(smokeTest),
          tray_(smokeTest ? SmokeNotifyIcon : Shell_NotifyIconW) {}
    ~Application()
    {
        themeMonitor_.reset();
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
        settingsButton_ = child(0, L"BUTTON", L"", WS_TABSTOP, SettingsButton);
        fileLabel_ = child(0, L"STATIC", L"", SS_CENTERIMAGE);
        files_ = child(0, L"COMBOBOX", L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP, FileList);
        openFile_ = child(0, L"BUTTON", L"", WS_TABSTOP, OpenFile);
        openFileDirectory_ = child(0, L"BUTTON", L"", WS_TABSTOP, OpenFileDirectory);
        directoryLabel_ = child(0, L"STATIC", L"", SS_CENTERIMAGE);
        directory_ = child(WS_EX_CLIENTEDGE, L"EDIT", L"",
            ES_READONLY | ES_AUTOHSCROLL | WS_TABSTOP);
        openDirectory_ = child(0, L"BUTTON", L"", WS_TABSTOP, OpenDirectory);
        filterLabel_ = child(0, L"STATIC", L"", SS_CENTERIMAGE);
        filter_ = child(0, L"COMBOBOX", L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP, Filter);
        scrollToEnd_ = child(0, L"BUTTON", L"", WS_TABSTOP | BS_ICON, ScrollToEnd);
        clearLogs_ = child(0, L"BUTTON", L"", WS_TABSTOP | BS_ICON, ClearLogs);
        CreateTooltips();
        logWindow_ = child(WS_EX_CLIENTEDGE, MSFTEDIT_CLASS, L"",
            ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_NOHIDESEL | WS_VSCROLL | WS_TABSTOP);
        view_ = std::make_unique<LogView>(logWindow_, logs_);
        if (smokeTest_) {
            std::fwprintf(stderr, L"Smoke: configuring fonts and icons.\n");
        }
        SetFontsAndIcons();
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
        ApplyLanguage(settings_.language);
        ApplyUiText();
        themeMonitor_ = std::make_unique<ThemeMonitor>(window_);
        UpdateTheme(themeMonitor_->SystemDark(), true);
        logs_.SetLimit(settings_.logLines);
        logs_.Tray(settingsValid_
            ? Text(L"XTray is ready. Configure paths in Settings, then click Run to start Xray.", L"XTray 已就绪；点击“设置”填写路径，再点击“运行”启动 Xray。")
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
        const int width = client.right;
        const auto move = [this](HWND control, int x, int y, int widthValue, int height) {
            MoveWindow(control, Scale(x), Scale(y), Scale(widthValue), Scale(height), TRUE);
        };
        const int logicalWidth = MulDiv(width, 96, static_cast<int>(dpi_));
        move(statusIcon_, 16, 16, 24, 28);
        move(status_, 48, 16, (std::max)(80, logicalWidth - 392), 28);
        const int actions = logicalWidth - 328;
        move(run_, actions, 16, 72, 28);
        move(stop_, actions + 80, 16, 72, 28);
        move(restart_, actions + 160, 16, 72, 28);
        move(settingsButton_, actions + 240, 16, 72, 28);
        move(fileLabel_, 16, 60, 96, 26);
        move(files_, 112, 60, logicalWidth - 408, 240);
        move(openFile_, logicalWidth - 288, 60, 112, 26);
        move(openFileDirectory_, logicalWidth - 168, 60, 152, 26);
        move(directoryLabel_, 16, 98, 96, 26);
        move(directory_, 112, 98, logicalWidth - 248, 26);
        move(openDirectory_, logicalWidth - 128, 98, 112, 26);
        move(filterLabel_, 16, 136, 96, 26);
        move(filter_, 112, 136, 136, 180);
        MoveWindow(scrollToEnd_, width - Scale(16) - Scale(72), Scale(134),
            Scale(32), Scale(32), TRUE);
        MoveWindow(clearLogs_, width - Scale(16) - Scale(32), Scale(134),
            Scale(32), Scale(32), TRUE);
        MoveWindow(logWindow_, Scale(16), Scale(174), (std::max)(1, width - Scale(32)),
            (std::max)(1, static_cast<int>(client.bottom) - Scale(190)), TRUE);
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
            std::wstring status = state == ProcessState::Starting ? Text(L"Starting...", L"启动中…")
                : state == ProcessState::Stopping ? Text(L"Stopping...", L"停止中…")
                : running ? Text(L"Running", L"运行中") : Text(L"Stopped", L"已停止");
            if (running) {
                status += L"  PID " + std::to_wstring(pid);
            }
            SetWindowTextW(status_, status.c_str());
            SetWindowTextW(window_, running ? Text(L"XTray — Running", L"XTray — 运行中") : Text(L"XTray — Stopped", L"XTray — 已停止"));
            EnableWindow(run_, state == ProcessState::Stopped);
            EnableWindow(stop_, state == ProcessState::Running);
            EnableWindow(restart_, state == ProcessState::Running);
            const size_t offset = running ? 3 : 0;
            SendMessageW(statusIcon_, STM_SETICON, reinterpret_cast<WPARAM>(icons_[offset + 2]), 0);
            SendMessageW(window_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icons_[offset]));
            SendMessageW(window_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icons_[offset + 1]));
            shownState_ = state;
            shownPid_ = pid;
            UpdateTray();
        } else if (!tray_.IsAdded() && GetTickCount64() >= nextTrayAttempt_) {
            UpdateTray();
        }
        view_->Refresh();
    }

    void Command(int id, int notification)
    {
        const bool button = notification == BN_CLICKED &&
            (id == Run || id == Stop || id == Restart || id == SettingsButton ||
                id == OpenFile || id == OpenFileDirectory || id == OpenDirectory ||
                id == ClearLogs || id == ScrollToEnd);
        const bool selection = notification == CBN_SELCHANGE && (id == FileList || id == Filter);
        // Native controls notify their parent during creation and programmatic updates.
        if (!ready_ || updatingUi_ || (!button && !selection)) {
            return;
        }
        if (id == SettingsButton) {
            if (EditSettings(window_, root_, settings_, logs_, startup_)) {
                settingsValid_ = true;
                ApplyUiText();
                UpdateTheme(themeMonitor_->SystemDark());
                UpdatePaths();
            }
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
        return theme_.Handle(message, wParam, lParam, result);
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
        return SmokeTestIconResources() && SmokeTestUiChanges() && SmokeTestLogButtons() &&
            SmokeTestThemes() && SmokeTestLogon() && SmokeTestTray();
    }

private:
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
        for (const auto control : {scrollToEnd_, clearLogs_}) {
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
        for (const auto& [control, label] : std::array{
            std::pair{scrollToEnd_, Text(L"Scroll to end", L"滚动至底部")},
            std::pair{clearLogs_, Text(L"Clear logs", L"清空日志")}}) {
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
        wchar_t text[128]{};
        TOOLINFOW tool{};
        tool.cbSize = TTTOOLINFOW_V2_SIZE;
        tool.hwnd = window_;
        tool.uId = reinterpret_cast<UINT_PTR>(control);
        tool.lpszText = text;
        SendMessageW(tooltip_, TTM_GETTEXTW, std::size(text), reinterpret_cast<LPARAM>(&tool));
        return text;
    }

    void UpdateToolIcons(SvgIconRenderer& renderer)
    {
        IconBatch<4> next;
        const auto& colors = theme_.Colors();
        next.values[0] = renderer.CreateIcon(SvgIcon::ClearLogs, Scale(20), colors.text);
        next.values[1] = renderer.CreateIcon(SvgIcon::ClearLogs, Scale(20), colors.disabled);
        next.values[2] = renderer.CreateIcon(SvgIcon::ScrollToEnd, Scale(20), colors.text);
        next.values[3] = renderer.CreateIcon(SvgIcon::ScrollToEnd, Scale(20), colors.disabled);
        IconBatch<4> old;
        old.values = toolIcons_;
        toolIcons_ = next.Release();
        SetButtonIcons(clearLogs_, toolIcons_[0], toolIcons_[1]);
        SetButtonIcons(scrollToEnd_, toolIcons_[2], toolIcons_[3]);
        toolIconDpi_ = dpi_;
        toolIconColor_ = colors.text;
        toolDisabledColor_ = colors.disabled;
    }

    void UpdateTheme(bool systemDark, bool force = false)
    {
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
    }

    void UpdateTray()
    {
        if (exiting_ || !ready_) {
            return;
        }
        const auto state = shownState_;
        const bool running = shownPid_ != 0 && shownPid_ != MAXDWORD;
        const auto* status = state == ProcessState::Starting ? Text(L"Starting...", L"启动中…")
            : state == ProcessState::Stopping ? Text(L"Stopping...", L"停止中…")
            : running ? Text(L"Running", L"运行中") : Text(L"Stopped", L"已停止");
        const std::wstring tooltip = std::wstring(L"XTray — ") + status + L"\n" + root_.native();
        nextTrayAttempt_ = GetTickCount64() + 2000;
        if (tray_.Update(window_, icons_[running ? 3 : 0], tooltip)) {
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
            item.fType = MFT_OWNERDRAW;
            item.fState = available ? MFS_ENABLED : MFS_DISABLED;
            item.wID = static_cast<UINT>(id);
            item.dwTypeData = const_cast<wchar_t*>(caption);
            item.dwItemData = reinterpret_cast<ULONG_PTR>(&accessible);
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
            std::pair{settingsButton_, Text(L"Settings", L"设置")},
            std::pair{fileLabel_, Text(L"Config files:", L"配置文件：")},
            std::pair{openFile_, Text(L"Open file", L"打开配置文件")},
            std::pair{openFileDirectory_, Text(L"Open file folder", L"打开配置文件目录")},
            std::pair{directoryLabel_, Text(L"Config folder:", L"配置目录：")},
            std::pair{openDirectory_, Text(L"Open folder", L"打开配置目录")},
            std::pair{filterLabel_, Text(L"Log source:", L"日志来源：")},
            std::pair{clearLogs_, Text(L"Clear logs", L"清空日志")},
            std::pair{scrollToEnd_, Text(L"Scroll to end", L"滚动至底部")}
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
        shownPid_ = MAXDWORD;
        updatingUi_ = false;
    }

    bool SmokeTestIconResources()
    {
        std::fwprintf(stderr, L"Smoke: embedded yellow EXE icon and SVG tint resources.\n");
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
        for (const auto kind : {SvgIcon::Application, SvgIcon::ClearLogs, SvgIcon::ScrollToEnd}) {
            for (const int side : {16, 20, 24, 32}) {
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
        return SendMessageW(clearLogs_, BM_GETIMAGE, IMAGE_ICON, 0) ==
                reinterpret_cast<LRESULT>(toolIcons_[0]) &&
            SendMessageW(scrollToEnd_, BM_GETIMAGE, IMAGE_ICON, 0) ==
                reinterpret_cast<LRESULT>(toolIcons_[2]);
    }

    bool SmokeTestLogButtons()
    {
        std::fwprintf(stderr, L"Smoke: icon button tooltips and filtered scroll-to-end behavior.\n");
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
            Command(ScrollToEnd, BN_CLICKED);
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
            view_->Refresh();
            CHARRANGE after{};
            SendMessageW(logWindow_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&after));
            if (after.cpMin != selected.cpMin || after.cpMax != selected.cpMax ||
                SendMessageW(logWindow_, EM_GETFIRSTVISIBLELINE, 0, 0) != 0) {
                return false;
            }
        }
        SendMessageW(filter_, CB_SETCURSEL, 0, 0);
        Command(Filter, CBN_SELCHANGE);
        Command(ClearLogs, BN_CLICKED);
        return true;
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
            GetWindowRect(clearLogs_, &clear);
            GetWindowRect(logWindow_, &logBounds);
            GetWindowRect(scrollToEnd_, &scrollButton);
            if (clear.right != logBounds.right || clear.right - clear.left != Scale(32) ||
                clear.bottom - clear.top != Scale(32) || scrollButton.bottom != clear.bottom ||
                scrollButton.right + Scale(8) != clear.left) {
                return false;
            }
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
            WindowText(status_) != L"Stopped" || logs_.Revision() != revision ||
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
        for (const bool dark : {true, false, true, false}) {
            settings_.theme = dark ? ThemeMode::Dark : ThemeMode::Light;
            UpdateTheme(!dark);
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
            SendMessageW(run_, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(canvas.device), PRF_CLIENT);
            const auto button = GetPixel(canvas.device, 6, 6);
            if (button != palette.button && button != palette.hot && button != palette.pressed) {
                return fail(L"native button background");
            }
            SendMessageW(filter_, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(canvas.device), PRF_CLIENT);
            if (GetPixel(canvas.device, 2, 2) != palette.surface) {
                return fail(L"native combo background");
            }
            const HMENU menu = CreateTrayMenu();
            MENUITEMINFOW item{};
            item.cbSize = sizeof(item);
            item.fMask = MIIM_DATA | MIIM_FTYPE;
            const bool ownerDraw = GetMenuItemInfoW(menu, 0, TRUE, &item) &&
                (item.fType & MFT_OWNERDRAW) != 0 && item.dwItemData != 0;
            const auto* accessible = reinterpret_cast<const MSAAMENUINFO*>(item.dwItemData);
            if (!ownerDraw || accessible->dwMSAASignature != MSAA_MENU_SIG ||
                std::wstring_view(accessible->pszWText) != Text(L"Show GUI", L"显示 GUI")) {
                DestroyMenu(menu);
                return fail(L"tray menu ownership and accessible name");
            }
            DRAWITEMSTRUCT draw{};
            draw.CtlType = ODT_MENU;
            draw.hDC = canvas.device;
            draw.rcItem = {0, 0, 200, 30};
            draw.itemData = item.dwItemData;
            if (ownerDraw) {
                SendMessageW(window_, WM_DRAWITEM, 0, reinterpret_cast<LPARAM>(&draw));
            }
            const auto background = GetPixel(canvas.device, 2, 2);
            draw.itemState = ODS_SELECTED;
            if (ownerDraw) {
                SendMessageW(window_, WM_DRAWITEM, 0, reinterpret_cast<LPARAM>(&draw));
            }
            const auto highlighted = GetPixel(canvas.device, 2, 2);
            DestroyMenu(menu);
            if (!ownerDraw || background != palette.window || highlighted != palette.selection) {
                return fail(L"tray menu background and selection colors");
            }
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
            process_.State() != ProcessState::Stopped || smokeTrayData.hIcon != icons_[0]) {
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
        }) || IsWindowVisible(window_) || smokeTrayData.hIcon != icons_[0]) {
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
        }) || IsWindowVisible(window_) || smokeTrayData.hIcon != icons_[3]) {
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
        if (!tray_.IsAdded() || smokeTrayData.hIcon != icons_[0] ||
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
        if (!checkMenu(false)) {
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
            smokeTrayData.hIcon != icons_[3]) {
            return false;
        }
        const DWORD themePid = process_.Pid();
        settings_.theme = ThemeMode::Dark;
        UpdateTheme(false);
        if (!wait([&] { return hasOutput(L"stream 2"); }) || process_.Pid() != themePid ||
            !theme_.Colors().dark) {
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
        if (!tray_.IsAdded() || smokeTrayData.hIcon != icons_[3]) {
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
        if (std::wstring_view(smokeTrayData.szTip).find(L"Running") == std::wstring_view::npos) {
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
            smokeTrayData.hIcon != icons_[0] || !checkMenu(false)) {
            return false;
        }
        settings_ = original;
        TrayCommand(Run);
        if (!wait([&] { return process_.State() == ProcessState::Stopped; }) ||
            smokeTrayData.hIcon != icons_[0]) {
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
        const std::array controls{status_, run_, stop_, restart_, settingsButton_, fileLabel_,
            files_, openFile_, openFileDirectory_, directoryLabel_, directory_, openDirectory_,
            filterLabel_, filter_, scrollToEnd_, clearLogs_};
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
        IconBatch<6> next;
        for (int state = 0; state < 2; ++state) {
            const auto index = static_cast<size_t>(state * 3);
            const COLORREF color = state != 0 ? kRunningIconColor : kStoppedIconColor;
            next.values[index] = renderer.CreateIcon(SvgIcon::Application, Scale(16), color);
            next.values[index + 1] = renderer.CreateIcon(SvgIcon::Application, Scale(32), color);
            next.values[index + 2] = renderer.CreateIcon(SvgIcon::Application, Scale(24), color);
        }
        UpdateToolIcons(renderer);
        IconBatch<6> old;
        old.values = icons_;
        icons_ = next.Release();
        const size_t offset = process_.Pid() != 0 ? 3 : 0;
        SendMessageW(statusIcon_, STM_SETICON, reinterpret_cast<WPARAM>(icons_[offset + 2]), 0);
        SendMessageW(window_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icons_[offset]));
        SendMessageW(window_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icons_[offset + 1]));
        if (ready_) {
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
    bool smokeTest_;
    TrayIcon tray_;
    ThemeWindow theme_;
    std::unique_ptr<ThemeMonitor> themeMonitor_;
    bool changingTheme_ = false;
    bool exiting_ = false;
    bool logonStarted_ = false;
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
    HWND run_ = nullptr, stop_ = nullptr, restart_ = nullptr, settingsButton_ = nullptr;
    HWND fileLabel_ = nullptr, files_ = nullptr, openFile_ = nullptr, openFileDirectory_ = nullptr;
    HWND directoryLabel_ = nullptr, directory_ = nullptr, openDirectory_ = nullptr;
    HWND filterLabel_ = nullptr, filter_ = nullptr, scrollToEnd_ = nullptr, clearLogs_ = nullptr;
    HWND logWindow_ = nullptr, tooltip_ = nullptr;
    HFONT font_ = nullptr, logFont_ = nullptr;
    std::array<HICON, 6> icons_{};
    std::array<HICON, 4> toolIcons_{};
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
            if (AdjustWindowRectExForDpi(&bounds, kWindowStyle, FALSE, 0, dpi)) {
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
    } catch (const std::exception& exception) {
        std::fwprintf(stderr, L"Window operation failed: %hs\n", exception.what());
        if (message == WM_CREATE) {
            if (app != nullptr) {
                app->startupError = std::current_exception();
            }
            return -1;
        }
        if (app != nullptr) {
            app->exitCode = 1;
        }
        PostMessageW(window, kExitMessage, 0, 0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int showCommand)
{
    const bool smokeTest = std::wstring_view(commandLine) == L"--smoke-test";
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
        if (!AdjustWindowRectExForDpi(&bounds, kWindowStyle, FALSE, 0, dpi)) {
            return ReportStartupError(smokeTest, WindowsError(Text(L"Cannot calculate the window size", L"计算窗口尺寸失败")));
        }
        LONG width = bounds.right - bounds.left;
        LONG height = bounds.bottom - bounds.top;
        width = (std::min)(width, workArea.right - workArea.left);
        height = (std::min)(height, workArea.bottom - workArea.top);
        const HWND window = CreateWindowExW(0, className.c_str(), L"XTray", kWindowStyle,
            workArea.left, workArea.top, width, height, nullptr, nullptr, instance, &app);
        if (window == nullptr) {
            if (app.startupError) {
                std::rethrow_exception(app.startupError);
            }
            return ReportStartupError(smokeTest, WindowsError(Text(L"Cannot create the window or its controls", L"创建窗口或控件失败")));
        }
        // The hidden window is already on the target monitor, so this DPI is its actual DPI.
        const UINT windowDpi = GetDpiForWindow(window);
        bounds = {0, 0, MulDiv(800, static_cast<int>(windowDpi), 96),
            MulDiv(600, static_cast<int>(windowDpi), 96)};
        if (!AdjustWindowRectExForDpi(&bounds, kWindowStyle, FALSE, 0, windowDpi)) {
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
        if (smokeTest) {
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
            if (!IsDialogMessageW(window, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        if (result == -1) {
            return ReportStartupError(smokeTest, WindowsError(
                Text(L"The window message loop failed", L"窗口消息循环失败")));
        }
        return app.exitCode != 0 ? app.exitCode : static_cast<int>(message.wParam);
    } catch (const std::exception& exception) {
        return ReportStartupError(smokeTest, Text(L"XTray could not start: ", L"XTray 启动失败：") + FromUtf8(exception.what()));
    }
}
