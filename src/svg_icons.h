#pragma once

#include <windows.h>

#include <memory>
#include <vector>

namespace xtray {

enum class SvgIcon { Application, ClearLogs, ScrollToEnd, CopyAll, WordWrap, TrayApplication };

inline constexpr COLORREF kApplicationIconColor = RGB(234, 179, 8);
inline constexpr COLORREF kRunningIconColor = RGB(34, 197, 94);
inline constexpr COLORREF kStoppedIconColor = RGB(239, 68, 68);

struct IconPixels {
    int size;
    // Top-down BGRA, with premultiplied alpha as required by HICON.
    std::vector<BYTE> bgra;
};

class SvgIconRenderer {
public:
    SvgIconRenderer();
    ~SvgIconRenderer();
    SvgIconRenderer(const SvgIconRenderer&) = delete;
    SvgIconRenderer& operator=(const SvgIconRenderer&) = delete;
    IconPixels Render(SvgIcon icon, int size, COLORREF color);
    HICON CreateIcon(SvgIcon icon, int size, COLORREF color);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace xtray
