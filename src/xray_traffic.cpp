#include "xray_traffic.h"
#include "xray_tool.h"

#include <winhttp.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <format>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace xtray {
namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using namespace winrt::Windows::Data::Json;

struct MetricsAddress {
    std::wstring host;
    INTERNET_PORT port;
};

std::optional<MetricsAddress> MetricsListener(std::string_view preview)
{
    JsonObject config{nullptr};
    if (!JsonObject::TryParse(winrt::hstring(FromUtf8(preview)), config)) { return std::nullopt; }
    const auto metrics = config.GetNamedObject(L"metrics", nullptr);
    const auto counters = config.GetNamedObject(L"stats", nullptr);
    const auto policy = config.GetNamedObject(L"policy", nullptr);
    if (!metrics || !counters || !policy) { return std::nullopt; }
    const auto system = policy.GetNamedObject(L"system", nullptr);
    if (!system || (!system.GetNamedBoolean(L"statsOutboundUplink", false) &&
        !system.GetNamedBoolean(L"statsOutboundDownlink", false))) { return std::nullopt; }
    auto listen = std::wstring(metrics.GetNamedString(L"listen", L"").c_str());
    const auto separator = listen.rfind(L':');
    if (separator == std::wstring::npos) { return std::nullopt; }
    const auto portText = ToUtf8(std::wstring_view(listen).substr(separator + 1));
    unsigned int port = 0;
    const auto end = portText.data() + portText.size();
    const auto converted = std::from_chars(portText.data(), end, port);
    if (converted.ec != std::errc{} || converted.ptr != end || port == 0 || port > 65535) {
        return std::nullopt;
    }

    auto host = listen.substr(0, separator);
    if (host.empty() || host == L"0.0.0.0") { host = L"127.0.0.1"; }
    if (host == L"[::]") { host = L"[::1]"; }
    if (host.find_first_of(L" \t\r\n/\\?#@") != std::wstring::npos) { return std::nullopt; }
    return MetricsAddress{std::move(host), static_cast<INTERNET_PORT>(port)};
}

struct CloseHttpHandle {
    void operator()(HINTERNET handle) const { WinHttpCloseHandle(handle); }
};

using HttpHandle = std::unique_ptr<void, CloseHttpHandle>;

std::string RequestStats(HINTERNET connection, std::stop_token stop)
{
    HttpHandle request(WinHttpOpenRequest(connection, L"GET", L"/debug/vars", nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0));
    if (!request) { return {}; }

    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy)) ||
        !WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr)) { return {}; }

    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) || status != 200) { return {}; }

    std::string body;
    std::array<char, 8192> buffer{};
    while (!stop.stop_requested()) {
        DWORD count = 0;
        if (!WinHttpReadData(request.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &count)) { return {}; }
        if (count == 0) { return body; }
        if (body.size() + count > 4 * 1024 * 1024) { return {}; }
        body.append(buffer.data(), count);
    }
    return {};
}

std::optional<double> ReadCounter(const JsonObject& counters, std::wstring_view direction)
{
    const double value = counters.GetNamedNumber(winrt::hstring(direction), -1);
    return std::isfinite(value) && value >= 0 ? std::optional<double>{value} : std::nullopt;
}

std::map<std::wstring, OutboundTraffic> ReadOutbounds(std::string_view response)
{
    JsonObject document{nullptr};
    if (!JsonObject::TryParse(winrt::hstring(FromUtf8(response)), document)) { return {}; }
    const auto stats = document.GetNamedObject(L"stats", nullptr);
    if (!stats) { return {}; }
    const auto outbounds = stats.GetNamedObject(L"outbound", nullptr);
    if (!outbounds) { return {}; }

    std::map<std::wstring, OutboundTraffic> result;
    for (const auto& entry : outbounds) {
        if (entry.Value().ValueType() != JsonValueType::Object) { continue; }
        const auto counters = entry.Value().GetObject();
        const auto upload = ReadCounter(counters, L"uplink");
        const auto download = ReadCounter(counters, L"downlink");
        if (upload || download) {
            result.emplace(std::wstring(entry.Key().c_str()),
                OutboundTraffic{TrafficDirection{upload, {}}, TrafficDirection{download, {}}});
        }
    }
    return result;
}

void UpdateDirection(TrafficDirection& direction, TrafficDirection& total,
    const TrafficDirection* previous, double seconds)
{
    if (!direction.bytes) { return; }
    total.bytes = total.bytes.value_or(0) + *direction.bytes;
    if (previous && previous->bytes && seconds > 0 && *direction.bytes >= *previous->bytes) {
        direction.bytesPerSecond = (*direction.bytes - *previous->bytes) / seconds;
        total.bytesPerSecond = total.bytesPerSecond.value_or(0) + *direction.bytesPerSecond;
    }
}

} // namespace

std::wstring FormatTraffic(std::optional<double> value, bool speed)
{
    if (!value) { return L"—"; }
    static constexpr std::array sizeUnits{L"B", L"KiB", L"MiB", L"GiB", L"TiB"};
    static constexpr std::array speedUnits{L"B/s", L"KiB/s", L"MiB/s"};
    double scaled = *value;
    size_t unit = 0;
    const auto unitCount = speed ? speedUnits.size() : sizeUnits.size();
    while (scaled >= 1024 && unit + 1 < unitCount) {
        scaled /= 1024;
        ++unit;
    }
    auto number = std::format(L"{:.{}f}", scaled, speed ? 1 : unit == 0 ? 0 : 2);
    if (number.find(L'.') != std::wstring::npos) {
        while (number.back() == L'0') { number.pop_back(); }
        if (number.back() == L'.') { number.pop_back(); }
    }
    return number + L" " + (speed ? speedUnits[unit] : sizeUnits[unit]);
}

XrayTraffic::XrayTraffic(std::filesystem::path root)
    : root_(std::move(root)), worker_([this](std::stop_token stop) { Run(stop); }) {}

void XrayTraffic::SetLaunch(std::optional<XrayLaunchConfiguration> launch)
{
    std::lock_guard lock(mutex_);
    ++generation_;
    launch_ = std::move(launch);
    sample_.reset();
    wake_.notify_one();
}

std::optional<XrayTrafficSample> XrayTraffic::TakeSample()
{
    std::lock_guard lock(mutex_);
    return std::exchange(sample_, std::nullopt);
}

bool XrayTraffic::Interrupted(std::stop_token stop, uint64_t generation, std::chrono::milliseconds delay)
{
    std::unique_lock lock(mutex_);
    wake_.wait_for(lock, stop, delay, [&] { return generation != generation_; });
    return stop.stop_requested() || generation != generation_;
}

void XrayTraffic::Publish(uint64_t generation, XrayTrafficSample sample)
{
    std::lock_guard lock(mutex_);
    if (generation == generation_) { sample_ = std::move(sample); }
}

void XrayTraffic::Run(std::stop_token stop)
{
    try {
        winrt::init_apartment();
    } catch (const winrt::hresult_error&) {
        return;
    }
    struct Apartment { ~Apartment() { winrt::uninit_apartment(); } } apartment;

    uint64_t generation = 0;
    for (;;) {
        std::optional<XrayLaunchConfiguration> launch;
        {
            std::unique_lock lock(mutex_);
            if (!wake_.wait(lock, stop, [&] { return generation != generation_; })) { return; }
            generation = generation_;
            launch = std::move(launch_);
        }
        if (!launch || stop.stop_requested()) { continue; }
        try {
            Monitor(stop, generation, std::move(*launch));
        } catch (...) {
            // Statistics are optional and must not affect the running proxy.
        }
    }
}

void XrayTraffic::Monitor(std::stop_token stop, uint64_t generation, XrayLaunchConfiguration launch)
{
    XrayTool preview(root_);
    if (!preview.Start(XrayToolKind::Preview, std::move(launch.settings), true,
        std::move(launch.arguments))) { return; }

    std::optional<MetricsAddress> address;
    const auto deadline = Clock::now() + 5s;
    do {
        if (auto result = preview.TakeResult()) {
            if (result->exitCode == 0 && result->error.empty()) {
                address = MetricsListener(result->output);
            }
            break;
        }
    } while (Clock::now() < deadline && !Interrupted(stop, generation, 50ms));
    if (!address || Interrupted(stop, generation, 0ms)) { return; }

    HttpHandle session(WinHttpOpen(L"XTray", WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session || !WinHttpSetTimeouts(session.get(), 750, 750, 750, 750)) { return; }
    HttpHandle connection(WinHttpConnect(session.get(), address->host.c_str(), address->port, 0));
    if (!connection) { return; }

    XrayTrafficSample previous;
    auto previousTime = Clock::now();
    for (;;) {
        const auto next = Clock::now() + 1s;
        XrayTrafficSample current;
        try {
            current.outbounds = ReadOutbounds(RequestStats(connection.get(), stop));
        } catch (const winrt::hresult_error&) {
            // A missing or invalid reply drops the baseline; the next valid one starts a new sample.
        } catch (const std::runtime_error&) {}
        const auto now = Clock::now();
        const double seconds = std::chrono::duration<double>(now - previousTime).count();
        bool uploadComplete = true, downloadComplete = true;
        for (auto& [tag, values] : current.outbounds) {
            const auto old = previous.outbounds.find(tag);
            const auto* prior = old != previous.outbounds.end() ? &old->second : nullptr;
            UpdateDirection(values.upload, current.total.upload, prior ? &prior->upload : nullptr, seconds);
            UpdateDirection(values.download, current.total.download, prior ? &prior->download : nullptr, seconds);
            uploadComplete &= !values.upload.bytes || values.upload.bytesPerSecond.has_value();
            downloadComplete &= !values.download.bytes || values.download.bytesPerSecond.has_value();
        }
        if (!uploadComplete) { current.total.upload.bytesPerSecond.reset(); }
        if (!downloadComplete) { current.total.download.bytesPerSecond.reset(); }
        const bool empty = current.outbounds.empty();
        if (!empty || !previous.outbounds.empty()) { Publish(generation, current); }
        previous = std::move(current);
        previousTime = now;
        const auto delay = empty ? 3s : std::max(Clock::duration::zero(), next - now);
        if (Interrupted(stop, generation, std::chrono::duration_cast<std::chrono::milliseconds>(delay))) { return; }
    }
}

} // namespace xtray
