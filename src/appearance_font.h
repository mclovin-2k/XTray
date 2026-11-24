#pragma once

#include "settings.h"
#include "win32.h"

namespace xtray {

inline int CALLBACK FoundFont(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM found)
{
    *reinterpret_cast<bool*>(found) = true;
    return 0;
}

inline LOGFONTW AppearanceFont(UINT dpi, const FontPreference& font)
{
    NONCLIENTMETRICSW metrics{sizeof(NONCLIENTMETRICSW)};
    if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi)) {
        throw std::runtime_error(Text("Cannot read the system UI font.", "无法读取系统界面字体。"));
    }
    LOGFONTW description = metrics.lfMessageFont;
    if (!font.face.empty()) {
        LOGFONTW family{};
        family.lfCharSet = DEFAULT_CHARSET;
        wcscpy_s(family.lfFaceName, font.face.c_str());
        bool installed = false;
        if (const HDC device = GetDC(nullptr)) {
            EnumFontFamiliesExW(device, &family, FoundFont, reinterpret_cast<LPARAM>(&installed), 0);
            ReleaseDC(nullptr, device);
        }
        if (installed) {
            wcscpy_s(description.lfFaceName, font.face.c_str());
            description.lfCharSet = DEFAULT_CHARSET;
            description.lfHeight = -MulDiv(font.sizeInTenths, static_cast<int>(dpi), 720);
        }
    }
    return description;
}

} // namespace xtray
