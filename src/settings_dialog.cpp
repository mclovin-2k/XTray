#include "settings_dialog.h"
#include "resource.h"
#include "win32.h"
#include "theme_window.h"
#include "ui_layout.h"
#include "xray_tool_window.h"

#include <commctrl.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <wrl/client.h>

#include <array>
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
    XrayToolWindow localTools{root};
    XrayToolWindow* tools = &localTools;
    HWND dialog = nullptr;
    HHOOK messageFilter = nullptr;
    bool filteringToolMessage = false;
    bool quietTools = false;
    bool startupAvailable = false;
    ThemeWindow theme;
    ThemeWindow contentTheme;
    HWND contents = nullptr;
    HWND tooltip = nullptr;
    int scroll = 0;
    int contentHeight = 0;
    int wheelRemainder = 0;
    std::function<void(HWND)> automation;
    std::exception_ptr error;
    std::array<HICON, 2> icons{};
    ~Context();
};

thread_local Context* activeSettings = nullptr;

Context::~Context()
{
    tools->CloseOwned(dialog);
    if (IsWindow(tooltip)) { DestroyWindow(tooltip); }
    if (messageFilter != nullptr) { UnhookWindowsHookEx(messageFilter); }
    if (activeSettings == this) { activeSettings = nullptr; }
    for (const auto icon : icons) { if (icon != nullptr) { DestroyIcon(icon); } }
}

LRESULT CALLBACK SettingsMessageFilter(int code, WPARAM wParam, LPARAM lParam)
{
    auto* context = activeSettings;
    if (code == MSGF_DIALOGBOX && context != nullptr && !context->filteringToolMessage) {
        // IsDialogMessage calls this filter itself; do not route the same message twice.
        struct Reset { bool& value; ~Reset() { value = false; } } reset{context->filteringToolMessage};
        context->filteringToolMessage = true;
        try {
            // DialogBox owns the message loop while modeless result windows remain open.
            if (XrayToolWindow::TranslateAny(*reinterpret_cast<MSG*>(lParam))) { return TRUE; }
        } catch (...) {
            context->error = std::current_exception();
            EndDialog(context->dialog, -1);
            return TRUE;
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

Settings ConfigDraft(HWND dialog, const Settings& saved)
{
    Settings draft = saved;
    const HWND body = GetDlgItem(dialog, IDC_SETTINGS_BODY);
    draft.executable = WindowText(GetDlgItem(body, IDC_EXECUTABLE));
    draft.configDirectory = WindowText(GetDlgItem(body, IDC_CONFIG_DIRECTORY));
    draft.configFiles = SplitConfigFiles(WindowText(GetDlgItem(body, IDC_CONFIG_FILES)));
    draft.configDirectoryFirst = SendMessageW(GetDlgItem(body, IDC_CONFIG_DIRECTORY_FIRST), BM_GETCHECK, 0, 0) == BST_CHECKED;
    return draft;
}

void UpdateConfigTools(Context& context)
{
    const HWND result = context.tools->Window();
    const bool available = !context.tools->Running() ||
        (result != nullptr && GetWindow(result, GW_OWNER) == context.dialog);
    for (const int id : {IDC_CHECK_CONFIG, IDC_PREVIEW_CONFIG}) {
        const HWND button = GetDlgItem(context.contents, id);
        if ((IsWindowEnabled(button) != FALSE) != available) { EnableWindow(button, available); }
    }
}

void SetDialogIcons(HWND dialog, Context& context)
{
    struct Icons {
        std::array<HICON, 2> values{};
        ~Icons() { for (const auto icon : values) { if (icon != nullptr) { DestroyIcon(icon); } } }
    } next;
    for (size_t index = 0; index < next.values.size(); ++index) {
        const int size = UiScale(dialog, index == 0 ? 16 : 32);
        next.values[index] = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),
            MAKEINTRESOURCEW(IDI_XTRAY), IMAGE_ICON, size, size, 0));
        if (next.values[index] == nullptr) {
            throw std::runtime_error("Cannot load settings window icons.");
        }
    }
    next.values.swap(context.icons);
    SendMessageW(dialog, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(context.icons[0]));
    SendMessageW(dialog, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(context.icons[1]));
}

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

const wchar_t* ConfigOrderTooltip()
{
    return Text(
        L"Checked: load directory configuration first, then the file list. Later inputs override or supplement earlier ones under Xray's rules.\n"
        L"Unchecked: keep Xray's native order, files before directory.\n"
        L"Only applies when both sources are set.",
        L"勾选：先加载目录内配置，再按文件列表顺序加载独立配置，后者按 Xray 规则覆盖或补充前者。\n"
        L"未勾选：使用 Xray 原生顺序，先文件、后目录。\n"
        L"仅同时配置目录和文件时生效。");
}

void CreateSettingsTooltip(Context& context)
{
    context.tooltip = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE, TOOLTIPS_CLASSW, nullptr,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT,
        CW_USEDEFAULT, CW_USEDEFAULT, context.dialog, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (context.tooltip == nullptr) {
        throw std::runtime_error(ToUtf8(WindowsError(Text(L"Cannot create the settings tooltip", L"无法创建设置悬停提示"))));
    }
    TOOLINFOW tool{};
    tool.cbSize = TTTOOLINFOW_V2_SIZE;
    tool.hwnd = context.contents;
    tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    tool.uId = reinterpret_cast<UINT_PTR>(SettingsControl(context.dialog, IDC_CONFIG_DIRECTORY_FIRST));
    tool.lpszText = const_cast<wchar_t*>(ConfigOrderTooltip());
    if (!SendMessageW(context.tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool))) {
        throw std::runtime_error(Text("Cannot register the settings tooltip.", "无法注册设置悬停提示。"));
    }
    SendMessageW(context.tooltip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 15000);
}

void LayoutSettingsContent(Context& context)
{
    const HWND body = context.contents;
    if (context.tooltip != nullptr) {
        SendMessageW(context.tooltip, TTM_POP, 0, 0);
        SendMessageW(context.tooltip, TTM_SETMAXTIPWIDTH, 0, UiScale(body, 400));
    }
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
        const std::array groups{std::pair{IDC_LABEL_LANGUAGE, IDC_LANGUAGE},
            std::pair{IDC_LABEL_THEME, IDC_THEME}, std::pair{IDC_LABEL_LOG_LINES, IDC_LOG_LINES}};
        std::array<SIZE, 3> sizes{};
        const int column = (available - sectionGap * 2) / 3;
        bool stacked = false;
        LONG labelColumn = 0;
        for (size_t index = 0; index < groups.size(); ++index) {
            sizes[index] = ControlTextSize(GetDlgItem(body, groups[index].first));
            labelColumn = (std::max)(labelColumn, sizes[index].cx);
            stacked = stacked || column < sizes[index].cx + gap +
                UiScale(body, groups[index].second == IDC_LOG_LINES ? 64 : 96);
        }
        for (size_t index = 0; index < groups.size(); ++index) {
            const auto [labelId, inputId] = groups[index];
            const int x = margin + (stacked ? 0 : static_cast<int>(index) * (column + sectionGap));
            const int groupWidth = stacked ? available : index + 1 == groups.size()
                ? available - static_cast<int>(index) * (column + sectionGap) : column;
            const LONG labelWidth = stacked ? labelColumn : sizes[index].cx;
            const HWND input = GetDlgItem(body, inputId);
            const bool number = inputId == IDC_LOG_LINES;
            const int height = number ? SingleLineControlHeight(input) : ComboBoxHeight(input);
            place(labelId, x, y + (rowHeight - sizes[index].cy) / 2, sizes[index].cx, sizes[index].cy);
            place(inputId, x + labelWidth + gap, y + (rowHeight - height) / 2,
                groupWidth - labelWidth - gap, number ? height : rowHeight + UiScale(body, 160));
            if (stacked && index + 1 != groups.size()) { y += rowHeight + gap; }
        }
        y += rowHeight + sectionGap;
        label(IDC_LABEL_EXECUTABLE);
        pathRow(IDC_EXECUTABLE, IDC_BROWSE_EXECUTABLE, SingleLineControlHeight(GetDlgItem(body, IDC_EXECUTABLE)));
        label(IDC_LABEL_CONFIG_FILES);
        units = {0, 0, 0, 75};
        MapDialogRect(body, &units);
        pathRow(IDC_CONFIG_FILES, IDC_ADD_FILES, static_cast<int>(units.bottom));
        label(IDC_LABEL_CONFIG_DIRECTORY);
        pathRow(IDC_CONFIG_DIRECTORY, IDC_BROWSE_DIRECTORY, SingleLineControlHeight(GetDlgItem(body, IDC_CONFIG_DIRECTORY)));
        place(IDC_CONFIG_DIRECTORY_FIRST, margin, y, available, rowHeight);
        y += rowHeight + sectionGap;
        const int checkWidth = TextButtonWidth(GetDlgItem(body, IDC_CHECK_CONFIG));
        const int previewWidth = TextButtonWidth(GetDlgItem(body, IDC_PREVIEW_CONFIG));
        const bool stackTools = checkWidth + gap + previewWidth > available;
        const int toolsWidth = stackTools ? (std::max)(checkWidth, previewWidth) : checkWidth + gap + previewWidth;
        const int toolsHeight = stackTools ? rowHeight * 2 + gap : rowHeight;
        const int hintWidth = available - toolsWidth - gap;
        SIZE hintSize{};
        if (hintWidth >= UiScale(body, 96)) {
            hintSize = ControlTextSize(GetDlgItem(body, IDC_HINT_CONFIG_TOOLS), hintWidth);
        }
        const bool inlineHint = hintSize.cy > 0 && hintSize.cx <= hintWidth;
        const int toolsRowHeight = inlineHint ? (std::max)(toolsHeight, static_cast<int>(hintSize.cy)) : toolsHeight;
        const int toolsY = y + (toolsRowHeight - toolsHeight) / 2;
        place(IDC_CHECK_CONFIG, margin, toolsY, (std::min)(available, checkWidth), rowHeight);
        place(IDC_PREVIEW_CONFIG, stackTools ? margin : margin + checkWidth + gap,
            toolsY + (stackTools ? rowHeight + gap : 0), (std::min)(available, previewWidth), rowHeight);
        if (inlineHint) {
            place(IDC_HINT_CONFIG_TOOLS, margin + toolsWidth + gap,
                y + (toolsRowHeight - hintSize.cy) / 2, hintWidth, hintSize.cy);
            y += toolsRowHeight + sectionGap;
        } else {
            y += toolsHeight + gap;
            label(IDC_HINT_CONFIG_TOOLS);
            y += sectionGap - gap;
        }
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
                const int previousScroll = context->scroll;
                if (bounds.top < margin || bounds.bottom - bounds.top > client.bottom - margin * 2) {
                    context->scroll += bounds.top - margin;
                } else if (bounds.bottom > client.bottom - margin) {
                    context->scroll += bounds.bottom - client.bottom + margin;
                }
                context->scroll = std::clamp(context->scroll, 0,
                    (std::max)(0, context->contentHeight - static_cast<int>(client.bottom)));
                // Moving an already visible combo box closes its newly opened drop-down.
                if (context->scroll == previousScroll) { return TRUE; }
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
    SetWindowTextW(dialog, Text(L"Settings", L"设置"));
    const auto set = [dialog](int id, const wchar_t* text) { SetWindowTextW(SettingsControl(dialog, id), text); };
    set(IDC_LABEL_EXECUTABLE, Text(L"Xray executable:", L"Xray 程序路径："));
    set(IDC_LABEL_CONFIG_FILES, Text(L"Config files (one per line, in load order; optional):",
        L"配置文件（每行一个，按顺序加载；可留空）："));
    set(IDC_LABEL_CONFIG_DIRECTORY, Text(L"Config directory (optional):", L"配置目录（可留空）："));
    set(IDC_CONFIG_DIRECTORY_FIRST, Text(L"Load config directory before config files", L"先加载配置目录，再加载配置文件"));
    set(IDC_LABEL_LOG_LINES, Text(L"Log lines per source:", L"每类日志保留行数："));
    set(IDC_LABEL_LANGUAGE, Text(L"Language:", L"语言："));
    set(IDC_LABEL_THEME, Text(L"Appearance:", L"外观："));
    set(IDC_LOGON_STARTUP, Text(L"Start XTray and Xray when I sign in", L"登录后自动启动 XTray 和 Xray"));
    set(IDC_BROWSE_EXECUTABLE, Text(L"Browse...", L"浏览…"));
    set(IDC_ADD_FILES, Text(L"Add files...", L"添加文件…"));
    set(IDC_BROWSE_DIRECTORY, Text(L"Browse...", L"浏览…"));
    set(IDC_CHECK_CONFIG, Text(L"Check current configuration", L"检查当前配置"));
    set(IDC_PREVIEW_CONFIG, Text(L"Preview current configuration", L"预览当前配置"));
    set(IDC_HINT_CONFIG_TOOLS, Text(L"Uses this form's current contents without saving.",
        L"使用本窗口当前填写的内容执行，无需先保存。"));
    set(IDC_HINT_RELATIVE, Text(L"Relative paths use the XTray.exe directory.",
        L"相对路径以 XTray.exe 所在目录为基准。"));
    set(IDC_HINT_APPLY, Text(L"Paths and load order apply on the next run/restart; at least one config source is required.",
        L"路径和加载顺序下次运行或重启生效；运行前至少设置一种配置来源。"));
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
        context->dialog = dialog;
        activeSettings = context;
        context->messageFilter = SetWindowsHookExW(WH_MSGFILTER, SettingsMessageFilter, nullptr, GetCurrentThreadId());
        if (context->messageFilter == nullptr) {
            throw std::runtime_error(ToUtf8(WindowsError(Text(L"Cannot enable result window keyboard navigation",
                L"无法初始化结果窗口键盘操作"))));
        }
        context->theme.Attach(dialog);
        SetDialogIcons(dialog, *context);
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
        CheckSettingsButton(dialog, IDC_CONFIG_DIRECTORY_FIRST,
            context->settings.configDirectoryFirst ? BST_CHECKED : BST_UNCHECKED);
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
        CreateSettingsTooltip(*context);
        FitSettingsWindow(dialog, *context);
        LayoutSettings(dialog, *context);
        SetFocus(SettingsControl(dialog, IDC_LANGUAGE));
        UpdateConfigTools(*context);
        if (SetTimer(dialog, 1, 100, nullptr) == 0) {
            throw std::runtime_error(ToUtf8(WindowsError(Text(L"Cannot update settings tools", L"无法更新设置工具状态"))));
        }
        if (context->automation) {
            PostMessageW(dialog, kAutomateDialog, 0, 0);
        }
        return FALSE;
    }
    if (context == nullptr) {
        return FALSE;
    }
    if (message == WM_NCDESTROY) {
        context->tools->CloseOwned(dialog);
        if (IsWindow(context->tooltip)) { DestroyWindow(context->tooltip); }
        context->tooltip = nullptr;
        KillTimer(dialog, 1);
        if (context->messageFilter != nullptr) {
            UnhookWindowsHookEx(context->messageFilter);
            context->messageFilter = nullptr;
        }
        if (activeSettings == context) { activeSettings = nullptr; }
        context->theme.Detach();
        SetWindowLongPtrW(dialog, DWLP_USER, 0);
        return FALSE;
    }
    if (message == WM_SIZE) {
        LayoutSettings(dialog, *context);
        return FALSE;
    }
    if (message == WM_TIMER && wParam == 1) {
        UpdateConfigTools(*context);
        return TRUE;
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
        SetDialogIcons(dialog, *context);
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
    case IDC_CHECK_CONFIG:
    case IDC_PREVIEW_CONFIG: {
        try {
            const auto kind = LOWORD(wParam) == IDC_CHECK_CONFIG ? XrayToolKind::Check : XrayToolKind::Preview;
            context->tools->Open(dialog, kind, ConfigDraft(dialog, context->settings), true,
                context->theme.Colors().dark, context->quietTools, XrayToolSource::SettingsDraft);
        } catch (const std::exception& exception) {
            context->tools->CloseOwned(dialog);
            if (context->quietTools) { throw; }
            const auto error = FromUtf8(exception.what());
            MessageBoxW(dialog, error.c_str(), Text(L"Xray tool", L"Xray 工具"), MB_OK | MB_ICONERROR);
        }
        return TRUE;
    }
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
        Settings draft = ConfigDraft(dialog, context->settings);
        draft.language = static_cast<Language>(
            SendMessageW(SettingsControl(dialog, IDC_LANGUAGE), CB_GETCURSEL, 0, 0));
        draft.theme = static_cast<ThemeMode>(
            SendMessageW(SettingsControl(dialog, IDC_THEME), CB_GETCURSEL, 0, 0));
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
    StartupRegistration& startup, XrayToolWindow* tools)
{
    Context context{root, settings, logs, startup};
    if (tools != nullptr) { context.tools = tools; }
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
    std::filesystem::create_directories(root);
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
        context.quietTools = true;
        context.automation = [&](HWND window) {
            const auto checkCaption = [&] {
                if (WindowText(window) != Text(L"Settings", L"设置") ||
                    (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_DLGMODALFRAME) != 0 ||
                    IsWindowEnabled(owner)) {
                    std::fwprintf(stderr, L"Smoke: settings caption or modal window style failed.\n");
                    return false;
                }
                for (size_t index = 0; index < context.icons.size(); ++index) {
                    const auto icon = reinterpret_cast<HICON>(SendMessageW(window, WM_GETICON,
                        index == 0 ? ICON_SMALL : ICON_BIG, 0));
                    ICONINFO info{};
                    if (icon == nullptr || icon != context.icons[index] || !GetIconInfo(icon, &info)) {
                        std::fwprintf(stderr, L"Smoke: settings caption icon is missing.\n");
                        return false;
                    }
                    BITMAP bitmap{};
                    const bool read = GetObjectW(info.hbmColor, sizeof(bitmap), &bitmap) == sizeof(bitmap);
                    if (info.hbmColor != nullptr) { DeleteObject(info.hbmColor); }
                    if (info.hbmMask != nullptr) { DeleteObject(info.hbmMask); }
                    const int size = UiScale(window, index == 0 ? 16 : 32);
                    if (!read || bitmap.bmWidth != size || bitmap.bmHeight != size) {
                        std::fwprintf(stderr, L"Smoke: settings caption icon DPI size failed (index=%zu).\n", index);
                        return false;
                    }
                }
                return true;
            };
            std::fwprintf(stderr, L"Smoke: localized Settings title, native icon caption, and DPI icon refresh.\n");
            passed = checkCaption() && passed;
            const HWND languageChoice = SettingsControl(window, IDC_LANGUAGE);
            const HWND themeChoice = SettingsControl(window, IDC_THEME);
            passed = passed && GetFocus() == languageChoice &&
                GetNextDlgTabItem(window, languageChoice, FALSE) == themeChoice &&
                GetNextDlgTabItem(window, themeChoice, FALSE) == SettingsControl(window, IDC_LOG_LINES) &&
                GetNextDlgTabItem(window, SettingsControl(window, IDC_LOG_LINES), FALSE) == SettingsControl(window, IDC_EXECUTABLE) &&
                GetNextDlgTabItem(window, SettingsControl(window, IDC_BROWSE_DIRECTORY), FALSE) == SettingsControl(window, IDC_CONFIG_DIRECTORY_FIRST) &&
                GetNextDlgTabItem(window, SettingsControl(window, IDC_CONFIG_DIRECTORY_FIRST), FALSE) == SettingsControl(window, IDC_CHECK_CONFIG) &&
                SettingsButtonState(window, IDC_CONFIG_DIRECTORY_FIRST) == BST_UNCHECKED;
            const auto dispatchFocusMessages = [&] {
                MSG pending{};
                while (PeekMessageW(&pending, context.contents, kEnsureSettingVisible, kEnsureSettingVisible, PM_REMOVE)) {
                    if (pending.message == WM_QUIT) {
                        PostQuitMessage(static_cast<int>(pending.wParam));
                        passed = false;
                        break;
                    }
                    DispatchMessageW(&pending);
                }
            };
            const auto checkAlignment = [&] {
                for (const auto& [labelId, inputId] : {std::pair{IDC_LABEL_LANGUAGE, IDC_LANGUAGE},
                    std::pair{IDC_LABEL_THEME, IDC_THEME}, std::pair{IDC_LABEL_LOG_LINES, IDC_LOG_LINES}}) {
                    RECT label{}, input{};
                    GetWindowRect(SettingsControl(window, labelId), &label);
                    GetWindowRect(SettingsControl(window, inputId), &input);
                    if (std::abs(label.top + label.bottom - input.top - input.bottom) > 1 ||
                        label.bottom - label.top < ControlTextSize(SettingsControl(window, labelId)).cy ||
                        label.right >= input.left) {
                        std::fwprintf(stderr, L"Smoke: settings inline label alignment failed (id=%d).\n", labelId);
                        return false;
                    }
                }
                RECT languageLabel{}, themeLabel{}, languageInput{}, themeInput{}, save{}, cancel{};
                GetWindowRect(SettingsControl(window, IDC_LABEL_LANGUAGE), &languageLabel);
                GetWindowRect(SettingsControl(window, IDC_LABEL_THEME), &themeLabel);
                GetWindowRect(languageChoice, &languageInput);
                GetWindowRect(themeChoice, &themeInput);
                GetWindowRect(SettingsControl(window, IDOK), &save);
                GetWindowRect(SettingsControl(window, IDCANCEL), &cancel);
                RECT logLabel{}, logInput{};
                GetWindowRect(SettingsControl(window, IDC_LABEL_LOG_LINES), &logLabel);
                GetWindowRect(SettingsControl(window, IDC_LOG_LINES), &logInput);
                if (languageLabel.left == themeLabel.left) {
                    if (languageInput.left != themeInput.left || languageInput.right != themeInput.right ||
                        logInput.left != languageInput.left || logInput.right != languageInput.right ||
                        logLabel.left != languageLabel.left || themeLabel.top <= languageInput.bottom ||
                        logLabel.top <= themeInput.bottom) { return false; }
                } else {
                    const LONG firstWidth = languageInput.right - languageLabel.left;
                    const LONG secondWidth = themeInput.right - themeLabel.left;
                    const LONG thirdWidth = logInput.right - logLabel.left;
                    if (std::abs(languageInput.top + languageInput.bottom - themeInput.top - themeInput.bottom) > 1 ||
                        std::abs(languageInput.top + languageInput.bottom - logInput.top - logInput.bottom) > 1 ||
                        std::abs(firstWidth - secondWidth) > 2 || std::abs(firstWidth - thirdWidth) > 2 ||
                        themeLabel.left - languageInput.right != UiScale(window, 14) ||
                        logLabel.left - themeInput.right != UiScale(window, 14)) {
                        std::fwprintf(stderr, L"Smoke: settings top row is not three equal columns.\n");
                        return false;
                    }
                }
                RECT browse{};
                GetWindowRect(SettingsControl(window, IDC_BROWSE_EXECUTABLE), &browse);
                for (const int id : {IDC_ADD_FILES, IDC_BROWSE_DIRECTORY}) {
                    RECT button{};
                    GetWindowRect(SettingsControl(window, id), &button);
                    if (button.right != browse.right) { return false; }
                }
                RECT directory{}, order{}, hint{}, check{}, preview{}, startup{};
                const HWND orderControl = SettingsControl(window, IDC_CONFIG_DIRECTORY_FIRST);
                GetWindowRect(SettingsControl(window, IDC_CONFIG_DIRECTORY), &directory);
                GetWindowRect(orderControl, &order);
                const HWND hintControl = SettingsControl(window, IDC_HINT_CONFIG_TOOLS);
                GetWindowRect(hintControl, &hint);
                GetWindowRect(SettingsControl(window, IDC_CHECK_CONFIG), &check);
                GetWindowRect(SettingsControl(window, IDC_PREVIEW_CONFIG), &preview);
                GetWindowRect(SettingsControl(window, IDC_LOGON_STARTUP), &startup);
                if (order.left != directory.left || order.left != check.left || order.top < directory.bottom ||
                    check.top < order.bottom ||
                    order.right - order.left < ControlTextSize(orderControl).cx + UiScale(orderControl, 24)) {
                    std::fwprintf(stderr, L"Smoke: configuration order checkbox placement or caption width failed.\n");
                    return false;
                }
                const LONG buttonsRight = (std::max)(check.right, preview.right);
                const LONG buttonsTop = (std::min)(check.top, preview.top);
                const LONG buttonsBottom = (std::max)(check.bottom, preview.bottom);
                const bool beside = hint.left > buttonsRight;
                if (hint.right > order.right || hint.bottom - hint.top < ControlTextSize(hintControl, hint.right - hint.left).cy ||
                    startup.top < (std::max)(hint.bottom, buttonsBottom) ||
                    (beside ? std::abs(hint.top + hint.bottom - buttonsTop - buttonsBottom) > 1
                        : hint.left != order.left || hint.top < buttonsBottom)) {
                    std::fwprintf(stderr, L"Smoke: wrapped configuration tool hint overlaps or loses text.\n");
                    return false;
                }
                if (order.right - buttonsRight - UiScale(window, 8) >= UiScale(window, 96) && !beside) {
                    std::fwprintf(stderr, L"Smoke: configuration tool hint did not use the available right-hand column.\n");
                    return false;
                }
                std::array<wchar_t, 1024> tip{};
                TOOLINFOW tool{};
                tool.cbSize = TTTOOLINFOW_V2_SIZE;
                tool.hwnd = context.contents;
                tool.uId = reinterpret_cast<UINT_PTR>(orderControl);
                tool.lpszText = tip.data();
                SendMessageW(context.tooltip, TTM_GETTEXTW, tip.size(), reinterpret_cast<LPARAM>(&tool));
                const std::wstring_view tooltip(tip.data());
                if (tooltip != ConfigOrderTooltip() ||
                    tooltip.find(Text(L"Unchecked:", L"未勾选：")) == std::wstring_view::npos ||
                    SendMessageW(context.tooltip, TTM_GETMAXTIPWIDTH, 0, 0) != UiScale(window, 400)) {
                    std::fwprintf(stderr, L"Smoke: localized configuration order tooltip is missing or unwrapped.\n");
                    return false;
                }
                return std::abs(save.top + save.bottom - cancel.top - cancel.bottom) <= 1;
            };
            const auto checkDropdowns = [&] {
                for (const int id : {IDC_LANGUAGE, IDC_THEME}) {
                    const HWND control = SettingsControl(window, id);
                    SetFocus(SettingsControl(window, IDC_EXECUTABLE));
                    dispatchFocusMessages();
                    COMBOBOXINFO combo{sizeof(COMBOBOXINFO)};
                    if (!GetComboBoxInfo(control, &combo)) { return false; }
                    const auto point = MAKELPARAM((combo.rcButton.left + combo.rcButton.right) / 2,
                        (combo.rcButton.top + combo.rcButton.bottom) / 2);
                    const auto selection = SendMessageW(control, CB_GETCURSEL, 0, 0);
                    const int scrollBefore = context.scroll;
                    SendMessageW(control, WM_LBUTTONDOWN, MK_LBUTTON, point);
                    dispatchFocusMessages();
                    SendMessageW(control, WM_LBUTTONUP, 0, point);
                    const bool opened = GetFocus() == control &&
                        SendMessageW(control, CB_GETDROPPEDSTATE, 0, 0) != FALSE &&
                        SendMessageW(control, CB_GETCURSEL, 0, 0) == selection && context.scroll == scrollBefore;
                    SendMessageW(control, CB_SHOWDROPDOWN, FALSE, 0);
                    if (!opened) {
                        std::fwprintf(stderr, L"Smoke: settings first-click drop-down failed (id=%d, dark=%d).\n",
                            id, context.contentTheme.Colors().dark);
                        return false;
                    }
                }
                return true;
            };
            std::fwprintf(stderr, L"Smoke: settings combo boxes open on the first click after focus changes.\n");
            const bool initialDark = context.theme.Colors().dark;
            for (const bool dark : {false, true}) {
                context.theme.Apply(dark, true);
                passed = checkAlignment() && passed;
                passed = checkDropdowns() && passed;
            }
            context.theme.Apply(initialDark, true);
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
                (std::min)(bounds.right - bounds.left, static_cast<LONG>(UiScale(window, 320))),
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
            passed = checkInputs() && checkAlignment() && passed;
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
            dispatchFocusMessages();
            RECT focused{}, viewport{};
            GetWindowRect(checkbox, &focused);
            GetClientRect(context.contents, &viewport);
            MapWindowPoints(nullptr, context.contents, reinterpret_cast<POINT*>(&focused), 2);
            passed = passed && focused.top >= 0 && focused.bottom <= viewport.bottom &&
                WindowText(executable) == L".\\bin\\路径 with spaces\\xray.exe";
            context.theme.Apply(true, true);
            SendMessageW(window, kRefreshDialogTheme, 0, 0);
            passed = checkCaption() && checkInputs() && checkAlignment() && passed &&
                context.contentTheme.Colors().dark && SmokeTestNativeControl(checkbox) &&
                SmokeTestNativeControl(SettingsControl(window, IDC_CONFIG_DIRECTORY_FIRST));

            std::fwprintf(stderr, L"Smoke: settings draft tools, saved-settings isolation, keyboard navigation, and owner cancellation.\n");
            const auto original = settings;
            SetWindowTextW(executable, (CurrentExecutable().parent_path() / L"XTray.Tests.exe").c_str());
            SetWindowTextW(SettingsControl(window, IDC_CONFIG_FILES), L".\\tool-ok.conf\r\n.\\重复 ;= config.json");
            std::filesystem::create_directories(root / L"draft configs");
            SetWindowTextW(SettingsControl(window, IDC_CONFIG_DIRECTORY), L".\\draft configs\\");
            CheckSettingsButton(window, IDC_CONFIG_DIRECTORY_FIRST, BST_CHECKED);
            SetWindowTextW(SettingsControl(window, IDC_LOG_LINES), L"0");
            SendMessageW(SettingsControl(window, IDC_CHECK_CONFIG), BM_CLICK, 0, 0);
            CheckSettingsButton(window, IDC_CONFIG_DIRECTORY_FIRST, BST_UNCHECKED);
            SetWindowTextW(SettingsControl(window, IDC_CONFIG_FILES), L".\\tool-fail.conf");
            const auto waitTool = [&] {
                const auto until = GetTickCount64() + 4000;
                while (context.tools->Running() && GetTickCount64() < until) {
                    MSG pending{};
                    while (PeekMessageW(&pending, nullptr, 0, 0, PM_REMOVE)) {
                        if (pending.message == WM_QUIT) {
                            PostQuitMessage(static_cast<int>(pending.wParam));
                            return false;
                        }
                        if (!context.tools->Translate(pending) && !IsDialogMessageW(window, &pending)) {
                            TranslateMessage(&pending);
                            DispatchMessageW(&pending);
                        }
                    }
                    Sleep(5);
                }
                SendMessageW(context.tools->Window(), WM_TIMER, 1, 0);
                return !context.tools->Running();
            };
            passed = waitTool() && passed;
            std::fwprintf(stderr, L"Smoke: draft configuration check completed; verifying source and keyboard navigation.\n");
            const HWND result = context.tools->Window();
            passed = passed && result != nullptr && GetWindow(result, GW_OWNER) == window &&
                WindowText(result) == Text(L"Check current configuration", L"检查当前配置") &&
                WindowText(GetDlgItem(result, 4103)).find(Text(L"passed", L"通过")) != std::wstring::npos &&
                WindowText(GetDlgItem(result, 4101)).find(L"-config=.\\tool-ok.conf") != std::wstring::npos &&
                WindowText(GetDlgItem(result, 4101)).find(L"-confdir=") == std::wstring::npos &&
                WindowText(GetDlgItem(result, 4105)).find(Text(L"current settings form", L"设置窗口当前填写")) != std::wstring::npos;
            ShowWindow(result, SW_SHOWNOACTIVATE);
            SetFocus(GetDlgItem(result, 4102));
            MSG tabTool{};
            tabTool.hwnd = GetDlgItem(result, 4102);
            tabTool.message = WM_KEYDOWN;
            tabTool.wParam = VK_TAB;
            passed = CallMsgFilterW(&tabTool, MSGF_DIALOGBOX) &&
                GetFocus() == GetDlgItem(result, 4104) && passed;
            SetFocus(executable);
            SetWindowTextW(SettingsControl(window, IDC_CONFIG_FILES), L".\\tool-ok.conf");
            SendMessageW(SettingsControl(window, IDC_PREVIEW_CONFIG), BM_CLICK, 0, 0);
            passed = waitTool() && passed;
            passed = passed && WindowText(context.tools->Window()) == Text(L"Preview current configuration", L"预览当前配置") &&
                WindowText(GetDlgItem(context.tools->Window(), 4101)).find(L"-dump\r\n-confdir=.\\draft configs\\") != std::wstring::npos &&
                settings.executable == original.executable && settings.configFiles == original.configFiles &&
                settings.configDirectory == original.configDirectory && settings.logLines == original.logLines &&
                settings.configDirectoryFirst == original.configDirectoryFirst &&
                registry.writes == 0 && !std::filesystem::exists(root / L"data" / L"settings.ini");
            SetWindowTextW(SettingsControl(window, IDC_CONFIG_FILES), L".\\tool-hold.conf");
            SendMessageW(SettingsControl(window, IDC_PREVIEW_CONFIG), BM_CLICK, 0, 0);
            std::fwprintf(stderr, L"Smoke: closing settings cancels its unfinished draft tool.\n");
            SendMessageW(window, WM_COMMAND, IDCANCEL, 0);
        };
        const auto result = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_SETTINGS),
            owner, SettingsProcedure, reinterpret_cast<LPARAM>(&context));
        if (result != IDCANCEL || context.error || !passed || registry.writes != 0 ||
            context.tools->Window() != nullptr || context.tools->Running()) {
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
                static_cast<LRESULT>(expected) &&
                (SettingsButtonState(window, IDC_CONFIG_DIRECTORY_FIRST) == BST_CHECKED) == (action == 3);
            std::fwprintf(stderr, L"Smoke: settings native buttons, checkbox, and combo boxes.\n");
            for (const int id : {IDOK, IDCANCEL, IDC_BROWSE_EXECUTABLE, IDC_ADD_FILES,
                IDC_BROWSE_DIRECTORY, IDC_CHECK_CONFIG, IDC_PREVIEW_CONFIG,
                IDC_CONFIG_DIRECTORY_FIRST, IDC_LOGON_STARTUP, IDC_LANGUAGE, IDC_THEME}) {
                passed = SmokeTestNativeControl(SettingsControl(window, id)) && passed;
            }
            SendMessageW(SettingsControl(window, IDC_CONFIG_DIRECTORY_FIRST), BM_CLICK, 0, 0);
            passed = passed &&
                (SettingsButtonState(window, IDC_CONFIG_DIRECTORY_FIRST) == BST_CHECKED) == (action != 3);
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
            settings.theme != (action >= 2 ? ThemeMode::Dark : ThemeMode::Light) ||
            settings.configDirectoryFirst != (action >= 2)) {
            return false;
        }
        if (action < 2 && std::filesystem::exists(root / L"data" / L"settings.ini")) {
            return false;
        }
    }
    const auto saved = LoadSettings(root);
    return saved.success && saved.settings.theme == ThemeMode::Dark && saved.settings.configDirectoryFirst;
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
