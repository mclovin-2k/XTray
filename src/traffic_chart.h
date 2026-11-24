#pragma once

#include "theme.h"
#include "xray_traffic.h"

#include <d2d1.h>
#include <wrl/client.h>

#include <array>
#include <deque>

namespace xtray {

class TrafficChart {
public:
    bool Create(HWND parent);
    HWND Window() const { return frame_; }
    void SetAppearance(const ThemePalette& colors);
    void SetFont(HFONT font);
    void RefreshLanguage();
    void Reset();
    void Append(const XrayTrafficSample& sample);
    void Tick();
    bool SmokeTest();

private:
    using Clock = std::chrono::steady_clock;
    struct Point {
        Clock::time_point time;
        std::optional<double> upload, download;
    };
    struct Series {
        size_t color = 0;
        bool visible = true;
        std::deque<Point> points;
    };
    struct Chart {
        std::map<std::wstring, Series> series;
        TrafficCounters counters;
        TrafficAvailability availability = TrafficAvailability::Stopped;
    };

    static LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
        UINT_PTR id, DWORD_PTR reference);
    LRESULT Notify(const NMHDR& header);
    void Select(size_t index);
    void Layout();
    void UpdateLabels();
    void UpdateLegend();
    Series* LegendSeries(int index) const;
    COLORREF SeriesColor(const Series& series) const;
    void Paint();
    void Draw(HDC device, const RECT& bounds);
    bool PrepareTarget(HDC device, const RECT& bounds);
    void DrawSeries(const Series& series, ID2D1SolidColorBrush& brush, const D2D1_RECT_F& plot,
        Clock::time_point start, double ceiling);

    HWND frame_ = nullptr, canvas_ = nullptr, legend_ = nullptr, directions_ = nullptr;
    std::array<HWND, 2> tabs_{};
    ThemePalette colors_ = PaletteFor(false);
    HFONT font_ = nullptr;
    std::array<Chart, 2> charts_;
    size_t active_ = 0;
    Series* hovered_ = nullptr;
    bool updatingLegend_ = false;
    bool trackingMouse_ = false;
    Clock::time_point nextPaint_{};
    const wchar_t* message_ = L"";
    Microsoft::WRL::ComPtr<ID2D1Factory> factory_;
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> target_;
    Microsoft::WRL::ComPtr<ID2D1StrokeStyle> dashed_;
};

} // namespace xtray
