#include "log_view.h"
#include "win32.h"
#include "text_view.h"

#include <richedit.h>

#include <algorithm>
#include <climits>

namespace xtray {
namespace {

std::wstring DisplayText(const LogLine& line)
{
    std::wstring text = line.source == LogSource::Tray ? L"[XTRAY] " : L"[XRAY] ";
    for (size_t index = 0; index < line.text.size(); ++index) {
        const wchar_t character = line.text[index];
        if (character == L'\r' && index + 1 < line.text.size() && line.text[index + 1] == L'\n') {
            ++index;
            text.push_back(L'\r');
        } else if (character == L'\n') {
            text.push_back(L'\r');
        } else {
            text.push_back(character == L'\0' ? L'\uFFFD' : character);
        }
    }
    // A partial line remains visible, with its source label, until more bytes arrive.
    if (text.back() != L'\r') {
        text.push_back(L'\r');
    }
    return text;
}

} // namespace

LogView::LogView(HWND window, Logs& logs) : window_(window), logs_(logs)
{
    SendMessageW(window_, EM_SETTEXTMODE, TM_RICHTEXT, 0);
    SendMessageW(window_, EM_SETUNDOLIMIT, 0, 0);
    SendMessageW(window_, EM_EXLIMITTEXT, 0, LONG_MAX);
    SendMessageW(window_, EM_SETBKGNDCOLOR, 0, RGB(255, 255, 255));
    SendMessageW(window_, EM_SETEVENTMASK, 0, ENM_SCROLL | ENM_SELCHANGE);
    SetTextWordWrap(window_, true);
}

void LogView::SetTheme(const ThemePalette& palette)
{
    if (palette_ == palette) {
        return;
    }
    palette_ = palette;
    formatAll_ = true;
    revision_ = UINT64_MAX;
    SendMessageW(window_, WM_SETREDRAW, FALSE, 0);
    SendMessageW(window_, EM_SETBKGNDCOLOR, 0, palette_.logBackground);
    Refresh();
}

void LogView::SetFilter(int filter)
{
    if (filter_ != filter) {
        filter_ = filter;
        revision_ = UINT64_MAX;
        SetWindowTextW(window_, L"");
        lines_.clear();
        Refresh(true);
    }
}

void LogView::Clear()
{
    logs_.Clear(filter_ == 0 ? std::nullopt
        : std::optional{filter_ == 1 ? LogSource::Tray : LogSource::Xray});
    Refresh(true);
}

void LogView::ScrollToEnd()
{
    Refresh(true);
}

bool LogView::Following() const
{
    CHARRANGE selection{};
    SendMessageW(window_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&selection));
    return selection.cpMin == selection.cpMax && TextViewAtEnd(window_);
}

void LogView::SetWordWrap(bool enabled)
{
    if (wordWrap_ == enabled) { return; }
    const bool follow = Following();
    SetTextWordWrap(window_, enabled);
    wordWrap_ = enabled;
    if (follow) { ScrollToEnd(); }
}

std::wstring LogView::CopyText() const
{
    std::wstring text;
    for (const auto& line : logs_.Snapshot().lines) {
        if ((filter_ == 1 && line->source != LogSource::Tray) ||
            (filter_ == 2 && line->source != LogSource::Xray)) {
            continue;
        }
        for (const wchar_t character : DisplayText(*line)) {
            if (character == L'\r') { text += L"\r\n"; }
            else { text += character; }
        }
    }
    return text;
}

void LogView::InvalidateFormat(HFONT font)
{
    formatAll_ = true;
    revision_ = UINT64_MAX;
    const auto fail = [](const char* operation) {
        throw std::runtime_error(std::string(Text("Cannot measure log source labels (", "无法测量日志来源标签（")) +
            operation + Text(").", "）。"));
    };
    HDC device = GetDC(window_);
    LOGFONTW description{};
    if (device == nullptr) {
        fail("GetDC");
    }
    if (font == nullptr || GetObjectW(font, static_cast<int>(sizeof(description)), &description) == 0) {
        ReleaseDC(window_, device);
        fail("GetObjectW");
    }
    description.lfWeight = FW_BOLD;
    const HFONT bold = CreateFontIndirectW(&description);
    if (bold == nullptr) {
        ReleaseDC(window_, device);
        fail("CreateFontIndirectW");
    }
    const HGDIOBJ previous = SelectObject(device, bold);
    std::array<SIZE, 2> labels{};
    SIZE space{};
    bool measured = GetTextExtentPoint32W(device, L"[XTRAY]", 7, &labels[0]) != FALSE &&
        GetTextExtentPoint32W(device, L"[XRAY]", 6, &labels[1]) != FALSE;
    SelectObject(device, font);
    measured = measured && GetTextExtentPoint32W(device, L" ", 1, &space) != FALSE;
    SelectObject(device, previous);
    DeleteObject(bold);
    ReleaseDC(window_, device);
    if (!measured) {
        fail("GetTextExtentPoint32W");
    }
    const int dpi = static_cast<int>(GetDpiForWindow(window_));
    for (size_t index = 0; index < labels.size(); ++index) {
        indentTwips_[index] = MulDiv(labels[index].cx + space.cx, 1440, dpi);
    }
}

LogView::Anchor LogView::Capture(LONG position) const
{
    LONG begin = 0;
    for (const auto& line : lines_) {
        const auto length = static_cast<LONG>(line.display->size());
        if (position < begin + length) {
            return {line.line->id, (std::max)(LONG{0}, position - begin)};
        }
        begin += length;
    }
    if (!lines_.empty()) {
        return {lines_.back().line->id, static_cast<LONG>(lines_.back().display->size())};
    }
    return {UINT64_MAX, 0};
}

LONG LogView::Restore(Anchor anchor, const std::vector<ViewLine>& lines) const
{
    LONG position = 0;
    for (const auto& line : lines) {
        const auto length = static_cast<LONG>(line.display->size());
        if (line.line->id >= anchor.id) {
            return position + (line.line->id == anchor.id
                ? (std::min)(anchor.offset, length) : 0);
        }
        position += length;
    }
    return position;
}

void LogView::Replace(LONG position, LONG length, const ViewLine* line)
{
    CHARRANGE range{position, position + length};
    SendMessageW(window_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&range));
    SendMessageW(window_, EM_REPLACESEL, FALSE,
        reinterpret_cast<LPARAM>(line == nullptr ? L"" : line->display->c_str()));
    if (line != nullptr) {
        ApplyFormat(position, *line);
    }
}

void LogView::ApplyFormat(LONG position, const ViewLine& line)
{
    const LONG labelLength = line.line->source == LogSource::Tray ? 7 : 6;
    // Exclude the paragraph terminator so the next paragraph keeps its own formatting.
    CHARRANGE paragraph{position, position + static_cast<LONG>(line.display->size()) - 1};
    SendMessageW(window_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&paragraph));
    CHARFORMAT2W format{};
    format.cbSize = sizeof(format);
    format.dwMask = CFM_COLOR | CFM_BOLD;
    format.crTextColor = line.line->source == LogSource::Tray
        ? palette_.trayLog : palette_.xrayLog;
    SendMessageW(window_, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&format));
    CHARRANGE label{position, position + labelLength};
    SendMessageW(window_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&label));
    format.dwMask = CFM_BOLD;
    format.dwEffects = CFE_BOLD;
    SendMessageW(window_, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&format));

    PARAFORMAT paragraphFormat{};
    paragraphFormat.cbSize = sizeof(paragraphFormat);
    paragraphFormat.dwMask = PFM_STARTINDENT | PFM_OFFSET;
    paragraphFormat.dxStartIndent = 0;
    paragraphFormat.dxOffset = indentTwips_[static_cast<size_t>(line.line->source)];
    SendMessageW(window_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&paragraph));
    SendMessageW(window_, EM_SETPARAFORMAT, 0, reinterpret_cast<LPARAM>(&paragraphFormat));
}

void LogView::Refresh(bool latest)
{
    if (!latest && revision_ == logs_.Revision()) {
        return;
    }
    const auto snapshot = logs_.Snapshot();
    std::vector<ViewLine> next;
    next.reserve(snapshot.lines.size());
    size_t previous = 0;
    for (const auto& line : snapshot.lines) {
        if ((filter_ == 1 && line->source != LogSource::Tray) ||
            (filter_ == 2 && line->source != LogSource::Xray)) {
            continue;
        }
        while (previous < lines_.size() && lines_[previous].line->id < line->id) {
            ++previous;
        }
        if (previous < lines_.size() && lines_[previous].line == line) {
            next.push_back(lines_[previous]);
        } else {
            next.push_back({line, std::make_shared<const std::wstring>(DisplayText(*line))});
        }
    }

    CHARRANGE selection{};
    SendMessageW(window_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&selection));
    const auto start = Capture(selection.cpMin);
    const auto end = Capture(selection.cpMax);
    const bool follow = latest || Following();
    const auto firstLine = SendMessageW(window_, EM_GETFIRSTVISIBLELINE, 0, 0);
    const LONG topCharacter = static_cast<LONG>(SendMessageW(window_, EM_LINEINDEX, firstLine, 0));
    const auto top = Capture((std::max)(LONG{0}, topCharacter));
    POINT oldPoint{};
    SendMessageW(window_, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&oldPoint),
        (std::max)(LONG{0}, topCharacter));
    POINT oldScroll{};
    SendMessageW(window_, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&oldScroll));

    SendMessageW(window_, WM_SETREDRAW, FALSE, 0);
    size_t left = 0;
    size_t right = 0;
    LONG position = 0;
    // IDs are ordered: insert/delete only changed ranges, rather than repainting 5000 lines.
    bool formatNext = false;
    while (left < lines_.size() || right < next.size()) {
        if (right == next.size() || (left < lines_.size() &&
            lines_[left].line->id < next[right].line->id)) {
            Replace(position, static_cast<LONG>(lines_[left].display->size()), nullptr);
            formatNext = true;
            ++left;
        } else if (left == lines_.size() || next[right].line->id < lines_[left].line->id) {
            Replace(position, 0, &next[right]);
            formatNext = true;
            position += static_cast<LONG>(next[right].display->size());
            ++right;
        } else {
            if (lines_[left].display != next[right].display) {
                Replace(position, static_cast<LONG>(lines_[left].display->size()), &next[right]);
                formatNext = true;
            } else {
                if (formatAll_ || formatNext) {
                    ApplyFormat(position, next[right]);
                }
                formatNext = false;
            }
            position += static_cast<LONG>(next[right].display->size());
            ++left;
            ++right;
        }
    }

    if (follow) {
        CHARRANGE tail{position, position};
        SendMessageW(window_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&tail));
        SendMessageW(window_, WM_VSCROLL, SB_BOTTOM, 0);
    } else {
        CHARRANGE restored{Restore(start, next), Restore(end, next)};
        SendMessageW(window_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&restored));
        POINT point{};
        SendMessageW(window_, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&point), Restore(top, next));
        POINT current{};
        SendMessageW(window_, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&current));
        POINT target{oldScroll.x, (std::max)(LONG{0}, current.y + point.y - oldPoint.y)};
        SendMessageW(window_, EM_SETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&target));
    }
    lines_ = std::move(next);
    revision_ = snapshot.revision;
    formatAll_ = false;
    SendMessageW(window_, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME);
}

} // namespace xtray
