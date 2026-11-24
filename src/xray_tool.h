#pragma once

#include "xray_config.h"
#include "win32.h"

#include <atomic>
#include <optional>
#include <thread>

namespace xtray {

enum class XrayToolKind { Check, Preview, Version };

struct XrayToolResult {
    std::filesystem::path executable;
    std::string output;
    std::wstring error;
    std::optional<DWORD> exitCode;
    bool cancelled = false;
};

// One independent, short-lived command; it never writes to the proxy's logs.
class XrayTool {
public:
    explicit XrayTool(std::filesystem::path root) : root_(std::move(root)) {}
    ~XrayTool();
    XrayTool(const XrayTool&) = delete;
    XrayTool& operator=(const XrayTool&) = delete;
    bool Start(XrayToolKind kind, Settings settings, bool settingsValid = true,
        std::optional<XrayConfigArguments> arguments = std::nullopt);
    void Cancel();
    bool Running() const { return running_.load(); }
    DWORD Pid() const { return pid_.load(); }
    std::optional<XrayToolResult> TakeResult();
private:
    void Execute(XrayToolKind kind, const Settings& settings, bool settingsValid,
        std::optional<XrayConfigArguments> arguments);
    void Worker(XrayToolKind kind, Settings settings, bool settingsValid,
        std::optional<XrayConfigArguments> arguments) noexcept;
    const std::filesystem::path root_;
    std::atomic<bool> running_ = false, cancel_ = false, ready_ = false;
    std::atomic<DWORD> pid_ = 0;
    std::thread worker_;
    XrayToolResult result_;
};

} // namespace xtray
