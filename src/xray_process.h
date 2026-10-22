#pragma once

#include "logs.h"
#include "settings.h"
#include "win32.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

namespace xtray {

enum class ProcessState { Stopped, Starting, Running, Stopping };

struct XrayRunInfo {
    bool attempted = false;
    std::filesystem::path executable;
    std::wstring commandLine;
    std::filesystem::path workingDirectory;
    ProcessState state = ProcessState::Stopped;
    DWORD pid = 0;
    std::optional<std::chrono::system_clock::time_point> started, ended;
    std::chrono::steady_clock::duration elapsed{};
    std::optional<DWORD> exitCode;
    std::wstring error;
};

class XrayProcess {
public:
    XrayProcess(std::filesystem::path root, Logs& logs);
    ~XrayProcess();
    XrayProcess(const XrayProcess&) = delete;
    XrayProcess& operator=(const XrayProcess&) = delete;
    bool Run(Settings settings);
    bool Stop();
    bool Restart(Settings settings);
    void Shutdown();
    ProcessState State() const { return state_.load(); }
    DWORD Pid() const { return pid_.load(); }
    XrayRunInfo RunInfo() const;
private:
    enum class Operation { Run, Stop, Restart };
    struct Command { Operation operation; Settings settings; };
    bool Enqueue(Command command);
    void Worker();
    void StartProcess(const Settings& settings);
    bool StopProcess();
    void DrainOutput();
    void FinishProcess();
    const std::filesystem::path root_;
    Logs& logs_;
    std::atomic<ProcessState> state_{ProcessState::Stopped};
    std::atomic<DWORD> pid_{0};
    mutable std::mutex infoMutex_;
    XrayRunInfo info_;
    std::chrono::steady_clock::time_point startedAt_{};
    std::mutex mutex_;
    std::condition_variable wake_;
    std::optional<Command> pending_;
    bool quit_ = false;
    bool busy_ = false;
    Handle process_;
    Handle job_;
    Handle output_;
    std::thread worker_;
};

} // namespace xtray
