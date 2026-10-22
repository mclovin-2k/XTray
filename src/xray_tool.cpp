#include "xray_tool.h"
#include "xray_config.h"

#include <algorithm>
#include <array>

namespace xtray {
namespace {

class ToolAttributes {
public:
    ToolAttributes()
    {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 2, 0, &size);
        storage_.resize(size);
        list_ = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
        if (!InitializeProcThreadAttributeList(list_, 2, 0, &size)) {
            list_ = nullptr;
        }
    }
    ~ToolAttributes()
    {
        if (list_ != nullptr) { DeleteProcThreadAttributeList(list_); }
    }
    LPPROC_THREAD_ATTRIBUTE_LIST Get() const { return list_; }
private:
    std::vector<unsigned char> storage_;
    LPPROC_THREAD_ATTRIBUTE_LIST list_ = nullptr;
};

void Require(bool success, const wchar_t* operation)
{
    if (!success) {
        throw std::runtime_error(ToUtf8(WindowsError(operation)));
    }
}

} // namespace

XrayTool::~XrayTool() { Cancel(); }

bool XrayTool::Start(XrayToolKind kind, Settings settings, bool settingsValid)
{
    if (Running()) { return false; }
    if (worker_.joinable()) { worker_.join(); }
    result_ = {};
    ready_ = false;
    cancel_ = false;
    running_ = true;
    try {
        worker_ = std::thread(&XrayTool::Worker, this, kind, std::move(settings), settingsValid);
    } catch (...) {
        running_ = false;
        throw;
    }
    return true;
}

void XrayTool::Cancel()
{
    cancel_ = true;
    if (worker_.joinable()) { worker_.join(); }
}

std::optional<XrayToolResult> XrayTool::TakeResult()
{
    if (!ready_.load(std::memory_order_acquire)) { return std::nullopt; }
    if (worker_.joinable()) { worker_.join(); }
    ready_ = false;
    return std::move(result_);
}

void XrayTool::Worker(XrayToolKind kind, Settings settings, bool settingsValid) noexcept
{
    try {
        Execute(kind, settings, settingsValid);
    } catch (const std::exception& exception) {
        try {
            result_.error = FromUtf8(exception.what());
        } catch (...) {
            // Even an allocation failure must release the process and publish completion.
        }
    } catch (...) {
        try {
            result_.error = Text(L"The Xray tool failed.", L"Xray 工具执行失败。");
        } catch (...) {}
    }
    result_.cancelled = cancel_.load();
    pid_ = 0;
    ready_.store(true, std::memory_order_release);
    running_ = false;
}

void XrayTool::Execute(XrayToolKind kind, const Settings& settings, bool settingsValid)
{
    if (!settingsValid) {
        result_.error = Text(L"Saved settings could not be read. Open File > Settings and save them again.",
            L"尚未成功读取已保存设置，请打开“文件 → 设置”重新填写并保存。");
        return;
    }
    if (settings.executable.empty()) {
        result_.error = Text(L"Set the Xray executable path in File > Settings.",
            L"请在“文件 → 设置”中填写 Xray 程序路径。");
        return;
    }
    result_.executable = ResolvePath(root_, settings.executable);
    if (!result_.executable.is_absolute()) {
        result_.error = Text(L"Cannot resolve the Xray executable path.", L"无法解析 Xray 程序路径。");
        return;
    }
    if (kind != XrayToolKind::Version && settings.configFiles.empty() && settings.configDirectory.empty()) {
        result_.error = Text(L"Set at least one configuration file or one configuration directory.",
            L"请至少设置一个配置文件或一个配置目录。");
        return;
    }
    auto command = QuoteArgument(result_.executable.native()) +
        (kind == XrayToolKind::Version ? L" version"
            : kind == XrayToolKind::Check ? L" run -test" : L" run -dump");
    XrayConfigArguments configuration;
    if (kind != XrayToolKind::Version) {
        configuration = PrepareXrayConfigArguments(root_, settings);
        command += configuration.arguments;
    }
    if (command.size() >= 32767) {
        result_.error = Text(L"The Xray command exceeds the Windows length limit.",
            L"Xray 命令超过 Windows 长度限制，请减少配置文件数量或缩短路径。");
        return;
    }
    Handle job(CreateJobObjectW(nullptr, nullptr));
    Require(static_cast<bool>(job), Text(L"Cannot create the tool job", L"创建工具 Job 失败"));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    Require(SetInformationJobObject(job.Get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)) != FALSE,
        Text(L"Cannot configure the tool job", L"设置工具 Job 失败"));

    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    Require(CreatePipe(&read, &write, &security, 0) != FALSE,
        Text(L"Cannot create the tool output pipe", L"创建工具输出管道失败"));
    Handle output(read), input(write);
    Require(SetHandleInformation(output.Get(), HANDLE_FLAG_INHERIT, 0) != FALSE,
        Text(L"Cannot configure the tool output pipe", L"设置工具输出管道失败"));
    Handle nullInput(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    Require(static_cast<bool>(nullInput), Text(L"Cannot create tool standard input", L"创建工具标准输入失败"));

    ToolAttributes attributes;
    Require(attributes.Get() != nullptr, Text(L"Cannot initialize tool process attributes", L"初始化工具进程属性失败"));
    std::array<HANDLE, 2> inherited{input.Get(), nullInput.Get()};
    HANDLE jobHandle = job.Get();
    Require(UpdateProcThreadAttribute(attributes.Get(), 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        inherited.data(), inherited.size() * sizeof(HANDLE), nullptr, nullptr) != FALSE &&
        UpdateProcThreadAttribute(attributes.Get(), 0, PROC_THREAD_ATTRIBUTE_JOB_LIST,
            &jobHandle, sizeof(jobHandle), nullptr, nullptr) != FALSE,
        Text(L"Cannot configure tool process attributes", L"设置工具进程属性失败"));

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
    if (cancel_) { return; }
    Require(CreateProcessW(result_.executable.c_str(), command.data(), nullptr, nullptr, TRUE,
        flags, configuration.environment.empty() ? nullptr : configuration.environment.data(), root_.c_str(),
        &startup.StartupInfo, &information) != FALSE,
        Text(L"Cannot start the Xray tool", L"启动 Xray 工具失败"));
    Handle process(information.hProcess), thread(information.hThread);
    pid_ = information.dwProcessId;
    input.Reset();

    std::array<char, 16384> buffer{};
    const auto drain = [&] {
        // Batches keep cancellation responsive; none of the captured output is discarded.
        for (int batch = 0; batch < 64; ++batch) {
            DWORD available = 0;
            if (!PeekNamedPipe(output.Get(), nullptr, 0, nullptr, &available, nullptr)) {
                if (GetLastError() == ERROR_BROKEN_PIPE) { return false; }
                Require(false, Text(L"Cannot inspect tool output", L"读取工具输出管道失败"));
            }
            if (available == 0) { return false; }
            DWORD copied = 0;
            Require(ReadFile(output.Get(), buffer.data(),
                (std::min)(available, static_cast<DWORD>(buffer.size())), &copied, nullptr) != FALSE,
                Text(L"Cannot read tool output", L"读取工具输出失败"));
            result_.output.append(buffer.data(), copied);
        }
        return true;
    };
    for (;;) {
        drain();
        if (cancel_) {
            job.Reset();
            WaitForSingleObject(process.Get(), 5000);
            break;
        }
        const auto wait = WaitForSingleObject(process.Get(), 20);
        Require(wait != WAIT_FAILED, Text(L"Cannot wait for the Xray tool", L"等待 Xray 工具失败"));
        if (wait == WAIT_OBJECT_0) { break; }
    }
    DWORD code = 0;
    Require(GetExitCodeProcess(process.Get(), &code) != FALSE,
        Text(L"Cannot read the Xray tool exit code", L"读取 Xray 工具退出码失败"));
    result_.exitCode = code;
    job.Reset();
    while (drain()) {}
}

} // namespace xtray
