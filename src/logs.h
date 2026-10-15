#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace xtray {

enum class LogSource { Tray, Xray };

class Utf8Stream {
public:
    std::wstring Append(std::string_view bytes, bool finish = false);
private:
    std::string pending_;
};

struct LogLine {
    uint64_t id = 0;
    LogSource source = LogSource::Tray;
    std::string raw;
    std::wstring text;
    bool complete = false;
};

struct LogSnapshot {
    uint64_t revision = 0;
    std::vector<std::shared_ptr<const LogLine>> lines;
};

class Logs {
public:
    explicit Logs(size_t limit = 5000) : limit_(limit == 0 ? 1 : limit) {}
    void Tray(const std::wstring& message);
    void Xray(std::string_view bytes);
    void FinishXray();
    void ClearXray();
    void SetLimit(size_t limit);
    uint64_t Revision() const;
    LogSnapshot Snapshot() const;
private:
    struct Buffer {
        std::deque<std::shared_ptr<LogLine>> lines;
        Utf8Stream decoder;
    };
    void Append(LogSource source, std::string_view bytes);
    static void MakeWritable(std::shared_ptr<LogLine>& line);
    mutable std::mutex mutex_;
    std::array<Buffer, 2> buffers_;
    size_t limit_;
    uint64_t nextId_ = 1;
    uint64_t revision_ = 0;
};

} // namespace xtray
