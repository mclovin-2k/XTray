#pragma once

#include "settings.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace xtray {

inline constexpr wchar_t kLogonArgument[] = L"--logon";
bool IsLogonCommandLine(std::wstring_view commandLine);

struct StartupStatus {
    bool success = true;
    bool enabled = false;
    std::wstring error;
};

class StartupRegistry {
public:
    virtual ~StartupRegistry() = default;
    virtual bool Read(const std::wstring& name, std::optional<std::wstring>& command,
        std::wstring& error) = 0;
    virtual bool Write(const std::wstring& name, const std::optional<std::wstring>& command,
        std::wstring& error) = 0;
};

class StartupRegistration {
public:
    StartupRegistration(std::filesystem::path root, std::filesystem::path executable,
        StartupRegistry* registry = nullptr);
    StartupStatus Query() const;
    bool Save(const Settings& settings, bool enabled, std::wstring& error);

private:
    std::filesystem::path root_;
    std::filesystem::path executable_;
    StartupRegistry* registry_;
};

} // namespace xtray
