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
        MessageBoxW(owner, L"无法打开 Windows 文件选择对话框。", L"XTray", MB_OK | MB_ICONERROR);
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
        MessageBoxW(owner, L"无法设置 Windows 文件选择对话框。", L"XTray", MB_OK | MB_ICONERROR);
        return {};
    }
    result = dialog->Show(owner);
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        return {};
    }
    ComPtr<IShellItemArray> items;
    if (FAILED(result) || FAILED(dialog->GetResults(items.GetAddressOf()))) {
        MessageBoxW(owner, L"无法读取选择结果。", L"XTray", MB_OK | MB_ICONERROR);
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

INT_PTR CALLBACK SettingsProcedure(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* context = reinterpret_cast<Context*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        context = reinterpret_cast<Context*>(lParam);
        SetWindowLongPtrW(dialog, DWLP_USER, lParam);
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
        Settings draft;
        draft.executable = WindowText(GetDlgItem(dialog, IDC_EXECUTABLE));
        draft.configDirectory = WindowText(GetDlgItem(dialog, IDC_CONFIG_DIRECTORY));
        draft.configFiles = SplitConfigFiles(WindowText(GetDlgItem(dialog, IDC_CONFIG_FILES)));
        const auto number = WindowText(GetDlgItem(dialog, IDC_LOG_LINES));
        wchar_t* end = nullptr;
        errno = 0;
        const auto limit = std::wcstoull(number.c_str(), &end, 10);
        if (number.empty() || errno == ERANGE || end != number.c_str() + number.size() ||
            number.front() == L'-' || limit == 0 || limit > INT_MAX) {
            MessageBoxW(dialog, L"日志保留行数必须为有效正整数。", L"XTray", MB_OK | MB_ICONERROR);
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
        context->logs.SetLimit(context->settings.logLines);
        context->logs.Tray(L"设置已保存；路径将在下次运行或重启时使用。");
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
        const auto error = WindowsError(L"无法打开设置窗口");
        logs.Tray(error);
        MessageBoxW(owner, error.c_str(), L"XTray", MB_OK | MB_ICONERROR);
    }
    return result == IDOK;
}

} // namespace xtray
