#include "xray_traffic.h"
#include "language.h"
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
    TrafficCounters outbound, inbound;
};

XrayTrafficSample UnavailableSample(TrafficAvailability availability)
{
    XrayTrafficSample sample;
    sample.outbound.availability = sample.inbound.availability = availability;
    sample.time = Clock::now();
    return sample;
}

std::optional<MetricsAddress> MetricsListener(std::string_view preview, TrafficAvailability& availability)
{
    JsonObject config{nullptr};
    if (!JsonObject::TryParse(winrt::hstring(FromUtf8(preview)), config)) { return std::nullopt; }
    const auto metrics = config.GetNamedObject(L"metrics", nullptr);
    if (!metrics || metrics.GetNamedString(L"listen", L"").empty()) {
        availability = TrafficAvailability::MetricsDisabled;
        return std::nullopt;
    }
    const auto counters = config.GetNamedObject(L"stats", nullptr);
    const auto policy = config.GetNamedObject(L"policy", nullptr);
    const auto system = policy ? policy.GetNamedObject(L"system", nullptr) : JsonObject{nullptr};
    const TrafficCounters outbound{
        counters && system && system.GetNamedBoolean(L"statsOutboundUplink", false),
        counters && system && system.GetNamedBoolean(L"statsOutboundDownlink", false)};
    const TrafficCounters inbound{
        counters && system && system.GetNamedBoolean(L"statsInboundUplink", false),
        counters && system && system.GetNamedBoolean(L"statsInboundDownlink", false)};
    if (!outbound.Any() && !inbound.Any()) {
        availability = TrafficAvailability::StatisticsDisabled;
        return std::nullopt;
    }
    availability = TrafficAvailability::InvalidMetricsAddress;
    const auto listen = std::wstring(metrics.GetNamedString(L"listen").c_str());
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
    return MetricsAddress{std::move(host), static_cast<INTERNET_PORT>(port), outbound, inbound};
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

void ReadGroup(const JsonObject& stats, const wchar_t* name, TrafficGroup& group)
{
    if (!group.counters.Any()) { return; }
    group.availability = TrafficAvailability::NoCounters;
    const auto endpoints = stats ? stats.GetNamedObject(name, nullptr) : JsonObject{nullptr};
    if (!endpoints) { return; }

    for (const auto& entry : endpoints) {
        if (entry.Value().ValueType() != JsonValueType::Object) { continue; }
        const auto counters = entry.Value().GetObject();
        const auto upload = group.counters.upload ? ReadCounter(counters, L"uplink") : std::nullopt;
        const auto download = group.counters.download ? ReadCounter(counters, L"downlink") : std::nullopt;
        if (upload || download) {
            group.endpoints.emplace(std::wstring(entry.Key().c_str()),
                EndpointTraffic{TrafficDirection{upload, {}}, TrafficDirection{download, {}}});
        }
    }
    if (!group.endpoints.empty()) { group.availability = TrafficAvailability::Available; }
}

void ReadMetrics(std::string_view response, XrayTrafficSample& sample)
{
    JsonObject document{nullptr};
    if (!JsonObject::TryParse(winrt::hstring(FromUtf8(response)), document)) { return; }
    const auto stats = document.GetNamedObject(L"stats", nullptr);
    ReadGroup(stats, L"outbound", sample.outbound);
    ReadGroup(stats, L"inbound", sample.inbound);
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

void UpdateGroupRates(TrafficGroup& group, const TrafficGroup& previous, double seconds)
{
    bool uploadComplete = true, downloadComplete = true;
    for (auto& [tag, values] : group.endpoints) {
        const auto old = previous.endpoints.find(tag);
        const auto* prior = old != previous.endpoints.end() ? &old->second : nullptr;
        UpdateDirection(values.upload, group.total.upload, prior ? &prior->upload : nullptr, seconds);
        UpdateDirection(values.download, group.total.download, prior ? &prior->download : nullptr, seconds);
        uploadComplete &= !values.upload.bytes || values.upload.bytesPerSecond.has_value();
        downloadComplete &= !values.download.bytes || values.download.bytesPerSecond.has_value();
    }
    if (!uploadComplete) { group.total.upload.bytesPerSecond.reset(); }
    if (!downloadComplete) { group.total.download.bytesPerSecond.reset(); }
}

} // namespace

std::wstring FormatTraffic(std::optional<double> value, bool speed)
{
    if (!value) { return L"--"; }
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

const wchar_t* TrafficUnavailableReason(TrafficAvailability availability, bool inbound)
{
    switch (availability) {
    case TrafficAvailability::Stopped:
        return Text(L"Xray is not running.", L"Xray 尚未运行。");
    case TrafficAvailability::Checking:
        return Text(L"Checking Xray Metrics settings…", L"正在检查 Xray Metrics 配置…");
    case TrafficAvailability::ConfigurationUnavailable:
        return Text(L"XTray could not read the merged Xray configuration.", L"无法读取 Xray 的合并配置。");
    case TrafficAvailability::MetricsDisabled:
        return Text(L"Metrics is disabled in Xray.", L"未启用 Xray Metrics。");
    case TrafficAvailability::InvalidMetricsAddress:
        return Text(L"Xray metrics.listen does not contain a valid listening address.", L"Xray 的 metrics.listen 监听地址无效。");
    case TrafficAvailability::StatisticsDisabled:
        return inbound
            ? Text(L"Inbound traffic statistics are disabled.", L"未启用入站流量统计。")
            : Text(L"Outbound traffic statistics are disabled.", L"未启用出站流量统计。");
    case TrafficAvailability::MetricsUnavailable:
        return Text(L"XTray cannot read the Xray Metrics endpoint.", L"无法读取 Xray Metrics 接口。");
    case TrafficAvailability::NoCounters:
        return inbound
            ? Text(L"Xray Metrics has not returned any inbound counters.", L"Xray Metrics 未返回入站流量计数。")
            : Text(L"Xray Metrics has not returned any outbound counters.", L"Xray Metrics 未返回出站流量计数。");
    default:
        return L"";
    }
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
            Publish(generation, UnavailableSample(TrafficAvailability::ConfigurationUnavailable));
        }
    }
}

void XrayTraffic::Monitor(std::stop_token stop, uint64_t generation, XrayLaunchConfiguration launch)
{
    const auto publishUnavailable = [&](TrafficAvailability availability) {
        Publish(generation, UnavailableSample(availability));
    };
    XrayTool preview(root_);
    if (!preview.Start(XrayToolKind::Preview, std::move(launch.settings), true,
        std::move(launch.arguments))) {
        publishUnavailable(TrafficAvailability::ConfigurationUnavailable);
        return;
    }

    std::optional<MetricsAddress> address;
    auto availability = TrafficAvailability::ConfigurationUnavailable;
    const auto deadline = Clock::now() + 5s;
    do {
        if (auto result = preview.TakeResult()) {
            if (result->exitCode == 0 && result->error.empty()) {
                address = MetricsListener(result->output, availability);
            }
            break;
        }
    } while (Clock::now() < deadline && !Interrupted(stop, generation, 50ms));
    if (Interrupted(stop, generation, 0ms)) { return; }
    if (!address) {
        publishUnavailable(availability);
        return;
    }

    auto unavailable = UnavailableSample(TrafficAvailability::MetricsUnavailable);
    unavailable.outbound.counters = address->outbound;
    unavailable.inbound.counters = address->inbound;
    for (auto* group : {&unavailable.outbound, &unavailable.inbound}) {
        if (!group->counters.Any()) { group->availability = TrafficAvailability::StatisticsDisabled; }
    }
    HttpHandle session(WinHttpOpen(L"XTray", WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session || !WinHttpSetTimeouts(session.get(), 750, 750, 750, 750)) {
        Publish(generation, unavailable);
        return;
    }
    HttpHandle connection(WinHttpConnect(session.get(), address->host.c_str(), address->port, 0));
    if (!connection) {
        Publish(generation, unavailable);
        return;
    }

    XrayTrafficSample previous;
    for (;;) {
        const auto next = Clock::now() + 1s;
        auto current = unavailable;
        try {
            ReadMetrics(RequestStats(connection.get(), stop), current);
        } catch (const winrt::hresult_error&) {
            // A missing or invalid reply drops the baseline; the next valid one starts a new sample.
            current = unavailable;
        } catch (const std::runtime_error&) {
            current = unavailable;
        }
        const auto now = Clock::now();
        current.time = now;
        const double seconds = std::chrono::duration<double>(now - previous.time).count();
        UpdateGroupRates(current.outbound, previous.outbound, seconds);
        UpdateGroupRates(current.inbound, previous.inbound, seconds);
        const bool empty = current.outbound.endpoints.empty() && current.inbound.endpoints.empty();
        if (!empty || current.outbound.availability != previous.outbound.availability ||
            current.inbound.availability != previous.inbound.availability) {
            Publish(generation, current);
        }
        previous = std::move(current);
        const auto delay = empty ? 3s : std::max(Clock::duration::zero(), next - now);
        if (Interrupted(stop, generation, std::chrono::duration_cast<std::chrono::milliseconds>(delay))) { return; }
    }
}

} // namespace xtray
