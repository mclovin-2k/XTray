#include "settings_dialog.h"
#include "resource.h"
#include "win32.h"

#include <shobjidl.h>
#include <wrl/client.h>

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cwchar>

namespace xtray {
namespace {

using Microsoft::WRL::ComPtr;

struct Context {
    const std::filesystem::path& root;
    Settings& settings;
    Logs& logs;
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
}

INT_PTR CALLBACK SettingsProcedure(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* context = reinterpret_cast<Context*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        context = reinterpret_cast<Context*>(lParam);
        SetWindowLongPtrW(dialog, DWLP_USER, lParam);
        SetDialogLanguage(dialog);
        SendDlgItemMessageW(dialog, IDC_LANGUAGE, CB_SETCURSEL,
            static_cast<WPARAM>(context->settings.language), 0);
        SetDlgItemTextW(dialog, IDC_EXECUTABLE, context->settings.executable.c_str());
        SetDlgItemTextW(dialog, IDC_CONFIG_DIRECTORY, context->settings.configDirectory.c_str());
        std::wstring files;
        for (const auto& file : context->settings.configFiles) {
            files += file + L"\r\n";
        }
        SetDlgItemTextW(dialog, IDC_CONFIG_FILES, files.c_str());
        SetDlgItemTextW(dialog, IDC_LOG_LINES, std::to_wstring(context->settings.logLines).c_str());
        return TRUE;
    }
    if (context == nullptr) {
        return FALSE;
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
        const auto error = WindowsError(Text(L"Cannot open Settings", L"无法打开设置窗口"));
        logs.Tray(error);
        MessageBoxW(owner, error.c_str(), L"XTray", MB_OK | MB_ICONERROR);
    }
    return result == IDOK;
}

} // namespace xtray
