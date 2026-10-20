#include "settings_dialog.h"
#include "resource.h"
#include "win32.h"
#include "theme_window.h"
#include "ui_layout.h"

#include <shobjidl.h>
#include <uxtheme.h>
#include <wrl/client.h>

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstdio>
#include <cwchar>
#include <exception>
#include <functional>
#include <map>
#include <stdexcept>

namespace xtray {
namespace {

using Microsoft::WRL::ComPtr;
constexpr UINT kRefreshDialogTheme = WM_APP + 46;
constexpr UINT kAutomateDialog = WM_APP + 47;
constexpr UINT kEnsureSettingVisible = WM_APP + 48;
constexpr UINT_PTR kSettingFocusSubclass = 3;

struct Context {
    const std::filesystem::path& root;
    Settings& settings;
    Logs& logs;
    StartupRegistration& startup;
    bool startupAvailable = false;
    ThemeWindow theme;
    ThemeWindow contentTheme;
    HWND contents = nullptr;
    int scroll = 0;
    int contentHeight = 0;
    int wheelRemainder = 0;
    std::function<void(HWND)> automation;
    std::exception_ptr error;
};

HWND SettingsControl(HWND dialog, int id)
{
    const HWND direct = GetDlgItem(dialog, id);
    return direct != nullptr ? direct : GetDlgItem(GetDlgItem(dialog, IDC_SETTINGS_BODY), id);
}

void CheckSettingsButton(HWND dialog, int id, UINT state)
{
    SendMessageW(SettingsControl(dialog, id), BM_SETCHECK, state, 0);
}

UINT SettingsButtonState(HWND dialog, int id)
{
    return static_cast<UINT>(SendMessageW(SettingsControl(dialog, id), BM_GETCHECK, 0, 0));
}

void LayoutSettingsContent(Context& context)
{
    const HWND body = context.contents;
    struct Placement { HWND control; int x, y, width, height; };
    std::vector<Placement> placements;
    const int margin = UiScale(body, 16);
    const int gap = UiScale(body, 8);
    const int sectionGap = UiScale(body, 14);
    RECT units{0, 0, 0, 15};
    MapDialogRect(body, &units);
    const int rowHeight = (std::max)(static_cast<int>(units.bottom),
        static_cast<int>(ControlTextSize(GetDlgItem(body, IDC_EXECUTABLE)).cy) + UiScale(body, 8));
    int contentHeight = 0;
    for (int pass = 0; pass < 2; ++pass) {
        RECT client{};
        GetClientRect(body, &client);
        const int available = (std::max)(1, static_cast<int>(client.right) - margin * 2);
        placements.clear();
        const auto place = [&](int id, int x, int y, int width, int height) {
            placements.push_back({GetDlgItem(body, id), x, y, (std::max)(1, width), height});
        };
        int y = margin;
        const auto label = [&](int id) {
            const int height = (std::max)(1L, ControlTextSize(GetDlgItem(body, id), available).cy);
            place(id, margin, y, available, height);
            y += height + gap;
        };
        const auto pathRow = [&](int editId, int browseId, int height) {
            const int buttonWidth = TextButtonWidth(GetDlgItem(body, browseId));
            const int buttonX = margin + available - buttonWidth;
            place(browseId, buttonX, y, buttonWidth, rowHeight);
            place(editId, margin, y + (std::max)(0, (rowHeight - height) / 2),
                buttonX - gap - margin, height);
            y += (std::max)(height, rowHeight) + sectionGap;
        };
        label(IDC_LABEL_EXECUTABLE);
        pathRow(IDC_EXECUTABLE, IDC_BROWSE_EXECUTABLE, SingleLineControlHeight(GetDlgItem(body, IDC_EXECUTABLE)));
        label(IDC_LABEL_CONFIG_FILES);
        units = {0, 0, 0, 75};
        MapDialogRect(body, &units);
        pathRow(IDC_CONFIG_FILES, IDC_ADD_FILES, static_cast<int>(units.bottom));
        label(IDC_LABEL_CONFIG_DIRECTORY);
        pathRow(IDC_CONFIG_DIRECTORY, IDC_BROWSE_DIRECTORY, SingleLineControlHeight(GetDlgItem(body, IDC_CONFIG_DIRECTORY)));
        const auto logLabel = ControlTextSize(GetDlgItem(body, IDC_LABEL_LOG_LINES));
        const int logHeight = SingleLineControlHeight(GetDlgItem(body, IDC_LOG_LINES));
        const int logRow = (std::max)(static_cast<int>(logLabel.cy), logHeight);
        place(IDC_LABEL_LOG_LINES, margin, y + (logRow - logLabel.cy) / 2, logLabel.cx, logLabel.cy);
        place(IDC_LOG_LINES, margin + logLabel.cx + gap, y + (logRow - logHeight) / 2,
            (std::min)(UiScale(body, 112), available - static_cast<int>(logLabel.cx) - gap), logHeight);
        y += logRow + sectionGap;
        const int languageLabel = static_cast<int>(ControlTextSize(GetDlgItem(body, IDC_LABEL_LANGUAGE)).cx);
        const int themeLabel = static_cast<int>(ControlTextSize(GetDlgItem(body, IDC_LABEL_THEME)).cx);
        const int half = (available - sectionGap) / 2;
        const bool stacked = half < (std::max)(languageLabel, themeLabel) + gap + UiScale(body, 96);
        const int groupWidth = stacked ? available : half;
        place(IDC_LABEL_LANGUAGE, margin, y, languageLabel, rowHeight);
        place(IDC_LANGUAGE, margin + languageLabel + gap, y,
            groupWidth - languageLabel - gap, rowHeight + UiScale(body, 160));
        if (stacked) { y += rowHeight + gap; }
        const int themeX = stacked ? margin : margin + half + sectionGap;
        place(IDC_LABEL_THEME, themeX, y, themeLabel, rowHeight);
        place(IDC_THEME, themeX + themeLabel + gap, y,
            available + margin - themeX - themeLabel - gap, rowHeight + UiScale(body, 160));
        y += rowHeight + sectionGap;
        place(IDC_LOGON_STARTUP, margin, y, available, rowHeight);
        y += rowHeight + sectionGap;
        label(IDC_HINT_RELATIVE);
        label(IDC_HINT_APPLY);
        contentHeight = y + margin - gap;
        const int maximum = (std::max)(0, contentHeight - static_cast<int>(client.bottom));
        context.scroll = std::clamp(context.scroll, 0, maximum);
        SCROLLINFO scroll{sizeof(SCROLLINFO), SIF_RANGE | SIF_PAGE | SIF_POS};
        scroll.nMax = (std::max)(0, contentHeight - 1);
        scroll.nPage = static_cast<UINT>((std::max)(1L, client.bottom));
        scroll.nPos = context.scroll;
        SetScrollInfo(body, SB_VERT, &scroll, TRUE);
        RECT updated{};
        GetClientRect(body, &updated);
        if (updated.right == client.right) { break; }
    }
    context.contentHeight = contentHeight;
    for (const auto& placement : placements) {
        MoveWindow(placement.control, placement.x, placement.y - context.scroll,
            placement.width, placement.height, TRUE);
    }
    InvalidateRect(body, nullptr, TRUE);
}

void LayoutSettings(HWND dialog, Context& context)
{
    if (context.contents == nullptr) { return; }
    RECT client{};
    GetClientRect(dialog, &client);
    const int margin = UiScale(dialog, 16);
    const int gap = UiScale(dialog, 8);
    const HWND save = GetDlgItem(dialog, IDOK), cancel = GetDlgItem(dialog, IDCANCEL);
    const int saveWidth = TextButtonWidth(save), cancelWidth = TextButtonWidth(cancel);
    const int height = (std::max)(ControlTextSize(save).cy, ControlTextSize(cancel).cy) + UiScale(dialog, 12);
    const int buttonY = (std::max)(0, static_cast<int>(client.bottom) - margin - height);
    const int bodyHeight = (std::max)(1, buttonY - margin);
    MoveWindow(save, client.right - margin - cancelWidth - gap - saveWidth, buttonY, saveWidth, height, TRUE);
    MoveWindow(cancel, client.right - margin - cancelWidth, buttonY, cancelWidth, height, TRUE);
    MoveWindow(context.contents, 0, 0, client.right, bodyHeight, TRUE);
    LayoutSettingsContent(context);
}

void FitSettingsWindow(HWND dialog, Context& context)
{
    MONITORINFO monitor{sizeof(MONITORINFO)};
    RECT bounds{};
    if (!GetWindowRect(dialog, &bounds) ||
        !GetMonitorInfoW(MonitorFromWindow(dialog, MONITOR_DEFAULTTONEAREST), &monitor)) {
        throw std::runtime_error(Text("Cannot read the settings display area.", "无法读取设置窗口的显示区域。"));
    }
    const auto& work = monitor.rcWork;
    const LONG width = (std::min)(bounds.right - bounds.left, work.right - work.left);
    const LONG height = (std::min)(bounds.bottom - bounds.top, work.bottom - work.top);
    SetWindowPos(dialog, nullptr, std::clamp(bounds.left, work.left, work.right - width),
        std::clamp(bounds.top, work.top, work.bottom - height), width, height,
        SWP_NOZORDER | SWP_NOACTIVATE);
    LayoutSettings(dialog, context);
    RECT viewport{};
    GetClientRect(context.contents, &viewport);
    GetWindowRect(dialog, &bounds);
    const LONG preferredHeight = bounds.bottom - bounds.top +
        (std::max)(0, context.contentHeight - static_cast<int>(viewport.bottom));
    const LONG fittedHeight = (std::min)(preferredHeight, work.bottom - work.top);
    SetWindowPos(dialog, nullptr, bounds.left,
        std::clamp(bounds.top, work.top, work.bottom - fittedHeight),
        width, fittedHeight, SWP_NOZORDER | SWP_NOACTIVATE);
}

LRESULT CALLBACK SettingsFocusProcedure(HWND control, UINT message, WPARAM wParam,
    LPARAM lParam, UINT_PTR, DWORD_PTR body)
{
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(control, SettingsFocusProcedure, kSettingFocusSubclass);
    }
    const auto result = DefSubclassProc(control, message, wParam, lParam);
    if (message == WM_SETFOCUS) {
        PostMessageW(reinterpret_cast<HWND>(body), kEnsureSettingVisible, reinterpret_cast<WPARAM>(control), 0);
    }
    return result;
}

INT_PTR CALLBACK SettingsContentsProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* context = reinterpret_cast<Context*>(GetWindowLongPtrW(window, DWLP_USER));
    try {
        if (message == WM_INITDIALOG) {
            context = reinterpret_cast<Context*>(lParam);
            SetWindowLongPtrW(window, DWLP_USER, lParam);
            context->contents = window;
            SetWindowLongPtrW(window, GWLP_ID, IDC_SETTINGS_BODY);
            context->contentTheme.Attach(window);
            struct FocusControls { HWND body; bool success = true; } controls{window};
            EnumChildWindows(window, [](HWND control, LPARAM parameter) -> BOOL {
                auto& value = *reinterpret_cast<FocusControls*>(parameter);
                if ((GetWindowLongPtrW(control, GWL_STYLE) & WS_TABSTOP) != 0 &&
                    !SetWindowSubclass(control, SettingsFocusProcedure, kSettingFocusSubclass,
                        reinterpret_cast<DWORD_PTR>(value.body))) {
                    value.success = false;
                    return FALSE;
                }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&controls));
            if (!controls.success) {
                throw std::runtime_error(Text("Cannot enable settings keyboard scrolling.", "无法初始化设置窗口的键盘滚动。"));
            }
            return FALSE;
        }
        if (context == nullptr) { return FALSE; }
        if (message == WM_NCDESTROY) {
            context->contentTheme.Detach();
            context->contents = nullptr;
            SetWindowLongPtrW(window, DWLP_USER, 0);
            return FALSE;
        }
        LRESULT themed = 0;
        if (context->contentTheme.Handle(message, wParam, lParam, themed)) {
            if (message == WM_NOTIFY) {
                SetWindowLongPtrW(window, DWLP_MSGRESULT, themed);
                return TRUE;
            }
            return themed;
        }
        if (message == WM_COMMAND) {
            SendMessageW(GetParent(window), message, wParam, lParam);
            return TRUE;
        }
        if (message == WM_VSCROLL || message == WM_MOUSEWHEEL || message == kEnsureSettingVisible) {
            RECT client{};
            GetClientRect(window, &client);
            if (message == kEnsureSettingVisible) {
                const HWND control = reinterpret_cast<HWND>(wParam);
                if (!IsChild(window, control) || GetFocus() != control) { return TRUE; }
                RECT bounds{};
                GetWindowRect(control, &bounds);
                MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&bounds), 2);
                const int margin = UiScale(window, 8);
                if (bounds.top < margin || bounds.bottom - bounds.top > client.bottom - margin * 2) {
                    context->scroll += bounds.top - margin;
                } else if (bounds.bottom > client.bottom - margin) {
                    context->scroll += bounds.bottom - client.bottom + margin;
                }
            } else if (message == WM_MOUSEWHEEL) {
                UINT lines = 3;
                SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
                context->wheelRemainder += static_cast<short>(HIWORD(wParam));
                const int steps = context->wheelRemainder / WHEEL_DELTA;
                context->wheelRemainder %= WHEEL_DELTA;
                context->scroll -= steps * (lines == WHEEL_PAGESCROLL ? static_cast<int>(client.bottom)
                    : static_cast<int>(lines) * UiScale(window, 24));
            } else {
                SCROLLINFO scroll{sizeof(SCROLLINFO), SIF_TRACKPOS | SIF_RANGE | SIF_PAGE};
                GetScrollInfo(window, SB_VERT, &scroll);
                switch (LOWORD(wParam)) {
                case SB_TOP: context->scroll = 0; break;
                case SB_BOTTOM: context->scroll = scroll.nMax; break;
                case SB_LINEUP: context->scroll -= UiScale(window, 24); break;
                case SB_LINEDOWN: context->scroll += UiScale(window, 24); break;
                case SB_PAGEUP: context->scroll -= client.bottom; break;
                case SB_PAGEDOWN: context->scroll += client.bottom; break;
                case SB_THUMBPOSITION:
                case SB_THUMBTRACK: context->scroll = scroll.nTrackPos; break;
                }
            }
            LayoutSettingsContent(*context);
            return TRUE;
        }
        return FALSE;
    } catch (...) {
        if (context != nullptr) { context->error = std::current_exception(); }
        EndDialog(GetParent(window), -1);
        return TRUE;
    }
}

class MemoryStartupRegistry final : public StartupRegistry {
public:
    bool Read(const std::wstring& name, std::optional<std::wstring>& command,
        std::wstring& error) override
    {
        if (failRead) {
            error = L"Injected login-startup query failure.";
            return false;
        }
        const auto found = values.find(name);
        command = found == values.end() ? std::nullopt : std::optional<std::wstring>(found->second);
        return true;
    }

    bool Write(const std::wstring& name, const std::optional<std::wstring>& command,
        std::wstring&) override
    {
        ++writes;
        if (command) {
            values[name] = *command;
        } else {
            values.erase(name);
        }
        return true;
    }

    std::map<std::wstring, std::wstring> values;
    size_t writes = 0;
    bool failRead = false;
};

std::filesystem::path CurrentExecutable()
{
    std::wstring path(32768, L'\0');
    const auto copied = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (copied == 0 || copied >= path.size()) {
        throw std::runtime_error("Cannot locate the settings smoke-test executable.");
    }
    path.resize(copied);
    return path;
}

std::vector<std::wstring> Browse(HWND owner, bool directory, bool multiple)
{
    ComPtr<IFileOpenDialog> dialog;
    HRESULT result = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(dialog.GetAddressOf()));
    if (FAILED(result)) {
        MessageBoxW(owner, Text(L"Cannot open the Windows file picker.", L"无法打开 Windows 文件选择对话框。"), L"XTray", MB_OK | MB_ICONERROR);
        return {};
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_DONTADDTORECENT;
    options |= directory ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST;
    if (multiple) {
        options |= FOS_ALLOWMULTISELECT;
    }
    if (FAILED(dialog->SetOptions(options))) {
        MessageBoxW(owner, Text(L"Cannot configure the Windows file picker.", L"无法设置 Windows 文件选择对话框。"), L"XTray", MB_OK | MB_ICONERROR);
        return {};
    }
    result = dialog->Show(owner);
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        return {};
    }
    ComPtr<IShellItemArray> items;
    if (FAILED(result) || FAILED(dialog->GetResults(items.GetAddressOf()))) {
        MessageBoxW(owner, Text(L"Cannot read the selected paths.", L"无法读取选择结果。"), L"XTray", MB_OK | MB_ICONERROR);
        return {};
    }
    DWORD count = 0;
    if (FAILED(items->GetCount(&count))) {
        return {};
    }
    std::vector<std::wstring> paths;
    for (DWORD index = 0; index < count; ++index) {
        ComPtr<IShellItem> item;
        PWSTR path = nullptr;
        if (SUCCEEDED(items->GetItemAt(index, item.GetAddressOf())) &&
            SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            paths.emplace_back(path);
            CoTaskMemFree(path);
        }
    }
    return paths;
}

void SetDialogLanguage(HWND dialog)
{
    SetWindowTextW(dialog, Text(L"XTray Settings", L"XTray 设置"));
    const auto set = [dialog](int id, const wchar_t* text) { SetWindowTextW(SettingsControl(dialog, id), text); };
    set(IDC_LABEL_EXECUTABLE, Text(L"Xray executable:", L"Xray 程序路径："));
    set(IDC_LABEL_CONFIG_FILES, Text(L"Config files (one per line, in load order; optional):",
        L"配置文件（每行一个，按顺序加载；可留空）："));
    set(IDC_LABEL_CONFIG_DIRECTORY, Text(L"Config directory (optional):", L"配置目录（可留空）："));
    set(IDC_LABEL_LOG_LINES, Text(L"Log lines per source:", L"每类日志保留行数："));
    set(IDC_LABEL_LANGUAGE, Text(L"Language:", L"语言："));
    set(IDC_LABEL_THEME, Text(L"Appearance:", L"外观："));
    set(IDC_LOGON_STARTUP, Text(L"Start XTray and Xray when I sign in", L"登录后自动启动 XTray 和 Xray"));
    set(IDC_BROWSE_EXECUTABLE, Text(L"Browse...", L"浏览…"));
    set(IDC_ADD_FILES, Text(L"Add files...", L"添加文件…"));
    set(IDC_BROWSE_DIRECTORY, Text(L"Browse...", L"浏览…"));
    set(IDC_HINT_RELATIVE, Text(L"Relative paths use the XTray.exe directory.",
        L"相对路径以 XTray.exe 所在目录为基准。"));
    set(IDC_HINT_APPLY, Text(L"Paths apply on the next run/restart; at least one config source is required.",
        L"路径下次运行或重启生效；运行前至少设置一种配置来源。"));
    set(IDOK, Text(L"Save", L"保存"));
    set(IDCANCEL, Text(L"Cancel", L"取消"));
    for (const auto* item : {Text(L"Automatic", L"自动"), Text(L"English", L"英文"),
        Text(L"Chinese", L"中文")}) {
        SendMessageW(SettingsControl(dialog, IDC_LANGUAGE), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
    }
    for (const auto* item : {Text(L"Follow system", L"跟随系统"), Text(L"Light", L"浅色"),
        Text(L"Dark", L"深色")}) {
        SendMessageW(SettingsControl(dialog, IDC_THEME), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
    }
}

INT_PTR SettingsMessage(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* context = reinterpret_cast<Context*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        context = reinterpret_cast<Context*>(lParam);
        SetWindowLongPtrW(dialog, DWLP_USER, lParam);
        context->theme.Attach(dialog);
        if (CreateDialogParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_SETTINGS_CONTENT),
            dialog, SettingsContentsProcedure, lParam) == nullptr) {
            throw std::runtime_error(Text("Cannot create the settings content area.", "无法创建设置内容区。"));
        }
        SetWindowPos(context->contents, HWND_TOP, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        SetDialogLanguage(dialog);
        SendMessageW(SettingsControl(dialog, IDC_LANGUAGE), CB_SETCURSEL,
            static_cast<WPARAM>(context->settings.language), 0);
        SendMessageW(SettingsControl(dialog, IDC_THEME), CB_SETCURSEL,
            static_cast<WPARAM>(context->settings.theme), 0);
        SetWindowTextW(SettingsControl(dialog, IDC_EXECUTABLE), context->settings.executable.c_str());
        SetWindowTextW(SettingsControl(dialog, IDC_CONFIG_DIRECTORY), context->settings.configDirectory.c_str());
        std::wstring files;
        for (const auto& file : context->settings.configFiles) {
            files += file + L"\r\n";
        }
        SetWindowTextW(SettingsControl(dialog, IDC_CONFIG_FILES), files.c_str());
        SetWindowTextW(SettingsControl(dialog, IDC_LOG_LINES), std::to_wstring(context->settings.logLines).c_str());
        const auto startup = context->startup.Query();
        context->startupAvailable = startup.success;
        CheckSettingsButton(dialog, IDC_LOGON_STARTUP, startup.enabled ? BST_CHECKED : BST_UNCHECKED);
        if (!startup.success) {
            context->logs.Tray(startup.error);
            SetWindowTextW(SettingsControl(dialog, IDC_LOGON_STARTUP), Text(
                L"Login startup unavailable (see XTray log)", L"登录自启不可用（请查看 XTray 日志）"));
            EnableWindow(SettingsControl(dialog, IDC_LOGON_STARTUP), FALSE);
        }
        const auto* owner = ThemeWindow::From(GetWindow(dialog, GW_OWNER));
        context->theme.Apply(owner != nullptr && owner->Colors().dark, true);
        FitSettingsWindow(dialog, *context);
        LayoutSettings(dialog, *context);
        SetFocus(SettingsControl(dialog, IDC_EXECUTABLE));
        if (context->automation) {
            PostMessageW(dialog, kAutomateDialog, 0, 0);
        }
        return FALSE;
    }
    if (context == nullptr) {
        return FALSE;
    }
    if (message == WM_NCDESTROY) {
        context->theme.Detach();
        SetWindowLongPtrW(dialog, DWLP_USER, 0);
        return FALSE;
    }
    if (message == WM_SIZE) {
        LayoutSettings(dialog, *context);
        return FALSE;
    }
    if (message == WM_MOUSEWHEEL) {
        SendMessageW(context->contents, message, wParam, lParam);
        return TRUE;
    }
    if (message == WM_DPICHANGED || message == WM_DISPLAYCHANGE) {
        PostMessageW(dialog, kRefreshDialogTheme, 0, 0);
        return FALSE;
    }
    if (message == kRefreshDialogTheme) {
        context->theme.Apply(context->theme.Colors().dark, true);
        FitSettingsWindow(dialog, *context);
        LayoutSettings(dialog, *context);
        return TRUE;
    }
    if (message == kAutomateDialog && context->automation) {
        context->automation(dialog);
        return TRUE;
    }
    LRESULT themed = 0;
    if (context->theme.Handle(message, wParam, lParam, themed)) {
        if (message == WM_NOTIFY) {
            SetWindowLongPtrW(dialog, DWLP_MSGRESULT, themed);
            return TRUE;
        }
        return themed;
    }
    if (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(wParam) == IDCANCEL)) {
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    if (message != WM_COMMAND) {
        return FALSE;
    }
    switch (LOWORD(wParam)) {
    case IDC_BROWSE_EXECUTABLE:
    case IDC_ADD_FILES:
    case IDC_BROWSE_DIRECTORY: {
        const auto id = LOWORD(wParam);
        const auto paths = Browse(dialog, id == IDC_BROWSE_DIRECTORY, id == IDC_ADD_FILES);
        if (paths.empty()) {
            return TRUE;
        }
        if (id == IDC_ADD_FILES) {
            std::wstring text = WindowText(SettingsControl(dialog, IDC_CONFIG_FILES));
            if (!text.empty() && text.back() != L'\n') {
                text += L"\r\n";
            }
            for (const auto& path : paths) {
                text += path + L"\r\n";
            }
            SetWindowTextW(SettingsControl(dialog, IDC_CONFIG_FILES), text.c_str());
        } else {
            SetWindowTextW(SettingsControl(dialog, id == IDC_BROWSE_EXECUTABLE
                ? IDC_EXECUTABLE : IDC_CONFIG_DIRECTORY), paths.front().c_str());
        }
        return TRUE;
    }
    case IDOK: {
        Settings draft = context->settings;
        draft.language = static_cast<Language>(
            SendMessageW(SettingsControl(dialog, IDC_LANGUAGE), CB_GETCURSEL, 0, 0));
        draft.theme = static_cast<ThemeMode>(
            SendMessageW(SettingsControl(dialog, IDC_THEME), CB_GETCURSEL, 0, 0));
        draft.executable = WindowText(SettingsControl(dialog, IDC_EXECUTABLE));
        draft.configDirectory = WindowText(SettingsControl(dialog, IDC_CONFIG_DIRECTORY));
        draft.configFiles = SplitConfigFiles(WindowText(SettingsControl(dialog, IDC_CONFIG_FILES)));
        const auto number = WindowText(SettingsControl(dialog, IDC_LOG_LINES));
        wchar_t* end = nullptr;
        errno = 0;
        const auto limit = std::wcstoull(number.c_str(), &end, 10);
        if (number.empty() || errno == ERANGE || end != number.c_str() + number.size() ||
            number.front() == L'-' || limit == 0 || limit > INT_MAX) {
            MessageBoxW(dialog, Text(L"The log line limit must be a positive integer.", L"日志保留行数必须为有效正整数。"), L"XTray", MB_OK | MB_ICONERROR);
            return TRUE;
        }
        draft.logLines = static_cast<size_t>(limit);
        std::wstring error;
        const bool startupEnabled = SettingsButtonState(dialog, IDC_LOGON_STARTUP) == BST_CHECKED;
        const bool saved = context->startupAvailable
            ? context->startup.Save(draft, startupEnabled, error)
            : SaveSettings(context->root, draft, error);
        if (!saved) {
            context->logs.Tray(error);
            MessageBoxW(dialog, error.c_str(), L"XTray", MB_OK | MB_ICONERROR);
            return TRUE;
        }
        context->settings = std::move(draft);
        ApplyLanguage(context->settings.language);
        context->logs.SetLimit(context->settings.logLines);
        context->logs.Tray(Text(L"Settings saved; paths will be used on the next run or restart.", L"设置已保存；路径将在下次运行或重启时使用。"));
        EndDialog(dialog, IDOK);
        return TRUE;
    }
    }
    return FALSE;
}

INT_PTR CALLBACK SettingsProcedure(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
    try {
        return SettingsMessage(dialog, message, wParam, lParam);
    } catch (...) {
        auto* context = reinterpret_cast<Context*>(GetWindowLongPtrW(dialog, DWLP_USER));
        if (context != nullptr) {
            context->error = std::current_exception();
        }
        EndDialog(dialog, -1);
        return TRUE;
    }
}

} // namespace

std::wstring WindowText(HWND window)
{
    const int length = GetWindowTextLengthW(window);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(window, text.data(), static_cast<int>(text.size()));
    text.resize(static_cast<size_t>(copied));
    return text;
}

bool EditSettings(HWND owner, const std::filesystem::path& root, Settings& settings, Logs& logs,
    StartupRegistration& startup)
{
    Context context{root, settings, logs, startup};
    const INT_PTR result = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_SETTINGS),
        owner, SettingsProcedure, reinterpret_cast<LPARAM>(&context));
    if (result == -1) {
        auto error = WindowsError(Text(L"Cannot open Settings", L"无法打开设置窗口"));
        if (context.error) {
            try {
                std::rethrow_exception(context.error);
            } catch (const std::exception& exception) {
                error = Text(L"Cannot display Settings: ", L"无法显示设置窗口：") + FromUtf8(exception.what());
            } catch (...) {
                error = Text(L"Cannot display Settings.", L"无法显示设置窗口。");
            }
        }
        logs.Tray(error);
        MessageBoxW(owner, error.c_str(), L"XTray", MB_OK | MB_ICONERROR);
    }
    return result == IDOK;
}

bool SmokeTestSettingsLayout(HWND owner, const std::filesystem::path& root)
{
    std::fwprintf(stderr, L"Smoke: settings scroll viewport, fixed footer, and text-sized buttons.\n");
    const auto language = CurrentLanguage();
    bool passed = true;
    for (const auto selected : {Language::English, Language::Chinese}) {
        ApplyLanguage(selected);
        Settings settings;
        settings.configDirectory = L".\\配置 directory\\fragments";
        Logs logs;
        MemoryStartupRegistry registry;
        StartupRegistration startup(root, CurrentExecutable(), &registry);
        Context context{root, settings, logs, startup};
        context.automation = [&](HWND window) {
            RECT bounds{};
            GetWindowRect(window, &bounds);
            MONITORINFO monitor{sizeof(MONITORINFO)};
            SCROLLINFO initialScroll{sizeof(SCROLLINFO), SIF_ALL};
            if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor) &&
                bounds.bottom - bounds.top < monitor.rcWork.bottom - monitor.rcWork.top - UiScale(window, 8)) {
                passed = passed && GetScrollInfo(context.contents, SB_VERT, &initialScroll) &&
                    initialScroll.nMax + 1 <= static_cast<int>(initialScroll.nPage);
            }
            SetWindowPos(window, nullptr, 0, 0,
                (std::min)(bounds.right - bounds.left, static_cast<LONG>(UiScale(window, 520))),
                UiScale(window, 340), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            RECT client{}, saveBefore{}, saveAfter{}, cancel{}, bodyBounds{};
            GetClientRect(window, &client);
            GetWindowRect(SettingsControl(window, IDOK), &saveBefore);
            GetWindowRect(SettingsControl(window, IDCANCEL), &cancel);
            GetWindowRect(context.contents, &bodyBounds);
            RECT footer{saveBefore.left, saveBefore.top, cancel.right, cancel.bottom};
            MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&footer), 2);
            passed = passed && footer.left >= 0 && footer.top >= 0 &&
                footer.right <= client.right && footer.bottom <= client.bottom &&
                bodyBounds.bottom <= saveBefore.top && registry.writes == 0;
            for (const int id : {IDOK, IDCANCEL, IDC_BROWSE_EXECUTABLE, IDC_ADD_FILES, IDC_BROWSE_DIRECTORY}) {
                RECT button{};
                const HWND control = SettingsControl(window, id);
                GetWindowRect(control, &button);
                const auto text = ControlTextSize(control);
                const int padding = static_cast<int>(button.right - button.left - text.cx);
                passed = passed && padding >= UiScale(control, 23) && padding <= UiScale(control, 25);
            }
            const auto checkInputs = [&] {
                for (const auto& [id, neighbor] : {std::pair{IDC_EXECUTABLE, IDC_BROWSE_EXECUTABLE},
                    std::pair{IDC_CONFIG_DIRECTORY, IDC_BROWSE_DIRECTORY}, std::pair{IDC_LOG_LINES, IDC_LABEL_LOG_LINES}}) {
                    const HWND control = SettingsControl(window, id);
                    RECT edit{}, other{}, client{}, text{};
                    GetWindowRect(control, &edit);
                    GetWindowRect(SettingsControl(window, neighbor), &other);
                    GetClientRect(control, &client);
                    SendMessageW(control, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&text));
                    const LONG bottom = client.bottom - text.bottom;
                    const LONG center = std::abs(edit.top + edit.bottom - other.top - other.bottom);
                    if ((GetWindowLongPtrW(control, GWL_STYLE) & ES_MULTILINE) != 0 ||
                        text.bottom - text.top < ControlTextSize(control).cy || bottom < 0 || std::abs(text.top - bottom) > 1 || center > 1) {
                        std::fwprintf(stderr, L"Smoke: single-line settings input failed (id=%d, top=%ld, bottom=%ld, center=%ld).\n",
                            id, text.top, bottom, center);
                        return false;
                    }
                }
                return true;
            };
            passed = checkInputs() && passed;
            SCROLLINFO scroll{sizeof(SCROLLINFO), SIF_ALL};
            passed = passed && GetScrollInfo(context.contents, SB_VERT, &scroll) &&
                scroll.nMax + 1 > static_cast<int>(scroll.nPage);
            SendMessageW(context.contents, WM_VSCROLL, SB_BOTTOM, 0);
            GetScrollInfo(context.contents, SB_VERT, &scroll);
            GetWindowRect(SettingsControl(window, IDOK), &saveAfter);
            passed = passed && scroll.nPos > 0 && EqualRect(&saveBefore, &saveAfter);
            const HWND executable = SettingsControl(window, IDC_EXECUTABLE);
            SetWindowTextW(executable, L".\\bin\\路径 with spaces\\xray.exe");
            SetFocus(executable);
            MSG tab{};
            tab.hwnd = executable;
            tab.message = WM_KEYDOWN;
            tab.wParam = VK_TAB;
            passed = passed && IsDialogMessageW(window, &tab) &&
                GetFocus() == SettingsControl(window, IDC_BROWSE_EXECUTABLE);
            const HWND checkbox = SettingsControl(window, IDC_LOGON_STARTUP);
            SetFocus(checkbox);
            MSG pending{};
            while (PeekMessageW(&pending, context.contents, kEnsureSettingVisible, kEnsureSettingVisible, PM_REMOVE)) {
                if (pending.message == WM_QUIT) {
                    PostQuitMessage(static_cast<int>(pending.wParam));
                    passed = false;
                    break;
                }
                DispatchMessageW(&pending);
            }
            RECT focused{}, viewport{};
            GetWindowRect(checkbox, &focused);
            GetClientRect(context.contents, &viewport);
            MapWindowPoints(nullptr, context.contents, reinterpret_cast<POINT*>(&focused), 2);
            passed = passed && focused.top >= 0 && focused.bottom <= viewport.bottom &&
                WindowText(executable) == L".\\bin\\路径 with spaces\\xray.exe";
            context.theme.Apply(true, true);
            passed = checkInputs() && passed && context.contentTheme.Colors().dark && SmokeTestNativeControl(checkbox);
            SendMessageW(window, WM_COMMAND, IDCANCEL, 0);
        };
        const auto result = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_SETTINGS),
            owner, SettingsProcedure, reinterpret_cast<LPARAM>(&context));
        if (result != IDCANCEL || context.error || !passed || registry.writes != 0) {
            ApplyLanguage(language);
            return false;
        }
    }
    ApplyLanguage(language);
    return true;
}

bool SmokeTestSettingsTheme(HWND owner, const std::filesystem::path& root)
{
    Settings settings;
    settings.theme = ThemeMode::Light;
    Logs logs;
    MemoryStartupRegistry registry;
    StartupRegistration startup(root, CurrentExecutable(), &registry);
    bool passed = true;
    for (int action = 0; action < 4; ++action) {
        Context context{root, settings, logs, startup};
        context.automation = [&](HWND window) {
            const auto expected = action == 3 ? ThemeMode::Dark : ThemeMode::Light;
            passed = passed && SendMessageW(SettingsControl(window, IDC_THEME), CB_GETCURSEL, 0, 0) ==
                static_cast<LRESULT>(expected);
            std::fwprintf(stderr, L"Smoke: settings native buttons, checkbox, and combo boxes.\n");
            for (const int id : {IDOK, IDCANCEL, IDC_BROWSE_EXECUTABLE, IDC_ADD_FILES,
                IDC_BROWSE_DIRECTORY, IDC_LOGON_STARTUP, IDC_LANGUAGE, IDC_THEME}) {
                passed = SmokeTestNativeControl(SettingsControl(window, id)) && passed;
            }
            const auto before = context.theme.Colors();
            SendMessageW(SettingsControl(window, IDC_THEME), CB_SETCURSEL,
                static_cast<WPARAM>(action == 3 ? ThemeMode::System : ThemeMode::Dark), 0);
            passed = passed && context.theme.Colors() == before &&
                settings.theme == expected;
            if (action == 0) {
                if (auto* parent = ThemeWindow::From(owner)) {
                    parent->ApplyOwned(!before.dark);
                    passed = passed && context.theme.Colors().dark != before.dark &&
                        SendMessageW(SettingsControl(window, IDC_THEME), CB_GETCURSEL, 0, 0) ==
                            static_cast<LRESULT>(ThemeMode::Dark);
                    parent->ApplyOwned(before.dark);
                }
                SendMessageW(window, WM_COMMAND, IDCANCEL, 0);
            } else if (action == 1) {
                MSG escape{};
                escape.hwnd = window;
                escape.message = WM_KEYDOWN;
                escape.wParam = VK_ESCAPE;
                if (!IsDialogMessageW(window, &escape)) {
                    passed = false;
                    EndDialog(window, IDCANCEL);
                }
            } else if (action == 2) {
                SendMessageW(window, WM_COMMAND, IDOK, 0);
            } else {
                SendMessageW(window, WM_CLOSE, 0, 0);
            }
        };
        const auto result = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_SETTINGS),
            owner, SettingsProcedure, reinterpret_cast<LPARAM>(&context));
        if (result != (action == 2 ? IDOK : IDCANCEL) || context.error || !passed ||
            settings.theme != (action >= 2 ? ThemeMode::Dark : ThemeMode::Light)) {
            return false;
        }
        if (action < 2 && std::filesystem::exists(root / L"data" / L"settings.ini")) {
            return false;
        }
    }
    const auto saved = LoadSettings(root);
    return saved.success && saved.settings.theme == ThemeMode::Dark;
}

bool SmokeTestSettingsStartup(HWND owner, const std::filesystem::path& root)
{
    std::fwprintf(stderr, L"Smoke: login-startup draft, save, reopen, and themed native checkbox.\n");
    Settings settings;
    settings.language = CurrentLanguage();
    Logs logs;
    MemoryStartupRegistry registry;
    StartupRegistration startup(root, CurrentExecutable(), &registry);
    bool passed = true;
    for (int action = 0; action < 4; ++action) {
        Context context{root, settings, logs, startup};
        context.automation = [&](HWND window) {
            const bool initiallyEnabled = action == 2;
            const HWND checkbox = SettingsControl(window, IDC_LOGON_STARTUP);
            passed = passed && context.startupAvailable && IsWindowEnabled(checkbox) &&
                (SettingsButtonState(window, IDC_LOGON_STARTUP) == BST_CHECKED) == initiallyEnabled;
            if (action != 3) {
                SendMessageW(checkbox, BM_CLICK, 0, 0);
                const bool checked = !initiallyEnabled;
                passed = passed &&
                    (SettingsButtonState(window, IDC_LOGON_STARTUP) == BST_CHECKED) == checked;
                for (const bool dark : {false, true}) {
                    context.theme.Apply(dark, true);
                    if (dark) {
                        std::fwprintf(stderr, L"Smoke: dark startup checkbox caption with a native light checkbox theme.\n");
                        // Reproduce a theme that ignores WM_CTLCOLOR's caption color, including on Windows 11.
                        SetWindowTheme(checkbox, L"Explorer", nullptr);
                    }
                    passed = passed &&
                        (SettingsButtonState(window, IDC_LOGON_STARTUP) == BST_CHECKED) == checked &&
                        SmokeTestNativeControl(checkbox);
                    EnableWindow(checkbox, FALSE);
                    passed = passed && !IsWindowEnabled(checkbox) &&
                        (SettingsButtonState(window, IDC_LOGON_STARTUP) == BST_CHECKED) == checked &&
                        SmokeTestNativeControl(checkbox);
                    EnableWindow(checkbox, TRUE);
                }
                if (action == 0) {
                    SetFocus(checkbox);
                    SendMessageW(checkbox, WM_KEYDOWN, VK_SPACE, 0);
                    SendMessageW(checkbox, WM_KEYUP, VK_SPACE, 0);
                    passed = passed && SettingsButtonState(window, IDC_LOGON_STARTUP) == BST_UNCHECKED;
                    SendMessageW(checkbox, BM_CLICK, 0, 0);
                }
            } else {
                SendMessageW(checkbox, BM_CLICK, 0, 0);
            }
            SendMessageW(window, action == 3 ? WM_CLOSE : WM_COMMAND,
                action == 0 ? IDCANCEL : IDOK, 0);
        };
        const auto result = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_SETTINGS),
            owner, SettingsProcedure, reinterpret_cast<LPARAM>(&context));
        if (context.error || !passed || result != (action == 0 || action == 3 ? IDCANCEL : IDOK) ||
            registry.writes != static_cast<size_t>(action == 0 ? 0 : action == 1 ? 1 : 2)) {
            return false;
        }
        const auto status = startup.Query();
        if (!status.success || status.enabled != (action == 1)) {
            return false;
        }
        if (action == 0 && (std::filesystem::exists(root / L"data" / L"settings.ini") ||
            std::filesystem::exists(root / L"data" / L"startup.ini"))) {
            return false;
        }
    }

    std::fwprintf(stderr, L"Smoke: unavailable login startup preserves registration and permits ordinary settings.\n");
    registry.failRead = true;
    const auto before = registry.values;
    Context context{root, settings, logs, startup};
    context.automation = [&](HWND window) {
        passed = passed && !context.startupAvailable &&
            !IsWindowEnabled(SettingsControl(window, IDC_LOGON_STARTUP));
        SetWindowTextW(SettingsControl(window, IDC_LOG_LINES), L"733");
        SendMessageW(window, WM_COMMAND, IDOK, 0);
    };
    const auto result = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_SETTINGS),
        owner, SettingsProcedure, reinterpret_cast<LPARAM>(&context));
    const auto saved = LoadSettings(root);
    bool reported = false;
    for (const auto& line : logs.Snapshot().lines) {
        reported = reported || line->text.find(L"Injected login-startup query failure.") != std::wstring::npos;
    }
    return result == IDOK && !context.error && passed && reported && saved.success &&
        saved.settings.logLines == 733 && registry.writes == 2 && registry.values == before;
}

} // namespace xtray
