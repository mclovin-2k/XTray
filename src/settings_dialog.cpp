#include "settings_dialog.h"
#include "resource.h"
#include "win32.h"
#include "theme_window.h"

#include <shobjidl.h>
#include <wrl/client.h>

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cwchar>
#include <exception>
#include <functional>

namespace xtray {
namespace {

using Microsoft::WRL::ComPtr;
constexpr UINT kRefreshDialogTheme = WM_APP + 46;
constexpr UINT kAutomateDialog = WM_APP + 47;

struct Context {
    const std::filesystem::path& root;
    Settings& settings;
    Logs& logs;
    ThemeWindow theme;
    std::function<void(HWND)> automation;
    std::exception_ptr error;
};

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
    const auto set = [dialog](int id, const wchar_t* text) { SetDlgItemTextW(dialog, id, text); };
    set(IDC_LABEL_EXECUTABLE, Text(L"Xray executable:", L"Xray 程序路径："));
    set(IDC_LABEL_CONFIG_FILES, Text(L"Config files (one per line, in load order; optional):",
        L"配置文件（每行一个，按顺序加载；可留空）："));
    set(IDC_LABEL_CONFIG_DIRECTORY, Text(L"Config directory (optional):", L"配置目录（可留空）："));
    set(IDC_LABEL_LOG_LINES, Text(L"Log lines per source:", L"每类日志保留行数："));
    set(IDC_LABEL_LANGUAGE, Text(L"Language:", L"语言："));
    set(IDC_LABEL_THEME, Text(L"Appearance:", L"外观："));
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
        SendDlgItemMessageW(dialog, IDC_LANGUAGE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
    }
    for (const auto* item : {Text(L"Follow system", L"跟随系统"), Text(L"Light", L"浅色"),
        Text(L"Dark", L"深色")}) {
        SendDlgItemMessageW(dialog, IDC_THEME, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
    }
}

INT_PTR SettingsMessage(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* context = reinterpret_cast<Context*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (context == nullptr && message == WM_MEASUREITEM && lParam != 0) {
        auto& item = *reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
        if (item.CtlType == ODT_COMBOBOX) {
            const UINT dpi = GetDpiForWindow(dialog);
            item.itemHeight = static_cast<UINT>(MulDiv(22, static_cast<int>(dpi == 0 ? 96 : dpi), 96));
            return TRUE;
        }
    }
    if (message == WM_INITDIALOG) {
        context = reinterpret_cast<Context*>(lParam);
        SetWindowLongPtrW(dialog, DWLP_USER, lParam);
        context->theme.Attach(dialog);
        SetDialogLanguage(dialog);
        SendDlgItemMessageW(dialog, IDC_LANGUAGE, CB_SETCURSEL,
            static_cast<WPARAM>(context->settings.language), 0);
        SendDlgItemMessageW(dialog, IDC_THEME, CB_SETCURSEL,
            static_cast<WPARAM>(context->settings.theme), 0);
        SetDlgItemTextW(dialog, IDC_EXECUTABLE, context->settings.executable.c_str());
        SetDlgItemTextW(dialog, IDC_CONFIG_DIRECTORY, context->settings.configDirectory.c_str());
        std::wstring files;
        for (const auto& file : context->settings.configFiles) {
            files += file + L"\r\n";
        }
        SetDlgItemTextW(dialog, IDC_CONFIG_FILES, files.c_str());
        SetDlgItemTextW(dialog, IDC_LOG_LINES, std::to_wstring(context->settings.logLines).c_str());
        const auto* owner = ThemeWindow::From(GetWindow(dialog, GW_OWNER));
        context->theme.Apply(owner != nullptr && owner->Colors().dark, true);
        if (context->automation) {
            PostMessageW(dialog, kAutomateDialog, 0, 0);
        }
        return TRUE;
    }
    if (context == nullptr) {
        return FALSE;
    }
    if (message == WM_NCDESTROY) {
        context->theme.Detach();
        SetWindowLongPtrW(dialog, DWLP_USER, 0);
        return FALSE;
    }
    if (message == WM_DPICHANGED) {
        PostMessageW(dialog, kRefreshDialogTheme, 0, 0);
        return FALSE;
    }
    if (message == kRefreshDialogTheme) {
        context->theme.Apply(context->theme.Colors().dark, true);
        return TRUE;
    }
    if (message == kAutomateDialog && context->automation) {
        context->automation(dialog);
        return TRUE;
    }
    LRESULT themed = 0;
    if (context->theme.Handle(message, wParam, lParam, themed)) {
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
            std::wstring text = WindowText(GetDlgItem(dialog, IDC_CONFIG_FILES));
            if (!text.empty() && text.back() != L'\n') {
                text += L"\r\n";
            }
            for (const auto& path : paths) {
                text += path + L"\r\n";
            }
            SetDlgItemTextW(dialog, IDC_CONFIG_FILES, text.c_str());
        } else {
            SetDlgItemTextW(dialog, id == IDC_BROWSE_EXECUTABLE
                ? IDC_EXECUTABLE : IDC_CONFIG_DIRECTORY, paths.front().c_str());
        }
        return TRUE;
    }
    case IDOK: {
        Settings draft = context->settings;
        draft.language = static_cast<Language>(
            SendDlgItemMessageW(dialog, IDC_LANGUAGE, CB_GETCURSEL, 0, 0));
        draft.theme = static_cast<ThemeMode>(
            SendDlgItemMessageW(dialog, IDC_THEME, CB_GETCURSEL, 0, 0));
        draft.executable = WindowText(GetDlgItem(dialog, IDC_EXECUTABLE));
        draft.configDirectory = WindowText(GetDlgItem(dialog, IDC_CONFIG_DIRECTORY));
        draft.configFiles = SplitConfigFiles(WindowText(GetDlgItem(dialog, IDC_CONFIG_FILES)));
        const auto number = WindowText(GetDlgItem(dialog, IDC_LOG_LINES));
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
        if (!SaveSettings(context->root, draft, error)) {
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

bool EditSettings(HWND owner, const std::filesystem::path& root, Settings& settings, Logs& logs)
{
    Context context{root, settings, logs};
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

bool SmokeTestSettingsTheme(HWND owner, const std::filesystem::path& root)
{
    Settings settings;
    settings.theme = ThemeMode::Light;
    Logs logs;
    bool passed = true;
    for (int action = 0; action < 4; ++action) {
        Context context{root, settings, logs};
        context.automation = [&](HWND window) {
            const auto expected = action == 3 ? ThemeMode::Dark : ThemeMode::Light;
            passed = passed && SendDlgItemMessageW(window, IDC_THEME, CB_GETCURSEL, 0, 0) ==
                static_cast<LRESULT>(expected);
            const auto before = context.theme.Colors();
            SendDlgItemMessageW(window, IDC_THEME, CB_SETCURSEL,
                static_cast<WPARAM>(action == 3 ? ThemeMode::System : ThemeMode::Dark), 0);
            passed = passed && context.theme.Colors() == before &&
                settings.theme == expected;
            if (action == 0) {
                if (auto* parent = ThemeWindow::From(owner)) {
                    parent->ApplyOwned(!before.dark);
                    passed = passed && context.theme.Colors().dark != before.dark &&
                        SendDlgItemMessageW(window, IDC_THEME, CB_GETCURSEL, 0, 0) ==
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

} // namespace xtray
