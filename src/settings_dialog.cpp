#include "settings_dialog.h"
#include "resource.h"
#include "win32.h"
#include "theme_window.h"
#include "ui_layout.h"
#include "xray_config.h"
#include "xray_tool_window.h"

#include <commctrl.h>
#include <oleacc.h>
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
constexpr int kConfigFileCheckSize = 17;
constexpr int kConfigFileCheckPadding = 6;

struct ConfigFileRow {
    std::wstring path;
    bool enabled = true;
};

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
    HFONT pathFont = nullptr;
    std::vector<ConfigFileRow> configFiles;
    bool refreshingConfigFiles = false;
    int configFileColumnWidth = 0;
    std::wstring configFileTooltip;
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
    if (pathFont != nullptr) { DeleteObject(pathFont); }
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

Settings ConfigDraft(const Context& context)
{
    Settings draft = context.settings;
    const HWND body = context.contents;
    draft.executable = WindowText(GetDlgItem(body, IDC_EXECUTABLE));
    draft.configDirectory = WindowText(GetDlgItem(body, IDC_CONFIG_DIRECTORY));
    draft.configFiles.clear();
    draft.disabledConfigFiles.clear();
    for (const auto& row : context.configFiles) {
        if (!row.enabled) { draft.disabledConfigFiles.insert(draft.configFiles.size()); }
        draft.configFiles.push_back(row.path);
    }
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

std::vector<int> SelectedConfigFiles(HWND list)
{
    std::vector<int> selected;
    for (int index = ListView_GetNextItem(list, -1, LVNI_SELECTED); index != -1;
        index = ListView_GetNextItem(list, index, LVNI_SELECTED)) {
        selected.push_back(index);
    }
    return selected;
}

void InitConfigFileList(HWND list)
{
    ListView_SetExtendedListViewStyle(list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    LVCOLUMNW column{};
    column.mask = LVCF_WIDTH;
    if (SendMessageW(list, LVM_INSERTCOLUMNW, 0, reinterpret_cast<LPARAM>(&column)) == -1) {
        throw std::runtime_error(Text("Cannot create the config file list.", "无法创建配置文件列表。"));
    }
}

void SizeConfigFileChecks(HWND list)
{
    const int size = UiScale(list, kConfigFileCheckSize);
    const int padding = UiScale(list, kConfigFileCheckPadding);
    const int width = size + padding;
    const auto current = ListView_GetImageList(list, LVSIL_STATE);
    int currentWidth = 0, currentHeight = 0;
    if (current != nullptr && ImageList_GetIconSize(current, &currentWidth, &currentHeight) &&
        currentWidth == width && currentHeight == size) {
        return;
    }

    struct Images {
        HDC device = CreateCompatibleDC(nullptr);
        HBITMAP bitmap = nullptr;
        HGDIOBJ saved = nullptr;
        HIMAGELIST value = nullptr;
        ~Images()
        {
            if (saved != nullptr && saved != HGDI_ERROR) { SelectObject(device, saved); }
            if (bitmap != nullptr) { DeleteObject(bitmap); }
            if (device != nullptr) { DeleteDC(device); }
            if (value != nullptr) { ImageList_Destroy(value); }
        }
    } images;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width * 2;
    info.bmiHeader.biHeight = -size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    images.bitmap = CreateDIBSection(images.device, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    images.value = ImageList_Create(width, size, ILC_COLOR32, 2, 0);
    if (images.device == nullptr || images.bitmap == nullptr || bits == nullptr || images.value == nullptr) {
        throw std::runtime_error(Text("Cannot create config file checkbox images.", "无法创建配置文件复选框图像。"));
    }
    images.saved = SelectObject(images.device, images.bitmap);
    if (images.saved == nullptr || images.saved == HGDI_ERROR) {
        throw std::runtime_error(Text("Cannot select the checkbox bitmap.", "无法选中复选框位图。"));
    }

    auto* pixels = static_cast<DWORD*>(bits);
    std::fill_n(pixels, static_cast<size_t>(width) * 2 * size, DWORD{0});
    for (int state = 0; state < 2; ++state) {
        const int left = state * width + padding / 2;
        RECT bounds{left, 0, left + size, size};
        if (!DrawFrameControl(images.device, &bounds, DFC_BUTTON,
            DFCS_BUTTONCHECK | DFCS_FLAT | (state == 0 ? 0 : DFCS_CHECKED))) {
            throw std::runtime_error(Text("Cannot draw config file checkbox images.", "无法绘制配置文件复选框图像。"));
        }
    }
    GdiFlush();
    for (int state = 0; state < 2; ++state) {
        const int left = state * width + padding / 2;
        for (int y = 0; y < size; ++y) {
            auto* row = pixels + y * width * 2 + left;
            for (int x = 0; x < size; ++x) { row[x] |= 0xFF000000u; }
        }
    }
    SelectObject(images.device, images.saved);
    images.saved = nullptr;
    if (ImageList_Add(images.value, images.bitmap, nullptr) != 0) {
        throw std::runtime_error(Text("Cannot add config file checkbox images.", "无法添加配置文件复选框图像。"));
    }

    const auto previous = ListView_SetImageList(list, images.value, LVSIL_STATE);
    if (ListView_GetImageList(list, LVSIL_STATE) != images.value) {
        throw std::runtime_error(Text("Cannot set config file checkbox images.", "无法设置配置文件复选框图像。"));
    }
    images.value = nullptr;
    if (previous != nullptr) { ImageList_Destroy(previous); }
}

void UpdateConfigFileActions(const Context& context)
{
    const auto selected = SelectedConfigFiles(GetDlgItem(context.contents, IDC_CONFIG_FILES));
    const int count = static_cast<int>(selected.size());
    const int fileCount = static_cast<int>(context.configFiles.size());
    const auto enable = [&](int id, bool enabled) {
        const HWND button = GetDlgItem(context.contents, id);
        if (!enabled && GetFocus() == button) {
            SetFocus(GetDlgItem(context.contents, IDC_CONFIG_FILES));
        }
        EnableWindow(button, enabled);
    };
    enable(IDC_MOVE_CONFIG_UP, count != 0 && selected.back() >= count);
    enable(IDC_MOVE_CONFIG_DOWN, count != 0 && selected.front() < fileCount - count);
    enable(IDC_REMOVE_CONFIG, count != 0);
    enable(IDC_CONVERT_CONFIG, count != 0);
}

void SizeConfigFileColumn(const Context& context)
{
    const HWND list = GetDlgItem(context.contents, IDC_CONFIG_FILES);
    RECT client{};
    GetClientRect(list, &client);
    const int width = (std::max)(context.configFileColumnWidth, static_cast<int>(client.right));
    if (ListView_GetColumnWidth(list, 0) != width) {
        ListView_SetColumnWidth(list, 0, width);
    }
}

void UpdateConfigFileExtent(Context& context)
{
    const HWND list = GetDlgItem(context.contents, IDC_CONFIG_FILES);
    LONG width = 0;
    for (size_t index = 0; index < context.configFiles.size(); ++index) {
        const auto label = std::to_wstring(index + 1) + L".  " + context.configFiles[index].path;
        // The native label is limited to 260 characters. Hovering shows the original path.
        width = (std::max)(width, ControlTextSize(list, std::wstring_view(label).substr(0, 260)).cx);
    }
    context.configFileColumnWidth = width + UiScale(list, kConfigFileCheckSize + kConfigFileCheckPadding + 12);
    SizeConfigFileColumn(context);
}

void DrawConfigFileSelection(HWND list, HDC device, const ThemePalette& colors)
{
    RECT client{};
    GetClientRect(list, &client);
    const int saved = SaveDC(device);
    if (saved == 0) { return; }
    SetDCBrushColor(device, colors.selection);
    SetTextColor(device, colors.selectionText);
    SetBkMode(device, TRANSPARENT);
    if (const auto font = reinterpret_cast<HFONT>(SendMessageW(list, WM_GETFONT, 0, 0))) {
        SelectObject(device, font);
    }
    const HBRUSH brush = static_cast<HBRUSH>(GetStockObject(DC_BRUSH));
    const bool showFocus = GetFocus() == list && (SendMessageW(list, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS) == 0;
    const int top = (std::max)(0, ListView_GetTopIndex(list));
    for (int index = ListView_GetNextItem(list, top - 1, LVNI_SELECTED); index != -1;
        index = ListView_GetNextItem(list, index, LVNI_SELECTED)) {
        RECT row{}, label{};
        if (!ListView_GetItemRect(list, index, &row, LVIR_BOUNDS) ||
            !ListView_GetItemRect(list, index, &label, LVIR_LABEL)) { continue; }
        if (row.top >= client.bottom) { break; }
        // Keep the checkbox column native. Windows has already drawn the state image.
        row.left = label.left;
        RECT visible{};
        if (!IntersectRect(&visible, &row, &client)) { continue; }
        FillRect(device, &visible, brush);
        std::array<wchar_t, 261> caption{};
        ListView_GetItemText(list, index, 0, caption.data(), static_cast<int>(caption.size()));
        RECT text = row;
        text.left += UiScale(list, 2);
        text.right -= UiScale(list, 2);
        DrawTextW(device, caption.data(), -1, &text,
            DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (showFocus && (ListView_GetItemState(list, index, LVIS_FOCUSED) & LVIS_FOCUSED) != 0) {
            DrawFocusRect(device, &row);
        }
    }
    RestoreDC(device, saved);
}

void RefreshConfigFiles(Context& context, const std::vector<int>& selected = {}, int reveal = -1)
{
    const HWND list = GetDlgItem(context.contents, IDC_CONFIG_FILES);
    const int top = ListView_GetTopIndex(list);
    {
        context.refreshingConfigFiles = true;
        SendMessageW(list, WM_SETREDRAW, FALSE, 0);
        struct Redraw {
            Context& context;
            HWND list;
            ~Redraw() {
                context.refreshingConfigFiles = false;
                SendMessageW(list, WM_SETREDRAW, TRUE, 0);
                RedrawWindow(list, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME);
            }
        } redraw{context, list};
        ListView_DeleteAllItems(list);
        for (size_t index = 0; index < context.configFiles.size(); ++index) {
            const auto& row = context.configFiles[index];
            auto label = std::to_wstring(index + 1) + L".  " + row.path;
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = static_cast<int>(index);
            item.pszText = label.data();
            if (SendMessageW(list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item)) == -1) {
                throw std::runtime_error(Text("Cannot display the config file list.", "无法显示配置文件列表。"));
            }
            ListView_SetCheckState(list, item.iItem, row.enabled);
        }
        UpdateConfigFileExtent(context);
        for (const int index : selected) {
            ListView_SetItemState(list, index, LVIS_SELECTED, LVIS_SELECTED);
        }
        RECT row{};
        if (top > 0 && ListView_GetItemRect(list, top, &row, LVIR_BOUNDS)) {
            ListView_Scroll(list, 0, row.top);
        }
        if (reveal >= 0) {
            ListView_SetSelectionMark(list, reveal);
            ListView_SetItemState(list, reveal, LVIS_FOCUSED, LVIS_FOCUSED);
            ListView_EnsureVisible(list, reveal, FALSE);
        }
    }
    UpdateConfigFileActions(context);
}

void MoveConfigFiles(Context& context, bool up)
{
    auto selected = SelectedConfigFiles(GetDlgItem(context.contents, IDC_CONFIG_FILES));
    const int count = static_cast<int>(selected.size());
    if (count == 0) { return; }
    for (int step = 0; step < count; ++step) {
        const int position = up ? step : count - step - 1;
        int& index = selected[position];
        const int next = index + (up ? -1 : 1);
        const int neighbor = position + (up ? -1 : 1);
        if (next < 0 || next >= static_cast<int>(context.configFiles.size()) ||
            (neighbor >= 0 && neighbor < count && selected[neighbor] == next)) {
            continue;
        }
        std::swap(context.configFiles[index], context.configFiles[next]);
        index = next;
    }
    RefreshConfigFiles(context, selected, up ? selected.front() : selected.back());
}

void RemoveConfigFiles(Context& context)
{
    const auto selected = SelectedConfigFiles(GetDlgItem(context.contents, IDC_CONFIG_FILES));
    if (selected.empty()) { return; }
    auto nextSelected = selected.begin();
    int index = 0;
    std::erase_if(context.configFiles, [&](const ConfigFileRow&) {
        const bool remove = nextSelected != selected.end() && *nextSelected == index;
        if (remove) { ++nextSelected; }
        ++index;
        return remove;
    });
    if (context.configFiles.empty()) {
        RefreshConfigFiles(context);
    } else {
        const int next = (std::min)(selected.front(), static_cast<int>(context.configFiles.size()) - 1);
        RefreshConfigFiles(context, {next}, next);
    }
}

bool EqualWindowsRoot(const std::wstring& left, const std::wstring& right)
{
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}

std::optional<std::wstring> ConvertWindowsPath(
    const std::filesystem::path& root, const std::wstring& value, bool toAbsolute)
{
    using std::filesystem::path;
    if (value.empty() || value == L"stdin:" || value.find(L"://") != std::wstring::npos) {
        return std::nullopt;
    }
    path input(value);
    input.make_preferred();
    if (input.is_absolute() == toAbsolute) {
        // Drive-relative and rooted paths do not have a portable relative representation.
        if (!toAbsolute && (input.has_root_name() || input.has_root_directory())) { return std::nullopt; }
        return value;
    }
    if ((input.has_root_name() || input.has_root_directory()) && !input.is_absolute()) {
        return std::nullopt;
    }
    const auto base = root.lexically_normal().make_preferred();
    const auto absolute = ResolvePath(root, value).make_preferred();
    if (!base.is_absolute() || !absolute.is_absolute() ||
        base.native().starts_with(L"\\\\?\\") || base.native().starts_with(L"\\\\.\\") ||
        absolute.native().starts_with(L"\\\\?\\") || absolute.native().starts_with(L"\\\\.\\")) {
        return std::nullopt;
    }
    if (toAbsolute) { return absolute.native(); }
    const auto baseRoot = base.root_name().native();
    if (!EqualWindowsRoot(baseRoot, absolute.root_name().native())) { return std::nullopt; }
    const auto baseParts = base.relative_path();
    const auto targetParts = absolute.relative_path();
    auto from = baseParts.begin();
    auto to = targetParts.begin();
    if (baseRoot.starts_with(L"\\\\")) {
        // The MSVC root name includes the UNC server. The share is the first component.
        if (from == baseParts.end() || to == targetParts.end() ||
            !EqualWindowsRoot(from->native(), to->native())) {
            return std::nullopt;
        }
        ++from;
        ++to;
    }
    while (from != baseParts.end() && to != targetParts.end() && *from == *to) {
        ++from;
        ++to;
    }
    path relative;
    for (; from != baseParts.end(); ++from) {
        if (!from->empty()) { relative /= L".."; }
    }
    if (relative.empty()) { relative = L"."; }
    for (; to != targetParts.end(); ++to) {
        relative /= *to;
    }
    return relative == path(L".") ? L".\\" : relative.native();
}

int SplitButtonWidth(HWND button)
{
    return TextButtonWidth(button) + UiScale(button, 22);
}

enum class PathConversion { Relative, Absolute, Toggle };

void ConvertSettingPaths(Context& context, int buttonId, PathConversion direction)
{
    const bool fileList = buttonId == IDC_CONVERT_CONFIG;
    const int inputId = buttonId == IDC_BROWSE_EXECUTABLE ? IDC_EXECUTABLE : IDC_CONFIG_DIRECTORY;
    const HWND input = GetDlgItem(context.contents, fileList ? IDC_CONFIG_FILES : inputId);
    const auto selected = fileList ? SelectedConfigFiles(input) : std::vector<int>{0};
    const auto inputValue = fileList ? std::wstring{} : WindowText(input);
    struct Change { int index; std::wstring value; };
    std::vector<Change> changes, needsConsent;
    std::wstring skipped, tailPaths;
    const bool directory = buttonId == IDC_BROWSE_DIRECTORY;
    for (const int index : selected) {
        const auto& value = fileList ? context.configFiles[index].path : inputValue;
        if (value.empty()) { continue; }
        const bool toAbsolute = direction == PathConversion::Absolute ||
            (direction == PathConversion::Toggle && !std::filesystem::path(value).is_absolute());
        auto converted = ConvertWindowsPath(context.root, value, toAbsolute);
        if (!converted) {
            skipped += L"\n" + value;
        } else if (*converted != value) {
            if ((fileList || directory) &&
                XrayConfigPathUsesTail(value, directory) != XrayConfigPathUsesTail(*converted, directory)) {
                tailPaths += L"\n" + value + L"\n  → " + *converted;
                needsConsent.push_back({index, std::move(*converted)});
            } else {
                changes.push_back({index, std::move(*converted)});
            }
        }
    }
    if (!needsConsent.empty()) {
        const auto message = Text(
            L"These path changes add or remove “tail”. Xray may change where it inserts new outbounds when merging configurations.\n\nConvert these paths too?\n",
            L"以下路径转换会增加或移除 “tail”。Xray 合并配置时，新增出站的插入位置可能改变。\n\n也要转换这些路径吗？\n") + tailPaths;
        if (MessageBoxW(context.dialog, message.c_str(), Text(L"Convert paths", L"转换路径"),
            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES) {
            for (auto& change : needsConsent) { changes.push_back(std::move(change)); }
        }
    }
    if (!changes.empty()) {
        if (fileList) {
            for (auto& change : changes) { context.configFiles[change.index].path = std::move(change.value); }
            RefreshConfigFiles(context, selected, ListView_GetNextItem(input, -1, LVNI_FOCUSED));
        } else {
            SetWindowTextW(input, changes.front().value.c_str());
        }
    }
    if (!skipped.empty()) {
        const auto message = Text(
            L"These paths could not be converted. Relative paths require the same drive or network share as XTray.exe.\n",
            L"以下路径无法转换。相对路径要求目标与 XTray.exe 位于同一盘符或网络共享。\n") + skipped;
        MessageBoxW(context.dialog, message.c_str(), Text(L"Convert paths", L"转换路径"), MB_OK | MB_ICONINFORMATION);
    }
}

void ShowPathConversions(Context& context, int id)
{
    struct Popup {
        HMENU menu = CreatePopupMenu();
        ~Popup() { if (menu != nullptr) { DestroyMenu(menu); } }
    } popup;
    const auto captions = std::array{
        Text(L"Make relative", L"转为相对路径"), Text(L"Make absolute", L"转为绝对路径")};
    std::array<MSAAMENUINFO, 2> accessible{};
    const bool dark = context.theme.Colors().dark;
    const HWND button = GetDlgItem(context.contents, id);
    const bool enabled = GetWindowTextLengthW(GetDlgItem(context.contents,
        id == IDC_BROWSE_EXECUTABLE ? IDC_EXECUTABLE : IDC_CONFIG_DIRECTORY)) != 0;
    if (popup.menu == nullptr) {
        throw std::runtime_error(Text("Cannot create the path conversion menu.", "无法创建路径转换菜单。"));
    }
    for (UINT index = 0; index < captions.size(); ++index) {
        const auto caption = const_cast<wchar_t*>(captions[index]);
        accessible[index] = {MSAA_MENU_SIG, static_cast<DWORD>(std::wcslen(caption)), caption};
        MENUITEMINFOW item{sizeof(item)};
        item.fMask = MIIM_FTYPE | MIIM_STRING | MIIM_STATE | MIIM_ID | MIIM_DATA;
        item.fType = dark ? MFT_OWNERDRAW : MFT_STRING;
        item.fState = enabled ? MFS_ENABLED : MFS_DISABLED;
        item.wID = index + 1;
        item.dwTypeData = caption;
        item.dwItemData = dark ? reinterpret_cast<ULONG_PTR>(&accessible[index]) : 0;
        if (!InsertMenuItemW(popup.menu, index, TRUE, &item)) {
            throw std::runtime_error(Text("Cannot populate the path conversion menu.", "无法设置路径转换菜单。"));
        }
    }
    RECT bounds{};
    GetWindowRect(button, &bounds);
    const UINT command = TrackPopupMenuEx(popup.menu,
        TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTALIGN | TPM_RIGHTBUTTON,
        bounds.right, bounds.bottom, context.dialog, nullptr);
    if (command != 0) {
        ConvertSettingPaths(context, id, command == 2 ? PathConversion::Absolute : PathConversion::Relative);
    }
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
    tool.uId = reinterpret_cast<UINT_PTR>(SettingsControl(context.dialog, IDC_CONFIG_FILES));
    tool.lpszText = LPSTR_TEXTCALLBACKW;
    if (!SendMessageW(context.tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool))) {
        throw std::runtime_error(Text("Cannot register the config file tooltip.", "无法注册配置文件悬停提示。"));
    }
    SendMessageW(context.tooltip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 15000);
}

void SetSettingsPathFont(Context& context)
{
    LOGFONTW description{};
    const auto bodyFont = reinterpret_cast<HFONT>(SendMessageW(context.contents, WM_GETFONT, 0, 0));
    if (GetObjectW(bodyFont, sizeof(description), &description) == 0) {
        throw std::runtime_error(Text("Cannot read the settings font.", "无法读取设置窗口字体。"));
    }
    description.lfHeight = -MulDiv(11, static_cast<int>(GetDpiForWindow(context.contents)), 72);
    const HFONT font = CreateFontIndirectW(&description);
    if (font == nullptr) {
        throw std::runtime_error(Text("Cannot create the settings path font.", "无法创建设置路径字体。"));
    }
    for (const int id : {IDC_EXECUTABLE, IDC_CONFIG_FILES, IDC_CONFIG_DIRECTORY}) {
        SendMessageW(GetDlgItem(context.contents, id), WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
    if (context.pathFont != nullptr) { DeleteObject(context.pathFont); }
    context.pathFont = font;
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
        static_cast<int>(ControlTextSize(GetDlgItem(body, IDC_LOG_LINES)).cy) + UiScale(body, 8));
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
        const auto pathRow = [&](int editId, int frameId, int browseId) {
            const int buttonWidth = SplitButtonWidth(GetDlgItem(body, browseId));
            const int buttonX = margin + available - buttonWidth;
            const int fieldWidth = buttonX - gap - margin;
            const int inset = UiScale(body, 5);
            const int editHeight = SingleLineControlHeight(GetDlgItem(body, editId));
            const int height = (std::max)(rowHeight, editHeight + UiScale(body, 6));
            // The native single-line edit keeps its own caret and input behavior inside the taller frame.
            place(frameId, margin, y, fieldWidth, height);
            place(editId, margin + inset, y + (height - editHeight) / 2, fieldWidth - inset * 2, editHeight);
            place(browseId, buttonX, y, buttonWidth, height);
            y += height + sectionGap;
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
        pathRow(IDC_EXECUTABLE, IDC_EXECUTABLE_FRAME, IDC_BROWSE_EXECUTABLE);
        label(IDC_LABEL_CONFIG_FILES);
        units = {0, 0, 0, 75};
        MapDialogRect(body, &units);
        const int moveUpWidth = TextButtonWidth(GetDlgItem(body, IDC_MOVE_CONFIG_UP));
        const int moveDownWidth = TextButtonWidth(GetDlgItem(body, IDC_MOVE_CONFIG_DOWN));
        const int actionsWidth = (std::max)({TextButtonWidth(GetDlgItem(body, IDC_ADD_FILES)),
            moveUpWidth + gap + moveDownWidth, TextButtonWidth(GetDlgItem(body, IDC_REMOVE_CONFIG)),
            TextButtonWidth(GetDlgItem(body, IDC_CONVERT_CONFIG))});
        const int actionHeight = rowHeight * 4 + gap * 3;
        const int listHeight = (std::max)(static_cast<int>(units.bottom), actionHeight);
        const bool stackFileActions = available - actionsWidth - gap < UiScale(body, 80);
        const int actionX = margin + available - actionsWidth;
        const int actionY = stackFileActions ? y + listHeight + gap : y;
        const int actionStep = rowHeight + gap;
        place(IDC_CONFIG_FILES, margin, y, stackFileActions ? available : actionX - gap - margin, listHeight);
        place(IDC_ADD_FILES, actionX, actionY, actionsWidth, rowHeight);
        place(IDC_MOVE_CONFIG_UP, actionX, actionY + actionStep,
            actionsWidth - gap - moveDownWidth, rowHeight);
        place(IDC_MOVE_CONFIG_DOWN, actionX + actionsWidth - moveDownWidth,
            actionY + actionStep, moveDownWidth, rowHeight);
        place(IDC_REMOVE_CONFIG, actionX, actionY + actionStep * 2, actionsWidth, rowHeight);
        place(IDC_CONVERT_CONFIG, actionX, actionY + actionStep * 3, actionsWidth, rowHeight);
        y += listHeight + sectionGap + (stackFileActions ? actionHeight + gap : 0);
        label(IDC_LABEL_CONFIG_DIRECTORY);
        pathRow(IDC_CONFIG_DIRECTORY, IDC_CONFIG_DIRECTORY_FRAME, IDC_BROWSE_DIRECTORY);
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
    SizeConfigFileColumn(context);
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
            InitConfigFileList(GetDlgItem(window, IDC_CONFIG_FILES));
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
        if (message == WM_NOTIFY && lParam != 0) {
            const auto& notification = *reinterpret_cast<const NMHDR*>(lParam);
            const auto id = static_cast<int>(notification.idFrom);
            if (id == IDC_CONFIG_FILES && notification.code == NM_CUSTOMDRAW) {
                const auto& draw = *reinterpret_cast<const NMLVCUSTOMDRAW*>(lParam);
                if (draw.nmcd.dwDrawStage == CDDS_POSTPAINT && !context->refreshingConfigFiles) {
                    DrawConfigFileSelection(notification.hwndFrom, draw.nmcd.hdc, context->contentTheme.Colors());
                }
                SetWindowLongPtrW(window, DWLP_MSGRESULT,
                    draw.nmcd.dwDrawStage == CDDS_PREPAINT ? CDRF_NOTIFYPOSTPAINT : CDRF_DODEFAULT);
                return TRUE;
            }
            if (id == IDC_CONFIG_FILES && notification.code == LVN_ITEMCHANGED &&
                !context->refreshingConfigFiles) {
                const auto& item = *reinterpret_cast<const NMLISTVIEW*>(lParam);
                if ((item.uChanged & LVIF_STATE) != 0) {
                    const UINT changed = item.uOldState ^ item.uNewState;
                    if ((changed & LVIS_STATEIMAGEMASK) != 0 && item.iItem >= 0 &&
                        static_cast<size_t>(item.iItem) < context->configFiles.size()) {
                        context->configFiles[item.iItem].enabled =
                            (item.uNewState & LVIS_STATEIMAGEMASK) == INDEXTOSTATEIMAGEMASK(2);
                    }
                    if ((changed & LVIS_SELECTED) != 0) { UpdateConfigFileActions(*context); }
                }
                return TRUE;
            }
            const HWND list = GetDlgItem(window, IDC_CONFIG_FILES);
            if (notification.hwndFrom == context->tooltip && notification.code == TTN_GETDISPINFOW &&
                notification.idFrom == reinterpret_cast<UINT_PTR>(list)) {
                LVHITTESTINFO hit{};
                GetCursorPos(&hit.pt);
                ScreenToClient(list, &hit.pt);
                const int index = ListView_HitTest(list, &hit);
                context->configFileTooltip.clear();
                if (index >= 0 && static_cast<size_t>(index) < context->configFiles.size()) {
                    context->configFileTooltip = context->configFiles[index].path;
                }
                reinterpret_cast<NMTTDISPINFOW*>(lParam)->lpszText = context->configFileTooltip.data();
                return TRUE;
            }
            if (notification.code == BCN_DROPDOWN && (id == IDC_BROWSE_EXECUTABLE ||
                id == IDC_BROWSE_DIRECTORY)) {
                ShowPathConversions(*context, id);
                return TRUE;
            }
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
    set(IDC_LABEL_CONFIG_FILES, Text(L"Config files (checked files load in list order):",
        L"配置文件（勾选项按列表顺序加载）："));
    set(IDC_LABEL_CONFIG_DIRECTORY, Text(L"Config directory (optional):", L"配置目录（可留空）："));
    set(IDC_CONFIG_DIRECTORY_FIRST, Text(L"Load config directory before config files", L"先加载配置目录，再加载配置文件"));
    set(IDC_LABEL_LOG_LINES, Text(L"Log lines per source:", L"每类日志保留行数："));
    set(IDC_LABEL_LANGUAGE, Text(L"Language:", L"语言："));
    set(IDC_LABEL_THEME, Text(L"Appearance:", L"外观："));
    set(IDC_LOGON_STARTUP, Text(L"Start XTray and Xray when I sign in", L"登录后自动启动 XTray 和 Xray"));
    set(IDC_BROWSE_EXECUTABLE, Text(L"Browse...", L"浏览…"));
    set(IDC_ADD_FILES, Text(L"Add files...", L"添加文件…"));
    set(IDC_MOVE_CONFIG_UP, Text(L"↑ Up", L"↑ 上移"));
    set(IDC_MOVE_CONFIG_DOWN, Text(L"↓ Down", L"↓ 下移"));
    set(IDC_REMOVE_CONFIG, Text(L"Remove", L"移除"));
    set(IDC_CONVERT_CONFIG, Text(L"Convert paths", L"转换路径"));
    set(IDC_BROWSE_DIRECTORY, Text(L"Browse...", L"浏览…"));
    set(IDC_CHECK_CONFIG, Text(L"Check current configuration", L"检查当前配置"));
    set(IDC_PREVIEW_CONFIG, Text(L"Preview current configuration", L"预览当前配置"));
    set(IDC_HINT_CONFIG_TOOLS, Text(L"Uses this form's current contents without saving.",
        L"使用本窗口当前填写的内容执行，无需先保存。"));
    set(IDC_HINT_RELATIVE, Text(L"Relative paths use the XTray.exe directory.",
        L"相对路径以 XTray.exe 所在目录为基准。"));
    set(IDC_HINT_APPLY, Text(L"Paths and load order apply on the next run/restart; at least one config source is required.",
        L"路径和加载顺序下次运行或重启生效；运行前请启用配置文件或设置配置目录。"));
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
        SetSettingsPathFont(*context);
        SendMessageW(SettingsControl(dialog, IDC_LANGUAGE), CB_SETCURSEL,
            static_cast<WPARAM>(context->settings.language), 0);
        SendMessageW(SettingsControl(dialog, IDC_THEME), CB_SETCURSEL,
            static_cast<WPARAM>(context->settings.theme), 0);
        SetWindowTextW(SettingsControl(dialog, IDC_EXECUTABLE), context->settings.executable.c_str());
        SetWindowTextW(SettingsControl(dialog, IDC_CONFIG_DIRECTORY), context->settings.configDirectory.c_str());
        CheckSettingsButton(dialog, IDC_CONFIG_DIRECTORY_FIRST,
            context->settings.configDirectoryFirst ? BST_CHECKED : BST_UNCHECKED);
        for (size_t index = 0; index < context->settings.configFiles.size(); ++index) {
            context->configFiles.push_back({context->settings.configFiles[index],
                !context->settings.disabledConfigFiles.contains(index)});
        }
        RefreshConfigFiles(*context);
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
        SizeConfigFileChecks(SettingsControl(dialog, IDC_CONFIG_FILES));
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
        SetSettingsPathFont(*context);
        context->theme.Apply(context->theme.Colors().dark, true);
        SizeConfigFileChecks(SettingsControl(dialog, IDC_CONFIG_FILES));
        UpdateConfigFileExtent(*context);
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
    case IDC_EXECUTABLE_FRAME:
    case IDC_CONFIG_DIRECTORY_FRAME:
        if (HIWORD(wParam) == STN_CLICKED) {
            SetFocus(SettingsControl(dialog, LOWORD(wParam) == IDC_EXECUTABLE_FRAME
                ? IDC_EXECUTABLE : IDC_CONFIG_DIRECTORY));
        }
        return TRUE;
    case IDC_CHECK_CONFIG:
    case IDC_PREVIEW_CONFIG: {
        try {
            const auto kind = LOWORD(wParam) == IDC_CHECK_CONFIG ? XrayToolKind::Check : XrayToolKind::Preview;
            context->tools->Open(dialog, kind, ConfigDraft(*context), true,
                context->theme.Colors().dark, context->quietTools, XrayToolSource::SettingsDraft);
        } catch (const std::exception& exception) {
            context->tools->CloseOwned(dialog);
            if (context->quietTools) { throw; }
            const auto error = FromUtf8(exception.what());
            MessageBoxW(dialog, error.c_str(), Text(L"Xray tool", L"Xray 工具"), MB_OK | MB_ICONERROR);
        }
        return TRUE;
    }
    case IDC_MOVE_CONFIG_UP:
    case IDC_MOVE_CONFIG_DOWN:
        MoveConfigFiles(*context, LOWORD(wParam) == IDC_MOVE_CONFIG_UP);
        return TRUE;
    case IDC_REMOVE_CONFIG:
        RemoveConfigFiles(*context);
        return TRUE;
    case IDC_CONVERT_CONFIG:
        ConvertSettingPaths(*context, IDC_CONVERT_CONFIG, PathConversion::Toggle);
        return TRUE;
    case IDC_BROWSE_EXECUTABLE:
    case IDC_ADD_FILES:
    case IDC_BROWSE_DIRECTORY: {
        const auto id = LOWORD(wParam);
        const auto paths = Browse(dialog, id == IDC_BROWSE_DIRECTORY, id == IDC_ADD_FILES);
        if (paths.empty()) {
            return TRUE;
        }
        if (id == IDC_ADD_FILES) {
            const int first = static_cast<int>(context->configFiles.size());
            for (const auto& path : paths) { context->configFiles.push_back({path}); }
            std::vector<int> added;
            for (int index = first; index < static_cast<int>(context->configFiles.size()); ++index) {
                added.push_back(index);
            }
            RefreshConfigFiles(*context, added, added.back());
        } else {
            SetWindowTextW(SettingsControl(dialog, id == IDC_BROWSE_EXECUTABLE
                ? IDC_EXECUTABLE : IDC_CONFIG_DIRECTORY), paths.front().c_str());
        }
        return TRUE;
    }
    case IDOK: {
        Settings draft = ConfigDraft(*context);
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

static bool SmokeTestConfigFileSelection(HWND list, HWND pathInput, const ThemePalette& colors)
{
    const int index = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    RECT client{}, row{}, label{};
    if (index == -1 || !GetClientRect(list, &client) ||
        !ListView_GetItemRect(list, index, &row, LVIR_BOUNDS) ||
        !ListView_GetItemRect(list, index, &label, LVIR_LABEL)) {
        return false;
    }
    struct Canvas {
        HDC device = CreateCompatibleDC(nullptr);
        HBITMAP bitmap = nullptr;
        HGDIOBJ previous = nullptr;
        ~Canvas() {
            if (previous != nullptr && previous != HGDI_ERROR) { SelectObject(device, previous); }
            if (bitmap != nullptr) { DeleteObject(bitmap); }
            if (device != nullptr) { DeleteDC(device); }
        }
    } canvas;
    HDC screen = GetDC(list);
    if (screen == nullptr || canvas.device == nullptr) {
        if (screen != nullptr) { ReleaseDC(list, screen); }
        return false;
    }
    canvas.bitmap = CreateCompatibleBitmap(screen, client.right, client.bottom);
    ReleaseDC(list, screen);
    if (canvas.bitmap == nullptr) { return false; }
    canvas.previous = SelectObject(canvas.device, canvas.bitmap);
    if (canvas.previous == nullptr || canvas.previous == HGDI_ERROR) { return false; }

    const int x = (std::min)(row.right, client.right) - UiScale(list, 10);
    const int y = (row.top + row.bottom) / 2;
    for (const HWND focus : {list, pathInput}) {
        SetFocus(focus);
        PatBlt(canvas.device, 0, 0, client.right, client.bottom, BLACKNESS);
        SendMessageW(list, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(canvas.device), PRF_CLIENT | PRF_ERASEBKGND);
        const COLORREF background = GetPixel(canvas.device, x, y);
        bool readable = false;
        const int right = (std::min)(static_cast<int>(label.left) + UiScale(list, 100), x);
        for (int textY = row.top + 2; textY < row.bottom - 2 && !readable; ++textY) {
            for (int textX = label.left + 2; textX < right; ++textX) {
                if (GetPixel(canvas.device, textX, textY) == colors.selectionText) {
                    readable = true;
                    break;
                }
            }
        }
        if (GetFocus() != focus || background != colors.selection || !readable) {
            std::fwprintf(stderr, L"Smoke: config file selection color failed (dark=%d, focused=%d, background=%lx, text=%d).\n",
                colors.dark, focus == list, background, readable);
            return false;
        }
    }
    return true;
}

bool SmokeTestSettingsLayout(HWND owner, const std::filesystem::path& root)
{
    std::fwprintf(stderr, L"Smoke: settings scroll viewport, fixed footer, and text-sized buttons.\n");
    std::fwprintf(stderr, L"Smoke: Windows config path forms and Xray tail matching.\n");
    const std::filesystem::path pathRoot(L"D:\\XTray");
    if (ConvertWindowsPath(pathRoot, L"d:/XTray/configs/中文.json", false) != L".\\configs\\中文.json" ||
        ConvertWindowsPath(pathRoot, L"D:\\shared\\one.json", false) != L"..\\shared\\one.json" ||
        ConvertWindowsPath(pathRoot, L".\\configs\\中文.json", true) != L"D:\\XTray\\configs\\中文.json" ||
        ConvertWindowsPath(pathRoot, L"D:\\XTray", false) != L".\\" ||
        ConvertWindowsPath(pathRoot, L"D:config.json", true) ||
        ConvertWindowsPath(pathRoot, L"E:\\other\\config.json", false) ||
        ConvertWindowsPath(pathRoot, L"https://example.invalid/config.json", true) ||
        ConvertWindowsPath(L"\\\\server\\share\\XTray", L"\\\\SERVER\\SHARE\\config.json", false) != L"..\\config.json" ||
        ConvertWindowsPath(L"\\\\server\\share\\XTray", L"\\\\server\\other\\config.json", false) ||
        !XrayConfigPathUsesTail(L".\\TAIL\\config.json", false) ||
        XrayConfigPathUsesTail(L"./tail/../config", true)) {
        std::fwprintf(stderr, L"Smoke: config path conversion failed.\n");
        return false;
    }
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
            std::fwprintf(stderr, L"Smoke: ordered multi-select config list, move arrows, and draft path conversion.\n");
            const HWND list = SettingsControl(window, IDC_CONFIG_FILES);
            const HWND up = SettingsControl(window, IDC_MOVE_CONFIG_UP);
            const HWND down = SettingsControl(window, IDC_MOVE_CONFIG_DOWN);
            const HWND remove = SettingsControl(window, IDC_REMOVE_CONFIG);
            const auto browseStyle = [&](int id) {
                return (GetWindowLongPtrW(SettingsControl(window, id), GWL_STYLE) & BS_TYPEMASK) == BS_SPLITBUTTON;
            };
            const auto checkList = [&] {
                if ((ListView_GetExtendedListViewStyle(list) & LVS_EX_CHECKBOXES) == 0 ||
                    (GetWindowLongPtrW(list, GWL_STYLE) & LVS_SINGLESEL) != 0 ||
                    !browseStyle(IDC_BROWSE_EXECUTABLE) || !browseStyle(IDC_BROWSE_DIRECTORY) ||
                    (GetWindowLongPtrW(SettingsControl(window, IDC_CONVERT_CONFIG), GWL_STYLE) & BS_TYPEMASK)
                        != BS_PUSHBUTTON ||
                    WindowText(SettingsControl(window, IDC_CONVERT_CONFIG)) != Text(L"Convert paths", L"转换路径") ||
                    WindowText(up) != Text(L"↑ Up", L"↑ 上移") ||
                    WindowText(down) != Text(L"↓ Down", L"↓ 下移") ||
                    IsWindowEnabled(up) || IsWindowEnabled(down) || IsWindowEnabled(remove)) {
                    return false;
                }
                const std::wstring longPath = L".\\" + std::wstring(220, L'中') + L"-tail.json";
                const std::vector<std::wstring> files{L".\\one-tail.json", longPath, L".\\two.json",
                    L".\\one-tail.json", L".\\three.json"};
                for (const auto& path : files) { context.configFiles.push_back({path}); }
                RefreshConfigFiles(context, {1, 2, 4});
                if (ListView_GetColumnWidth(list, 0) <= UiScale(list, 360) ||
                    !IsWindowEnabled(up) || !IsWindowEnabled(down) || !ListView_GetCheckState(list, 1)) {
                    return false;
                }
                const auto images = ListView_GetImageList(list, LVSIL_STATE);
                const int checkSize = UiScale(list, kConfigFileCheckSize);
                int imageWidth = 0, imageHeight = 0;
                RECT row{};
                if (images == nullptr || ImageList_GetImageCount(images) != 2 ||
                    !ImageList_GetIconSize(images, &imageWidth, &imageHeight) ||
                    imageHeight != checkSize || imageWidth != checkSize + UiScale(list, kConfigFileCheckPadding) ||
                    !ListView_GetItemRect(list, 1, &row, LVIR_BOUNDS) || row.bottom - row.top < checkSize) {
                    std::fwprintf(stderr, L"Smoke: config file checkbox dimensions failed.\n");
                    return false;
                }
                LVHITTESTINFO hit{};
                hit.pt = {row.left + checkSize, (row.top + row.bottom) / 2};
                if (ListView_HitTest(list, &hit) != 1 || (hit.flags & LVHT_ONITEMSTATEICON) == 0) {
                    std::fwprintf(stderr, L"Smoke: enlarged config file checkbox hit area failed.\n");
                    return false;
                }
                ListView_SetCheckState(list, 1, FALSE);
                if (ConfigDraft(context).disabledConfigFiles != std::set<size_t>{1}) { return false; }
                SendMessageW(up, BM_CLICK, 0, 0);
                if (SelectedConfigFiles(list) != std::vector<int>{0, 1, 3} ||
                    ListView_GetCheckState(list, 0) ||
                    ConfigDraft(context).disabledConfigFiles != std::set<size_t>{0} ||
                    ConfigDraft(context).configFiles != std::vector<std::wstring>{
                        files[1], files[2], files[0], files[4], files[3]}) { return false; }
                SendMessageW(down, BM_CLICK, 0, 0);
                if (SelectedConfigFiles(list) != std::vector<int>{1, 2, 4} ||
                    ConfigDraft(context).configFiles != files || ListView_GetCheckState(list, 1) ||
                    ConfigDraft(context).disabledConfigFiles != std::set<size_t>{1}) {
                    return false;
                }
                RefreshConfigFiles(context, {0, 1});
                if (IsWindowEnabled(up) || !IsWindowEnabled(down)) { return false; }
                const HWND convert = SettingsControl(window, IDC_CONVERT_CONFIG);
                SendMessageW(convert, BM_CLICK, 0, 0);
                const auto absolute = root / L"one-tail.json";
                if (ConfigDraft(context).configFiles.front() != absolute.native() ||
                    ConfigDraft(context).configFiles[3] != files[3] ||
                    SelectedConfigFiles(list) != std::vector<int>{0, 1}) { return false; }
                SendMessageW(convert, BM_CLICK, 0, 0);
                if (ConfigDraft(context).configFiles != files) { return false; }
                const auto longAbsolute = ResolvePath(root, files[1]).native();
                context.configFiles[1].path = longAbsolute;
                RefreshConfigFiles(context, {0, 1});
                SendMessageW(convert, BM_CLICK, 0, 0);
                if (context.configFiles[0].path != absolute.native() || context.configFiles[1].path != files[1] ||
                    ListView_GetCheckState(list, 1)) { return false; }
                SendMessageW(convert, BM_CLICK, 0, 0);
                if (context.configFiles[0].path != files[0] || context.configFiles[1].path != longAbsolute) { return false; }
                SendMessageW(remove, BM_CLICK, 0, 0);
                std::array<wchar_t, 64> first{};
                ListView_GetItemText(list, 0, 0, first.data(), static_cast<int>(first.size()));
                const auto draft = ConfigDraft(context);
                return draft.configFiles == std::vector<std::wstring>{files[2], files[3], files[4]} &&
                    draft.disabledConfigFiles.empty() && ListView_GetItemCount(list) == 3 &&
                    std::wstring_view(first.data()) == L"1.  .\\two.json" && settings.configFiles.empty();
            };
            if (!checkList()) {
                std::fwprintf(stderr, L"Smoke: config file list order, selection, or path conversion failed.\n");
                passed = false;
            }
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
                RECT listBounds{}, add{}, moveUp{}, moveDown{}, remove{}, convert{};
                GetWindowRect(list, &listBounds);
                GetWindowRect(SettingsControl(window, IDC_ADD_FILES), &add);
                GetWindowRect(up, &moveUp);
                GetWindowRect(down, &moveDown);
                GetWindowRect(SettingsControl(window, IDC_REMOVE_CONFIG), &remove);
                GetWindowRect(SettingsControl(window, IDC_CONVERT_CONFIG), &convert);
                const int controlsGap = UiScale(window, 8);
                const bool actionsBeside = add.left > listBounds.right;
                if (add.left != moveUp.left || add.left != remove.left || add.left != convert.left ||
                    add.right != moveDown.right || add.right != remove.right || add.right != convert.right ||
                    moveUp.top != moveDown.top || moveDown.left - moveUp.right != controlsGap ||
                    add.bottom + controlsGap != moveUp.top ||
                    moveUp.bottom + controlsGap != remove.top || remove.bottom + controlsGap != convert.top ||
                    (actionsBeside ? add.top != listBounds.top || convert.bottom > listBounds.bottom
                        : add.top - listBounds.bottom != controlsGap)) {
                    std::fwprintf(stderr, L"Smoke: configuration file actions overlap or are not aligned.\n");
                    return false;
                }
                RECT directory{}, order{}, hint{}, check{}, preview{}, startup{};
                const HWND orderControl = SettingsControl(window, IDC_CONFIG_DIRECTORY_FIRST);
                GetWindowRect(SettingsControl(window, IDC_CONFIG_DIRECTORY_FRAME), &directory);
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
                passed = SmokeTestConfigFileSelection(list, SettingsControl(window, IDC_EXECUTABLE),
                    context.contentTheme.Colors()) && passed;
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
            for (const int id : {IDOK, IDCANCEL, IDC_BROWSE_EXECUTABLE, IDC_BROWSE_DIRECTORY}) {
                RECT button{};
                const HWND control = SettingsControl(window, id);
                GetWindowRect(control, &button);
                const auto text = ControlTextSize(control);
                const int padding = static_cast<int>(button.right - button.left - text.cx);
                const int split = id == IDC_BROWSE_EXECUTABLE || id == IDC_BROWSE_DIRECTORY
                    ? UiScale(control, 22) : 0;
                passed = passed && padding >= UiScale(control, 23) + split &&
                    padding <= UiScale(control, 25) + split;
            }
            const auto checkInputs = [&] {
                const HWND logLines = SettingsControl(window, IDC_LOG_LINES);
                const auto fontHeight = [](HWND control) {
                    LOGFONTW description{};
                    GetObjectW(reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0)),
                        sizeof(description), &description);
                    return std::abs(description.lfHeight);
                };
                const LONG pathFontHeight = fontHeight(SettingsControl(window, IDC_EXECUTABLE));
                RECT itemBounds{};
                if (pathFontHeight <= fontHeight(logLines) ||
                    fontHeight(list) != pathFontHeight ||
                    fontHeight(SettingsControl(window, IDC_CONFIG_DIRECTORY)) != pathFontHeight ||
                    !ListView_GetItemRect(list, 0, &itemBounds, LVIR_BOUNDS) ||
                    itemBounds.bottom - itemBounds.top < ControlTextSize(list, L"Path").cy) {
                    std::fwprintf(stderr, L"Smoke: settings path font or file row height failed.\n");
                    return false;
                }
                struct Input { int id, neighbor, frame; };
                for (const auto& [id, neighbor, frame] : {
                    Input{IDC_EXECUTABLE, IDC_BROWSE_EXECUTABLE, IDC_EXECUTABLE_FRAME},
                    Input{IDC_CONFIG_DIRECTORY, IDC_BROWSE_DIRECTORY, IDC_CONFIG_DIRECTORY_FRAME},
                    Input{IDC_LOG_LINES, IDC_LABEL_LOG_LINES, 0}}) {
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
                    if (frame != 0) {
                        const HWND frameControl = SettingsControl(window, frame);
                        RECT bounds{};
                        GetWindowRect(frameControl, &bounds);
                        if (bounds.top != other.top || bounds.bottom != other.bottom ||
                            bounds.left >= edit.left || bounds.right <= edit.right ||
                            bounds.top >= edit.top || bounds.bottom <= edit.bottom ||
                            GetWindow(control, GW_HWNDNEXT) != frameControl) {
                            std::fwprintf(stderr, L"Smoke: path frame size or placement failed (id=%d).\n", id);
                            return false;
                        }
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
            const HWND executableFrame = SettingsControl(window, IDC_EXECUTABLE_FRAME);
            SendMessageW(context.contents, WM_COMMAND, MAKEWPARAM(IDC_EXECUTABLE_FRAME, STN_CLICKED),
                reinterpret_cast<LPARAM>(executableFrame));
            passed = GetFocus() == executable && passed;
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
            context.configFiles = {{L".\\tool-ok.conf"}, {L".\\重复 ;= config.json"},
                {L".\\tool-fail.conf", false}};
            RefreshConfigFiles(context);
            std::filesystem::create_directories(root / L"draft configs");
            SetWindowTextW(SettingsControl(window, IDC_CONFIG_DIRECTORY), L".\\draft configs\\");
            CheckSettingsButton(window, IDC_CONFIG_DIRECTORY_FIRST, BST_CHECKED);
            SetWindowTextW(SettingsControl(window, IDC_LOG_LINES), L"0");
            SendMessageW(SettingsControl(window, IDC_CHECK_CONFIG), BM_CLICK, 0, 0);
            CheckSettingsButton(window, IDC_CONFIG_DIRECTORY_FIRST, BST_UNCHECKED);
            context.configFiles = {{L".\\tool-fail.conf"}};
            RefreshConfigFiles(context);
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
                WindowText(GetDlgItem(result, 4101)).find(L"tool-fail.conf") == std::wstring::npos &&
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
            context.configFiles = {{L".\\tool-ok.conf"}};
            RefreshConfigFiles(context);
            SendMessageW(SettingsControl(window, IDC_PREVIEW_CONFIG), BM_CLICK, 0, 0);
            passed = waitTool() && passed;
            passed = passed && WindowText(context.tools->Window()) == Text(L"Preview current configuration", L"预览当前配置") &&
                WindowText(GetDlgItem(context.tools->Window(), 4101)).find(L"-dump\r\n-confdir=.\\draft configs\\") != std::wstring::npos &&
                settings.executable == original.executable && settings.configFiles == original.configFiles &&
                settings.configDirectory == original.configDirectory && settings.logLines == original.logLines &&
                settings.configDirectoryFirst == original.configDirectoryFirst &&
                registry.writes == 0 && !std::filesystem::exists(root / L"data" / L"settings.ini");
            context.configFiles = {{L".\\tool-hold.conf"}};
            RefreshConfigFiles(context);
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
                IDC_MOVE_CONFIG_UP, IDC_MOVE_CONFIG_DOWN, IDC_REMOVE_CONFIG, IDC_CONVERT_CONFIG,
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
