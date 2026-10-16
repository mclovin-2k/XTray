#include "startup.h"
#include "win32.h"

#include <fstream>
#include <iterator>
#include <map>

namespace {

using namespace xtray;

void Check(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class MemoryRegistry final : public StartupRegistry {
public:
    bool Read(const std::wstring& name, std::optional<std::wstring>& command,
        std::wstring& error) override
    {
        ++reads;
        if (failRead) {
            error = L"Fake registry read failure.";
            return false;
        }
        const auto found = entries.find(name);
        command = found == entries.end() ? std::optional<std::wstring>{} : found->second;
        return true;
    }

    bool Write(const std::wstring& name, const std::optional<std::wstring>& command,
        std::wstring& error) override
    {
        ++writes;
        if (writes == failWrite) {
            error = L"Fake registry write failure.";
            return false;
        }
        if (command) {
            entries[name] = *command;
        } else {
            entries.erase(name);
        }
        return true;
    }

    std::map<std::wstring, std::wstring> entries;
    size_t reads = 0;
    size_t writes = 0;
    size_t failWrite = 0;
    bool failRead = false;
};

std::string Bytes(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    Check(static_cast<bool>(file), "Cannot read startup test fixture.");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void Put(const std::filesystem::path& path, const std::string& bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << bytes;
    file.close();
    Check(static_cast<bool>(file), "Cannot write startup test fixture.");
}

std::filesystem::path Prepare(const std::filesystem::path& root)
{
    const auto executable = root / L"XTray 测试.exe";
    Put(executable, "This is a test fixture, never executed.");
    return executable;
}

std::filesystem::path Metadata(const std::filesystem::path& root)
{
    return root / L"data" / L"startup.ini";
}

std::filesystem::path SettingsPath(const std::filesystem::path& root)
{
    return root / L"data" / L"settings.ini";
}

void TestRegistration(const std::filesystem::path& root)
{
    MemoryRegistry registry;
    const auto executable = Prepare(root);
    StartupRegistration startup(root, executable, &registry);
    const auto status = startup.Query();
    Check(status.success && !status.enabled && registry.reads == 0 && registry.writes == 0 &&
        !std::filesystem::exists(root / L"data"), "Query created identity data or a startup entry.");
    std::wstring error;
    Settings settings;
    Check(startup.Save(settings, false, error) && !std::filesystem::exists(Metadata(root)) &&
        registry.writes == 0, "Saving disabled startup allocated an identity.");
    Check(startup.Save(settings, true, error) && startup.Query().enabled &&
        registry.entries.size() == 1, "Cannot enable startup.");
    const auto name = registry.entries.begin()->first;
    Check(name.starts_with(L"XTray-{") && name.size() == 44 &&
        registry.entries.begin()->second == QuoteArgument(executable.native()) + L" --logon",
        "Startup name or quoted Unicode path is incorrect.");
    const auto identity = Bytes(Metadata(root));
    Check(identity.starts_with("[XTray]\r\nVersion=1\r\nId={") &&
        identity.find("\n") == identity.find("\r\n") + 1 &&
        !identity.starts_with("\xEF\xBB\xBF"), "Startup identity encoding is incorrect.");
    StartupRegistration reopened(root, executable, &registry);
    Check(reopened.Query().success && reopened.Query().enabled, "Startup state did not persist.");
    registry.entries.erase(name);
    Check(!reopened.Query().enabled, "Startup state used stale cached settings.");
    Check(startup.Save(settings, true, error) && registry.entries.contains(name),
        "Cannot reenable a missing current startup entry.");
    const auto writes = registry.writes;
    Check(startup.Save(settings, true, error) && registry.writes == writes,
        "An unchanged startup entry was needlessly rewritten.");
    Check(startup.Save(settings, false, error) && !startup.Query().enabled &&
        registry.entries.empty() && Bytes(Metadata(root)) == identity,
        "Disabling startup did not remove only its own entry.");
}

void TestMoveAndCopy(const std::filesystem::path& root)
{
    MemoryRegistry registry;
    const auto original = root / L"original";
    const auto originalExecutable = Prepare(original);
    StartupRegistration startup(original, originalExecutable, &registry);
    Settings settings;
    std::wstring error;
    Check(startup.Save(settings, true, error), "Cannot prepare startup move.");
    const auto originalName = registry.entries.begin()->first;
    const auto originalCommand = registry.entries.begin()->second;

    const auto copy = root / L"copied";
    std::filesystem::copy(original, copy, std::filesystem::copy_options::recursive);
    StartupRegistration copied(copy, copy / originalExecutable.filename(), &registry);
    Check(copied.Query().success && !copied.Query().enabled,
        "A copied directory incorrectly inherited enabled startup.");
    Check(copied.Save(settings, true, error) && copied.Query().enabled &&
        registry.entries.size() == 2 && registry.entries.at(originalName) == originalCommand,
        "A copied directory overwrote the original startup entry.");
    Check(startup.Query().enabled, "Copy registration disabled original startup.");

    const auto moved = root / L"moved";
    std::filesystem::rename(original, moved);
    StartupRegistration relocated(moved, moved / originalExecutable.filename(), &registry);
    const auto oldBytes = Bytes(Metadata(moved));
    Check(relocated.Query().success && !relocated.Query().enabled &&
        Bytes(Metadata(moved)) == oldBytes && registry.entries.at(originalName) == originalCommand,
        "Move query wrote startup data or retained enabled status.");
    Check(relocated.Save(settings, false, error) &&
        registry.entries.at(originalName) == originalCommand && Bytes(Metadata(moved)) == oldBytes,
        "Saving disabled startup after a move deleted the old entry.");
    Check(relocated.Save(settings, true, error) && relocated.Query().enabled &&
        registry.entries.size() == 2 &&
        registry.entries.at(originalName) == QuoteArgument((moved / originalExecutable.filename()).native()) + L" --logon",
        "Explicit startup after a move did not reuse the original identity.");
    Check(copied.Query().enabled, "Updating a moved directory changed a different startup entry.");
    Check(relocated.Save(settings, false, error) && registry.entries.size() == 1 && copied.Query().enabled,
        "Disabling a moved directory removed another directory's startup entry.");
}

void TestUntrustedIdentity(const std::filesystem::path& root)
{
    MemoryRegistry registry;
    const auto executable = Prepare(root);
    StartupRegistration startup(root, executable, &registry);
    std::wstring error;
    Settings settings;
    const std::string damaged = "[XTray]\r\nVersion=1\r\nId=broken\r\nExecutable=C:\\XTray.exe\r\n";
    Put(Metadata(root), damaged);
    Check(!startup.Query().success && !startup.Save(settings, true, error) &&
        !error.empty() && registry.entries.empty() && registry.writes == 0 &&
        Bytes(Metadata(root)) == damaged, "Corrupt startup identity was changed.");
    std::filesystem::remove(Metadata(root));
    Check(startup.Save(settings, true, error), "Cannot prepare untrusted command.");
    const auto identity = Bytes(Metadata(root));
    const auto name = registry.entries.begin()->first;
    registry.entries[name] = L"\"C:\\Other program\\other.exe\" --something-else";
    const auto unknown = registry.entries;
    Check(!startup.Query().success && !startup.Save(settings, true, error) &&
        !startup.Save(settings, false, error) && registry.entries == unknown &&
        Bytes(Metadata(root)) == identity, "An entry with an unknown command was changed.");
    registry.failRead = true;
    Check(!startup.Query().success && !startup.Save(settings, true, error) &&
        registry.entries == unknown, "Registry read failure was treated as a missing entry.");

    MemoryRegistry missing;
    const auto orphanRoot = root / L"missing-identity";
    StartupRegistration orphan(orphanRoot, Prepare(orphanRoot), &missing);
    missing.entries[L"XTray-{AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA}"] = L"\"C:\\gone.exe\" --logon";
    const auto oldEntries = missing.entries;
    Check(orphan.Query().success && !orphan.Query().enabled &&
        orphan.Save(settings, true, error) && missing.entries.size() == 2 &&
        missing.entries.contains(oldEntries.begin()->first),
        "A missing identity guessed ownership of another startup entry.");
}

void TestRollback(const std::filesystem::path& root)
{
    Settings settings;
    std::wstring error;
    MemoryRegistry failed;
    const auto failureRoot = root / L"registry failure";
    StartupRegistration startup(failureRoot, Prepare(failureRoot), &failed);
    Check(SaveSettings(failureRoot, settings, error), "Cannot prepare ordinary settings.");
    const auto before = Bytes(SettingsPath(failureRoot));
    failed.failWrite = 1;
    settings.logLines = 25;
    Check(!startup.Save(settings, true, error) && failed.entries.empty() &&
        !std::filesystem::exists(Metadata(failureRoot)) &&
        Bytes(SettingsPath(failureRoot)) == before, "Registry failure changed settings or identity.");

    MemoryRegistry registry;
    const auto lockRoot = root / L"locked settings";
    StartupRegistration locked(lockRoot, Prepare(lockRoot), &registry);
    Check(SaveSettings(lockRoot, settings, error), "Cannot prepare settings lock.");
    const auto originalSettings = Bytes(SettingsPath(lockRoot));
    Handle file(CreateFileW(SettingsPath(lockRoot).c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    Check(static_cast<bool>(file), "Cannot lock test settings.");
    settings.logLines = 30;
    Check(!locked.Save(settings, true, error) && registry.entries.empty() &&
        !std::filesystem::exists(Metadata(lockRoot)) &&
        Bytes(SettingsPath(lockRoot)) == originalSettings && registry.writes == 2,
        "A settings commit failure did not roll back new startup data.");
    file.Reset();

    Check(locked.Save(settings, true, error), "Cannot prepare existing identity rollback.");
    const auto oldIdentity = Bytes(Metadata(lockRoot));
    const auto oldEntries = registry.entries;
    Handle secondLock(CreateFileW(SettingsPath(lockRoot).c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    Check(static_cast<bool>(secondLock), "Cannot relock settings.");
    Check(!locked.Save(settings, false, error) && registry.entries == oldEntries &&
        Bytes(Metadata(lockRoot)) == oldIdentity && locked.Query().enabled,
        "A settings commit failure did not restore an existing startup entry.");
    registry.failWrite = registry.writes + 2;
    Check(!locked.Save(settings, false, error) &&
        error.find(L"Fake registry write failure.") != std::wstring::npos,
        "A rollback failure was silently reported as success.");

    MemoryRegistry movedRegistry;
    const auto oldRoot = root / L"move rollback old";
    const auto oldExecutable = Prepare(oldRoot);
    StartupRegistration beforeMove(oldRoot, oldExecutable, &movedRegistry);
    Check(beforeMove.Save(settings, true, error), "Cannot prepare move rollback.");
    const auto movedRoot = root / L"move rollback new";
    std::filesystem::rename(oldRoot, movedRoot);
    StartupRegistration afterMove(movedRoot, movedRoot / oldExecutable.filename(), &movedRegistry);
    const auto originalIdentity = Bytes(Metadata(movedRoot));
    const auto originalEntries = movedRegistry.entries;
    const auto settingsBytes = Bytes(SettingsPath(movedRoot));
    Handle movedLock(CreateFileW(SettingsPath(movedRoot).c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    Check(static_cast<bool>(movedLock), "Cannot lock settings after a move.");
    Check(!afterMove.Save(settings, true, error) && movedRegistry.entries == originalEntries &&
        Bytes(Metadata(movedRoot)) == originalIdentity &&
        Bytes(SettingsPath(movedRoot)) == settingsBytes && !afterMove.Query().enabled,
        "A failed move update did not restore exact identity bytes and its old startup command.");
}

void TestLongCommand(const std::filesystem::path& root)
{
    MemoryRegistry registry;
    std::wstring error;
    const std::filesystem::path executable = L"C:\\" + std::wstring(250, L'a') + L"\\XTray.exe";
    StartupRegistration longPath(root, executable, &registry);
    Check(!longPath.Save(Settings{}, true, error) && !error.empty() &&
        registry.writes == 0 && !std::filesystem::exists(Metadata(root)),
        "An overlength Run command was registered.");
}

} // namespace

void TestStartup(const std::filesystem::path& root)
{
    Check(IsLogonCommandLine(L"--logon") && IsLogonCommandLine(L" \t--logon\r\n") &&
        !IsLogonCommandLine(L"") && !IsLogonCommandLine(L"--LOGON") &&
        !IsLogonCommandLine(L"--logon extra") && !IsLogonCommandLine(L"\"--logon\""),
        "Login invocation did not strictly match its supported argument.");
    TestRegistration(root / L"registration with spaces 中文");
    TestMoveAndCopy(root / L"move and copy");
    TestUntrustedIdentity(root / L"untrusted");
    TestRollback(root / L"rollback");
    TestLongCommand(root / L"long command");
}
