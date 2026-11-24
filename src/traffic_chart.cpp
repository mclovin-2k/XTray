#include "traffic_chart.h"
#include "language.h"
#include "ui_layout.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <utility>

namespace xtray {
namespace {

using namespace std::chrono_literals;

constexpr auto kHistory = 60s;
constexpr int kOutbound = 1, kInbound = 2;
constexpr std::array kColors{
    std::pair{RGB(22, 90, 158), RGB(114, 183, 255)},
    std::pair{RGB(165, 66, 19), RGB(255, 156, 92)},
    std::pair{RGB(24, 112, 73), RGB(89, 216, 161)},
    std::pair{RGB(116, 57, 156), RGB(197, 162, 255)},
    std::pair{RGB(161, 45, 97), RGB(255, 145, 194)},
    std::pair{RGB(8, 114, 120), RGB(97, 212, 217)},
    std::pair{RGB(116, 84, 0), RGB(233, 199, 97)},
    std::pair{RGB(173, 48, 55), RGB(255, 131, 137)}};
constexpr std::array kMarkers{L"●", L"■", L"◆", L"＋"};

D2D1_COLOR_F Color(COLORREF value)
{
    return D2D1::ColorF(static_cast<float>(GetRValue(value)) / 255,
        static_cast<float>(GetGValue(value)) / 255, static_cast<float>(GetBValue(value)) / 255);
}

void Fill(HDC device, const RECT& bounds, COLORREF color)
{
    SetDCBrushColor(device, color);
    FillRect(device, &bounds, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

} // namespace

bool TrafficChart::Create(HWND parent)
{
    const auto child = [](HWND owner, const wchar_t* type, DWORD style, DWORD ex = 0, int id = 0) {
        return CreateWindowExW(ex, type, L"", WS_CHILD | WS_VISIBLE | style,
            0, 0, 0, 0, owner, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            GetModuleHandleW(nullptr), nullptr);
    };
    frame_ = child(parent, L"STATIC", WS_CLIPCHILDREN, WS_EX_CONTROLPARENT);
    if (!frame_ || !SetWindowSubclass(frame_, Procedure, 1, reinterpret_cast<DWORD_PTR>(this))) {
        return false;
    }
    tabs_[0] = child(frame_, L"BUTTON", WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON | BS_PUSHLIKE, 0, kOutbound);
    tabs_[1] = child(frame_, L"BUTTON", BS_AUTORADIOBUTTON | BS_PUSHLIKE, 0, kInbound);
    directions_ = child(frame_, L"STATIC", SS_CENTERIMAGE | SS_ENDELLIPSIS | SS_NOPREFIX);
    canvas_ = child(frame_, L"STATIC", SS_NOPREFIX);
    legend_ = child(frame_, WC_LISTVIEWW, WS_TABSTOP | WS_GROUP | LVS_LIST | LVS_SINGLESEL);
    if (!tabs_[0] || !tabs_[1] || !directions_ || !canvas_ || !legend_ ||
        !SetWindowSubclass(canvas_, Procedure, 1, reinterpret_cast<DWORD_PTR>(this)) ||
        !SetWindowSubclass(legend_, Procedure, 1, reinterpret_cast<DWORD_PTR>(this))) {
        return false;
    }
    ListView_SetExtendedListViewStyle(legend_, LVS_EX_CHECKBOXES | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    SendMessageW(tabs_[0], BM_SETCHECK, BST_CHECKED, 0);
    RefreshLanguage();
    return true;
}

void TrafficChart::SetAppearance(const ThemePalette& colors)
{
    colors_ = colors;
    ListView_SetBkColor(legend_, colors_.window);
    ListView_SetTextColor(legend_, colors_.text);
    InvalidateRect(frame_, nullptr, TRUE);
    InvalidateRect(canvas_, nullptr, FALSE);
    InvalidateRect(legend_, nullptr, TRUE);
}

void TrafficChart::SetFont(HFONT font)
{
    font_ = font;
    for (auto control : {tabs_[0], tabs_[1], directions_, canvas_, legend_}) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
    Layout();
}

void TrafficChart::RefreshLanguage()
{
    SetWindowTextW(tabs_[0], Text(L"Outbound", L"出站"));
    SetWindowTextW(tabs_[1], Text(L"Inbound", L"入站"));
    SetWindowTextW(legend_, Text(L"Chart series", L"图表线路"));
    UpdateLabels();
    Layout();
    InvalidateRect(canvas_, nullptr, FALSE);
}

void TrafficChart::UpdateLabels()
{
    const auto& chart = charts_[active_];
    const auto upload = chart.counters.Any() && !chart.counters.upload
        ? Text(L"↑ Upload statistics disabled", L"↑ 上传统计未启用")
        : Text(L"↑ Upload · dashed", L"↑ 上传 · 虚线");
    const auto download = chart.counters.Any() && !chart.counters.download
        ? Text(L"↓ Download statistics disabled", L"↓ 下载统计未启用")
        : Text(L"↓ Download · solid", L"↓ 下载 · 实线");
    const auto directions = std::wstring(upload) + L"   " + download;
    SetWindowTextW(directions_, directions.c_str());
    message_ = charts_[0].availability == TrafficAvailability::StatisticsDisabled &&
        charts_[1].availability == TrafficAvailability::StatisticsDisabled
        ? Text(L"Traffic statistics are disabled.", L"未启用流量统计。")
        : TrafficUnavailableReason(chart.availability, active_ == 1);
    SetWindowTextW(canvas_, *message_ ? message_ : Text(L"Traffic history", L"速度曲线"));
}

void TrafficChart::Layout()
{
    if (!legend_) { return; }
    RECT bounds{};
    GetClientRect(frame_, &bounds);
    const auto scale = [this](int value) { return UiScale(frame_, value); };
    const auto width = static_cast<int>(bounds.right);
    const auto height = static_cast<int>(bounds.bottom);
    const int textHeight = SingleLineControlHeight(directions_);
    const int headerHeight = (std::min)(height, (std::max)(textHeight + scale(6), scale(24)));
    const int gap = scale(4);
    int x = 0;
    for (const auto tab : tabs_) {
        const int tabWidth = (std::max)(scale(54), static_cast<int>(ControlTextSize(tab).cx) + scale(18));
        MoveWindow(tab, x, 0, tabWidth, headerHeight, TRUE);
        x += tabWidth + gap;
    }
    MoveWindow(directions_, x + gap, 0, (std::max)(1, width - x - gap), headerHeight, TRUE);
    const int remaining = (std::max)(1, height - headerHeight - gap);
    const int scrollHeight = GetSystemMetricsForDpi(SM_CYHSCROLL, GetDpiForWindow(frame_));
    const int rowHeight = (std::max)(textHeight + scale(3), scale(19));
    const int legendHeight = (std::min)(remaining / 2,
        rowHeight * (remaining >= scale(135) ? 2 : 1) + scrollHeight + gap);
    MoveWindow(canvas_, 0, headerHeight + gap, width, remaining - legendHeight, TRUE);
    MoveWindow(legend_, 0, height - legendHeight, width, legendHeight, TRUE);

    int columnWidth = scale(96);
    for (const auto& [tag, series] : charts_[active_].series) {
        const auto labelWidth = ControlTextSize(legend_, tag).cx;
        columnWidth = (std::max)(columnWidth, static_cast<int>(labelWidth) + scale(50));
    }
    ListView_SetColumnWidth(legend_, 0, (std::min)(columnWidth, (std::max)(1, width - gap)));
}

void TrafficChart::Select(size_t index)
{
    active_ = index;
    for (size_t i = 0; i < tabs_.size(); ++i) {
        SendMessageW(tabs_[i], BM_SETCHECK, i == active_ ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    UpdateLabels();
    UpdateLegend();
    InvalidateRect(canvas_, nullptr, FALSE);
}

void TrafficChart::UpdateLegend()
{
    hovered_ = nullptr;
    updatingLegend_ = true;
    SendMessageW(legend_, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(legend_);
    int index = 0;
    for (auto& [tag, series] : charts_[active_].series) {
        auto label = kMarkers[series.color / kColors.size() % kMarkers.size()] + std::wstring(L" ") + tag;
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = index++;
        item.pszText = label.data();
        item.lParam = reinterpret_cast<LPARAM>(&series);
        const int added = ListView_InsertItem(legend_, &item);
        if (added != -1) { ListView_SetCheckState(legend_, added, series.visible); }
    }
    SendMessageW(legend_, WM_SETREDRAW, TRUE, 0);
    updatingLegend_ = false;
    Layout();
    InvalidateRect(legend_, nullptr, TRUE);
}

TrafficChart::Series* TrafficChart::LegendSeries(int index) const
{
    LVITEMW item{};
    item.mask = LVIF_PARAM;
    item.iItem = index;
    return index >= 0 && ListView_GetItem(legend_, &item)
        ? reinterpret_cast<Series*>(item.lParam) : nullptr;
}

void TrafficChart::Reset()
{
    hovered_ = nullptr;
    charts_ = {};
    nextPaint_ = {};
    UpdateLegend();
    UpdateLabels();
    InvalidateRect(canvas_, nullptr, FALSE);
}

void TrafficChart::Append(const XrayTrafficSample& sample)
{
    const auto time = sample.time == Clock::time_point{} ? Clock::now() : sample.time;
    const auto start = time - kHistory;
    const std::array groups{&sample.outbound, &sample.inbound};
    bool seriesAdded = false, labelsChanged = false;
    for (size_t i = 0; i < groups.size(); ++i) {
        auto& chart = charts_[i];
        const auto& group = *groups[i];
        if (i == active_) {
            labelsChanged = chart.availability != group.availability ||
                chart.counters.upload != group.counters.upload || chart.counters.download != group.counters.download;
        }
        chart.counters = group.counters;
        chart.availability = group.availability;
        for (const auto& [tag, value] : group.endpoints) {
            const auto [entry, inserted] = chart.series.try_emplace(tag);
            if (inserted) {
                entry->second.color = chart.series.size() - 1;
                seriesAdded |= i == active_;
            }
        }
        for (auto& [tag, series] : chart.series) {
            const auto found = group.endpoints.find(tag);
            Point point{time, {}, {}};
            if (found != group.endpoints.end()) {
                point.upload = found->second.upload.bytesPerSecond;
                point.download = found->second.download.bytesPerSecond;
            }
            auto& points = series.points;
            while (points.size() >= 2 && points[1].time <= start) { points.pop_front(); }
            points.push_back(point);
        }
    }
    if (labelsChanged) { UpdateLabels(); }
    if (seriesAdded) { UpdateLegend(); }
    nextPaint_ = Clock::now() + 1s;
    InvalidateRect(canvas_, nullptr, FALSE);
}

void TrafficChart::Tick()
{
    const auto now = Clock::now();
    if (now >= nextPaint_ && !charts_[active_].series.empty() && IsWindowVisible(frame_)) {
        nextPaint_ = now + 1s;
        InvalidateRect(canvas_, nullptr, FALSE);
    }
}

COLORREF TrafficChart::SeriesColor(const Series& series) const
{
    const auto& colors = kColors[series.color % kColors.size()];
    return colors_.dark ? colors.second : colors.first;
}

LRESULT TrafficChart::Notify(const NMHDR& header)
{
    if (updatingLegend_) { return 0; }
    if (header.code == LVN_ITEMCHANGED) {
        const auto& item = reinterpret_cast<const NMLISTVIEW&>(header);
        if ((item.uChanged & LVIF_STATE) != 0 && ((item.uOldState ^ item.uNewState) & LVIS_STATEIMAGEMASK) != 0) {
            if (auto* series = LegendSeries(item.iItem)) {
                series->visible = ListView_GetCheckState(legend_, item.iItem) != FALSE;
                InvalidateRect(canvas_, nullptr, FALSE);
            }
        }
    }
    if (header.code == NM_CUSTOMDRAW) {
        auto& item = const_cast<NMLVCUSTOMDRAW&>(reinterpret_cast<const NMLVCUSTOMDRAW&>(header));
        if (item.nmcd.dwDrawStage == CDDS_PREPAINT) { return CDRF_NOTIFYITEMDRAW; }
        if (item.nmcd.dwDrawStage == CDDS_ITEMPREPAINT && (item.nmcd.uItemState & CDIS_SELECTED) == 0) {
            if (const auto* series = reinterpret_cast<const Series*>(item.nmcd.lItemlParam)) {
                item.clrText = SeriesColor(*series);
            }
        }
    }
    return 0;
}

LRESULT CALLBACK TrafficChart::Procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR id, DWORD_PTR reference)
{
    auto& chart = *reinterpret_cast<TrafficChart*>(reference);
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(window, Procedure, id);
    } else if (window == chart.canvas_) {
        if (message == WM_PAINT) { chart.Paint(); return 0; }
        if (message == WM_ERASEBKGND) { return TRUE; }
    } else if (window == chart.legend_) {
        if (message == WM_MOUSEMOVE) {
            if (!chart.trackingMouse_) {
                TRACKMOUSEEVENT request{sizeof(request), TME_LEAVE, window, 0};
                chart.trackingMouse_ = TrackMouseEvent(&request) != FALSE;
            }
            LVHITTESTINFO hit{};
            hit.pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            auto* hovered = chart.LegendSeries(ListView_HitTest(window, &hit));
            if (std::exchange(chart.hovered_, hovered) != hovered) { InvalidateRect(chart.canvas_, nullptr, FALSE); }
        } else if (message == WM_MOUSELEAVE) {
            chart.trackingMouse_ = false;
            chart.hovered_ = nullptr;
            InvalidateRect(chart.canvas_, nullptr, FALSE);
        }
    } else if (window == chart.frame_) {
        switch (message) {
        case WM_SIZE:
            chart.Layout();
            break;
        case WM_ERASEBKGND: {
            RECT bounds{};
            GetClientRect(window, &bounds);
            Fill(reinterpret_cast<HDC>(wParam), bounds, chart.colors_.window);
            return TRUE;
        }
        case WM_COMMAND:
            if (HIWORD(wParam) == BN_CLICKED && (LOWORD(wParam) == kOutbound || LOWORD(wParam) == kInbound)) {
                chart.Select(LOWORD(wParam) == kInbound ? 1 : 0);
                return 0;
            }
            break;
        case WM_NOTIFY:
            if (lParam && reinterpret_cast<const NMHDR*>(lParam)->hwndFrom == chart.legend_) {
                return chart.Notify(*reinterpret_cast<const NMHDR*>(lParam));
            }
            [[fallthrough]];
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
        case WM_DRAWITEM:
            return SendMessageW(GetParent(window), message, wParam, lParam);
        }
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

bool TrafficChart::PrepareTarget(HDC device, const RECT& bounds)
{
    if (!factory_ && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
        factory_.ReleaseAndGetAddressOf()))) { return false; }
    if (!dashed_ && FAILED(factory_->CreateStrokeStyle(
        D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
            D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_ROUND, 10, D2D1_DASH_STYLE_DASH),
        nullptr, 0, dashed_.ReleaseAndGetAddressOf()))) { return false; }
    if (!target_) {
        const auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96, 96);
        if (FAILED(factory_->CreateDCRenderTarget(&properties, target_.ReleaseAndGetAddressOf()))) {
            return false;
        }
    }
    return SUCCEEDED(target_->BindDC(device, &bounds));
}

void TrafficChart::DrawSeries(const Series& series, ID2D1SolidColorBrush& brush, const D2D1_RECT_F& plot,
    Clock::time_point start, double ceiling)
{
    brush.SetColor(Color(SeriesColor(series)));
    const float width = static_cast<float>(UiScale(canvas_, &series == hovered_ ? 3 : 2));
    const bool markers = charts_[active_].series.size() > kColors.size();
    const auto drawMarker = [&](D2D1_POINT_2F center) {
        const auto [x, y] = center;
        const float size = width * 1.5f;
        switch (series.color / kColors.size() % kMarkers.size()) {
        case 0:
            target_->FillEllipse(D2D1::Ellipse(center, size, size), &brush);
            break;
        case 1:
            target_->FillRectangle(D2D1::RectF(x - size, y - size, x + size, y + size), &brush);
            break;
        case 2: {
            const auto top = D2D1::Point2F(x, y - size);
            const auto right = D2D1::Point2F(x + size, y);
            const auto bottom = D2D1::Point2F(x, y + size);
            const auto left = D2D1::Point2F(x - size, y);
            target_->DrawLine(top, right, &brush, width);
            target_->DrawLine(right, bottom, &brush, width);
            target_->DrawLine(bottom, left, &brush, width);
            target_->DrawLine(left, top, &brush, width);
            break;
        }
        case 3:
            target_->DrawLine(D2D1::Point2F(x - size, y), D2D1::Point2F(x + size, y), &brush, width);
            target_->DrawLine(D2D1::Point2F(x, y - size), D2D1::Point2F(x, y + size), &brush, width);
            break;
        }
    };
    for (const bool upload : {true, false}) {
        Microsoft::WRL::ComPtr<ID2D1PathGeometry> geometry;
        Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(factory_->CreatePathGeometry(geometry.GetAddressOf())) ||
            FAILED(geometry->Open(sink.GetAddressOf()))) { continue; }
        bool inFigure = false;
        auto markerTime = start;
        auto lastTime = start;
        for (const auto& point : series.points) {
            const auto value = upload ? point.upload : point.download;
            if (inFigure && (!value || point.time - lastTime > 2500ms)) {
                sink->EndFigure(D2D1_FIGURE_END_OPEN);
                inFigure = false;
            }
            if (!value) { continue; }
            const double age = std::chrono::duration<double>(point.time - start).count();
            const float rate = std::clamp(static_cast<float>(*value / ceiling), 0.0f, 1.0f);
            const auto position = D2D1::Point2F(
                plot.left + (plot.right - plot.left) * static_cast<float>(age / kHistory.count()),
                plot.bottom - 1 - (plot.bottom - plot.top - 2) * rate);
            if (inFigure) {
                sink->AddLine(position);
            } else {
                sink->BeginFigure(position, D2D1_FIGURE_BEGIN_HOLLOW);
                // A single valid sample should also be visible when there is a gap on both sides.
                target_->FillEllipse(D2D1::Ellipse(position, width / 2, width / 2), &brush);
                inFigure = true;
            }
            lastTime = point.time;
            if (markers && point.time >= markerTime) {
                drawMarker(position);
                markerTime = point.time + 10s;
            }
        }
        if (inFigure) { sink->EndFigure(D2D1_FIGURE_END_OPEN); }
        if (SUCCEEDED(sink->Close())) {
            target_->DrawGeometry(geometry.Get(), &brush, width, upload ? dashed_.Get() : nullptr);
        }
    }
}

void TrafficChart::Draw(HDC device, const RECT& bounds)
{
    Fill(device, bounds, colors_.window);
    const auto saved = SaveDC(device);
    SetBkMode(device, TRANSPARENT);
    SetTextColor(device, colors_.text);
    SelectObject(device, font_ ? font_ : GetStockObject(DEFAULT_GUI_FONT));
    TEXTMETRICW metrics{};
    GetTextMetricsW(device, &metrics);
    const auto scale = [this](int value) { return UiScale(canvas_, value); };
    const int row = (std::max)(scale(12), static_cast<int>(metrics.tmHeight));
    const auto start = Clock::now() - kHistory;
    const auto& chart = charts_[active_];
    double peak = 1;
    bool hasPoints = false;
    for (const auto& [tag, series] : chart.series) {
        if (!series.visible) { continue; }
        for (const auto& point : series.points) {
            if (point.time < start) { continue; }
            peak = (std::max)({peak, point.upload.value_or(0), point.download.value_or(0)});
            hasPoints |= point.upload.has_value() || point.download.has_value();
        }
    }
    const double rounded = std::exp2(std::ceil(std::log2(peak)));
    const double ceiling = std::isfinite(rounded) ? rounded : peak;
    const auto label = FormatTraffic(ceiling, true);
    SIZE labelSize{};
    GetTextExtentPoint32W(device, label.c_str(), static_cast<int>(label.size()), &labelSize);
    const int left = (std::max)(scale(64), static_cast<int>(labelSize.cx) + scale(8));
    const int right = static_cast<int>(bounds.right) - scale(2);
    const int bottom = static_cast<int>(bounds.bottom) - row - scale(2);
    const int top = row / 2;
    const RECT plotBounds{left, top, right, bottom};
    if (right <= left || bottom <= top) {
        auto status = bounds;
        DrawTextW(device, message_, -1, &status, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (saved != 0) { RestoreDC(device, saved); }
        return;
    }
    Fill(device, plotBounds, colors_.logBackground);
    const auto plot = D2D1::RectF(static_cast<float>(left), static_cast<float>(top),
        static_cast<float>(right - 1), static_cast<float>(bottom - 1));
    const int divisions = bottom - top >= 4 * row ? 4 : bottom - top >= 2 * row ? 2 : 1;
    if (PrepareTarget(device, bounds)) {
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
        target_->BeginDraw();
        target_->Clear(Color(colors_.window));
        if (SUCCEEDED(target_->CreateSolidColorBrush(Color(colors_.border), brush.GetAddressOf()))) {
            brush->SetColor(Color(colors_.logBackground));
            target_->FillRectangle(plot, brush.Get());
            const auto grid = colors_.dark ? RGB(62, 62, 62) : RGB(221, 221, 221);
            brush->SetColor(Color(grid));
            for (int index = 1; index < divisions; ++index) {
                const float y = plot.top + (plot.bottom - plot.top) * static_cast<float>(index) / divisions;
                target_->DrawLine(D2D1::Point2F(plot.left, y), D2D1::Point2F(plot.right, y), brush.Get());
            }
            for (int index = 1; index < 4; ++index) {
                const float x = plot.left + (plot.right - plot.left) * static_cast<float>(index) / 4;
                target_->DrawLine(D2D1::Point2F(x, plot.top), D2D1::Point2F(x, plot.bottom), brush.Get());
            }
            brush->SetColor(Color(colors_.border));
            target_->DrawRectangle(plot, brush.Get());
            target_->PushAxisAlignedClip(plot, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            for (const auto& [tag, series] : chart.series) {
                if (series.visible && &series != hovered_) { DrawSeries(series, *brush.Get(), plot, start, ceiling); }
            }
            if (hovered_ && hovered_->visible) { DrawSeries(*hovered_, *brush.Get(), plot, start, ceiling); }
            target_->PopAxisAlignedClip();
        }
        if (FAILED(target_->EndDraw())) { target_.Reset(); }
    }

    constexpr UINT textFlags = DT_SINGLELINE | DT_NOPREFIX;
    if (hasPoints) {
        for (int index = 0; index <= divisions; ++index) {
            const double fraction = static_cast<double>(index) / divisions;
            const auto tick = FormatTraffic(ceiling * (1 - fraction), true);
            const LONG y = top + static_cast<LONG>((bottom - top) * fraction);
            RECT textBounds{0, y - row / 2, left - scale(5), y + (row + 1) / 2};
            DrawTextW(device, tick.c_str(), -1, &textBounds, textFlags | DT_RIGHT | DT_VCENTER);
        }
    }
    RECT timeBounds{left, bottom + scale(2), right, bounds.bottom};
    DrawTextW(device, Text(L"−60 s", L"−60 秒"), -1, &timeBounds, textFlags | DT_LEFT);
    if (right - left >= scale(164)) {
        DrawTextW(device, Text(L"−30 s", L"−30 秒"), -1, &timeBounds, textFlags | DT_CENTER);
    }
    DrawTextW(device, Text(L"Now", L"现在"), -1, &timeBounds, textFlags | DT_RIGHT);

    if (*message_) {
        RECT overlay = plotBounds;
        const wchar_t* detail = nullptr;
        if (chart.availability == TrafficAvailability::StatisticsDisabled) {
            detail = Text(L"Enable statistics in the Xray configuration, then restart Xray.",
                L"在 Xray 配置中启用统计后，重启 Xray。");
        } else if (chart.availability == TrafficAvailability::MetricsDisabled) {
            detail = Text(L"Set metrics.listen in the Xray configuration, then restart Xray.",
                L"在 Xray 配置中设置 metrics.listen 后，重启 Xray。");
        }
        if (detail && bottom - top >= 3 * row) {
            RECT second = overlay;
            overlay.bottom = (top + bottom) / 2;
            second.top = overlay.bottom;
            DrawTextW(device, message_, -1, &overlay, textFlags | DT_CENTER | DT_BOTTOM | DT_END_ELLIPSIS);
            DrawTextW(device, detail, -1, &second, textFlags | DT_CENTER | DT_TOP | DT_END_ELLIPSIS);
        } else {
            DrawTextW(device, message_, -1, &overlay, textFlags | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS);
        }
    }
    if (saved != 0) { RestoreDC(device, saved); }
}

void TrafficChart::Paint()
{
    PAINTSTRUCT paint{};
    const auto device = BeginPaint(canvas_, &paint);
    RECT bounds{};
    GetClientRect(canvas_, &bounds);
    if (device && bounds.right > 0 && bounds.bottom > 0) {
        const auto buffer = CreateCompatibleDC(device);
        const auto bitmap = CreateCompatibleBitmap(device, bounds.right, bounds.bottom);
        if (buffer && bitmap) {
            const auto previous = SelectObject(buffer, bitmap);
            Draw(buffer, bounds);
            BitBlt(device, 0, 0, bounds.right, bounds.bottom, buffer, 0, 0, SRCCOPY);
            SelectObject(buffer, previous);
        } else {
            Draw(device, bounds);
        }
        if (bitmap) { DeleteObject(bitmap); }
        if (buffer) { DeleteDC(buffer); }
    }
    EndPaint(canvas_, &paint);
}

bool TrafficChart::SmokeTest()
{
    std::fwprintf(stderr, L"Smoke: traffic chart keeps separate histories and enabled directions.\n");
    const auto started = Clock::now() - 2s;
    XrayTrafficSample sample;
    sample.time = started;
    sample.outbound.availability = sample.inbound.availability = TrafficAvailability::Available;
    sample.outbound.counters = {true, false};
    sample.inbound.counters = {false, true};
    sample.outbound.endpoints.emplace(L"Proxy", EndpointTraffic{{1024, 128}, {}});
    sample.inbound.endpoints.emplace(L"Socks", EndpointTraffic{{}, {2048, 256}});
    Append(sample);
    sample.time += 1s;
    Append(sample);
    const bool outbound = ListView_GetItemCount(legend_) == 1 &&
        WindowText(directions_).find(Text(L"Download statistics disabled", L"下载统计未启用")) != std::wstring::npos;
    ListView_SetCheckState(legend_, 0, FALSE);
    SendMessageW(frame_, WM_COMMAND, MAKEWPARAM(kInbound, BN_CLICKED), reinterpret_cast<LPARAM>(tabs_[1]));
    const bool inbound = active_ == 1 && ListView_GetItemCount(legend_) == 1 &&
        ListView_GetCheckState(legend_, 0) &&
        charts_[1].series.at(L"Socks").points.size() == 2 &&
        WindowText(directions_).find(Text(L"Upload statistics disabled", L"上传统计未启用")) != std::wstring::npos;
    SendMessageW(frame_, WM_COMMAND, MAKEWPARAM(kOutbound, BN_CLICKED), reinterpret_cast<LPARAM>(tabs_[0]));
    const bool selection = active_ == 0 && !ListView_GetCheckState(legend_, 0) &&
        !charts_[0].series.at(L"Proxy").visible && charts_[0].series.at(L"Proxy").points.size() == 2;
    Reset();
    sample = {};
    sample.outbound.availability = sample.inbound.availability = TrafficAvailability::StatisticsDisabled;
    Append(sample);
    const bool unavailable = (GetWindowLongPtrW(canvas_, GWL_STYLE) & WS_VISIBLE) != 0 &&
        ListView_GetItemCount(legend_) == 0 &&
        WindowText(canvas_) == Text(L"Traffic statistics are disabled.", L"未启用流量统计。");
    Reset();
    return outbound && inbound && selection && unavailable;
}

} // namespace xtray
