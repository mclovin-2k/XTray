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
    // A TaskbarCreated notification can leave the icon registered after a DPI change.
    const bool updated = notify_(NIM_MODIFY, &data_) != FALSE;
    if (updated && added_) {
        return true;
    }
    if (!updated) {
        Remove();
        if (!notify_(NIM_ADD, &data_)) {
            return false;
        }
    }
    data_.uVersion = NOTIFYICON_VERSION_4;
    added_ = notify_(NIM_SETVERSION, &data_) != FALSE;
    if (!added_ && !updated) {
        notify_(NIM_DELETE, &data_);
    }
    return added_;
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
