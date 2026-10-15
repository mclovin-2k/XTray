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
    : root_(std::move(root)), logs_(logs), worker_(&XrayProcess::Worker, this)
{
}

XrayProcess::~XrayProcess()
{
    Shutdown();
}

bool XrayProcess::Run(Settings settings)
{
    return Enqueue({Operation::Run, std::move(settings)});
}

bool XrayProcess::Stop()
{
    return Enqueue({Operation::Stop, {}});
}

bool XrayProcess::Restart(Settings settings)
{
    return Enqueue({Operation::Restart, std::move(settings)});
}

bool XrayProcess::Enqueue(Command command)
{
    std::lock_guard lock(mutex_);
    const auto state = State();
    if (quit_ || busy_ || pending_ || state == ProcessState::Starting || state == ProcessState::Stopping ||
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
    if (worker_.joinable()) {
        worker_.join();
    }
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
                    StartProcess(command->settings);
                } else if (command->operation == Operation::Stop) {
                    StopProcess();
                } else if (StopProcess()) {
                    state_ = ProcessState::Starting;
                    StartProcess(command->settings);
                }
            }
            {
                std::lock_guard lock(mutex_);
                busy_ = false;
            }
            if (process_) {
                DrainOutput();
                if (WaitForSingleObject(process_.Get(), 0) == WAIT_OBJECT_0) {
                    FinishProcess();
                }
            }
        }
        StopProcess();
    } catch (const std::exception& exception) {
        logs_.Tray(L"进程管理错误：" + FromUtf8(exception.what()) + L"；请退出 XTray 后重新打开。");
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
    }
    output_.Reset();
    process_.Reset();
    pid_ = 0;
    state_ = ProcessState::Stopped;
}

void XrayProcess::StartProcess(const Settings& settings)
{
    logs_.ClearXray();
    const auto fail = [this](const std::wstring& error) {
        logs_.Tray(error);
        state_ = ProcessState::Stopped;
    };
    if (settings.configFiles.empty() && settings.configDirectory.empty()) {
        fail(L"请至少设置一个配置文件或一个配置目录。");
        return;
    }
    if (settings.executable.empty()) {
        fail(L"请设置 Xray 程序路径。");
        return;
    }

    const auto executable = ResolvePath(root_, settings.executable);
    if (!executable.is_absolute()) {
        fail(L"无法解析 Xray 程序路径，请使用绝对路径或相对于 XTray 目录的路径。");
        return;
    }
    std::wstring command = QuoteArgument(executable.native()) + L" run";
    for (const auto& config : settings.configFiles) {
        const auto path = ResolvePath(root_, config);
        if (!path.is_absolute()) {
            fail(L"无法解析配置文件路径：" + config);
            return;
        }
        command += L" -config " + QuoteArgument(path.native());
    }
    if (!settings.configDirectory.empty()) {
        const auto path = ResolvePath(root_, settings.configDirectory);
        if (!path.is_absolute()) {
            fail(L"无法解析配置目录路径：" + settings.configDirectory);
            return;
        }
        command += L" -confdir " + QuoteArgument(path.native());
    }
    if (command.size() >= 32767) {
        fail(L"Xray 启动命令超过 Windows 长度限制，请减少配置文件数量或缩短路径。");
        return;
    }

    Handle job(CreateJobObjectW(nullptr, nullptr));
    if (!job) {
        fail(WindowsError(L"创建 Xray Job 失败"));
        return;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job.Get(), JobObjectExtendedLimitInformation,
        &limits, sizeof(limits))) {
        fail(WindowsError(L"设置 Xray Job 失败"));
        return;
    }

    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read = nullptr;
    HANDLE write = nullptr;
    if (!CreatePipe(&read, &write, &security, 0)) {
        fail(WindowsError(L"创建输出管道失败"));
        return;
    }
    Handle output(read);
    Handle input(write);
    if (!SetHandleInformation(output.Get(), HANDLE_FLAG_INHERIT, 0)) {
        fail(WindowsError(L"设置输出管道失败"));
        return;
    }
    Handle nullInput(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!nullInput) {
        fail(WindowsError(L"创建空标准输入失败"));
        return;
    }

    Attributes attributes;
    if (attributes.Get() == nullptr) {
        fail(WindowsError(L"初始化进程属性失败"));
        return;
    }
    std::array<HANDLE, 2> inherited{input.Get(), nullInput.Get()};
    HANDLE jobHandle = job.Get();
    // Attach the child to its job during creation, leaving no unowned suspended process.
    if (!UpdateProcThreadAttribute(attributes.Get(), 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        inherited.data(), inherited.size() * sizeof(HANDLE), nullptr, nullptr) ||
        !UpdateProcThreadAttribute(attributes.Get(), 0, PROC_THREAD_ATTRIBUTE_JOB_LIST,
        &jobHandle, sizeof(jobHandle), nullptr, nullptr)) {
        fail(WindowsError(L"设置 Xray 进程属性失败"));
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
    logs_.Tray(L"正在运行 Xray：" + executable.native());
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, root_.c_str(),
        &startup.StartupInfo, &information)) {
        fail(WindowsError(L"启动 Xray 失败"));
        return;
    }
    Handle thread(information.hThread);
    process_.Reset(information.hProcess);
    job_ = std::move(job);
    output_ = std::move(output);
    input.Reset();
    pid_ = information.dwProcessId;
    state_ = ProcessState::Running;
    logs_.Tray(L"Xray 已启动，PID " + std::to_wstring(information.dwProcessId) + L"。");
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
                logs_.Tray(WindowsError(L"读取 Xray 输出管道失败", error));
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
                logs_.Tray(WindowsError(L"读取 Xray 输出失败", error));
            }
            output_.Reset();
            return;
        }
        logs_.Xray(std::string_view(buffer.data(), read));
    }
}

void XrayProcess::FinishProcess()
{
    DWORD code = 0;
    if (!GetExitCodeProcess(process_.Get(), &code)) {
        logs_.Tray(WindowsError(L"读取 Xray 退出码失败"));
    }
    job_.Reset();
    DrainOutput();
    logs_.FinishXray();
    output_.Reset();
    process_.Reset();
    pid_ = 0;
    logs_.Tray(L"Xray 已退出，退出码 " + std::to_wstring(code) + L"。");
    state_ = ProcessState::Stopped;
}

bool XrayProcess::StopProcess()
{
    if (!process_) {
        state_ = ProcessState::Stopped;
        return true;
    }
    if (WaitForSingleObject(process_.Get(), 0) != WAIT_OBJECT_0) {
        logs_.Tray(L"正在停止 Xray。");
        if (!TerminateJobObject(job_.Get(), ERROR_CANCELLED)) {
            const DWORD error = GetLastError();
            if (WaitForSingleObject(process_.Get(), 0) != WAIT_OBJECT_0) {
                logs_.Tray(WindowsError(L"停止 Xray 失败", error));
                state_ = ProcessState::Running;
                return false;
            }
        }
        const DWORD wait = WaitForSingleObject(process_.Get(), 5000);
        if (wait != WAIT_OBJECT_0) {
            logs_.Tray(wait == WAIT_TIMEOUT
                ? L"停止 Xray 超时，尚未确认退出；未启动新进程。"
                : WindowsError(L"等待 Xray 退出失败"));
            state_ = ProcessState::Running;
            return false;
        }
    }
    FinishProcess();
    return true;
}

} // namespace xtray
