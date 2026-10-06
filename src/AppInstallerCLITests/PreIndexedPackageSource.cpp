// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "pch.h"
#include "TestSource.h"
#include "TestCommon.h"
#include "TestSettings.h"
#include <winget/RepositorySource.h>
#include <AppInstallerRuntime.h>
#include <AppInstallerStrings.h>
#include <Microsoft/PreIndexedPackageSourceFactory.h>
#include <Microsoft/PreIndexed/SourceData.h>
#include <winget/Settings.h>

using namespace std::string_literals;
using namespace std::string_view_literals;
using namespace TestCommon;
using namespace AppInstaller;
using namespace AppInstaller::Repository;
using namespace AppInstaller::Runtime;
using namespace AppInstaller::Settings;
using namespace AppInstaller::Utility;

namespace fs = std::filesystem;

constexpr std::string_view s_RepositorySettings_UserSources = "usersources"sv;

constexpr std::string_view s_MsixFile_1 = "index.1.0.0.0.signed.msix";
constexpr std::string_view s_MsixFile_2 = "index.2.0.0.0.signed.msix";
constexpr std::string_view s_Msix_FamilyName = "AppInstallerCLITestsFakeIndex_8wekyb3d8bbwe";
constexpr std::string_view s_IndexMsixName = "source.msix"sv;

void CopyIndexFileToDirectory(const fs::path& from, const fs::path& to)
{
    fs::path toFile = to;
    toFile /= s_IndexMsixName;
    if (fs::exists(toFile))
    {
        fs::remove(toFile);
    }
    fs::copy_file(from, toFile);
}

fs::path GetPathToFileDir()
{
    fs::path result = GetPathTo(Runtime::PathName::LocalState);
    result /= AppInstaller::Repository::Microsoft::PreIndexedPackageSourceFactory::Type();
    result /= s_Msix_FamilyName;
    return result;
}

std::string GetContents(const fs::path& file)
{
    REQUIRE(fs::exists(file));
    std::ifstream stream(file);
    return ReadEntireStream(stream);
}

void CleanSources()
{
    RemoveSetting(Stream::UserSources);
    RemoveSetting(Stream::SourcesMetadata);
    fs::remove_all(GetPathToFileDir());
}

TEST_CASE("PIPS_Add", "[pips]")
{
    if (!Runtime::IsRunningAsAdmin())
    {
        WARN("Test requires admin privilege. Skipped.");
        return;
    }

    CleanSources();

    TempDirectory dir("pipssource");
    TestDataFile index(s_MsixFile_1);
    CopyIndexFileToDirectory(index, dir);

    bool shouldCleanCert = InstallCertFromSignedPackage(index);

    SourceDetails details;
    details.Name = "TestName";
    details.Type = AppInstaller::Repository::Microsoft::PreIndexedPackageSourceFactory::Type();
    details.Arg = dir;
    ProgressCallback callback;

    AddSource(details, callback);

    fs::path state = GetPathToFileDir();
    REQUIRE(fs::exists(state));

    fs::path indexMsix = state;
    indexMsix /= s_IndexMsixName;
    REQUIRE(fs::exists(indexMsix));
    REQUIRE(fs::file_size(indexMsix) > 0);

    if (shouldCleanCert)
    {
        UninstallCertFromSignedPackage(index);
    }
}

TEST_CASE("PIPS_UpdateSameVersion", "[pips]")
{
    if (!Runtime::IsRunningAsAdmin())
    {
        WARN("Test requires admin privilege. Skipped.");
        return;
    }

    CleanSources();

    TempDirectory dir("pipssource");
    TestDataFile index(s_MsixFile_1);
    CopyIndexFileToDirectory(index, dir);

    bool shouldCleanCert = InstallCertFromSignedPackage(index);

    SourceDetails details;
    details.Name = "TestName";
    details.Type = AppInstaller::Repository::Microsoft::PreIndexedPackageSourceFactory::Type();
    details.Arg = dir;
    TestProgress callback;

    AddSource(details, callback);

    fs::path state = GetPathToFileDir();
    REQUIRE(fs::exists(state));

    bool progressCalled = false;
    callback.m_OnProgress = [&](uint64_t, uint64_t, ProgressType) { progressCalled = true; };

    UpdateSource(details.Name, callback);
    REQUIRE(!progressCalled);

    if (shouldCleanCert)
    {
        UninstallCertFromSignedPackage(index);
    }
}

TEST_CASE("PIPS_UpdateNewVersion", "[pips]")
{
    if (!Runtime::IsRunningAsAdmin())
    {
        WARN("Test requires admin privilege. Skipped.");
        return;
    }

    CleanSources();

    TempDirectory dir("pipssource");
    TestDataFile indexMsix1(s_MsixFile_1);
    CopyIndexFileToDirectory(indexMsix1, dir);

    bool shouldCleanCert = InstallCertFromSignedPackage(indexMsix1);

    SourceDetails details;
    details.Name = "TestName";
    details.Type = AppInstaller::Repository::Microsoft::PreIndexedPackageSourceFactory::Type();
    details.Arg = dir;
    TestProgress callback;

    AddSource(details, callback);

    fs::path state = GetPathToFileDir();
    REQUIRE(fs::exists(state));

    fs::path indexMsix = state;
    indexMsix /= s_IndexMsixName;
    std::string indexContents1 = GetContents(indexMsix);

    TestDataFile indexMsix2(s_MsixFile_2);
    CopyIndexFileToDirectory(indexMsix2, dir);

    bool progressCalled = false;
    callback.m_OnProgress = [&](uint64_t, uint64_t, ProgressType) { progressCalled = true; };

    UpdateSource(details.Name, callback);
    REQUIRE(progressCalled);

    std::string indexContents2 = GetContents(indexMsix);
    REQUIRE(indexContents1 != indexContents2);

    if (shouldCleanCert)
    {
        UninstallCertFromSignedPackage(indexMsix1);
    }
}

TEST_CASE("PIPS_Remove", "[pips]")
{
    if (!Runtime::IsRunningAsAdmin())
    {
        WARN("Test requires admin privilege. Skipped.");
        return;
    }

    CleanSources();

    TempDirectory dir("pipssource");
    TestDataFile index(s_MsixFile_1);
    CopyIndexFileToDirectory(index, dir);

    bool shouldCleanCert = InstallCertFromSignedPackage(index);

    SourceDetails details;
    details.Name = "TestName";
    details.Type = AppInstaller::Repository::Microsoft::PreIndexedPackageSourceFactory::Type();
    details.Arg = dir;
    ProgressCallback callback;

    AddSource(details, callback);

    fs::path state = GetPathToFileDir();
    REQUIRE(fs::exists(state));

    fs::path indexMsix = state;
    indexMsix /= s_IndexMsixName;
    REQUIRE(fs::exists(indexMsix));

    RemoveSource(details.Name, callback);
    REQUIRE(!fs::exists(state));

    if (shouldCleanCert)
    {
        UninstallCertFromSignedPackage(index);
    }
}

using namespace AppInstaller::Repository::Microsoft::PreIndexed;

TEST_CASE("PreIndexedSourceData_BareIdentity", "[pips][sourcedata]")
{
    SourceData data{ "Microsoft.Winget.Source_8wekyb3d8bbwe"sv };

    REQUIRE(data.BaseIdentity() == "Microsoft.Winget.Source_8wekyb3d8bbwe");
    REQUIRE(!data.HasDeltaIdentity());

    // A source with no delta round trips to exactly what it was read from, so that the stored
    // value does not churn for sources that are not delta capable.
    REQUIRE(data.Serialize() == "Microsoft.Winget.Source_8wekyb3d8bbwe");
}

TEST_CASE("PreIndexedSourceData_Empty", "[pips][sourcedata]")
{
    SourceData data{ ""sv };

    REQUIRE(!data.HasBaseIdentity());
    REQUIRE(!data.HasDeltaIdentity());
    REQUIRE(data.Serialize().empty());
}

TEST_CASE("PreIndexedSourceData_RoundTrip", "[pips][sourcedata]")
{
    SourceData original;
    original.BaseIdentity("Base_8wekyb3d8bbwe");
    original.DeltaIdentity("Delta_8wekyb3d8bbwe");

    std::string serialized = original.Serialize();
    INFO(serialized);

    SourceData parsed{ serialized };

    REQUIRE(parsed.BaseIdentity() == "Base_8wekyb3d8bbwe");
    REQUIRE(parsed.DeltaIdentity() == "Delta_8wekyb3d8bbwe");
    REQUIRE(parsed.Serialize() == serialized);
}

TEST_CASE("PreIndexedSourceData_UnknownMembersIgnored", "[pips][sourcedata]")
{
    // A newer client may add to this value; an older one must still be able to use the source.
    SourceData data{ R"({"baseIdentity":"Base_8wekyb3d8bbwe","somethingNew":42})"sv };

    REQUIRE(data.BaseIdentity() == "Base_8wekyb3d8bbwe");
    REQUIRE(!data.HasDeltaIdentity());
}

TEST_CASE("PreIndexedSourceData_Malformed", "[pips][sourcedata]")
{
    // A value that is structured but unreadable is an error, not a package family name.
    std::string truncated = R"({"baseIdentity":)";
    std::string noBase = R"({"deltaIdentity":"Delta_8wekyb3d8bbwe"})";
    std::string wrongType = R"({"baseIdentity":42})";

    REQUIRE_THROWS(SourceData{ truncated });
    REQUIRE_THROWS(SourceData{ noBase });
    REQUIRE_THROWS(SourceData{ wrongType });
}

TEST_CASE("PreIndexedSourceData_WellKnownWinGetSourceNamesADelta", "[pips][sourcedata]")
{
    SourceData data{ GetWellKnownSourceDetails(WellKnownSource::WinGet).Data };

    REQUIRE(data.BaseIdentity() == "Microsoft.Winget.Source_8wekyb3d8bbwe");
    REQUIRE(data.HasDeltaIdentity());

    // The source's own identity has to stay the one it has always been known by, since the local
    // state directory and the cross process lock are derived from it.
    REQUIRE(GetWellKnownSourceDetails(WellKnownSource::WinGet).Identifier == data.BaseIdentity());
}