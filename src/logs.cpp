#include "logs.h"
#include "win32.h"

#include <algorithm>

namespace xtray {

std::wstring Utf8Stream::Append(std::string_view bytes, bool finish)
{
    pending_.append(bytes);
    std::wstring result;
    size_t position = 0;
    while (position < pending_.size()) {
        const auto first = static_cast<unsigned char>(pending_[position]);
        if (first < 0x80) {
            result.push_back(static_cast<wchar_t>(first));
            ++position;
            continue;
        }
        size_t length = 0;
        unsigned int codepoint = 0;
        if (first >= 0xC2 && first <= 0xDF) {
            length = 2;
            codepoint = first & 0x1F;
        } else if (first >= 0xE0 && first <= 0xEF) {
            length = 3;
            codepoint = first & 0x0F;
        } else if (first >= 0xF0 && first <= 0xF4) {
            length = 4;
            codepoint = first & 0x07;
        } else {
            result.push_back(L'\uFFFD');
            ++position;
            continue;
        }

        size_t consumed = 1;
        bool invalid = false;
        while (consumed < length && position + consumed < pending_.size()) {
            const auto byte = static_cast<unsigned char>(pending_[position + consumed]);
            if (byte < 0x80 || byte > 0xBF ||
                (consumed == 1 && ((first == 0xE0 && byte < 0xA0) ||
                    (first == 0xED && byte > 0x9F) ||
                    (first == 0xF0 && byte < 0x90) ||
                    (first == 0xF4 && byte > 0x8F)))) {
                invalid = true;
                break;
            }
            codepoint = (codepoint << 6) | (byte & 0x3F);
            ++consumed;
        }
        if (!invalid && consumed < length && !finish) {
            break;
        }
        if (invalid || consumed < length) {
            result.push_back(L'\uFFFD');
            position += consumed;
            continue;
        }
        if (codepoint <= 0xFFFF) {
            result.push_back(static_cast<wchar_t>(codepoint));
        } else {
            codepoint -= 0x10000;
            result.push_back(static_cast<wchar_t>(0xD800 + (codepoint >> 10)));
            result.push_back(static_cast<wchar_t>(0xDC00 + (codepoint & 0x3FF)));
        }
        position += length;
    }
    pending_.erase(0, position);
    return result;
}

void Logs::MakeWritable(std::shared_ptr<LogLine>& line)
{
    // Snapshots stay immutable while the reader appends to a partial line.
    if (line.use_count() != 1) {
        line = std::make_shared<LogLine>(*line);
    }
}

void Logs::Append(LogSource source, std::string_view bytes)
{
    if (bytes.empty()) {
        return;
    }
    std::lock_guard lock(mutex_);
    auto& buffer = buffers_[static_cast<size_t>(source)];
    size_t position = 0;
    while (position < bytes.size()) {
        if (buffer.lines.empty() || buffer.lines.back()->complete) {
            auto line = std::make_shared<LogLine>();
            line->id = nextId_++;
            line->source = source;
            buffer.lines.push_back(std::move(line));
        }
        auto& line = buffer.lines.back();
        MakeWritable(line);
        const size_t newline = bytes.find('\n', position);
        const size_t end = newline == std::string_view::npos ? bytes.size() : newline + 1;
        const auto part = bytes.substr(position, end - position);
        line->raw.append(part);
        line->text += buffer.decoder.Append(part);
        line->complete = newline != std::string_view::npos;
        position = end;
        while (buffer.lines.size() > limit_) {
            buffer.lines.pop_front();
        }
    }
    ++revision_;
}

void Logs::Tray(const std::wstring& message)
{
    Append(LogSource::Tray, ToUtf8(message + L"\r\n"));
}

void Logs::Xray(std::string_view bytes)
{
    Append(LogSource::Xray, bytes);
}

void Logs::FinishXray()
{
    std::lock_guard lock(mutex_);
    auto& buffer = buffers_[static_cast<size_t>(LogSource::Xray)];
    const auto tail = buffer.decoder.Append({}, true);
    if (!tail.empty() && !buffer.lines.empty()) {
        MakeWritable(buffer.lines.back());
        buffer.lines.back()->text += tail;
        ++revision_;
    }
}

void Logs::ClearXray()
{
    std::lock_guard lock(mutex_);
    buffers_[static_cast<size_t>(LogSource::Xray)] = {};
    ++revision_;
}

void Logs::SetLimit(size_t limit)
{
    std::lock_guard lock(mutex_);
    limit_ = (std::max)(size_t{1}, limit);
    for (auto& buffer : buffers_) {
        while (buffer.lines.size() > limit_) {
            buffer.lines.pop_front();
        }
    }
    ++revision_;
}

uint64_t Logs::Revision() const
{
    std::lock_guard lock(mutex_);
    return revision_;
}

LogSnapshot Logs::Snapshot() const
{
    std::lock_guard lock(mutex_);
    LogSnapshot result;
    result.revision = revision_;
    const auto& tray = buffers_[0].lines;
    const auto& xray = buffers_[1].lines;
    result.lines.reserve(tray.size() + xray.size());
    auto left = tray.begin();
    auto right = xray.begin();
    while (left != tray.end() || right != xray.end()) {
        if (right == xray.end() || (left != tray.end() && (*left)->id < (*right)->id)) {
            result.lines.push_back(*left++);
        } else {
            result.lines.push_back(*right++);
        }
    }
    return result;
}

} // namespace xtray
