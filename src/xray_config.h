#pragma once

#include "settings.h"

#include <array>
#include <optional>
#include <stop_token>
#include <string_view>

namespace xtray {

struct XrayConfigArguments {
    std::wstring arguments;
    // Empty preserves inheritance; a populated block requires CREATE_UNICODE_ENVIRONMENT.
    std::vector<wchar_t> environment;
    std::vector<std::wstring> expandedDirectoryFiles;
};

struct XrayLaunchConfiguration {
    Settings settings;
    XrayConfigArguments arguments;
};

struct XrayConfigFileFingerprint {
    std::filesystem::path path;
    std::optional<std::array<unsigned char, 32>> hash;
    bool operator==(const XrayConfigFileFingerprint&) const = default;
};

using XrayConfigFingerprint = std::vector<XrayConfigFileFingerprint>;

std::optional<XrayConfigFingerprint> CaptureXrayConfigFiles(
    const std::filesystem::path& root, const Settings& settings, std::stop_token stop,
    const XrayConfigArguments* prepared = nullptr);

bool XrayConfigPathUsesTail(std::wstring_view path, bool directory);

XrayConfigArguments PrepareXrayConfigArguments(
    const std::filesystem::path& root, const Settings& settings);

} // namespace xtray
