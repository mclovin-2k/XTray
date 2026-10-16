#pragma once

#include "win32.h"

namespace xtray {

inline constexpr UINT kShowInstanceMessage = WM_APP + 41;
inline constexpr LRESULT kInstanceShown = 0x58545259;

class DirectoryInstance {
public:
    explicit DirectoryInstance(const std::filesystem::path& directory);
    bool IsPrimary() const { return primary_; }
    const std::wstring& WindowClass() const { return windowClass_; }
    bool ActivateExisting(DWORD timeout = 5000) const;
private:
    Handle mutex_;
    std::wstring windowClass_;
    bool primary_ = false;
};

} // namespace xtray
