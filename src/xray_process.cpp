#include "xray_process.h"

#include <array>

namespace xtray {
namespace {

class Attributes {
public:
    Attributes()
    {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 2, 0, &size);
        storage_.resize(size);
        list_ = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
        if (!InitializeProcThreadAttributeList(list_, 2, 0, &size)) {
            list_ = nullptr;
        }
    }
    ~Attributes()
    {
        if (list_ != nullptr) {
            DeleteProcThreadAttributeList(list_);
        }
    }
    LPPROC_THREAD_ATTRIBUTE_LIST Get() const { return list_; }
private:
    std::vector<unsigned char> storage_;
    LPPROC_THREAD_ATTRIBUTE_LIST list_ = nullptr;
};

} // namespace

XrayProcess::XrayProcess(std::filesystem::path root, Logs& logs)
    : root_(std::move(root)), logs_(logs),
      configWorker_([this](std::stop_token stop) { CheckConfigFiles(stop); }),
      worker_(&XrayProcess::Worker, this)
{
}

XrayProcess::~XrayProcess()
{
    Shutdown();
}

XrayRunInfo XrayProcess::RunInfo() const
{
    std::lock_guard lock(infoMutex_);
    auto result = info_;
    result.state = State();
    if (result.started && !result.ended) {
        result.elapsed = std::chrono::steady_clock::now() - startedAt_;
    }
    return result;
}

bool XrayProcess::HasPendingSettings(const Settings& settings) const
{
    std::lock_guard lock(infoMutex_);
    return info_.started && !info_.ended &&
        (launchSettings_.executable != settings.executable ||
            launchSettings_.configFiles != settings.configFiles ||
            launchSettings_.configDirectory != settings.configDirectory ||
            launchSettings_.LoadsConfigDirectoryFirst() != settings.LoadsConfigDirectoryFirst());
}

void XrayProcess::RequestConfigFileCheck()
{
    std::lock_guard lock(infoMutex_);
    if (State() == ProcessState::Running && configBaseline_ && !info_.ended) {
        configCheckPending_ = true;
        configCheckWake_.notify_one();
    }
}

bool XrayProcess::ConfigFilesChanged() const
{
    std::lock_guard lock(infoMutex_);
    return State() == ProcessState::Running && !info_.ended && configFilesChanged_;
}

void XrayProcess::CheckConfigFiles(std::stop_token stop)
{
    while (!stop.stop_requested()) {
        std::shared_ptr<const ConfigBaseline> baseline;
        {
            std::unique_lock lock(infoMutex_);
            if (!configCheckWake_.wait(lock, stop, [this] { return configCheckPending_; })) { return; }
            configCheckPending_ = false;
            baseline = configBaseline_;
        }
        if (!baseline) { continue; }
        const auto current = CaptureXrayConfigFiles(root_, baseline->settings, stop);
        if (!current) { continue; }

        const bool changed = *current != baseline->fingerprint;
        std::lock_guard lock(infoMutex_);
        if (configBaseline_ == baseline && !info_.ended) {
            configFilesChanged_ = changed;
        }
    }
}

std::vector<UnexpectedXrayExit> XrayProcess::TakeUnexpectedExits()
{
    std::lock_guard lock(mutex_);
    return std::exchange(unexpectedExits_, {});
}

bool XrayProcess::Run(Settings settings)
{
    return Enqueue({Operation::Run, std::move(settings)});
}

bool XrayProcess::Stop()
{
    return Enqueue({Operation::Stop, {}});
}

bool XrayProcess::Restart(Settings settings, DWORD expectedPid)
{
    return Enqueue({Operation::Restart, std::move(settings)}, expectedPid);
}

bool XrayProcess::Enqueue(Command command, DWORD expectedPid)
{
    std::lock_guard lock(mutex_);
    const auto state = State();
    if (quit_ || busy_ || pending_ || state == ProcessState::Starting || state == ProcessState::Stopping ||
        (expectedPid != 0 && (state != ProcessState::Running || Pid() != expectedPid)) ||
        (command.operation == Operation::Run && state != ProcessState::Stopped) ||
        (command.operation == Operation::Stop && state == ProcessState::Stopped)) {
        return false;
    }
    state_ = command.operation == Operation::Run ||
        (command.operation == Operation::Restart && state == ProcessState::Stopped)
        ? ProcessState::Starting : ProcessState::Stopping;
    pending_ = std::move(command);
    wake_.notify_one();
    return true;
}

void XrayProcess::Shutdown()
{
    {
        std::lock_guard lock(mutex_);
        quit_ = true;
    }
    wake_.notify_one();
    configWorker_.request_stop();
    if (configWorker_.joinable()) { CancelSynchronousIo(configWorker_.native_handle()); }
    if (worker_.joinable()) {
        if (capturingConfigBaseline_) { CancelSynchronousIo(worker_.native_handle()); }
        worker_.join();
    }
    if (configWorker_.joinable()) { configWorker_.join(); }
}

void XrayProcess::Worker()
{
    try {
        for (;;) {
            std::optional<Command> command;
            {
                std::unique_lock lock(mutex_);
                wake_.wait_for(lock, std::chrono::milliseconds(20),
                    [this] { return quit_ || pending_.has_value(); });
                if (quit_) {
                    break;
                }
                command = std::move(pending_);
                pending_.reset();
                busy_ = command.has_value();
            }
            if (command) {
                if (command->operation == Operation::Run) {
                    StartProcess(std::move(command->settings));
                } else if (command->operation == Operation::Stop) {
                    StopProcess();
                } else if (StopProcess()) {
                    state_ = ProcessState::Starting;
                    StartProcess(std::move(command->settings));
                }
            }
            {
                std::lock_guard lock(mutex_);
                // Publish completion and release the command slot under the same lock.
                if (command) {
                    state_ = process_ ? ProcessState::Running : ProcessState::Stopped;
                }
                busy_ = false;
            }
            if (process_) {
                DrainOutput();
                if (WaitForSingleObject(process_.Get(), 0) == WAIT_OBJECT_0) {
                    FinishProcess(true);
                }
            }
        }
        StopProcess();
    } catch (const std::exception& exception) {
        const auto error = Text(L"Process management error: ", L"进程管理错误：") + FromUtf8(exception.what()) + Text(L"; exit XTray and open it again.", L"；请退出 XTray 后重新打开。");
        logs_.Tray(error);
        {
            std::lock_guard lock(infoMutex_);
            info_.error = error;
        }
        std::lock_guard lock(mutex_);
        quit_ = true;
        busy_ = false;
    }
    // The job is never inherited: closing it also covers shutdown/error paths.
    job_.Reset();
    if (process_) {
        WaitForSingleObject(process_.Get(), 5000);
        DrainOutput();
        logs_.FinishXray();
        std::lock_guard lock(infoMutex_);
        info_.ended = std::chrono::system_clock::now();
        info_.elapsed = std::chrono::steady_clock::now() - startedAt_;
        DWORD code = 0;
        if (GetExitCodeProcess(process_.Get(), &code) && code != STILL_ACTIVE) { info_.exitCode = code; }
    }
    output_.Reset();
    process_.Reset();
    pid_ = 0;
    state_ = ProcessState::Stopped;
}

void XrayProcess::StartProcess(Settings settings)
{
    logs_.ClearXray();
    const auto executable = settings.executable.empty() ? std::filesystem::path{}
        : ResolvePath(root_, settings.executable);
    std::wstring command = QuoteArgument(executable.native()) + L" run";
    {
        std::lock_guard lock(infoMutex_);
        info_ = {};
        configBaseline_.reset();
        configCheckPending_ = false;
        configFilesChanged_ = false;
        info_.attempted = true;
        info_.executable = executable;
        info_.commandLine = command;
        info_.workingDirectory = root_;
    }
    const auto fail = [this](const std::wstring& error) {
        {
            std::lock_guard lock(infoMutex_);
            info_.error = error;
        }
        logs_.Tray(error);
    };
    XrayConfigArguments configuration;
    try {
        configuration = PrepareXrayConfigArguments(root_, settings);
        command += configuration.arguments;
        std::lock_guard lock(infoMutex_);
        info_.commandLine = command;
    } catch (const std::exception& exception) {
        fail(FromUtf8(exception.what()));
        return;
    }
    if (settings.configFiles.empty() && settings.configDirectory.empty()) {
        fail(Text(L"Set at least one configuration file or one configuration directory.", L"请至少设置一个配置文件或一个配置目录。"));
        return;
    }
    if (settings.executable.empty()) {
        fail(Text(L"Set the Xray executable path.", L"请设置 Xray 程序路径。"));
        return;
    }

    if (!executable.is_absolute()) {
        fail(Text(L"Cannot resolve the Xray executable path. Use an absolute path or a path relative to XTray.", L"无法解析 Xray 程序路径，请使用绝对路径或相对于 XTray 目录的路径。"));
        return;
    }
    if (command.size() >= 32767) {
        fail(Text(L"The Xray command exceeds the Windows length limit. Use fewer configuration files or shorter paths.", L"Xray 启动命令超过 Windows 长度限制，请减少配置文件数量或缩短路径。"));
        return;
    }

    Handle job(CreateJobObjectW(nullptr, nullptr));
    if (!job) {
        fail(WindowsError(Text(L"Cannot create the Xray job", L"创建 Xray Job 失败")));
        return;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job.Get(), JobObjectExtendedLimitInformation,
        &limits, sizeof(limits))) {
        fail(WindowsError(Text(L"Cannot configure the Xray job", L"设置 Xray Job 失败")));
        return;
    }

    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read = nullptr;
    HANDLE write = nullptr;
    if (!CreatePipe(&read, &write, &security, 0)) {
        fail(WindowsError(Text(L"Cannot create the output pipe", L"创建输出管道失败")));
        return;
    }
    Handle output(read);
    Handle input(write);
    if (!SetHandleInformation(output.Get(), HANDLE_FLAG_INHERIT, 0)) {
        fail(WindowsError(Text(L"Cannot configure the output pipe", L"设置输出管道失败")));
        return;
    }
    Handle nullInput(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!nullInput) {
        fail(WindowsError(Text(L"Cannot create empty standard input", L"创建空标准输入失败")));
        return;
    }

    Attributes attributes;
    if (attributes.Get() == nullptr) {
        fail(WindowsError(Text(L"Cannot initialize process attributes", L"初始化进程属性失败")));
        return;
    }
    std::array<HANDLE, 2> inherited{input.Get(), nullInput.Get()};
    HANDLE jobHandle = job.Get();
    // Attach the child to its job during creation, leaving no unowned suspended process.
    if (!UpdateProcThreadAttribute(attributes.Get(), 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        inherited.data(), inherited.size() * sizeof(HANDLE), nullptr, nullptr) ||
        !UpdateProcThreadAttribute(attributes.Get(), 0, PROC_THREAD_ATTRIBUTE_JOB_LIST,
        &jobHandle, sizeof(jobHandle), nullptr, nullptr)) {
        fail(WindowsError(Text(L"Cannot configure Xray process attributes", L"设置 Xray 进程属性失败")));
        return;
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = nullInput.Get();
    startup.StartupInfo.hStdOutput = input.Get();
    startup.StartupInfo.hStdError = input.Get();
    startup.lpAttributeList = attributes.Get();
    PROCESS_INFORMATION information{};
    const DWORD flags = CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT |
        (configuration.environment.empty() ? 0 : CREATE_UNICODE_ENVIRONMENT);
    const auto stop = configWorker_.get_stop_token();
    std::shared_ptr<const ConfigBaseline> baseline;
    capturingConfigBaseline_ = true;
    if (auto fingerprint = CaptureXrayConfigFiles(root_, settings, stop, &configuration)) {
        baseline = std::make_shared<ConfigBaseline>(ConfigBaseline{settings, std::move(*fingerprint)});
    }
    capturingConfigBaseline_ = false;
    if (stop.stop_requested()) { return; }

    logs_.Tray(Text(L"Starting Xray: ", L"正在运行 Xray：") + executable.native());
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
        flags, configuration.environment.empty() ? nullptr : configuration.environment.data(), root_.c_str(),
        &startup.StartupInfo, &information)) {
        fail(WindowsError(Text(L"Cannot start Xray", L"启动 Xray 失败")));
        return;
    }
    Handle thread(information.hThread);
    process_.Reset(information.hProcess);
    job_ = std::move(job);
    output_ = std::move(output);
    input.Reset();
    {
        std::lock_guard lock(infoMutex_);
        launchSettings_ = std::move(settings);
        configBaseline_ = std::move(baseline);
        info_.pid = information.dwProcessId;
        info_.started = std::chrono::system_clock::now();
        startedAt_ = std::chrono::steady_clock::now();
    }
    pid_ = information.dwProcessId;
    logs_.Tray(Text(L"Xray started, PID ", L"Xray 已启动，PID ") + std::to_wstring(information.dwProcessId) + Text(L".", L"。"));
}

void XrayProcess::DrainOutput()
{
    if (!output_) {
        return;
    }
    std::array<char, 4096> buffer{};
    // Bound each batch so continuous output cannot starve control requests.
    for (int batch = 0; batch < 64; ++batch) {
        DWORD available = 0;
        if (!PeekNamedPipe(output_.Get(), nullptr, 0, nullptr, &available, nullptr)) {
            const DWORD error = GetLastError();
            if (error != ERROR_BROKEN_PIPE) {
                logs_.Tray(WindowsError(Text(L"Cannot read the Xray output pipe", L"读取 Xray 输出管道失败"), error));
            }
            output_.Reset();
            return;
        }
        if (available == 0) {
            return;
        }
        DWORD read = 0;
        const auto count = (std::min)(available, static_cast<DWORD>(buffer.size()));
        if (!ReadFile(output_.Get(), buffer.data(), count, &read, nullptr) || read == 0) {
            const DWORD error = GetLastError();
            if (error != ERROR_BROKEN_PIPE) {
                logs_.Tray(WindowsError(Text(L"Cannot read Xray output", L"读取 Xray 输出失败"), error));
            }
            output_.Reset();
            return;
        }
        logs_.Xray(std::string_view(buffer.data(), read));
    }
}

void XrayProcess::FinishProcess(bool exitedOnItsOwn)
{
    DWORD code = 0;
    const bool hasCode = GetExitCodeProcess(process_.Get(), &code) != FALSE;
    if (!hasCode) {
        logs_.Tray(WindowsError(Text(L"Cannot read the Xray exit code", L"读取 Xray 退出码失败")));
    }
    {
        std::lock_guard lock(infoMutex_);
        info_.ended = std::chrono::system_clock::now();
        info_.elapsed = std::chrono::steady_clock::now() - startedAt_;
        if (hasCode) { info_.exitCode = code; }
    }
    job_.Reset();
    DrainOutput();
    logs_.FinishXray();
    output_.Reset();
    process_.Reset();
    pid_ = 0;
    const auto logId = logs_.Tray(hasCode
        ? Text(L"Xray exited, exit code ", L"Xray 已退出，退出码 ") + std::to_wstring(code) + Text(L".", L"。")
        : Text(L"Xray exited.", L"Xray 已退出。"));
    std::lock_guard lock(mutex_);
    if (exitedOnItsOwn && !quit_ && !pending_) {
        unexpectedExits_.push_back({logId, hasCode ? std::optional{code} : std::nullopt});
    }
    if (!busy_ && !pending_) {
        state_ = ProcessState::Stopped;
    }
}

bool XrayProcess::StopProcess()
{
    if (!process_) {
        return true;
    }
    if (WaitForSingleObject(process_.Get(), 0) != WAIT_OBJECT_0) {
        logs_.Tray(Text(L"Stopping Xray.", L"正在停止 Xray。"));
        if (!TerminateJobObject(job_.Get(), ERROR_CANCELLED)) {
            const DWORD error = GetLastError();
            if (WaitForSingleObject(process_.Get(), 0) != WAIT_OBJECT_0) {
                logs_.Tray(WindowsError(Text(L"Cannot stop Xray", L"停止 Xray 失败"), error));
                return false;
            }
        }
        const DWORD wait = WaitForSingleObject(process_.Get(), 5000);
        if (wait != WAIT_OBJECT_0) {
            logs_.Tray(wait == WAIT_TIMEOUT
                ? Text(L"Timed out stopping Xray. Its exit was not confirmed; no replacement was started.", L"停止 Xray 超时，尚未确认退出；未启动新进程。")
                : WindowsError(Text(L"Cannot wait for Xray to exit", L"等待 Xray 退出失败")));
            return false;
        }
    }
    FinishProcess(false);
    return true;
}

} // namespace xtray
