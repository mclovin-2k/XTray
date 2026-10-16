#pragma once

#include "logs.h"
#include <windows.h>

namespace xtray {

class LogView {
public:
    LogView(HWND window, Logs& logs);
    void SetFilter(int filter);
    void Clear();
    void InvalidateFormat();
    void Refresh(bool latest = false);
private:
    struct ViewLine {
        std::shared_ptr<const LogLine> line;
        std::shared_ptr<const std::wstring> display;
    };
    struct Anchor { uint64_t id; LONG offset; };
    Anchor Capture(LONG position) const;
    LONG Restore(Anchor anchor, const std::vector<ViewLine>& lines) const;
    void Replace(LONG position, LONG length, const ViewLine* line);
    void ApplyFormat(LONG position, const ViewLine& line);
    HWND window_;
    Logs& logs_;
    int filter_ = 0;
    bool formatAll_ = true;
    std::array<LONG, 2> indentTwips_{};
    uint64_t revision_ = UINT64_MAX;
    std::vector<ViewLine> lines_;
};

} // namespace xtray
