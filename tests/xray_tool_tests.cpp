#include "xray_tool.h"
#include "xray_process.h"

#include <array>
#include <cstdio>
#include <fstream>
#include <functional>

namespace {

using namespace xtray;

void Require(bool condition, const char* reason)
{
    if (!condition) { throw std::runtime_error(reason); }
}

bool Wait(const std::function<bool()>& predicate)
{
    const auto until = GetTickCount64() + 5000;
    do {
        if (predicate()) { return true; }
        Sleep(10);
    } while (GetTickCount64() < until);
    return predicate();
}

void WriteTool(HANDLE destination, std::string_view bytes)
{
    while (!bytes.empty()) {
        DWORD copied = 0;
        if (!WriteFile(destination, bytes.data(), static_cast<DWORD>(bytes.size()), &copied, nullptr) || copied == 0) {
            ExitProcess(97);
        }
        bytes.remove_prefix(copied);
    }
}

Settings ToolSettings(const std::filesystem::path& root, const wchar_t* first = L".\\tool-ok.conf")
{
    Settings settings;
    settings.executable = ExecutablePath().native();
    settings.configFiles = {first, L".\\重复 ;= config.json", L".\\重复 ;= config.json",
        (root / L"external folder" / L"absolute.json").native()};
    settings.configDirectory = L"..\\shared configs\\";
    return settings;
}

XrayToolResult Result(XrayTool& tool)
{
    Require(Wait([&] { return !tool.Running(); }), "Xray tool did not complete.");
    auto result = tool.TakeResult();
    Require(result.has_value(), "Xray tool did not publish a result.");
    Require(!tool.TakeResult(), "Xray tool result was delivered twice.");
    return std::move(*result);
}

} // namespace

std::optional<int> FakeXrayTool(int argc, wchar_t** argv)
{
    if (argc == 3 && std::wstring_view(argv[1]) == L"--tool-owner") {
        const std::filesystem::path root = argv[2];
        xtray::XrayTool task(root);
        task.Start(xtray::XrayToolKind::Preview, ToolSettings(root, L".\\tool-hold.conf"));
        if (!Wait([&] { return task.Pid() != 0; })) { return 94; }
        std::ofstream marker(root / L"tool-child.pid");
        marker << task.Pid();
        marker.close();
        Sleep(INFINITE);
        return 0;
    }
    const bool version = argc >= 2 && std::wstring_view(argv[1]) == L"version";
    const bool config = argc >= 3 && std::wstring_view(argv[1]) == L"run" &&
        (std::wstring_view(argv[2]) == L"-test" || std::wstring_view(argv[2]) == L"-dump");
    if (!version && !config) { return std::nullopt; }
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE), error = GetStdHandle(STD_ERROR_HANDLE);
    WriteTool(output, GetConsoleWindow() == nullptr ? "console=0\n" : "console=1\n");
    WriteTool(output, "cwd=" + xtray::ToUtf8(std::filesystem::current_path().native()) + "\n");
    if (version) {
        WriteTool(output, "Xray test-version (fake build; no server)\n");
        return argc == 2 ? 0 : 93;
    }
    if (argc < 5 || argc % 2 != 1) { return 92; }
    WriteTool(output, xtray::ToUtf8(argv[2]) + "\n");
    std::filesystem::path first;
    for (int index = 3; index + 1 < argc; index += 2) {
        const std::wstring_view flag = argv[index];
        if (flag != L"-config" && flag != L"-confdir") { return 91; }
        if (flag == L"-config" && first.empty()) {
            first = std::filesystem::path(argv[index + 1]).filename();
        }
        WriteTool(output, xtray::ToUtf8(std::wstring(flag) + L"=" + argv[index + 1]) + "\n");
    }
    if (first == L"tool-hold.conf") {
        for (;;) {
            WriteTool(output, "tool waiting for cancellation\n");
            Sleep(10);
        }
    }
    WriteTool(output, "\xE4\xB8");
    Sleep(10);
    WriteTool(output, "\xAD\xE6\x96\x87 tool stdout\n");
    WriteTool(error, "tool stderr\n");
    if (first == L"tool-fail.conf") {
        WriteTool(error, "invalid configuration: expected failure\n");
        return 23;
    }
    if (first == L"tool-large.conf") {
        for (unsigned index = 0; index < 12000; ++index) {
            WriteTool(output, "tool line " + std::to_string(index) + ": complete configuration output retained\n");
        }
    }
    if (first == L"tool-large.conf") {
        WriteTool(output, std::string(2400, 'X') + "\n");
    }
    WriteTool(output, "tool output end");
    return 0;
}

void TestXrayTools(const std::filesystem::path& root)
{
    using namespace xtray;
    std::puts("Tools: check, dump, version, complete output, cancellation, and proxy isolation.");
    XrayTool tool(root);
    auto settings = ToolSettings(root);
    Require(tool.Start(XrayToolKind::Check, settings), "Cannot start the configuration check.");
    settings.configFiles.clear();
    auto checked = Result(tool);
    const auto first = checked.output.find("-config=.\\重复 ;= config.json\n");
    const auto second = first == std::string::npos ? first : checked.output.find("-config=.\\重复 ;= config.json\n", first + 1);
    Require(checked.exitCode == 0 && checked.error.empty() &&
        checked.executable == ExecutablePath() &&
        checked.output.find("console=0\n") != std::string::npos &&
        checked.output.find("cwd=" + ToUtf8(root.native()) + "\n") != std::string::npos &&
        checked.output.find("-test\n-confdir=..\\shared configs\\\n-config=.\\tool-ok.conf\n") != std::string::npos &&
        checked.output.find(ToUtf8((root / L"external folder" / L"absolute.json").native())) != std::string::npos &&
        checked.output.find("-confdir=..\\shared configs\\\n") != std::string::npos &&
        first != std::string::npos && second > first,
        "Configuration check changed paths, ordering, duplicates, cwd, hidden execution, or the settings snapshot.");
    Require(FromUtf8(checked.output).find(L"中文 tool stdout") != std::wstring::npos &&
        checked.output.find("tool stderr\n") != std::string::npos,
        "Tool output lost split UTF-8 or stderr.");

    settings = ToolSettings(root, L".\\tool-large.conf");
    Require(tool.Start(XrayToolKind::Preview, settings), "Cannot start the merged preview.");
    auto large = Result(tool);
    Require(large.exitCode == 0 && large.output.size() > 500000 &&
        large.output.find("-dump\n-confdir=..\\shared configs\\\n-config=.\\tool-large.conf\n") != std::string::npos &&
        large.output.find("tool line 0:") != std::string::npos &&
        large.output.find("tool line 11999:") != std::string::npos &&
        large.output.ends_with("tool output end"), "Tool preview truncated its full output.");
    Require(tool.Start(XrayToolKind::Check, ToolSettings(root, L".\\tool-fail.conf")), "Cannot start the failing check.");
    auto failed = Result(tool);
    Require(failed.exitCode == 23 && failed.error.empty() &&
        failed.output.find("中文 tool stdout") != std::string::npos &&
        failed.output.find("expected failure") != std::string::npos,
        "Configuration check lost its failure exit code or original output.");

    auto directoryOnly = ToolSettings(root);
    directoryOnly.configFiles.clear();
    Require(tool.Start(XrayToolKind::Preview, directoryOnly), "Cannot preview a directory without configuration files.");
    const auto directoryResult = Result(tool);
    Require(directoryResult.exitCode == 0 &&
        directoryResult.output.find("-config=") == std::string::npos &&
        directoryResult.output.find("-confdir=..\\shared configs\\\n") != std::string::npos,
        "Directory-only configuration source was not preserved.");

    Settings noConfig;
    noConfig.executable = ExecutablePath().native();
    Require(tool.Start(XrayToolKind::Version, noConfig), "Cannot query version without a configuration.");
    auto version = Result(tool);
    Require(version.exitCode == 0 && version.output.find("Xray test-version") != std::string::npos,
        "Version command required configuration files.");
    Require(tool.Start(XrayToolKind::Check, noConfig), "Cannot report missing configuration.");
    Require(!Result(tool).error.empty(), "Missing configuration was not reported.");
    Require(tool.Start(XrayToolKind::Version, noConfig, false), "Cannot report unreadable saved settings.");
    Require(!Result(tool).error.empty(), "Unreadable settings were silently accepted.");
    noConfig.executable = (root / L"missing-tool.exe").native();
    Require(tool.Start(XrayToolKind::Version, noConfig), "Cannot report a missing executable.");
    const auto missing = Result(tool);
    Require(!missing.error.empty() && !missing.exitCode, "Missing tool executable was not reported.");

    Logs logs;
    XrayProcess proxy(root, logs);
    auto proxySettings = ToolSettings(root, L".\\hold.conf");
    Require(proxy.Run(proxySettings), "Cannot start the isolation-test proxy.");
    Require(Wait([&] {
        const auto snapshot = logs.Snapshot();
        for (const auto& line : snapshot.lines) {
            if (line->text.find(L"partial without newline") != std::wstring::npos) { return true; }
        }
        return false;
    }), "Isolation-test proxy did not settle.");
    const DWORD proxyPid = proxy.Pid();
    const auto revision = logs.Revision();
    Require(tool.Start(XrayToolKind::Preview, ToolSettings(root, L".\\tool-hold.conf")), "Cannot start cancellation task.");
    Require(Wait([&] { return tool.Pid() != 0; }), "Cancellation task did not start.");
    Handle child(OpenProcess(SYNCHRONIZE, FALSE, tool.Pid()));
    Require(static_cast<bool>(child), "Cannot observe the tool child.");
    Require(!tool.Start(XrayToolKind::Version, proxySettings), "A second tool was started simultaneously.");
    const auto cancellation = GetTickCount64();
    tool.Cancel();
    const auto cancelled = tool.TakeResult();
    Require(cancelled && cancelled->cancelled && GetTickCount64() - cancellation < 2000 &&
        WaitForSingleObject(child.Get(), 0) == WAIT_OBJECT_0,
        "Closing the tool task did not promptly clean up its process.");
    Require(proxy.State() == ProcessState::Running && proxy.Pid() == proxyPid && logs.Revision() == revision,
        "A tool task altered the running proxy or its logs.");
    Require(tool.Start(XrayToolKind::Check, ToolSettings(root)), "Cannot reuse the tool after cancellation.");
    Require(Result(tool).exitCode == 0, "Reused tool failed.");
    Require(proxy.Pid() == proxyPid && logs.Revision() == revision, "Completed tool changed proxy state.");

    Handle destructorChild;
    {
        XrayTool owned(root);
        Require(owned.Start(XrayToolKind::Preview, ToolSettings(root, L".\\tool-hold.conf")), "Cannot start destructor task.");
        Require(Wait([&] { return owned.Pid() != 0; }), "Destructor task did not start.");
        destructorChild.Reset(OpenProcess(SYNCHRONIZE, FALSE, owned.Pid()));
    }
    Require(destructorChild && WaitForSingleObject(destructorChild.Get(), 0) == WAIT_OBJECT_0,
        "Tool process survived its C++ owner.");

    std::puts("Tools: abnormal owner termination closes the independent tool job.");
    auto command = QuoteArgument(ExecutablePath().native()) + L" --tool-owner " + QuoteArgument(root.native());
    STARTUPINFOW startup{sizeof(STARTUPINFOW)};
    PROCESS_INFORMATION information{};
    Require(CreateProcessW(ExecutablePath().c_str(), command.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, root.c_str(), &startup, &information) != FALSE,
        "Cannot start tool crash-test owner.");
    Handle thread(information.hThread);
    struct Owner {
        Handle process;
        ~Owner()
        {
            if (process && WaitForSingleObject(process.Get(), 0) != WAIT_OBJECT_0) {
                TerminateProcess(process.Get(), 123);
                WaitForSingleObject(process.Get(), 5000);
            }
        }
    } owner{Handle(information.hProcess)};
    DWORD pid = 0;
    Require(Wait([&] {
        std::ifstream marker(root / L"tool-child.pid");
        marker >> pid;
        return pid != 0;
    }), "Tool crash-test owner did not report the child.");
    Handle orphan(OpenProcess(SYNCHRONIZE, FALSE, pid));
    Require(static_cast<bool>(orphan), "Cannot observe the crash-test tool child.");
    Require(TerminateProcess(owner.process.Get(), 123) != FALSE, "Cannot terminate the tool owner.");
    Require(WaitForSingleObject(owner.process.Get(), 5000) == WAIT_OBJECT_0 &&
        WaitForSingleObject(orphan.Get(), 5000) == WAIT_OBJECT_0,
        "Tool process survived abnormal owner termination.");
}
