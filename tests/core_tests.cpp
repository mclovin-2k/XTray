#include "instance.h"
#include "tray.h"
#include "theme.h"
#include "logs.h"
#include "xray_process.h"
#include "settings.h"
#include "win32.h"

#include <shellapi.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cwchar>
#include <fstream>
#include <functional>
#include <iterator>

void TestStartup(const std::filesystem::path& root);

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
    if (first.filename() == L"paths.conf") {
        for (int index = 2; index + 1 < argc; index += 2) {
            const std::filesystem::path argument(argv[index + 1]);
            const auto source = std::wstring_view(argv[index]) == L"-confdir"
                ? argument / L"marker.txt" : argument;
            std::ifstream file(source, std::ios::binary);
            if (!file) {
                Write(error, "Cannot open path probe input: " + ToUtf8(source.native()) + "\n");
                return 95;
            }
            const std::string content(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>{});
            Write(output, "loaded[" + std::to_string(index / 2 - 1) + "]=" + content + "\n");
        }
        Write(output, "paths ready\n");
        Sleep(INFINITE);
        return 0;
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
    if (first.filename() == L"stream.conf") {
        for (unsigned index = 0;; ++index) {
            Write(output, "stream " + std::to_string(index) + "\n");
            Sleep(20);
        }
    }
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

struct TrayCall {
    DWORD operation;
    NOTIFYICONDATAW data;
};
std::vector<TrayCall> trayCalls;
DWORD failedTrayOperation = MAXDWORD;

BOOL WINAPI TestNotifyIcon(DWORD operation, PNOTIFYICONDATAW data)
{
    trayCalls.push_back({operation, *data});
    return operation != failedTrayOperation;
}

void TestTray()
{
    trayCalls.clear();
    failedTrayOperation = MAXDWORD;
    const auto window = reinterpret_cast<HWND>(static_cast<INT_PTR>(1));
    const auto red = reinterpret_cast<HICON>(static_cast<INT_PTR>(2));
    const auto green = reinterpret_cast<HICON>(static_cast<INT_PTR>(3));
    TrayIcon tray(TestNotifyIcon);
    Check(tray.Update(window, red, L"XTray — Stopped") && tray.IsAdded(),
        "A tray icon could not be registered.");
    Check(trayCalls.size() == 2 && trayCalls[0].operation == NIM_ADD &&
        trayCalls[1].operation == NIM_SETVERSION &&
        trayCalls[1].data.uVersion == NOTIFYICON_VERSION_4 &&
        trayCalls[0].data.uCallbackMessage == kTrayCallbackMessage &&
        trayCalls[0].data.uID == kTrayIconId &&
        (trayCalls[0].data.uFlags & NIF_SHOWTIP) != 0,
        "The tray callback, tooltip, or version contract is incorrect.");
    Check(tray.Update(window, green, L"XTray — Running") &&
        trayCalls.back().operation == NIM_MODIFY && trayCalls.back().data.hIcon == green &&
        std::wstring_view(trayCalls.back().data.szTip) == L"XTray — Running",
        "Updating tray state added a duplicate icon or kept stale content.");
    tray.Focus();
    Check(trayCalls.back().operation == NIM_SETFOCUS, "Tray focus was not restored.");
    tray.Lost();
    const auto beforeRecovery = trayCalls.size();
    Check(tray.Update(window, green, std::wstring(300, L'x')) &&
        trayCalls[beforeRecovery].operation == NIM_ADD &&
        std::wcslen(trayCalls[beforeRecovery].data.szTip) == 127,
        "Explorer recovery or tooltip truncation failed.");
    failedTrayOperation = NIM_MODIFY;
    const auto beforeFailure = trayCalls.size();
    Check(tray.Update(window, red, L"已停止") &&
        trayCalls[beforeFailure].operation == NIM_MODIFY &&
        trayCalls[beforeFailure + 1].operation == NIM_DELETE &&
        trayCalls[beforeFailure + 2].operation == NIM_ADD,
        "A failed tray modification was not recovered.");
    tray.Remove();
    failedTrayOperation = NIM_ADD;
    Check(!tray.Update(window, red, L"Stopped") && !tray.IsAdded(),
        "A failed tray registration was reported as available.");
    failedTrayOperation = NIM_SETVERSION;
    Check(!tray.Update(window, red, L"Stopped") && !tray.IsAdded() &&
        trayCalls.back().operation == NIM_DELETE,
        "An icon with an unusable callback version was retained.");
    failedTrayOperation = MAXDWORD;
    Check(tray.Update(window, red, L"Stopped"), "Tray recovery failed after a temporary outage.");
    tray.Remove();
    const auto removed = trayCalls.size();
    tray.Remove();
    Check(trayCalls.size() == removed && trayCalls.back().operation == NIM_DELETE,
        "Removing the tray icon is not idempotent.");
}

struct InstanceChild {
    Handle process;
    ~InstanceChild()
    {
        if (process && WaitForSingleObject(process.Get(), 0) != WAIT_OBJECT_0) {
            TerminateProcess(process.Get(), 123);
            WaitForSingleObject(process.Get(), 5000);
        }
    }
};

InstanceChild StartInstanceChild(const wchar_t* mode, const std::filesystem::path& root)
{
    const auto executable = ExecutablePath();
    std::wstring command = QuoteArgument(executable.native()) + L" " + mode + L" " +
        QuoteArgument(root.native());
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION information{};
    Check(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, root.c_str(), &startup, &information) != FALSE,
        "Cannot start the directory-instance test child.");
    Handle thread(information.hThread);
    return InstanceChild{Handle(information.hProcess)};
}

DWORD ProbeInstance(const std::filesystem::path& root)
{
    auto child = StartInstanceChild(L"--instance-probe", root);
    Check(WaitForSingleObject(child.process.Get(), 5000) == WAIT_OBJECT_0,
        "Directory-instance probe did not exit.");
    DWORD code = MAXDWORD;
    Check(GetExitCodeProcess(child.process.Get(), &code) != FALSE,
        "Cannot read the directory-instance probe result.");
    return code;
}

LRESULT CALLBACK InstanceTestWindow(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == kShowInstanceMessage) {
        auto* received = reinterpret_cast<bool*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (received != nullptr) {
            *received = true;
        }
        return kInstanceShown;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void TestInstances(const std::filesystem::path& root)
{
    const auto first = root / L"one 中文";
    const auto other = root / L"two";
    std::filesystem::create_directories(first);
    std::filesystem::create_directories(other);
    {
        DirectoryInstance primary(first);
        Check(primary.IsPrimary(), "The first instance could not reserve its directory.");
        DirectoryInstance repeated(first / L".");
        std::wstring upper = first.native();
        CharUpperBuffW(upper.data(), static_cast<DWORD>(upper.size()));
        DirectoryInstance alternate(upper);
        Check(!repeated.IsPrimary() && !alternate.IsPrimary() &&
            repeated.WindowClass() == primary.WindowClass() &&
            alternate.WindowClass() == primary.WindowClass(),
            "Equivalent paths created independent directory instances.");
        DirectoryInstance independent(other);
        Check(independent.IsPrimary() && independent.WindowClass() != primary.WindowClass(),
            "Independent directories share an instance lock.");
        Check(ProbeInstance(first) == 17 && ProbeInstance(other) == 17,
            "A second process bypassed an occupied directory.");
        const HINSTANCE module = GetModuleHandleW(nullptr);
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.hInstance = module;
        windowClass.lpfnWndProc = InstanceTestWindow;
        windowClass.lpszClassName = primary.WindowClass().c_str();
        Check(RegisterClassExW(&windowClass) != 0, "Cannot register the instance test window.");
        struct WindowLifetime {
            HWND window;
            HINSTANCE module;
            const wchar_t* name;
            ~WindowLifetime()
            {
                if (window != nullptr) {
                    DestroyWindow(window);
                }
                UnregisterClassW(name, module);
            }
        } lifetime{CreateWindowExW(0, windowClass.lpszClassName, L"Instance test",
            WS_OVERLAPPED, 0, 0, 10, 10, nullptr, nullptr, module, nullptr),
            module, windowClass.lpszClassName};
        Check(lifetime.window != nullptr, "Cannot create the instance test window.");
        bool received = false;
        SetWindowLongPtrW(lifetime.window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&received));
        Check(repeated.ActivateExisting(0) && received,
            "A duplicate did not activate the verified current-user window.");
        received = false;
        auto activation = StartInstanceChild(L"--instance-activate", first);
        Check(WaitUntil([&] {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            return WaitForSingleObject(activation.process.Get(), 0) == WAIT_OBJECT_0;
        }), "The duplicate activation process did not finish.");
        DWORD activated = MAXDWORD;
        Check(GetExitCodeProcess(activation.process.Get(), &activated) != FALSE &&
            activated == 18 && received, "Cross-process window activation failed.");
    }
    Check(ProbeInstance(first) == 0 && ProbeInstance(other) == 0,
        "Exiting an instance left its directory reserved.");
    const auto crash = root / L"crash";
    std::filesystem::create_directories(crash);
    auto child = StartInstanceChild(L"--instance-owner", crash);
    Check(WaitUntil([&] { return std::filesystem::exists(crash / L"owner.ready"); }),
        "The instance owner did not become ready.");
    {
        DirectoryInstance occupied(crash);
        Check(!occupied.IsPrimary(), "The live owner did not reserve its directory.");
    }
    Check(TerminateProcess(child.process.Get(), 123) != FALSE &&
        WaitForSingleObject(child.process.Get(), 5000) == WAIT_OBJECT_0,
        "Cannot terminate the instance test owner.");
    DirectoryInstance recovered(crash);
    Check(recovered.IsPrimary(), "Abnormal exit left a stale directory lock.");
    Check(!std::filesystem::exists(first / L"data"),
        "Runtime instance detection unexpectedly created application data files.");
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

void TestLanguage()
{
    Check(ResolveLanguage(Language::Automatic, MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED))
        == Language::Chinese, "Simplified Chinese UI was not recognized.");
    Check(ResolveLanguage(Language::Automatic, MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL))
        == Language::Chinese, "Traditional Chinese UI should use the Chinese translation.");
    Check(ResolveLanguage(Language::Automatic, MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US))
        == Language::English, "English UI was not recognized.");
    Check(ResolveLanguage(Language::Automatic, MAKELANGID(LANG_FRENCH, SUBLANG_FRENCH))
        == Language::English && ResolveLanguage(Language::Automatic, 0) == Language::English,
        "Other or unavailable UI languages should fall back to English.");
    Check(ResolveLanguage(Language::English, MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED))
        == Language::English && ResolveLanguage(Language::Chinese, 0) == Language::Chinese,
        "An explicit language did not override automatic detection.");
    ApplyLanguage(Language::Automatic);
    Check(CurrentLanguage() == ResolveLanguage(Language::Automatic, GetUserDefaultUILanguage()),
        "Automatic language did not use the current user's UI language.");
    ApplyLanguage(Language::English);
    Check(std::wstring_view(Text(L"Run", L"运行")) == L"Run" &&
        std::string_view(Text("Run", "运行")) == "Run", "English translation selection failed.");
    ApplyLanguage(Language::Chinese);
    Check(std::wstring_view(Text(L"Run", L"运行")) == L"运行", "Chinese translation selection failed.");
}

void TestLogClearing()
{
    Logs logs(2);
    logs.Tray(L"keep tray");
    logs.Xray("old output\npartial \xE4\xB8");
    const auto snapshot = logs.Snapshot();
    logs.Clear(LogSource::Xray);
    Check(logs.Snapshot().lines.size() == 1 && HasText(logs, LogSource::Tray, L"keep tray"),
        "Clearing Xray also cleared XTray.");
    Check(snapshot.lines.size() == 3 && snapshot.lines.back()->raw.ends_with("\xE4\xB8"),
        "Clearing changed a retained snapshot.");
    logs.Xray("\xAD after clear\n");
    Check(HasText(logs, LogSource::Xray, L"中 after clear") &&
        !HasText(logs, LogSource::Xray, L"old output") &&
        !HasText(logs, LogSource::Xray, L"\uFFFD"),
        "Clearing interrupted UTF-8 decoding or restored old output.");
    logs.Clear(LogSource::Tray);
    Check(logs.Snapshot().lines.size() == 1 &&
        HasText(logs, LogSource::Xray, L"after clear"), "Clearing XTray also cleared Xray.");
    logs.Clear();
    Check(logs.Snapshot().lines.empty(), "Clearing all retained logs failed.");
    logs.Tray(L"new management");
    logs.Xray("new one\nnew two\nnew three\n");
    Check(logs.Snapshot().lines.size() == 3 && HasText(logs, LogSource::Tray, L"new management") &&
        !HasText(logs, LogSource::Xray, L"new one"), "Clearing stopped capture or lost the line limit.");
    logs.Xray("\xE4\xB8");
    logs.ClearXray();
    logs.Xray("new run\n");
    Check(HasText(logs, LogSource::Xray, L"new run") &&
        !HasText(logs, LogSource::Xray, L"\uFFFD"), "A new run reused the previous UTF-8 decoder state.");
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

void TestThemeResolution()
{
    Check(!ResolveDark(ThemeMode::System, false) && ResolveDark(ThemeMode::System, true),
        "System mode does not follow the supplied Windows color mode.");
    for (const bool systemDark : {false, true}) {
        Check(!ResolveDark(ThemeMode::Light, systemDark) && ResolveDark(ThemeMode::Dark, systemDark),
            "An explicit appearance did not override Windows.");
    }
    Check(!IsLightColor(0, 0, 0) && IsLightColor(255, 255, 255) &&
        !IsLightColor(64, 64, 64) && IsLightColor(220, 220, 220),
        "Windows foreground brightness classification is incorrect.");
    const auto light = PaletteFor(false);
    const auto dark = PaletteFor(true);
    Check(light.logBackground == RGB(255, 255, 255) && light.trayLog == RGB(180, 83, 9) &&
        light.xrayLog == RGB(32, 32, 32) && dark.logBackground == RGB(30, 30, 30) &&
        dark.trayLog == RGB(253, 186, 116) && dark.xrayLog == RGB(230, 230, 230),
        "The agreed light/dark log colors changed.");
}

void TestSettings(const std::filesystem::path& root)
{
    std::filesystem::create_directories(root);
    Check(LoadSettings(root).success && LoadSettings(root).settings.language == Language::Automatic &&
        LoadSettings(root).settings.theme == ThemeMode::System,
        "Missing settings should use automatic language, system appearance, and defaults.");
    Settings settings;
    settings.executable = L"..\\bin\\xray.exe";
    settings.configFiles = {L".\\配置;=一.json", L"C:\\文件 with spaces\\二.json", L".\\配置;=一.json"};
    settings.configDirectory = L"..\\config directory\\";
    settings.logLines = 17;
    settings.language = Language::Chinese;
    settings.theme = ThemeMode::Dark;
    std::wstring error;
    Check(SaveSettings(root, settings, error), "Saving initial settings failed.");
    auto loaded = LoadSettings(root);
    Check(loaded.success && loaded.settings.executable == settings.executable &&
        loaded.settings.configFiles == settings.configFiles &&
        loaded.settings.configDirectory == settings.configDirectory &&
        loaded.settings.logLines == settings.logLines &&
        loaded.settings.language == settings.language && loaded.settings.theme == settings.theme,
        "Settings round-trip failed.");
    const auto path = root / L"data" / L"settings.ini";
    std::string bytes;
    {
        std::ifstream file(path, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
    Check(!bytes.starts_with("\xEF\xBB\xBF") && bytes.find("\r\n") != std::string::npos,
        "Settings must be UTF-8 without BOM and use CRLF.");
    for (const auto theme : {ThemeMode::System, ThemeMode::Light, ThemeMode::Dark}) {
        auto changed = settings;
        changed.theme = theme;
        Check(SaveSettings(root, changed, error) && LoadSettings(root).success &&
            LoadSettings(root).settings.theme == theme, "Appearance preference did not round-trip.");
    }
    const auto themeLine = bytes.find("Theme=dark\r\n");
    Check(themeLine != std::string::npos, "The appearance key was not saved.");
    auto withoutTheme = bytes;
    withoutTheme.erase(themeLine, std::string_view("Theme=dark\r\n").size());
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << withoutTheme;
    }
    Check(LoadSettings(root).success && LoadSettings(root).settings.theme == ThemeMode::System &&
        LoadSettings(root).settings.configFiles == settings.configFiles,
        "An existing INI without Theme should use system mode and preserve paths.");
    const auto oldLanguage = withoutTheme.find("Language=zh\r\n");
    withoutTheme.erase(oldLanguage, std::string_view("Language=zh\r\n").size());
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << withoutTheme;
    }
    Check(LoadSettings(root).success && LoadSettings(root).settings.theme == ThemeMode::System &&
        LoadSettings(root).settings.language == Language::Automatic,
        "Older settings without both optional UI fields were rejected.");
    auto invalidTheme = bytes;
    invalidTheme.replace(themeLine, std::string_view("Theme=dark").size(), "Theme=invalid");
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << invalidTheme;
    }
    Check(!LoadSettings(root).success, "An invalid appearance preference was accepted.");
    {
        std::ifstream file(path, std::ios::binary);
        std::string preserved(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>{});
        Check(preserved == invalidTheme, "An invalid appearance file was silently modified.");
    }
    Check(SaveSettings(root, settings, error), "Cannot restore settings after appearance tests.");
    auto invalid = settings;
    invalid.theme = static_cast<ThemeMode>(99);
    Check(!SaveSettings(root, invalid, error) && LoadSettings(root).settings.theme == ThemeMode::Dark,
        "An invalid appearance enum changed saved settings.");

    for (const auto language : {Language::Automatic, Language::English, Language::Chinese}) {
        auto changed = settings;
        changed.language = language;
        Check(SaveSettings(root, changed, error), "Saving a language preference failed.");
        Check(LoadSettings(root).success && LoadSettings(root).settings.language == language,
            "The saved language preference changed after loading.");
    }
    auto legacyBytes = bytes;
    const auto languageLine = legacyBytes.find("Language=zh\r\n");
    Check(languageLine != std::string::npos, "The language key was not saved.");
    legacyBytes.erase(languageLine, std::string_view("Language=zh\r\n").size());
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << legacyBytes;
    }
    Check(LoadSettings(root).success && LoadSettings(root).settings.language == Language::Automatic,
        "An existing INI without Language should remain valid and use automatic language.");
    {
        auto invalidLanguage = bytes;
        invalidLanguage.replace(languageLine, std::string_view("Language=zh").size(), "Language=invalid");
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << invalidLanguage;
    }
    Check(!LoadSettings(root).success, "An invalid language preference was accepted.");
    Check(SaveSettings(root, settings, error), "Cannot restore settings after language tests.");
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

void TestConfigurationPaths(const std::filesystem::path& root)
{
    const auto base = root / L"configuration path cases";
    const auto original = base / L"original root";
    const auto moved = base / L"tail moved root";
    const auto absoluteFile = base / L"absolute 文件.json";
    const auto absoluteDirectory = base / L"absolute 目录";
    std::filesystem::create_directories(original / L"configs");
    std::filesystem::create_directories(original / L"fragments 片段");
    std::filesystem::create_directories(base / L"shared 共享");
    std::filesystem::create_directories(absoluteDirectory);
    const auto write = [](const std::filesystem::path& path, const char* marker) {
        std::ofstream file(path, std::ios::binary);
        Check(static_cast<bool>(file), "Cannot create a configuration path fixture.");
        file << marker;
        Check(static_cast<bool>(file), "Cannot write a configuration path fixture.");
    };
    write(original / L"paths.conf", "relative-first");
    write(original / L"configs" / L"配置 ;=& one.json", "relative-unicode");
    write(base / L"shared 共享" / L"outside.json", "outside-relative");
    write(absoluteFile, "absolute-file");
    write(original / L"fragments 片段" / L"marker.txt", "relative-directory");
    write(absoluteDirectory / L"marker.txt", "absolute-directory");
    Settings settings;
    settings.executable = ExecutablePath().native();
    settings.configFiles = {L".\\paths.conf", L".\\configs\\..\\configs\\配置 ;=& one.json",
        L"..\\shared 共享\\outside.json", absoluteFile.native(), L".\\configs\\..\\configs\\配置 ;=& one.json"};
    settings.configDirectory = L".\\fragments 片段\\";
    std::wstring error;
    Check(SaveSettings(original, settings, error), "Cannot save original path settings.");
    const auto verify = [&](XrayProcess& process, const Logs& logs,
        const std::filesystem::path& directory, const Settings& expected,
        std::wstring_view directoryMarker, DWORD previousPid) {
        Check(WaitUntil([&] { return process.State() == ProcessState::Running &&
            process.Pid() != previousPid && HasText(logs, LogSource::Xray, L"paths ready"); }),
            "Configuration inputs could not be read from the explicit child working directory.");
        std::vector<std::wstring> arguments, loaded;
        for (const auto& line : logs.Snapshot().lines) {
            if (line->source != LogSource::Xray) {
                continue;
            }
            std::wstring_view text(line->text);
            // Log lines retain their CR/LF; compare the argument without its output framing.
            while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n')) {
                text.remove_suffix(1);
            }
            if (text.starts_with(L"-config=") || text.starts_with(L"-confdir=")) {
                arguments.emplace_back(text);
            } else if (text.starts_with(L"loaded[")) {
                loaded.emplace_back(text);
            }
        }
        std::vector<std::wstring> expectedArguments;
        for (const auto& file : expected.configFiles) {
            expectedArguments.push_back(L"-config=" + file);
        }
        std::vector<std::wstring> expectedContents{L"loaded[0]=relative-first", L"loaded[1]=relative-unicode",
            L"loaded[2]=outside-relative", L"loaded[3]=absolute-file", L"loaded[4]=relative-unicode"};
        if (!expected.configDirectory.empty()) {
            expectedArguments.push_back(L"-confdir=" + expected.configDirectory);
            expectedContents.push_back(L"loaded[5]=" + std::wstring(directoryMarker));
        }
        Check(arguments == expectedArguments, "Configuration path spelling, order, or duplicates changed.");
        Check(loaded == expectedContents, "Relative or absolute configuration paths selected the wrong files.");
        Check(HasText(logs, LogSource::Xray, L"cwd=" + directory.native()),
            "Configuration inputs used the wrong child working directory.");
        Check(HasText(logs, LogSource::Xray, L"console=0"), "The configuration path child acquired a console.");
    };
    {
        const auto loaded = LoadSettings(original);
        Check(loaded.success, "Cannot load original path settings.");
        Logs logs;
        XrayProcess process(original, logs);
        Check(process.Run(loaded.settings), "Original configuration path run was rejected.");
        verify(process, logs, original, settings, L"relative-directory", 0);
    }
    std::filesystem::rename(original, moved);
    const auto reloaded = LoadSettings(moved);
    Check(reloaded.success && reloaded.settings.configFiles == settings.configFiles &&
        reloaded.settings.configDirectory == settings.configDirectory,
        "Moving the portable directory changed the saved configuration paths.");
    Logs logs;
    XrayProcess process(moved, logs);
    Check(process.Run(reloaded.settings), "Moved configuration path run was rejected.");
    verify(process, logs, moved, settings, L"relative-directory", 0);
    auto absolute = reloaded.settings;
    absolute.configDirectory = absoluteDirectory.native();
    const DWORD relativePid = process.Pid();
    Check(process.Restart(absolute), "Absolute configuration directory restart was rejected.");
    verify(process, logs, moved, absolute, L"absolute-directory", relativePid);
    absolute.configDirectory.clear();
    const DWORD directoryPid = process.Pid();
    Check(process.Restart(absolute), "Files-only configuration restart was rejected.");
    verify(process, logs, moved, absolute, L"", directoryPid);
    auto missing = absolute;
    missing.configFiles[1] = L".\\missing 配置.json";
    Check(process.Restart(missing), "Missing configuration path restart was rejected.");
    Check(WaitUntil([&] { return process.State() == ProcessState::Stopped &&
        HasText(logs, LogSource::Xray, L"Cannot open path probe input: " + missing.configFiles[1]) &&
        HasText(logs, LogSource::Tray, L"退出码 95"); }),
        "A missing relative configuration path did not preserve the child error and exit code.");
}

void TestProcesses(const std::filesystem::path& root)
{
    Logs logs(64);
    XrayProcess process(root, logs);
    process.Run(Settings{});
    Check(WaitUntil([&] { return process.State() == ProcessState::Stopped; }) &&
        HasText(logs, LogSource::Tray, L"至少设置") && process.Pid() == 0,
        "Empty configuration sources did not fail before launch.");

    for (int retry = 0; retry < 20; ++retry) {
        Check(process.Run(Settings{}), "A completed operation still rejects the next command.");
        Check(WaitUntil([&] { return process.State() == ProcessState::Stopped; }),
            "Repeated failed run did not finish.");
    }

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
    const std::wstring alternateDrive = root.root_name().native() == L"Z:" ? L"Y:" : L"Z:";
    settings.configFiles.push_back(alternateDrive + L"\\external configs\\cross-drive.json");
    Check(process.Run(settings), "Run request was rejected.");
    Check(WaitUntil([&] { return HasText(logs, LogSource::Xray, L"partial without newline"); }),
        "Live partial stdout was not received.");
    Check(process.State() == ProcessState::Running && process.Pid() != 0,
        "Running status is incorrect.");
    Check(HasText(logs, LogSource::Xray, L"console=0"), "The child acquired a console.");
    Check(HasText(logs, LogSource::Xray, L"cwd=" + root.native()), "Child working directory is incorrect.");
    Check(HasText(logs, LogSource::Xray, L"-config=" + settings.configFiles[0]) &&
        HasText(logs, LogSource::Xray, L"-config=" + settings.configFiles[1]) &&
        HasText(logs, LogSource::Xray, L"-config=" + settings.configFiles[2]),
        "Absolute, Unicode relative, or cross-drive configuration arguments changed.");
    Check(HasText(logs, LogSource::Xray, L"-confdir=" + settings.configDirectory),
        "The relative directory argument changed.");
    Check(HasText(logs, LogSource::Xray, L"中文 stdout") &&
        HasText(logs, LogSource::Xray, L"stderr captured"), "stdout/stderr capture failed.");
    Check(!process.Run(settings), "A second run was allowed while Xray was running.");
    Check(!HasText(logs, LogSource::Xray, L"fake failure"), "Previous Xray output survived the next run.");

    Logs otherLogs;
    XrayProcess other(root, otherLogs);
    auto directoryOnly = settings;
    directoryOnly.configFiles.clear();
    Check(other.Run(directoryOnly), "Directory-only configuration was rejected.");
    Check(WaitUntil([&] { return other.State() == ProcessState::Running &&
        HasText(otherLogs, LogSource::Xray, L"-confdir=" + directoryOnly.configDirectory); }),
        "Directory-only startup did not preserve its relative path.");

    const DWORD oldPid = process.Pid();
    Handle old(OpenProcess(SYNCHRONIZE, FALSE, oldPid));
    logs.Tray(L"retained tray marker");
    Check(process.Restart(settings), "Restart request was rejected.");
    Check(WaitUntil([&] { return process.State() == ProcessState::Running &&
        process.Pid() != oldPid; }), "Restart did not create a new process.");
    Check(old && WaitForSingleObject(old.Get(), 0) == WAIT_OBJECT_0,
        "Restart created a replacement before the old process exited.");
    Check(HasText(logs, LogSource::Tray, L"retained tray marker"), "Restart removed XTray logs.");
    Check(WaitUntil([&] { return HasText(logs, LogSource::Xray, L"中文 stdout"); }),
        "The restarted child did not produce output before the language switch.");
    ApplyLanguage(Language::English);
    Check(WaitUntil([&] { return process.Stop(); }), "Stop request was rejected.");
    Check(WaitUntil([&] { return process.State() == ProcessState::Stopped; }),
        "Stop did not finish.");
    Check(HasText(logs, LogSource::Tray, L"Stopping Xray.") &&
        HasText(logs, LogSource::Tray, L"Xray 已启动") &&
        HasText(logs, LogSource::Xray, L"中文 stdout"),
        "Language switching rewrote old logs, translated Xray output, or left new logs untranslated.");
    ApplyLanguage(Language::Chinese);
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

void TestRuntimeFailure()
{
    std::fputs("Core: window callback failure reports its reason and cleans up the child.\n", stdout);
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE rawRead = nullptr, rawWrite = nullptr;
    Check(CreatePipe(&rawRead, &rawWrite, &security, 0) != FALSE,
        "Cannot create the GUI failure test output pipe.");
    Handle output(rawRead), writer(rawWrite);
    Check(SetHandleInformation(output.Get(), HANDLE_FLAG_INHERIT, 0) != FALSE,
        "Cannot configure the GUI failure test output pipe.");
    Handle nullInput(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    Check(static_cast<bool>(nullInput), "Cannot prepare GUI failure test standard input.");
    const auto executable = ExecutablePath().parent_path() / L"XTray.exe";
    auto command = QuoteArgument(executable.native()) + L" --smoke-test-failure";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = nullInput.Get();
    startup.hStdOutput = startup.hStdError = writer.Get();
    PROCESS_INFORMATION information{};
    Check(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, executable.parent_path().c_str(), &startup, &information) != FALSE,
        "Cannot start the controlled GUI failure test.");
    Handle thread(information.hThread);
    InstanceChild owner{Handle(information.hProcess)};
    writer.Reset();
    Check(WaitForSingleObject(owner.process.Get(), 10000) == WAIT_OBJECT_0,
        "The GUI exception blocked instead of returning a diagnostic exit code.");
    DWORD code = 0;
    Check(GetExitCodeProcess(owner.process.Get(), &code) != FALSE && code == 1,
        "The GUI callback exception did not fail its process.");
    std::string captured;
    std::array<char, 4096> buffer{};
    DWORD count = 0;
    while (ReadFile(output.Get(), buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr) && count != 0) {
        captured.append(buffer.data(), count);
    }
    const std::string marker = "Smoke: runtime failure child PID ";
    const auto position = captured.find(marker);
    Check(position != std::string::npos &&
        captured.find("XTray runtime failure: Injected window callback failure.") != std::string::npos,
        "The GUI runtime error lost the original reason or did not exercise a managed child.");
    const auto childPid = std::stoul(captured.substr(position + marker.size()));
    Check(childPid != 0, "The GUI runtime failure did not identify its managed child.");
    Handle child(OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(childPid)));
    const DWORD openError = GetLastError();
    Check(child ? WaitForSingleObject(child.Get(), 5000) == WAIT_OBJECT_0 : openError == ERROR_INVALID_PARAMETER,
        "The managed child survived its GUI owner's runtime failure.");
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
        if (argc == 3 && (std::wstring_view(argv[1]) == L"--instance-probe" ||
            std::wstring_view(argv[1]) == L"--instance-owner" ||
            std::wstring_view(argv[1]) == L"--instance-activate")) {
            DirectoryInstance instance(argv[2]);
            if (!instance.IsPrimary()) {
                return std::wstring_view(argv[1]) == L"--instance-activate"
                    ? (instance.ActivateExisting() ? 18 : 19) : 17;
            }
            if (std::wstring_view(argv[1]) == L"--instance-owner") {
                std::ofstream ready(std::filesystem::path(argv[2]) / L"owner.ready");
                ready << GetCurrentProcessId();
                ready.close();
                Sleep(INFINITE);
            }
            return 0;
        }
        if (argc == 3 && std::wstring_view(argv[1]) == L"--test-owner") {
            return JobOwner(argv[2]);
        }
        const auto root = ExecutablePath().parent_path() / L"test data 中文" /
            (std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        std::filesystem::create_directories(root);
        TestLanguage();
        TestThemeResolution();
        TestTray();
        TestInstances(root / L"instances");
        TestUtf8AndLogs();
        TestLogClearing();
        TestArguments();
        TestSettings(root / L"settings");
        TestStartup(root / L"startup");
        TestConfigurationPaths(root);
        TestProcesses(root);
        TestAbnormalExit(root);
        TestRuntimeFailure();
        std::puts("All core tests passed. No real Xray process or network listener was started.");
        return 0;
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "Test failed: %s\n", exception.what());
        return 1;
    }
}
