#pragma once

#include "xray_config.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <thread>

namespace xtray {

struct TrafficDirection {
    std::optional<double> bytes;
    std::optional<double> bytesPerSecond;
};

struct EndpointTraffic {
    TrafficDirection upload, download;
};

enum class TrafficAvailability {
    Stopped, Checking, ConfigurationUnavailable, MetricsDisabled, InvalidMetricsAddress,
    StatisticsDisabled, MetricsUnavailable, NoCounters, Available
};

struct TrafficCounters {
    bool upload = false;
    bool download = false;
    bool Any() const { return upload || download; }
};

struct TrafficGroup {
    EndpointTraffic total;
    std::map<std::wstring, EndpointTraffic> endpoints;
    TrafficCounters counters;
    TrafficAvailability availability = TrafficAvailability::Stopped;
};

struct XrayTrafficSample {
    TrafficGroup outbound, inbound;
    std::chrono::steady_clock::time_point time;
};

std::wstring FormatTraffic(std::optional<double> value, bool speed);
const wchar_t* TrafficUnavailableReason(TrafficAvailability availability, bool inbound);

class XrayTraffic {
public:
    explicit XrayTraffic(std::filesystem::path root);
    void SetLaunch(std::optional<XrayLaunchConfiguration> launch);
    std::optional<XrayTrafficSample> TakeSample();

private:
    void Run(std::stop_token stop);
    void Monitor(std::stop_token stop, uint64_t generation, XrayLaunchConfiguration launch);
    bool Interrupted(std::stop_token stop, uint64_t generation, std::chrono::milliseconds delay);
    void Publish(uint64_t generation, XrayTrafficSample sample);

    const std::filesystem::path root_;
    std::mutex mutex_;
    std::condition_variable_any wake_;
    uint64_t generation_ = 0;
    std::optional<XrayLaunchConfiguration> launch_;
    std::optional<XrayTrafficSample> sample_;
    std::jthread worker_;
};

} // namespace xtray
