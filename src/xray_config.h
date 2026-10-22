#pragma once

#include "settings.h"

namespace xtray {

struct XrayConfigArguments {
    std::wstring arguments;
    // Empty preserves inheritance; a populated block requires CREATE_UNICODE_ENVIRONMENT.
    std::vector<wchar_t> environment;
};

XrayConfigArguments PrepareXrayConfigArguments(
    const std::filesystem::path& root, const Settings& settings);

} // namespace xtray
