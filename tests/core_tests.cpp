#include "logs.h"
#include "process.h"
#include "settings.h"
#include "win32.h"

#include <shellapi.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iterator>

namespace {

using namespace xtray;

void Check(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool WaitUntil(const std::function<bool()>& condition, DWORD milliseconds = 10000)
{
    const auto deadline = GetTickCount64() + milliseconds;
    do {
        if (condition()) {
            return true;
        }
        Sleep(10);
    } while (GetTickCount64() < deadline);
    return condition();
}

bool HasText(const Logs& logs, LogSource source, const std::wstring& text)
{
    const auto snapshot = logs.Snapshot();
    for (const auto& line : snapshot.lines) {
        if (line->source == source && line->text.find(text) != std::wstring::npos) {
            return true;
        }
    }
    return false;
}

void Write(HANDLE output, std::string_view bytes)
{
    while (!bytes.empty()) {
        DWORD written = 0;
        if (!WriteFile(output, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) ||
            written == 0) {
            ExitProcess(97);
        }
        bytes.remove_prefix(written);
    }
}

int FakeXray(int argc, wchar_t** argv)
{
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    const HANDLE error = GetStdHandle(STD_ERROR_HANDLE);
    Write(output, GetConsoleWindow() == nullptr ? "console=0\n" : "console=1\n");
    Write(output, "cwd=" + ToUtf8(std::filesystem::current_path().native()) + "\n");
    std::filesystem::path first;
    if (argc < 4 || std::wstring_view(argv[1]) != L"run" || argc % 2 != 0) {
        Write(error, "Invalid fake Xray arguments.\n");
        return 98;
    }
    for (int index = 2; index + 1 < argc; index += 2) {
        const std::wstring_view flag = argv[index];
        if (flag != L"-config" && flag != L"-confdir") {
            return 96;
        }
        if (index == 2) {
            first = argv[index + 1];
        }
        Write(output, ToUtf8(std::wstring(flag) + L"=" + argv[index + 1]) + "\n");
    }
    if (first.filename() == L"fail.conf") {
        Write(output, "before failure\n");
        Write(error, "\xE9\x94");
        Sleep(30);
        Write(error, "\x99\xE8\xAF\xAF: fake failure\n");
        Write(error, "\xF0\x9F");
        return 23;
    }
    if (first.filename() == L"flood.conf") {
        for (int index = 0; index < 15000; ++index) {
            Write(output, "line " + std::to_string(index) + "\n");
        }
        Write(output, "flood complete\n");
    }
    Write(output, "\xE4\xB8");
    Sleep(30);
    Write(output, "\xAD\xE6\x96\x87 stdout\n");
    Write(error, "stderr captured\n");
    Write(output, "partial without newline");
    Sleep(INFINITE);
    return 0;
}

Settings FakeSettings(const std::filesystem::path& root, const wchar_t* mode = L"hold.conf")
{
    Settings settings;
    settings.executable = ExecutablePath().native();
    settings.configFiles = {(root / mode).native(), L".\\配置 ;= test.json"};
    settings.configDirectory = L".\\config directory\\";
    return settings;
}

int JobOwner(const std::filesystem::path& root)
{
    Logs logs;
    XrayProcess process(root, logs);
    process.Run(FakeSettings(root));
    if (!WaitUntil([&] { return process.State() == ProcessState::Running; })) {
        return 2;
    }
    {
        std::ofstream file(root / L"child.pid");
        file << process.Pid();
    }
    Sleep(INFINITE);
    return 0;
}

void TestUtf8AndLogs()
{
    Utf8Stream decoder;
    Check(decoder.Append("\xE4\xB8").empty(), "Incomplete UTF-8 must wait.");
    Check(decoder.Append("\xAD") == L"中", "Split UTF-8 character was corrupted.");
    Check(decoder.Append("\xF0\x9F\x98\x80") == L"\U0001F600", "Non-BMP UTF-8 failed.");
    Check(decoder.Append("\xFF") == L"\uFFFD", "Invalid UTF-8 did not recover.");
    Check(decoder.Append("\xE4").empty(), "Truncated UTF-8 was emitted early.");
    Check(decoder.Append({}, true) == L"\uFFFD", "EOF did not flush truncated UTF-8.");
    const auto invalid = decoder.Append("\xED\xA0\x80Z");
    Check(invalid.find(L'\uFFFD') != std::wstring::npos && invalid.back() == L'Z',
        "Invalid surrogate sequence prevented subsequent output.");

    Logs logs(2);
    logs.Tray(L"tray one");
    logs.Xray("one\r");
    auto snapshot = logs.Snapshot();
    const auto original = snapshot.lines.back();
    logs.Xray("\ntwo\nthree");
    Check(original->raw == "one\r", "A retained snapshot was mutated.");
    logs.Tray(L"tray two");
    logs.Tray(L"tray three");
    snapshot = logs.Snapshot();
    size_t tray = 0;
    size_t xray = 0;
    uint64_t previous = 0;
    for (const auto& line : snapshot.lines) {
        Check(line->id > previous, "Combined log order is incorrect.");
        previous = line->id;
        line->source == LogSource::Tray ? ++tray : ++xray;
    }
    Check(tray == 2 && xray == 2, "Per-source log limits are not independent.");
    Check(!HasText(logs, LogSource::Xray, L"one"), "Oldest Xray line was not pruned.");
    Check(HasText(logs, LogSource::Xray, L"three"), "Partial line is not visible.");
    logs.Xray("\xF0\x9F");
    logs.FinishXray();
    snapshot = logs.Snapshot();
    bool raw = false;
    for (const auto& line : snapshot.lines) {
        if (line->source == LogSource::Xray && line->raw.ends_with("\xF0\x9F")) {
            raw = line->text.ends_with(L"\uFFFD");
        }
    }
    Check(raw, "Invalid UTF-8 bytes were not retained alongside replacement text.");
    logs.ClearXray();
    Check(logs.Snapshot().lines.size() == 2, "Clearing Xray removed XTray logs.");
    logs.SetLimit(1);
    Check(logs.Snapshot().lines.size() == 1, "Changing the log limit did not prune immediately.");
}

void TestArguments()
{
    const std::vector<std::wstring> arguments{L"test.exe", L"", L"plain", L"a b", L"end\\",
        L"a\"b", L"中文 ;=& path\\", L"two\\\\\"quotes", L"C:\\"};
    std::wstring command;
    for (const auto& argument : arguments) {
        if (!command.empty()) {
            command.push_back(L' ');
        }
        command += QuoteArgument(argument);
    }
    int count = 0;
    LPWSTR* parsed = CommandLineToArgvW(command.c_str(), &count);
    Check(parsed != nullptr, "Cannot parse quoted arguments.");
    bool equal = static_cast<size_t>(count) == arguments.size();
    if (equal) {
        for (int index = 0; index < count; ++index) {
            equal = equal && arguments[static_cast<size_t>(index)] == parsed[index];
        }
    }
    LocalFree(parsed);
    Check(equal, "Windows argument quoting is incorrect.");
}

void TestSettings(const std::filesystem::path& root)
{
    std::filesystem::create_directories(root);
    Check(LoadSettings(root).success, "Missing settings should use defaults.");
    Settings settings;
    settings.executable = L"..\\bin\\xray.exe";
    settings.configFiles = {L".\\配置;=一.json", L"C:\\文件 with spaces\\二.json", L".\\配置;=一.json"};
    settings.configDirectory = L"..\\config directory\\";
    settings.logLines = 17;
    std::wstring error;
    Check(SaveSettings(root, settings, error), "Saving initial settings failed.");
    auto loaded = LoadSettings(root);
    Check(loaded.success && loaded.settings.executable == settings.executable &&
        loaded.settings.configFiles == settings.configFiles &&
        loaded.settings.configDirectory == settings.configDirectory &&
        loaded.settings.logLines == settings.logLines, "Settings round-trip failed.");
    const auto path = root / L"data" / L"settings.ini";
    std::string bytes;
    {
        std::ifstream file(path, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
    Check(!bytes.starts_with("\xEF\xBB\xBF") && bytes.find("\r\n") != std::string::npos,
        "Settings must be UTF-8 without BOM and use CRLF.");
    {
        Handle locked(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        Check(static_cast<bool>(locked), "Cannot lock settings for the failure test.");
        auto changed = settings;
        changed.logLines = 19;
        Check(!SaveSettings(root, changed, error), "Locked settings were overwritten.");
    }
    Check(LoadSettings(root).settings.logLines == 17, "Failed save changed the previous settings.");
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << "\xEF\xBB\xBF" << bytes;
    }
    Check(LoadSettings(root).success, "UTF-8 BOM was not accepted.");
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << "\xFF";
    }
    Check(!LoadSettings(root).success, "Invalid UTF-8 settings were silently accepted.");
    {
        std::ifstream file(path, std::ios::binary);
        std::string unchanged(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>{});
        Check(unchanged == "\xFF", "Reading invalid settings changed the file.");
    }
    Check(SaveSettings(root, Settings{}, error), "Explicit save could not repair invalid settings.");
    loaded = LoadSettings(root);
    Check(loaded.success && loaded.settings.configFiles.empty() &&
        loaded.settings.configDirectory.empty(), "Configuration sources must default to empty.");
}

void TestProcesses(const std::filesystem::path& root)
{
    Logs logs(64);
    XrayProcess process(root, logs);
    process.Run(Settings{});
    Check(WaitUntil([&] { return process.State() == ProcessState::Stopped; }) &&
        HasText(logs, LogSource::Tray, L"至少设置") && process.Pid() == 0,
        "Empty configuration sources did not fail before launch.");

    auto missing = FakeSettings(root);
    missing.executable = L".\\missing.exe";
    process.Run(missing);
    Check(WaitUntil([&] { return process.State() == ProcessState::Stopped; }) &&
        HasText(logs, LogSource::Tray, L"启动 Xray 失败"), "CreateProcess failure was not logged.");

    process.Run(FakeSettings(root, L"fail.conf"));
    Check(WaitUntil([&] { return process.State() == ProcessState::Stopped; }) &&
        HasText(logs, LogSource::Xray, L"错误: fake failure") &&
        HasText(logs, LogSource::Tray, L"退出码 23") &&
        HasText(logs, LogSource::Xray, L"\uFFFD"), "Early failure output or exit code was lost.");

    auto settings = FakeSettings(root);
    Check(process.Run(settings), "Run request was rejected.");
    Check(WaitUntil([&] { return HasText(logs, LogSource::Xray, L"partial without newline"); }),
        "Live partial stdout was not received.");
    Check(process.State() == ProcessState::Running && process.Pid() != 0,
        "Running status is incorrect.");
    Check(HasText(logs, LogSource::Xray, L"console=0"), "The child acquired a console.");
    Check(HasText(logs, LogSource::Xray, L"cwd=" + root.native()), "Child working directory is incorrect.");
    Check(HasText(logs, LogSource::Xray, L"-config=" +
        ResolvePath(root, settings.configFiles[1]).native()), "Unicode/multiple file arguments were corrupted.");
    Check(HasText(logs, LogSource::Xray, L"-confdir=" +
        ResolvePath(root, settings.configDirectory).native()), "Directory argument was corrupted.");
    Check(HasText(logs, LogSource::Xray, L"中文 stdout") &&
        HasText(logs, LogSource::Xray, L"stderr captured"), "stdout/stderr capture failed.");
    Check(!process.Run(settings), "A second run was allowed while Xray was running.");
    Check(!HasText(logs, LogSource::Xray, L"fake failure"), "Previous Xray output survived the next run.");

    Logs otherLogs;
    XrayProcess other(root, otherLogs);
    auto directoryOnly = settings;
    directoryOnly.configFiles.clear();
    Check(other.Run(directoryOnly), "Directory-only configuration was rejected.");
    Check(WaitUntil([&] { return other.State() == ProcessState::Running; }),
        "Independent instance did not start.");

    const DWORD oldPid = process.Pid();
    Handle old(OpenProcess(SYNCHRONIZE, FALSE, oldPid));
    logs.Tray(L"retained tray marker");
    Check(process.Restart(settings), "Restart request was rejected.");
    Check(WaitUntil([&] { return process.State() == ProcessState::Running &&
        process.Pid() != oldPid; }), "Restart did not create a new process.");
    Check(old && WaitForSingleObject(old.Get(), 0) == WAIT_OBJECT_0,
        "Restart created a replacement before the old process exited.");
    Check(HasText(logs, LogSource::Tray, L"retained tray marker"), "Restart removed XTray logs.");
    Check(WaitUntil([&] { return process.Stop(); }), "Stop request was rejected.");
    Check(WaitUntil([&] { return process.State() == ProcessState::Stopped; }),
        "Stop did not finish.");
    Check(other.State() == ProcessState::Running, "Stopping one instance stopped another.");
    other.Shutdown();

    process.Run(FakeSettings(root, L"flood.conf"));
    Check(WaitUntil([&] { return HasText(logs, LogSource::Xray, L"flood complete"); }),
        "Pipe capture did not keep up with a burst of output.");
    size_t count = 0;
    for (const auto& line : logs.Snapshot().lines) {
        if (line->source == LogSource::Xray) {
            ++count;
        }
    }
    Check(count <= 64, "Large output exceeded the configured line limit.");
    const DWORD ownedPid = process.Pid();
    Handle owned(OpenProcess(SYNCHRONIZE, FALSE, ownedPid));
    process.Shutdown();
    Check(owned && WaitForSingleObject(owned.Get(), 0) == WAIT_OBJECT_0,
        "Shutdown left the managed child running.");
}

void TestAbnormalExit(const std::filesystem::path& root)
{
    std::wstring command = QuoteArgument(ExecutablePath().native()) +
        L" --test-owner " + QuoteArgument(root.native());
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION information{};
    Check(CreateProcessW(ExecutablePath().c_str(), command.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, root.c_str(), &startup, &information) != FALSE,
        "Cannot create the crash-test owner process.");
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
    Check(WaitUntil([&] {
        std::ifstream file(root / L"child.pid");
        file >> pid;
        return pid != 0;
    }), "Crash-test owner did not report its child PID.");
    Handle child(OpenProcess(SYNCHRONIZE, FALSE, pid));
    Check(static_cast<bool>(child), "Cannot open crash-test child.");
    Check(TerminateProcess(owner.process.Get(), 123) != FALSE, "Cannot terminate the test owner.");
    Check(WaitForSingleObject(owner.process.Get(), 5000) == WAIT_OBJECT_0,
        "Test owner did not exit.");
    Check(WaitForSingleObject(child.Get(), 5000) == WAIT_OBJECT_0,
        "Closing the job on abnormal owner exit did not terminate the child.");
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    try {
        if (argc > 1 && std::wstring_view(argv[1]) == L"run") {
            return FakeXray(argc, argv);
        }
        if (argc == 3 && std::wstring_view(argv[1]) == L"--test-owner") {
            return JobOwner(argv[2]);
        }
        const auto root = ExecutablePath().parent_path() / L"test data 中文" /
            (std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        std::filesystem::create_directories(root);
        TestUtf8AndLogs();
        TestArguments();
        TestSettings(root / L"settings");
        TestProcesses(root);
        TestAbnormalExit(root);
        std::puts("All core tests passed. No real Xray process or network listener was started.");
        return 0;
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "Test failed: %s\n", exception.what());
        return 1;
    }
}
