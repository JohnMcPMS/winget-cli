// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "pch.h"
#include "TestCommon.h"
#include "TestHooks.h"
#include <AppInstallerErrors.h>
#include <PortableInstaller.h>
#include <Public/AppInstallerArchitecture.h>
#include <winget/Filesystem.h>
#include <winget/Manifest.h>
#include <winget/PathVariable.h>
#include <winget/PortableARPEntry.h>
#include <winget/SQLiteStorageBase.h>
#include <Microsoft/Schema/IPortableIndex.h>
#include <winget/PortableIndex.h>

using namespace std::string_literals;
using namespace AppInstaller::CLI::Portable;
using namespace AppInstaller::Filesystem;
using namespace AppInstaller::Manifest;
using namespace AppInstaller::Registry::Environment;
using namespace AppInstaller::Repository::Microsoft;
using namespace AppInstaller::SQLite;
using namespace AppInstaller::Repository::Microsoft::Schema;
using namespace AppInstaller::Utility;
using namespace AppInstaller::CLI::Workflow;
using namespace TestCommon;

TEST_CASE("PortableARPEntry_IsolatedRegistrationScopes", "[PortableInstaller][PortableAliasRegression]")
{
    TestHook::SetPortableTestEnvironment_Override testEnvironment;
    const auto scope = GENERATE(ScopeEnum::User, ScopeEnum::Machine);
    const auto arch = GENERATE(Architecture::X86, Architecture::X64);
    CAPTURE(scope, arch);
    const HKEY root = testEnvironment.GetARPRegistryRoot(scope);
    const HKEY otherRoot = testEnvironment.GetARPRegistryRoot(scope == ScopeEnum::User ? ScopeEnum::Machine : ScopeEnum::User);
    REQUIRE_THROWS_HR(AppInstaller::Registry::Portable::TestHook_SetPortableARPEntryRoots_Override(root, nullptr), E_INVALIDARG);
    REQUIRE_THROWS_HR(TestHook::SetPortableARPEntryRoots_Override(), E_UNEXPECTED);
    const std::string productCode = "PortableIsolatedRegistration";
    const auto subKey = std::wstring{ scope == ScopeEnum::Machine && arch == Architecture::X86 ?
        L"Software\\Wow6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" :
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" } + ConvertToUTF16(productCode);
    REQUIRE(!AppInstaller::Registry::Key::OpenIfExists(root, subKey));

    PortableARPEntry entry(scope, arch, productCode);
    CHECK_FALSE(entry.Exists());
    CHECK(!AppInstaller::Registry::Key::OpenIfExists(root, subKey));
    entry.SetValue(PortableValueName::DisplayName, std::string_view{ "Isolated registration" });
    {
        const auto key = AppInstaller::Registry::Key::OpenIfExists(root, subKey);
        REQUIRE(key);
        const auto name = key[std::wstring{ L"DisplayName" }];
        REQUIRE(name.has_value());
        CHECK(name->GetValue<AppInstaller::Registry::Value::Type::String>() == "Isolated registration");
    }
    CHECK(!AppInstaller::Registry::Key::OpenIfExists(otherRoot, subKey));
    entry.Delete();
    CHECK(!AppInstaller::Registry::Key::OpenIfExists(root, subKey));
}

TEST_CASE("PortableARPEntry_RegistrationIsCreatedOnFirstWrite", "[PortableInstaller][PortableAliasRegression]")
{
    TestHook::SetPortableTestEnvironment_Override testEnvironment;
    const HKEY arpRoot = testEnvironment.GetARPRegistryRoot(ScopeEnum::User);
    const auto writeKind = GENERATE("none", "utf16", "utf8", "boolean", "other-entry");
    CAPTURE(writeKind);
    TempDirectory identity("PortableARPLifecycle", false);
    const auto productCode = identity.GetPath().filename().u8string();
    const auto subKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" + ConvertToUTF16(productCode);
    REQUIRE(!AppInstaller::Registry::Key::OpenIfExists(arpRoot, subKey));
    auto cleanup = wil::scope_exit([&]() { AppInstaller::Registry::Key::Delete(arpRoot, subKey, KEY_WOW64_64KEY); });

    {
        PortableARPEntry entry(ScopeEnum::User, Architecture::X64, productCode);
        CHECK_FALSE(entry.Exists());
        CHECK(!entry.GetKey());
        CHECK_FALSE(entry[PortableValueName::DisplayName].has_value());
        CHECK(!AppInstaller::Registry::Key::OpenIfExists(arpRoot, subKey));

        if (std::string_view{ writeKind } == "utf16")
        {
            entry.SetValue(PortableValueName::DisplayName, std::wstring{ L"Portable lifecycle \u00E9" });
            const auto value = entry[PortableValueName::DisplayName];
            REQUIRE(value.has_value());
            CHECK(value->GetValue<AppInstaller::Registry::Value::Type::UTF16String>() == L"Portable lifecycle \u00E9");
        }
        else if (std::string_view{ writeKind } == "utf8")
        {
            const auto name = ConvertToUTF8(L"Portable lifecycle \u00E9");
            entry.SetValue(PortableValueName::DisplayName, std::string_view{ name });
            const auto value = entry[PortableValueName::DisplayName];
            REQUIRE(value.has_value());
            CHECK(value->GetValue<AppInstaller::Registry::Value::Type::String>() == name);
        }
        else if (std::string_view{ writeKind } == "boolean")
        {
            bool addedToPath = true;
            entry.SetValue(PortableValueName::InstallDirectoryAddedToPath, addedToPath);
            const auto value = entry[PortableValueName::InstallDirectoryAddedToPath];
            REQUIRE(value.has_value());
            CHECK(value->GetValue<AppInstaller::Registry::Value::Type::DWord>() == 1);
        }
        else if (std::string_view{ writeKind } == "other-entry")
        {
            PortableARPEntry writer(ScopeEnum::User, Architecture::X64, productCode);
            writer.SetValue(PortableValueName::DisplayName, std::string_view{ "Separate writer" });
            const auto value = entry[PortableValueName::DisplayName];
            REQUIRE(value.has_value());
            CHECK(value->GetValue<AppInstaller::Registry::Value::Type::String>() == "Separate writer");
        }
    }

    const auto key = AppInstaller::Registry::Key::OpenIfExists(arpRoot, subKey);
    CHECK(static_cast<bool>(key) == (std::string_view{ writeKind } != "none"));
    CHECK_FALSE(std::filesystem::exists(identity.GetPath()));
}

TEST_CASE("PortableInstaller_PreparationDoesNotCreateRegistration", "[PortableInstaller][PortableAliasRegression]")
{
    TestHook::SetPortableTestEnvironment_Override testEnvironment;
    const HKEY arpRoot = testEnvironment.GetARPRegistryRoot(ScopeEnum::User);
    const auto operation = GENERATE(OperationType::Install, OperationType::Upgrade, OperationType::Uninstall);
    CAPTURE(operation);
    TempDirectory targetDirectory("PortablePreparationTarget", false);
    TempFile source("PortablePreparationSource.exe");
    {
        std::ofstream file(source.GetPath(), std::ios::binary);
        REQUIRE(file.is_open());
        file << "unapplied executable";
        REQUIRE(file.good());
    }
    const auto productCode = targetDirectory.GetPath().filename().u8string();
    const auto subKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" + ConvertToUTF16(productCode);
    REQUIRE(!AppInstaller::Registry::Key::OpenIfExists(arpRoot, subKey));
    auto cleanup = wil::scope_exit([&]() { AppInstaller::Registry::Key::Delete(arpRoot, subKey, KEY_WOW64_64KEY); });

    {
        PortableInstaller installer(ScopeEnum::User, Architecture::X64, productCode, operation);
        CHECK_FALSE(installer.ARPEntryExists());
        CHECK(installer.InstallLocation.empty());
        CHECK(installer.VerifyExpectedState());
        installer.TargetInstallLocation = targetDirectory.GetPath();
        if (operation != OperationType::Uninstall)
        {
            installer.RecordToIndex = true;
            installer.SetDesiredState({ PortableFileEntry::CreateFileEntry(source.GetPath(), targetDirectory.GetPath() / "tool.exe", {}) });
        }
        CHECK(!AppInstaller::Registry::Key::OpenIfExists(arpRoot, subKey));
    }

    CHECK(!AppInstaller::Registry::Key::OpenIfExists(arpRoot, subKey));
    CHECK_FALSE(std::filesystem::exists(targetDirectory.GetPath()));
    CHECK(std::filesystem::is_regular_file(source.GetPath()));
}

TEST_CASE("PortableInstaller_PartialFailureRegistersAndCleansUp", "[PortableInstaller][PortableAliasRegression]")
{
    TestHook::SetPortableTestEnvironment_Override testEnvironment;
    const HKEY arpRoot = testEnvironment.GetARPRegistryRoot(ScopeEnum::User);
    const bool recordToIndex = GENERATE(false, true);
    CAPTURE(recordToIndex);
    TempDirectory targetDirectory("PortableOwnedRegistrationTarget", false);
    TempDirectory linksDirectory("PortableOwnedRegistrationLinks");
    TempDirectory blockedDirectory("PortableOwnedRegistrationBlocked");
    TempFile source("PortableOwnedRegistrationSource.exe");
    {
        std::ofstream file(source.GetPath(), std::ios::binary);
        REQUIRE(file.is_open());
        file << "partially installed executable";
        REQUIRE(file.good());
    }
    const auto productCode = targetDirectory.GetPath().filename().u8string();
    const auto subKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" + ConvertToUTF16(productCode);
    REQUIRE(!AppInstaller::Registry::Key::OpenIfExists(arpRoot, subKey));
    const auto originalLinks = AppInstaller::Runtime::GetPathDetailsFor(AppInstaller::Runtime::PathName::PortableLinksUserLocation);
    auto cleanup = wil::scope_exit([&]()
    {
        AppInstaller::Runtime::TestHook_SetPathOverride(AppInstaller::Runtime::PathName::PortableLinksUserLocation, originalLinks);
        PathVariable(ScopeEnum::User).Remove(targetDirectory.GetPath());
        PathVariable(ScopeEnum::User).Remove(linksDirectory.GetPath());
        AppInstaller::Registry::Key::Delete(arpRoot, subKey, KEY_WOW64_64KEY);
    });
    AppInstaller::Runtime::TestHook_SetPathOverride(AppInstaller::Runtime::PathName::PortableLinksUserLocation, linksDirectory.GetPath());

    const auto targetPath = targetDirectory.GetPath() / "tool.exe";
    const auto expectedHash = SHA256::ConvertToString(SHA256::ComputeHashFromFile(source.GetPath()));
    PortableInstaller installer(ScopeEnum::User, Architecture::X64, productCode, OperationType::Install);
    installer.TargetInstallLocation = targetDirectory.GetPath();
    installer.RecordToIndex = recordToIndex;
    installer.WinGetPackageIdentifier = productCode;
    installer.WinGetSourceIdentifier = "*DefaultSource";
    installer.SetDesiredState({
        PortableFileEntry::CreateFileEntry(source.GetPath(), targetPath, expectedHash),
        PortableFileEntry::CreateSymlinkEntry(blockedDirectory.GetPath(), targetPath),
    });

    REQUIRE_THROWS_HR(installer.Install(), APPINSTALLER_CLI_ERROR_PORTABLE_SYMLINK_PATH_IS_DIRECTORY);
    REQUIRE(std::filesystem::is_regular_file(targetPath));
    CHECK(SHA256::ConvertToString(SHA256::ComputeHashFromFile(targetPath)) == expectedHash);
    CHECK_FALSE(std::filesystem::exists(source.GetPath()));
    {
        const auto key = AppInstaller::Registry::Key::OpenIfExists(arpRoot, subKey);
        REQUIRE(key);
        const auto identifier = key[std::wstring{ L"WinGetPackageIdentifier" }];
        REQUIRE(identifier.has_value());
        CHECK(identifier->GetValue<AppInstaller::Registry::Value::Type::String>() == productCode);
    }
    const auto indexPath = targetDirectory.GetPath() / installer.GetPortableIndexFileName();
    CHECK(std::filesystem::exists(indexPath) == recordToIndex);

    installer.PrepareForCleanUp();
    REQUIRE_NOTHROW(installer.Uninstall());
    CHECK(!AppInstaller::Registry::Key::OpenIfExists(arpRoot, subKey));
    CHECK_FALSE(std::filesystem::exists(targetDirectory.GetPath()));
    CHECK(std::filesystem::is_directory(blockedDirectory.GetPath()));
}

TEST_CASE("PortableInstaller_InstallAndUpgradeReplaceInstalledState", "[PortableInstaller]")
{
    const bool recordToIndex = GENERATE(false, true);
    const bool targetAlreadyExists = GENERATE(false, true);
    const auto operation = GENERATE(OperationType::Install, OperationType::Upgrade);
    CAPTURE(recordToIndex, targetAlreadyExists, operation);
    const std::string productCode = "PortableUpgradeStateTest";
    TempDirectory oldDirectory("PortableBeforeUpgrade", false);
    TempDirectory newDirectory("PortableAfterUpgrade", targetAlreadyExists);
    TempFile oldSource("PortableBeforeUpgrade.exe");
    TempFile newSource("PortableAfterUpgrade.exe");
    std::ofstream(oldSource.GetPath()) << "before";
    std::ofstream(newSource.GetPath()) << "after";
    const auto oldPath = oldDirectory.GetPath() / "before.exe";
    const auto newPath = newDirectory.GetPath() / "after.exe";
    PortableARPEntry arpEntry(ScopeEnum::User, Architecture::X64, productCode);
    auto cleanup = wil::scope_exit([&]() { arpEntry.Delete(); });

    PortableInstaller installer(ScopeEnum::User, Architecture::X64, productCode, OperationType::Install);
    installer.TargetInstallLocation = oldDirectory.GetPath();
    installer.RecordToIndex = recordToIndex;
    installer.SetDesiredState({ PortableFileEntry::CreateFileEntry(oldSource.GetPath(), oldPath, std::string(64, '0')) });
    installer.Install();

    PortableInstaller updater(ScopeEnum::User, Architecture::X64, productCode, operation);
    REQUIRE(updater.IsUpdate() == (operation == OperationType::Upgrade));
    REQUIRE_FALSE(updater.VerifyExpectedState());
    updater.TargetInstallLocation = newDirectory.GetPath();
    updater.RecordToIndex = recordToIndex;
    updater.SetDesiredState({ PortableFileEntry::CreateFileEntry(newSource.GetPath(), newPath, {}) });
    updater.Install();
    REQUIRE_FALSE(std::filesystem::exists(oldDirectory.GetPath()));
    REQUIRE(std::filesystem::exists(newPath));

    PortableInstaller uninstaller(ScopeEnum::User, Architecture::X64, productCode, OperationType::Uninstall);
    REQUIRE(uninstaller.InstallLocation == newDirectory.GetPath());
    uninstaller.Uninstall();
    REQUIRE(std::filesystem::exists(newDirectory.GetPath()) == targetAlreadyExists);
}

TEST_CASE("PortableInstaller_CaseOnlyAliasPreservesSource", "[PortableInstaller][PortableAliasRegression]")
{
    TestHook::SetPortableTestEnvironment_Override testEnvironment;
    const bool recordToIndex = GENERATE(false, true);
    const auto aliasFilename = GENERATE("Tool.exe", "tool.exe");
    CAPTURE(recordToIndex, aliasFilename);
    TempDirectory sourceDirectory("PortableAliasIdentitySource");
    TempDirectory targetDirectory("PortableAliasIdentityTarget", false);
    TempDirectory linksDirectory("PortableAliasIdentityLinks");
    const auto sourcePath = sourceDirectory.GetPath() / "Tool.exe";
    {
        std::ofstream sourceFile(sourcePath, std::ios::binary);
        REQUIRE(sourceFile.is_open());
        sourceFile << "portable alias identity executable";
        REQUIRE(sourceFile.good());
    }
    const auto expectedHash = SHA256::ConvertToString(SHA256::ComputeHashFromFile(sourcePath));
    const auto targetPath = targetDirectory.GetPath() / "Tool.exe";
    const auto aliasPath = targetDirectory.GetPath() / aliasFilename;
    const auto productCode = targetDirectory.GetPath().filename().u8string();
    const auto originalLinks = AppInstaller::Runtime::GetPathDetailsFor(AppInstaller::Runtime::PathName::PortableLinksUserLocation);
    PortableARPEntry arpEntry(ScopeEnum::User, Architecture::X64, productCode);
    auto cleanup = wil::scope_exit([&]()
    {
        AppInstaller::Runtime::TestHook_SetPathOverride(AppInstaller::Runtime::PathName::PortableLinksUserLocation, originalLinks);
        PathVariable(ScopeEnum::User).Remove(linksDirectory.GetPath());
        arpEntry.Delete();
    });
    AppInstaller::Runtime::TestHook_SetPathOverride(AppInstaller::Runtime::PathName::PortableLinksUserLocation, linksDirectory.GetPath());

    PortableInstaller installer(ScopeEnum::User, Architecture::X64, productCode, OperationType::Install);
    installer.TargetInstallLocation = targetDirectory.GetPath();
    installer.RecordToIndex = recordToIndex;
    installer.SetDesiredState({
        PortableFileEntry::CreateFileEntry(sourcePath, targetPath, expectedHash),
        PortableFileEntry::CreateHardlinkEntry(aliasPath, targetPath, expectedHash),
    });

    REQUIRE_NOTHROW(installer.Install());

    REQUIRE(std::filesystem::exists(targetPath));
    REQUIRE(std::filesystem::exists(aliasPath));
    CHECK(std::filesystem::equivalent(targetPath, aliasPath));
    CHECK(SHA256::ConvertToString(SHA256::ComputeHashFromFile(targetPath)) == expectedHash);
    PortableInstaller uninstaller(ScopeEnum::User, Architecture::X64, productCode, OperationType::Uninstall);
    REQUIRE(uninstaller.VerifyExpectedState());
    uninstaller.Uninstall();
    CHECK_FALSE(std::filesystem::exists(targetPath));
    CHECK_FALSE(std::filesystem::exists(targetDirectory.GetPath()));
}

TEST_CASE("PortableInstaller_RollbackRemovesPartialInstall", "[PortableInstaller]")
{
    const std::string productCode = "PortableRollbackStateTest";
    TempDirectory targetDirectory("PortableRollbackState", false);
    TempDirectory blockedAlias("PortableBlockedAlias", true);
    TempFile source("PortableRollbackSource.exe");
    std::ofstream(source.GetPath()) << "fresh";
    const auto targetPath = targetDirectory.GetPath() / "fresh.exe";
    PortableARPEntry arpEntry(ScopeEnum::User, Architecture::X64, productCode);
    auto cleanup = wil::scope_exit([&]() { arpEntry.Delete(); });

    PortableInstaller installer(ScopeEnum::User, Architecture::X64, productCode, OperationType::Install);
    installer.TargetInstallLocation = targetDirectory.GetPath();
    installer.RecordToIndex = true;
    installer.SetDesiredState({
        PortableFileEntry::CreateFileEntry(source.GetPath(), targetPath, {}),
        PortableFileEntry::CreateSymlinkEntry(blockedAlias.GetPath(), targetPath),
    });
    REQUIRE_THROWS_HR(installer.Install(), APPINSTALLER_CLI_ERROR_PORTABLE_SYMLINK_PATH_IS_DIRECTORY);
    REQUIRE(std::filesystem::exists(targetPath));

    const auto indexPath = targetDirectory.GetPath() / installer.GetPortableIndexFileName();
    REQUIRE(std::filesystem::exists(indexPath));
    installer.PrepareForCleanUp();
    installer.Uninstall();
    REQUIRE_FALSE(std::filesystem::exists(targetDirectory.GetPath()));
    REQUIRE(std::filesystem::exists(blockedAlias.GetPath()));
}

TEST_CASE("PortableInstaller_ReinstallReplacesDirectoryTarget", "[PortableInstaller]")
{
    const bool trackedDirectory = GENERATE(false, true);
    CAPTURE(trackedDirectory);
    const std::string productCode = "PortableDirectoryReinstallTest";
    TempDirectory installRoot("PortableDirectoryReinstallRoot", true);
    TempDirectory oldSource("PortableDirectoryReinstallOld", true);
    TempDirectory newSource("PortableDirectoryReinstallNew", true);
    std::ofstream(oldSource.GetPath() / "old.txt") << "old";
    std::ofstream(newSource.GetPath() / "new.txt") << "new";
    const auto directoryPath = installRoot.GetPath() / "package";
    PortableARPEntry arpEntry(ScopeEnum::User, Architecture::X64, productCode);
    auto cleanup = wil::scope_exit([&]() { arpEntry.Delete(); });

    if (trackedDirectory)
    {
        PortableInstaller installer(ScopeEnum::User, Architecture::X64, productCode, OperationType::Install);
        installer.TargetInstallLocation = installRoot.GetPath();
        installer.RecordToIndex = true;
        installer.SetDesiredState({ PortableFileEntry::CreateDirectoryEntry(oldSource.GetPath(), directoryPath) });
        installer.Install();
        REQUIRE(std::filesystem::exists(directoryPath / "old.txt"));
    }
    else
    {
        std::filesystem::create_directory(directoryPath);
    }

    PortableInstaller reinstaller(ScopeEnum::User, Architecture::X64, productCode, OperationType::Install);
    reinstaller.TargetInstallLocation = installRoot.GetPath();
    reinstaller.RecordToIndex = true;
    reinstaller.SetDesiredState({ PortableFileEntry::CreateDirectoryEntry(newSource.GetPath(), directoryPath) });
    REQUIRE_FALSE(reinstaller.IsUpdate());
    reinstaller.Install();
    REQUIRE(std::filesystem::exists(directoryPath / "new.txt"));
    REQUIRE_FALSE(std::filesystem::exists(directoryPath / "old.txt"));

    PortableInstaller uninstaller(ScopeEnum::User, Architecture::X64, productCode, OperationType::Uninstall);
    uninstaller.Uninstall();
    REQUIRE_FALSE(std::filesystem::exists(directoryPath));
    REQUIRE(std::filesystem::exists(installRoot.GetPath()));
}

TEST_CASE("PortableInstaller_RollbackPreservesUnappliedDirectories", "[PortableInstaller]")
{
    const bool recordToIndex = GENERATE(false, true);
    const bool installFileFirst = GENERATE(false, true);
    CAPTURE(recordToIndex, installFileFirst);
    const std::string productCode = "PortableUnappliedDirectoriesTest";
    TempDirectory installRoot("PortableUnappliedDirectoriesRoot", true);
    TempDirectory directorySource("PortableUnappliedDirectorySource", true);
    TempFile fileSource("PortableAppliedFile.txt");
    std::ofstream(fileSource.GetPath()) << "new";
    const auto installedFile = installRoot.GetPath() / "new.txt";
    const auto failedDirectory = installRoot.GetPath() / "existing";
    const auto unattemptedDirectory = installRoot.GetPath() / "unattempted";
    std::filesystem::create_directory(failedDirectory);
    std::filesystem::create_directory(unattemptedDirectory);
    std::ofstream(failedDirectory / "keep.txt") << "existing";
    std::ofstream(unattemptedDirectory / "keep.txt") << "unattempted";

    PortableARPEntry arpEntry(ScopeEnum::User, Architecture::X64, productCode);
    auto cleanup = wil::scope_exit([&]() { arpEntry.Delete(); });
    PortableInstaller installer(ScopeEnum::User, Architecture::X64, productCode, OperationType::Install);
    installer.TargetInstallLocation = installRoot.GetPath();
    installer.RecordToIndex = recordToIndex;
    std::vector<PortableFileEntry> desiredEntries;
    if (installFileFirst)
    {
        desiredEntries.emplace_back(PortableFileEntry::CreateFileEntry(fileSource.GetPath(), installedFile, {}));
    }
    desiredEntries.emplace_back(PortableFileEntry::CreateDirectoryEntry(directorySource.GetPath(), failedDirectory));
    desiredEntries.emplace_back(PortableFileEntry::CreateDirectoryEntry(directorySource.GetPath(), unattemptedDirectory));
    installer.SetDesiredState(desiredEntries);
    REQUIRE_THROWS_HR(installer.Install(), HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));
    REQUIRE(std::filesystem::exists(installedFile) == installFileFirst);
    REQUIRE(std::filesystem::exists(directorySource.GetPath()));

    const auto indexPath = installRoot.GetPath() / installer.GetPortableIndexFileName();
    if (recordToIndex)
    {
        auto index = PortableIndex::Open(indexPath.u8string(), SQLiteStorageBase::OpenDisposition::Read);
        REQUIRE(index.GetAllPortableFiles().size() == static_cast<size_t>(installFileFirst ? 1 : 0));
    }

    installer.PrepareForCleanUp();
    installer.Uninstall();
    REQUIRE_FALSE(std::filesystem::exists(installedFile));
    REQUIRE_FALSE(std::filesystem::exists(indexPath));
    for (const auto& directory : { failedDirectory, unattemptedDirectory })
    {
        std::ifstream file(directory / "keep.txt");
        std::string contents;
        std::getline(file, contents);
        REQUIRE(contents == (directory == failedDirectory ? "existing" : "unattempted"));
    }
}

TEST_CASE("PortableInstaller_RollbackHandlesFailedReplacement", "[PortableInstaller]")
{
    const bool directoryEntry = GENERATE(false, true);
    CAPTURE(directoryEntry);
    const std::string productCode = "PortableUnindexedReplacementTest";
    TempDirectory installRoot("PortableUnindexedReplacementRoot", false);
    TempFile source("PortableUnindexedReplacementSource.txt");
    std::ofstream(source.GetPath()) << "source";
    const auto targetPath = installRoot.GetPath() / "missing.txt";
    auto desiredEntry = directoryEntry ?
        PortableFileEntry::CreateDirectoryEntry(source.GetPath(), targetPath) :
        PortableFileEntry::CreateFileEntry(source.GetPath(), targetPath, {});
    std::filesystem::remove(source.GetPath());

    PortableARPEntry arpEntry(ScopeEnum::User, Architecture::X64, productCode);
    auto cleanup = wil::scope_exit([&]() { arpEntry.Delete(); });
    PortableInstaller installer(ScopeEnum::User, Architecture::X64, productCode, OperationType::Install);
    installer.TargetInstallLocation = installRoot.GetPath();
    installer.RecordToIndex = true;
    installer.SetDesiredState({ desiredEntry });
    REQUIRE_THROWS(installer.Install());
    if (directoryEntry)
    {
        REQUIRE_FALSE(std::filesystem::exists(targetPath));
    }

    {
        const auto indexPath = installRoot.GetPath() / installer.GetPortableIndexFileName();
        auto index = PortableIndex::Open(indexPath.u8string(), SQLiteStorageBase::OpenDisposition::Read);
        REQUIRE(index.Exists(desiredEntry));
    }
    installer.PrepareForCleanUp();
    REQUIRE_NOTHROW(installer.Uninstall());
    REQUIRE_FALSE(std::filesystem::exists(installRoot.GetPath()));
}

TEST_CASE("PortableInstaller_IndexFailureDoesNotInstallFile", "[PortableInstaller]")
{
    const std::string productCode = "PortableIndexFailureTest";
    TempDirectory installRoot("PortableIndexFailureRoot", true);
    TempFile source("PortableIndexFailureSource.txt");
    std::ofstream(source.GetPath()) << "replacement";
    const auto targetPath = installRoot.GetPath() / "existing.txt";
    std::ofstream(targetPath) << "original";
    PortableARPEntry arpEntry(ScopeEnum::User, Architecture::X64, productCode);
    auto cleanup = wil::scope_exit([&]() { arpEntry.Delete(); });
    PortableInstaller installer(ScopeEnum::User, Architecture::X64, productCode, OperationType::Install);
    installer.TargetInstallLocation = installRoot.GetPath();
    installer.RecordToIndex = true;
    installer.SetDesiredState({ PortableFileEntry::CreateFileEntry(source.GetPath(), targetPath, {}) });

    const auto indexPath = installRoot.GetPath() / installer.GetPortableIndexFileName();
    {
        auto index = PortableIndex::CreateNew(indexPath.u8string());
    }
    {
        auto connection = Connection::Create(indexPath.u8string(), Connection::OpenDisposition::ReadWrite);
        Statement::Create(connection,
            "CREATE TRIGGER FailPortableInsert BEFORE INSERT ON portable "
            "BEGIN SELECT RAISE(ABORT, 'Test index failure'); END;").Execute();
    }

    REQUIRE_THROWS(installer.Install());
    REQUIRE(std::filesystem::exists(source.GetPath()));
    installer.PrepareForCleanUp();
    REQUIRE_NOTHROW(installer.Uninstall());
    REQUIRE_FALSE(std::filesystem::exists(indexPath));
    std::ifstream targetFile(targetPath);
    std::string contents;
    std::getline(targetFile, contents);
    REQUIRE(contents == "original");
}

TEST_CASE("PortableInstaller_UninstallRemovesMissingIndexedEntries", "[PortableInstaller]")
{
    const std::string productCode = "PortableMissingIndexedEntriesTest";
    TempDirectory installRoot("PortableMissingIndexedEntriesRoot", false);
    PortableARPEntry arpEntry(ScopeEnum::User, Architecture::X64, productCode);
    auto cleanup = wil::scope_exit([&]() { arpEntry.Delete(); });
    PortableInstaller installer(ScopeEnum::User, Architecture::X64, productCode, OperationType::Install);
    installer.TargetInstallLocation = installRoot.GetPath();
    installer.RecordToIndex = true;
    installer.Install();

    {
        const auto indexPath = installRoot.GetPath() / installer.GetPortableIndexFileName();
        auto index = PortableIndex::Open(indexPath.u8string(), SQLiteStorageBase::OpenDisposition::ReadWrite);
        const auto missingFile = installRoot.GetPath() / "missing.exe";
        index.AddPortableFile(PortableFileEntry::CreateFileEntry({}, missingFile, std::string(64, '0')));
        index.AddPortableFile(PortableFileEntry::CreateHardlinkEntry(installRoot.GetPath() / "alias.exe", missingFile));
        index.AddPortableFile(PortableFileEntry::CreateSymlinkEntry(installRoot.GetPath() / "link.exe", missingFile));
        index.AddPortableFile(PortableFileEntry::CreateDirectoryEntry({}, installRoot.GetPath() / "missing"));
    }

    PortableInstaller uninstaller(ScopeEnum::User, Architecture::X64, productCode, OperationType::Uninstall);
    REQUIRE(uninstaller.VerifyExpectedState());
    REQUIRE_NOTHROW(uninstaller.Uninstall());
    REQUIRE_FALSE(std::filesystem::exists(installRoot.GetPath()));
}

TEST_CASE("PortableInstaller_FailedDirectoryUpgradeRetainsOwnership", "[PortableInstaller]")
{
    const bool retryUpgrade = GENERATE(false, true);
    CAPTURE(retryUpgrade);
    const std::string productCode = "PortablePartialDirectoryUpgradeTest";
    TempDirectory installRoot("PortablePartialDirectoryUpgradeRoot", true);
    TempDirectory source("PortablePartialDirectoryUpgradeSource", true);
    const auto sourceSubdirectory = source.GetPath() / "nested";
    std::filesystem::create_directory(sourceSubdirectory);
    const auto sourceFile = sourceSubdirectory / "locked.txt";
    std::ofstream(sourceFile) << "replacement";
    const auto targetDirectory = installRoot.GetPath() / "package";
    const auto untouchedDirectory = installRoot.GetPath() / "untouched";
    std::filesystem::create_directory(untouchedDirectory);
    std::ofstream(untouchedDirectory / "keep.txt") << "untouched";
    const auto desiredEntry = PortableFileEntry::CreateDirectoryEntry(source.GetPath(), targetDirectory);
    const auto untouchedEntry = PortableFileEntry::CreateDirectoryEntry(source.GetPath(), untouchedDirectory);
    PortableARPEntry arpEntry(ScopeEnum::User, Architecture::X64, productCode);
    auto cleanup = wil::scope_exit([&]() { arpEntry.Delete(); });

    PortableInstaller installer(ScopeEnum::User, Architecture::X64, productCode, OperationType::Install);
    installer.TargetInstallLocation = installRoot.GetPath();
    installer.RecordToIndex = true;
    installer.Install();

    bool sameVolume = false;
    AppInstaller::Filesystem::TestHook_SetIsSameVolume_Override(&sameVolume);
    auto restoreVolume = wil::scope_exit([]()
        {
            AppInstaller::Filesystem::TestHook_SetIsSameVolume_Override(nullptr);
        });
    wil::unique_hfile lockedFile{ CreateFileW(sourceFile.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) };
    REQUIRE(lockedFile);

    PortableInstaller updater(ScopeEnum::User, Architecture::X64, productCode, OperationType::Upgrade);
    updater.TargetInstallLocation = installRoot.GetPath();
    updater.RecordToIndex = true;
    updater.SetDesiredState({ desiredEntry, untouchedEntry });
    REQUIRE_THROWS_AS(updater.Install(), std::filesystem::filesystem_error);
    REQUIRE(std::filesystem::is_directory(targetDirectory / "nested"));
    PortableARPEntry currentARPEntry(ScopeEnum::User, Architecture::X64, productCode);
    REQUIRE(currentARPEntry.Exists());

    {
        const auto indexPath = installRoot.GetPath() / updater.GetPortableIndexFileName();
        auto index = PortableIndex::Open(indexPath.u8string(), SQLiteStorageBase::OpenDisposition::Read);
        REQUIRE(index.GetAllPortableFiles().size() == 1);
        REQUIRE(index.Exists(desiredEntry));
        REQUIRE_FALSE(index.Exists(untouchedEntry));
    }

    lockedFile.reset();
    if (retryUpgrade)
    {
        PortableInstaller retry(ScopeEnum::User, Architecture::X64, productCode, OperationType::Upgrade);
        retry.TargetInstallLocation = installRoot.GetPath();
        retry.RecordToIndex = true;
        retry.SetDesiredState({ desiredEntry });
        REQUIRE(retry.VerifyExpectedState());
        REQUIRE_NOTHROW(retry.Install());
        REQUIRE(std::filesystem::exists(targetDirectory / "nested" / "locked.txt"));
    }

    PortableInstaller uninstaller(ScopeEnum::User, Architecture::X64, productCode, OperationType::Uninstall);
    REQUIRE_FALSE(uninstaller.Purge);
    REQUIRE(uninstaller.VerifyExpectedState());
    REQUIRE_NOTHROW(uninstaller.Uninstall());
    REQUIRE_FALSE(std::filesystem::exists(targetDirectory));
    std::ifstream untouchedFile(untouchedDirectory / "keep.txt");
    std::string contents;
    std::getline(untouchedFile, contents);
    REQUIRE(contents == "untouched");
}

TEST_CASE("PortableInstaller_InstallToRegistry", "[PortableInstaller]")
{
    TempDirectory tempDirectory = TestCommon::TempDirectory("TempDirectory", false);

    std::vector<PortableFileEntry> desiredTestState;

    TestCommon::TempFile testPortable("testPortable.txt");
    std::ofstream file(testPortable, std::ofstream::out);
    file.close();

    std::filesystem::path targetPath = tempDirectory.GetPath() / "testPortable.txt";
    std::filesystem::path symlinkPath = tempDirectory.GetPath() / "testSymlink.exe";

    desiredTestState.emplace_back(std::move(PortableFileEntry::CreateFileEntry(testPortable.GetPath(), targetPath, {})));
    desiredTestState.emplace_back(std::move(PortableFileEntry::CreateSymlinkEntry(symlinkPath, targetPath)));

    PortableInstaller portableInstaller = PortableInstaller(ScopeEnum::User, Architecture::X64, "testProductCode", OperationType::Install);
    portableInstaller.TargetInstallLocation = tempDirectory.GetPath();
    portableInstaller.SetDesiredState(desiredTestState);
    REQUIRE(portableInstaller.VerifyExpectedState());

    portableInstaller.Install();

    auto entry = portableInstaller.GetAppsAndFeaturesEntry();
    REQUIRE(entry.ProductCode == portableInstaller.GetProductCode());

    PortableInstaller portableInstaller2 = PortableInstaller(ScopeEnum::User, Architecture::X64, "testProductCode", OperationType::Uninstall);
    REQUIRE(portableInstaller2.ARPEntryExists());
    REQUIRE(std::filesystem::exists(portableInstaller2.PortableTargetFullPath));
    REQUIRE(AppInstaller::Filesystem::SymlinkExists(portableInstaller2.PortableSymlinkFullPath));

    portableInstaller2.Uninstall();
    REQUIRE_FALSE(std::filesystem::exists(portableInstaller2.PortableTargetFullPath));
    REQUIRE_FALSE(AppInstaller::Filesystem::SymlinkExists(portableInstaller2.PortableSymlinkFullPath));
    REQUIRE_FALSE(std::filesystem::exists(portableInstaller2.InstallLocation));
}

TEST_CASE("PortableInstaller_InstallToIndex_CreateInstallRoot", "[PortableInstaller]")
{
    TempDirectory installRootDirectory = TestCommon::TempDirectory("PortableInstallRoot", false);

    std::vector<PortableFileEntry> desiredTestState;

    TestCommon::TempFile testPortable("testPortable.txt");
    std::ofstream file1(testPortable, std::ofstream::out);
    file1.close();

    TestCommon::TempFile testPortable2("testPortable2.txt");
    std::ofstream file2(testPortable2, std::ofstream::out);
    file2.close();

    TestCommon::TempDirectory testDirectoryFolder("testDirectory", true);

    std::filesystem::path installRootPath = installRootDirectory.GetPath();
    std::filesystem::path targetPath = installRootPath / "testPortable.txt";
    std::filesystem::path targetPath2 = installRootPath / "testPortable2.txt";
    std::filesystem::path symlinkPath = installRootPath / "testSymlink.exe";
    std::filesystem::path symlinkPath2 = installRootPath / "testSymlink2.exe";
    std::filesystem::path directoryPath = installRootPath / "testDirectory";

    desiredTestState.emplace_back(std::move(PortableFileEntry::CreateFileEntry(testPortable.GetPath(), targetPath, {})));
    desiredTestState.emplace_back(std::move(PortableFileEntry::CreateFileEntry(testPortable2.GetPath(), targetPath2, {})));
    desiredTestState.emplace_back(std::move(PortableFileEntry::CreateSymlinkEntry(symlinkPath, targetPath)));
    desiredTestState.emplace_back(std::move(PortableFileEntry::CreateSymlinkEntry(symlinkPath2, targetPath2)));
    desiredTestState.emplace_back(std::move(PortableFileEntry::CreateDirectoryEntry(testDirectoryFolder.GetPath(), directoryPath)));

    PortableInstaller portableInstaller = PortableInstaller(ScopeEnum::User, Architecture::X64, "testProductCode", OperationType::Install);
    portableInstaller.TargetInstallLocation = installRootDirectory.GetPath();
    portableInstaller.RecordToIndex = true;
    portableInstaller.SetDesiredState(desiredTestState);
    REQUIRE(portableInstaller.VerifyExpectedState());

    portableInstaller.Install();

    REQUIRE(std::filesystem::exists(installRootPath / portableInstaller.GetPortableIndexFileName()));
    REQUIRE(std::filesystem::exists(targetPath));
    REQUIRE(std::filesystem::exists(targetPath2));
    REQUIRE(AppInstaller::Filesystem::SymlinkExists(symlinkPath));
    REQUIRE(AppInstaller::Filesystem::SymlinkExists(symlinkPath2));
    REQUIRE(std::filesystem::exists(directoryPath));

    PortableInstaller portableInstaller2 = PortableInstaller(ScopeEnum::User, Architecture::X64, "testProductCode", OperationType::Uninstall);
    REQUIRE(portableInstaller2.ARPEntryExists());

    portableInstaller2.Uninstall();

    // Install root directory should be removed since it was created.
    REQUIRE_FALSE(std::filesystem::exists(installRootPath));
    REQUIRE_FALSE(std::filesystem::exists(targetPath));
    REQUIRE_FALSE(std::filesystem::exists(targetPath2));
    REQUIRE_FALSE(AppInstaller::Filesystem::SymlinkExists(symlinkPath));
    REQUIRE_FALSE(AppInstaller::Filesystem::SymlinkExists(symlinkPath2));
    REQUIRE_FALSE(std::filesystem::exists(directoryPath));
}

TEST_CASE("PortableInstaller_InstallToIndex_ExistingInstallRoot", "[PortableInstaller]")
{
    TempDirectory installRootDirectory = TestCommon::TempDirectory("PortableInstallRoot", true);

    std::vector<PortableFileEntry> desiredTestState;

    TestCommon::TempFile testPortable("testPortable.txt");
    std::ofstream file1(testPortable, std::ofstream::out);
    file1.close();

    TestCommon::TempFile testPortable2("testPortable2.txt");
    std::ofstream file2(testPortable2, std::ofstream::out);
    file2.close();

    TestCommon::TempDirectory testDirectoryFolder("testDirectory", true);

    std::filesystem::path installRootPath = installRootDirectory.GetPath();
    std::filesystem::path targetPath = installRootPath / "testPortable.txt";
    std::filesystem::path targetPath2 = installRootPath / "testPortable2.txt";
    std::filesystem::path symlinkPath = installRootPath / "testSymlink.exe";
    std::filesystem::path symlinkPath2 = installRootPath / "testSymlink2.exe";
    std::filesystem::path directoryPath = installRootPath / "testDirectory";

    desiredTestState.emplace_back(std::move(PortableFileEntry::CreateFileEntry(testPortable.GetPath(), targetPath, {})));
    desiredTestState.emplace_back(std::move(PortableFileEntry::CreateFileEntry(testPortable2.GetPath(), targetPath2, {})));
    desiredTestState.emplace_back(std::move(PortableFileEntry::CreateSymlinkEntry(symlinkPath, targetPath)));
    desiredTestState.emplace_back(std::move(PortableFileEntry::CreateSymlinkEntry(symlinkPath2, targetPath2)));
    desiredTestState.emplace_back(std::move(PortableFileEntry::CreateDirectoryEntry(testDirectoryFolder.GetPath(), directoryPath)));

    PortableInstaller portableInstaller = PortableInstaller(ScopeEnum::User, Architecture::X64, "testProductCode", OperationType::Install);
    portableInstaller.TargetInstallLocation = installRootDirectory.GetPath();
    portableInstaller.RecordToIndex = true;
    portableInstaller.SetDesiredState(desiredTestState);
    REQUIRE(portableInstaller.VerifyExpectedState());

    portableInstaller.Install();

    REQUIRE(std::filesystem::exists(installRootPath / portableInstaller.GetPortableIndexFileName()));
    REQUIRE(std::filesystem::exists(targetPath));
    REQUIRE(std::filesystem::exists(targetPath2));
    REQUIRE(AppInstaller::Filesystem::SymlinkExists(symlinkPath));
    REQUIRE(AppInstaller::Filesystem::SymlinkExists(symlinkPath2));
    REQUIRE(std::filesystem::exists(directoryPath));

    PortableInstaller portableInstaller2 = PortableInstaller(ScopeEnum::User, Architecture::X64, "testProductCode", OperationType::Uninstall);
    REQUIRE(portableInstaller2.ARPEntryExists());

    portableInstaller2.Uninstall();

    // Install root directory should still exist since it was created previously.
    REQUIRE(std::filesystem::exists(installRootPath));
    REQUIRE_FALSE(std::filesystem::exists(targetPath));
    REQUIRE_FALSE(std::filesystem::exists(targetPath2));
    REQUIRE_FALSE(AppInstaller::Filesystem::SymlinkExists(symlinkPath));
    REQUIRE_FALSE(AppInstaller::Filesystem::SymlinkExists(symlinkPath2));
    REQUIRE_FALSE(std::filesystem::exists(directoryPath));
}

TEST_CASE("PortableInstaller_UnicodeSymlinkPath", "[PortableInstaller]")
{
    TempDirectory tempDirectory = TestCommon::TempDirectory("TempDirectory", false);

    // Modify install location path to include unicode characters.
    std::filesystem::path testInstallLocation = tempDirectory.GetPath() / std::filesystem::path{ ConvertToUTF16("романтический") };

    std::vector<PortableFileEntry> desiredTestState;

    TestCommon::TempFile testPortable("testPortable.txt");
    std::ofstream file(testPortable, std::ofstream::out);
    file.close();

    std::filesystem::path targetPath = testInstallLocation / "testPortable.txt";
    std::filesystem::path symlinkPath = tempDirectory.GetPath() / "testSymlink.exe";

    desiredTestState.emplace_back(std::move(PortableFileEntry::CreateFileEntry(testPortable.GetPath(), targetPath, {})));
    desiredTestState.emplace_back(std::move(PortableFileEntry::CreateSymlinkEntry(symlinkPath, targetPath)));

    PortableInstaller portableInstaller = PortableInstaller(ScopeEnum::User, Architecture::X64, "testProductCode", OperationType::Install);
    portableInstaller.TargetInstallLocation = testInstallLocation;
    portableInstaller.SetDesiredState(desiredTestState);
    REQUIRE(portableInstaller.VerifyExpectedState());

    portableInstaller.Install();

    PortableInstaller portableInstaller2 = PortableInstaller(ScopeEnum::User, Architecture::X64, "testProductCode", OperationType::Uninstall);
    REQUIRE(portableInstaller2.ARPEntryExists());
    REQUIRE(std::filesystem::exists(portableInstaller2.PortableTargetFullPath));
    REQUIRE(AppInstaller::Filesystem::SymlinkExists(portableInstaller2.PortableSymlinkFullPath));

    portableInstaller2.Uninstall();
    REQUIRE_FALSE(std::filesystem::exists(portableInstaller2.PortableTargetFullPath));
    REQUIRE_FALSE(AppInstaller::Filesystem::SymlinkExists(portableInstaller2.PortableSymlinkFullPath));
    REQUIRE_FALSE(std::filesystem::exists(portableInstaller2.InstallLocation));
}
