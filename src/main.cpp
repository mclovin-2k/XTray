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
#include <memory>
#include <string_view>

#if !defined(_M_X64)
#error XTray supports Windows x64 only.
#endif

namespace {

using namespace xtray;

constexpr wchar_t kWindowClass[] = L"XTray.MainWindow";
constexpr DWORD kWindowStyle = WS_OVERLAPPEDWINDOW;
enum ControlId { Run = 2001, Stop, Restart, SettingsButton, FileList, OpenFile,
    OpenFileDirectory, OpenDirectory, Filter, ClearLogs };

int ReportStartupError(bool smokeTest, const std::wstring& error)
{
    if (smokeTest) {
        std::fwprintf(stderr, L"%ls\n", error.c_str());
    } else {
        MessageBoxW(nullptr, error.c_str(), L"XTray", MB_OK | MB_ICONERROR);
    }
    return 1;
}

HICON StatusIcon(int size, bool running)
{
    BITMAPINFO information{};
    information.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    information.bmiHeader.biWidth = size;
    information.bmiHeader.biHeight = -size;
    information.bmiHeader.biPlanes = 1;
    information.bmiHeader.biBitCount = 32;
    void* data = nullptr;
    HBITMAP color = CreateDIBSection(nullptr, &information, DIB_RGB_COLORS, &data, nullptr, 0);
    if (color == nullptr) {
        return nullptr;
    }
    auto* pixels = static_cast<DWORD*>(data);
    const int radius = size * 2 / 5;
    const DWORD fill = running ? 0xFF16803C : 0xFFCC3030;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const int dx = x - size / 2;
            const int dy = y - size / 2;
            pixels[y * size + x] = dx * dx + dy * dy <= radius * radius ? fill : 0;
        }
    }
    const size_t maskStride = static_cast<size_t>((size + 15) / 16 * 2);
    const std::vector<BYTE> maskBytes(maskStride * static_cast<size_t>(size), 0);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, maskBytes.data());
    if (mask == nullptr) {
        DeleteObject(color);
        return nullptr;
    }
    ICONINFO icon{TRUE, 0, 0, mask, color};
    HICON result = CreateIconIndirect(&icon);
    DeleteObject(mask);
    DeleteObject(color);
    return result;
}

class Application {
public:
    Application(std::filesystem::path root, bool smokeTest)
        : root_(std::move(root)), process_(root_, logs_), smokeTest_(smokeTest) {}
    ~Application()
    {
        process_.Shutdown();
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
    }

    bool Initialize(HWND window)
    {
        window_ = window;
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
        files_ = child(0, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, FileList);
        openFile_ = child(0, L"BUTTON", L"", WS_TABSTOP, OpenFile);
        openFileDirectory_ = child(0, L"BUTTON", L"", WS_TABSTOP, OpenFileDirectory);
        directoryLabel_ = child(0, L"STATIC", L"", SS_CENTERIMAGE);
        directory_ = child(WS_EX_CLIENTEDGE, L"EDIT", L"",
            ES_READONLY | ES_AUTOHSCROLL | WS_TABSTOP);
        openDirectory_ = child(0, L"BUTTON", L"", WS_TABSTOP, OpenDirectory);
        filterLabel_ = child(0, L"STATIC", L"", SS_CENTERIMAGE);
        filter_ = child(0, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, Filter);
        clearLogs_ = child(0, L"BUTTON", L"", WS_TABSTOP, ClearLogs);
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
        ApplyLanguage(settings_.language);
        ApplyUiText();
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
        MoveWindow(clearLogs_, width - Scale(16) - Scale(112), Scale(136),
            Scale(112), Scale(26), TRUE);
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
        }
        view_->Refresh();
    }

    void Command(int id, int notification)
    {
        const bool button = notification == BN_CLICKED &&
            (id == Run || id == Stop || id == Restart || id == SettingsButton ||
                id == OpenFile || id == OpenFileDirectory || id == OpenDirectory || id == ClearLogs);
        const bool selection = notification == CBN_SELCHANGE && (id == FileList || id == Filter);
        // Native controls notify their parent during creation and programmatic updates.
        if (!ready_ || updatingUi_ || (!button && !selection)) {
            return;
        }
        if (id == SettingsButton) {
            if (EditSettings(window_, root_, settings_, logs_)) {
                settingsValid_ = true;
                ApplyUiText();
                UpdatePaths();
            }
        } else if (id == ClearLogs) {
            view_->Clear();
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

    void Shutdown() { process_.Shutdown(); }
    int exitCode = 0;

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
        return SmokeTestUiChanges();
    }

private:
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
            std::pair{clearLogs_, Text(L"Clear logs", L"清空日志")}
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
        shownPid_ = MAXDWORD;
        updatingUi_ = false;
    }

    bool SmokeTestUiChanges()
    {
        std::fwprintf(stderr, L"Smoke: centered window and clear-button alignment.\n");
        RECT bounds{}, clear{}, logBounds{};
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
            if (clear.right != logBounds.right) {
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
            WindowText(clearLogs_) == L"清空日志";
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
            filterLabel_, filter_, clearLogs_};
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
        view_->InvalidateFormat();

        std::array<HICON, 6> icons{};
        for (int state = 0; state < 2; ++state) {
            const auto index = static_cast<size_t>(state * 3);
            icons[index] = StatusIcon(Scale(16), state != 0);
            icons[index + 1] = StatusIcon(Scale(32), state != 0);
            icons[index + 2] = StatusIcon(Scale(24), state != 0);
        }
        for (const auto icon : icons) {
            if (icon == nullptr) {
                for (const auto created : icons) {
                    if (created != nullptr) {
                        DestroyIcon(created);
                    }
                }
                throw std::runtime_error(Text("Cannot create state icons.", "无法创建状态图标。"));
            }
        }
        const auto old = icons_;
        icons_ = icons;
        const size_t offset = process_.Pid() != 0 ? 3 : 0;
        SendMessageW(statusIcon_, STM_SETICON, reinterpret_cast<WPARAM>(icons_[offset + 2]), 0);
        SendMessageW(window_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icons_[offset]));
        SendMessageW(window_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icons_[offset + 1]));
        for (const auto icon : old) {
            if (icon != nullptr) {
                DestroyIcon(icon);
            }
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
    Logs logs_;
    XrayProcess process_;
    bool smokeTest_;
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
    HWND filterLabel_ = nullptr, filter_ = nullptr, clearLogs_ = nullptr, logWindow_ = nullptr;
    HFONT font_ = nullptr, logFont_ = nullptr;
    std::array<HICON, 6> icons_{};
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
            switch (message) {
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
                [[fallthrough]];
            case WM_CLOSE:
                app->Shutdown();
                DestroyWindow(window);
                return 0;
            case WM_DESTROY:
                KillTimer(window, 1);
                PostQuitMessage(app->exitCode);
                return 0;
            }
        }
    } catch (const std::exception& exception) {
        std::fwprintf(stderr, L"Window operation failed: %hs\n", exception.what());
        if (message == WM_CREATE) {
            return -1;
        }
        if (app != nullptr) {
            app->exitCode = 1;
        }
        PostMessageW(window, WM_CLOSE, 0, 0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int showCommand)
{
    const bool smokeTest = std::wstring_view(commandLine) == L"--smoke-test";
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
        Application app(ExecutablePath().parent_path(), smokeTest);
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = WindowProcedure;
        windowClass.hInstance = instance;
        windowClass.hCursor = static_cast<HCURSOR>(LoadImageW(
            nullptr, IDC_ARROW, IMAGE_CURSOR, 0, 0, LR_DEFAULTSIZE | LR_SHARED));
        windowClass.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        windowClass.lpszClassName = kWindowClass;
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
        const HWND window = CreateWindowExW(0, kWindowClass, L"XTray", kWindowStyle,
            workArea.left, workArea.top, width, height, nullptr, nullptr, instance, &app);
        if (window == nullptr) {
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
                app.exitCode = ReportStartupError(true, Text(L"The Rich Edit log UI smoke test failed.", L"Rich Edit 日志界面自检失败。"));
            }
            if (!PostMessageW(window, WM_CLOSE, 0, 0)) {
                app.Shutdown();
                DestroyWindow(window);
                return ReportStartupError(true, WindowsError(Text(L"Cannot close the test window", L"关闭测试窗口失败")));
            }
        } else {
            ShowWindow(window, showCommand);
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
        return result == -1 ? ReportStartupError(smokeTest, WindowsError(Text(L"The window message loop failed", L"窗口消息循环失败")))
                            : static_cast<int>(message.wParam);
    } catch (const std::exception& exception) {
        return ReportStartupError(smokeTest, Text(L"XTray could not start: ", L"XTray 启动失败：") + FromUtf8(exception.what()));
    }
}
