#include "tray.h"
#include "language.h"

#include <cwchar>

namespace xtray {

bool TrayIcon::Update(HWND window, HICON icon, const std::wstring& tooltip)
{
    failure_ = TrayFailure::None;
    if (window == nullptr) {
        failure_ = TrayFailure::MissingWindow;
        return false;
    }
    if (icon == nullptr) {
        failure_ = TrayFailure::MissingIcon;
        return false;
    }
    if (added_ && data_.hWnd != window) {
        Remove();
    }
    data_ = {};
    data_.cbSize = sizeof(data_);
    data_.hWnd = window;
    data_.uID = kTrayIconId;
    data_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data_.uCallbackMessage = kTrayCallbackMessage;
    data_.hIcon = icon;
    wcsncpy_s(data_.szTip, tooltip.c_str(), _TRUNCATE);
    // A TaskbarCreated notification can leave the icon registered after a DPI change.
    const bool updated = notify_(NIM_MODIFY, &data_) != FALSE;
    if (updated && added_) {
        return true;
    }
    if (!updated) {
        Remove();
        if (!notify_(NIM_ADD, &data_)) {
            failure_ = TrayFailure::Add;
            return false;
        }
    }
    data_.uVersion = NOTIFYICON_VERSION_4;
    added_ = notify_(NIM_SETVERSION, &data_) != FALSE;
    if (!added_) {
        failure_ = TrayFailure::SetVersion;
    }
    if (!added_ && !updated) {
        notify_(NIM_DELETE, &data_);
    }
    return added_;
}

const wchar_t* TrayIcon::FailureText() const
{
    switch (failure_) {
    case TrayFailure::MissingWindow:
        return Text(L"The main window handle is null", L"主窗口句柄为空");
    case TrayFailure::MissingIcon:
        return Text(L"The tray icon handle is null", L"托盘图标句柄为空");
    case TrayFailure::Add:
        return Text(L"Cannot add the tray icon (NIM_ADD)", L"无法添加托盘图标（NIM_ADD）");
    case TrayFailure::SetVersion:
        return Text(L"Cannot set tray icon interaction version 4 (NIM_SETVERSION)",
            L"无法设置托盘图标交互版本 4（NIM_SETVERSION）");
    default:
        return L"";
    }
}

void TrayIcon::Remove()
{
    if (added_) {
        notify_(NIM_DELETE, &data_);
        added_ = false;
    }
}

void TrayIcon::Focus()
{
    if (added_) {
        notify_(NIM_SETFOCUS, &data_);
    }
}

} // namespace xtray
