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

struct OutboundTraffic {
    TrafficDirection upload, download;
};

struct XrayTrafficSample {
    OutboundTraffic total;
    std::map<std::wstring, OutboundTraffic> outbounds;
};

std::wstring FormatTraffic(std::optional<double> value, bool speed);

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
