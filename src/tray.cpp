#include "tray.h"

#include <cwchar>

namespace xtray {

bool TrayIcon::Update(HWND window, HICON icon, const std::wstring& tooltip)
{
    if (window == nullptr || icon == nullptr) {
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
    if (added_ && notify_(NIM_MODIFY, &data_)) {
        return true;
    }
    if (added_) {
        Remove();
    }
    if (!notify_(NIM_ADD, &data_)) {
        return false;
    }
    added_ = true;
    data_.uVersion = NOTIFYICON_VERSION_4;
    if (!notify_(NIM_SETVERSION, &data_)) {
        Remove();
        return false;
    }
    return true;
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
