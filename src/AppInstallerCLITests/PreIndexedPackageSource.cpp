// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "pch.h"
#include "TestSource.h"
#include "TestCommon.h"
#include "TestHooks.h"
#include "TestMsixPackage.h"
#include "TestSettings.h"
#include "SQLiteIndexTestCommon.h"
#include <winget/RepositorySource.h>
#include <AppInstallerRuntime.h>
#include <AppInstallerStrings.h>
#include <Microsoft/PreIndexedPackageSourceFactory.h>
#include <Microsoft/PreIndexed/SourceData.h>
#include <Microsoft/SQLiteIndex.h>
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

TEST_CASE("PreIndexedSourceData_BareIdentity", "[pips][source_data]")
{
    SourceData data{ "Microsoft.Winget.Source_8wekyb3d8bbwe"sv };

    REQUIRE(data.BaseIdentity() == "Microsoft.Winget.Source_8wekyb3d8bbwe");
    REQUIRE(!data.HasDeltaIdentity());

    // A source with no delta round trips to exactly what it was read from, so that the stored
    // value does not churn for sources that are not delta capable.
    REQUIRE(data.Serialize() == "Microsoft.Winget.Source_8wekyb3d8bbwe");
}

TEST_CASE("PreIndexedSourceData_Empty", "[pips][source_data]")
{
    SourceData data{ ""sv };

    REQUIRE(!data.HasBaseIdentity());
    REQUIRE(!data.HasDeltaIdentity());
    REQUIRE(data.Serialize().empty());
}

TEST_CASE("PreIndexedSourceData_RoundTrip", "[pips][source_data]")
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

TEST_CASE("PreIndexedSourceData_UnknownMembersIgnored", "[pips][source_data]")
{
    // A newer client may add to this value; an older one must still be able to use the source.
    SourceData data{ R"({"baseIdentity":"Base_8wekyb3d8bbwe","somethingNew":42})"sv };

    REQUIRE(data.BaseIdentity() == "Base_8wekyb3d8bbwe");
    REQUIRE(!data.HasDeltaIdentity());
}

TEST_CASE("PreIndexedSourceData_Malformed", "[pips][source_data]")
{
    // A value that is structured but unreadable is an error, not a package family name.
    std::string truncated = R"({"baseIdentity":)";
    std::string noBase = R"({"deltaIdentity":"Delta_8wekyb3d8bbwe"})";
    std::string wrongType = R"({"baseIdentity":42})";

    REQUIRE_THROWS(SourceData{ truncated });
    REQUIRE_THROWS(SourceData{ noBase });
    REQUIRE_THROWS(SourceData{ wrongType });
}

TEST_CASE("PreIndexedSourceData_WellKnownWinGetSourceNamesDelta", "[pips][source_data]")
{
    SourceData data{ GetWellKnownSourceDetails(WellKnownSource::WinGet).Data };

    REQUIRE(data.BaseIdentity() == "Microsoft.Winget.Source_8wekyb3d8bbwe");
    REQUIRE(data.HasDeltaIdentity());

    // The source's own identity has to stay the one it has always been known by, since the local
    // state directory and the cross process lock are derived from it.
    REQUIRE(GetWellKnownSourceDetails(WellKnownSource::WinGet).Identifier == data.BaseIdentity());
}

// The cases below build their own packages rather than using the signed test data, so they need
// no certificate installation and therefore no administrator rights. They exercise the local file
// store, which is the only mechanism an unpackaged test process can reach; the deployed mechanism
// is covered by E2E tests.

namespace
{
    using SQLiteIndex = AppInstaller::Repository::Microsoft::SQLiteIndex;
    using SQLiteStorageBase = AppInstaller::SQLite::SQLiteStorageBase;

    // Where the source publishes its baseline, and the version it publishes there. The delta
    // records both, and the client checks the second before downloading anything.
    constexpr std::string_view s_BaselineRelativePath = "baselines/1.2.3.4/baseline.msix"sv;
    constexpr std::string_view s_BaselineVersion = "1.2.3.4"sv;

    constexpr std::string_view s_DeltaMsixName = "delta.msix"sv;
    constexpr std::string_view s_BaselineMsixName = "baseline.msix"sv;

    IndexFields MakeIndexFields(const std::string& id, std::string name)
    {
        return IndexFields{
            id,
            std::move(name),
            "moniker"s,
            "1.0"s,
            ""s,
            { "tag1" },
            { "command1" },
            id + "/1.0" };
    }

    // Where the local file store keeps a source's packages.
    fs::path GetStatePathFor(const std::string& sourceIdentity)
    {
        fs::path result = GetPathTo(Runtime::PathName::LocalState);
        result /= AppInstaller::Repository::Microsoft::PreIndexedPackageSourceFactory::Type();
        result /= sourceIdentity;
        return result;
    }

    void CleanSourcesFor(const std::string& sourceIdentity)
    {
        RemoveSetting(Stream::UserSources);
        RemoveSetting(Stream::SourcesMetadata);
        fs::remove_all(GetStatePathFor(sourceIdentity));
    }

    // AddSource takes the details by const reference, so what a source actually recorded has to be
    // read back out of the source list.
    SourceDetails GetStoredDetails(std::string_view name)
    {
        for (const auto& details : GetSources())
        {
            if (details.Name == name)
            {
                return details;
            }
        }

        FAIL("Source was not found in the current sources");
        return {};
    }

    // A pre-indexed source published into a local directory.
    //
    // The packages are built here rather than checked in, so that a test can choose the identities
    // and versions that the delta pairing logic turns on. They are unsigned, so every test using
    // this must hold a TestHook::SetSourcePackageTrustValidation_Override.
    struct TestPreIndexedSource
    {
        TempDirectory Remote{ "pipsremote" };

        // The long lived index that the source is built from. It is never prepared itself; each
        // publish prepares a copy, since preparing is a one way operation.
        TempFile WorkingFile{ "pips_working"s, ".db"s };

        // The designated baseline, and the empty delta that designating it produces.
        TempFile BaselineFile{ "pips_baseline"s, ".db"s };
        TempFile BaselineDeltaFile{ "pips_baseline_delta"s, ".db"s };

        // The full index and the delta are published under different identities, since the client
        // has to be able to ask for one without getting the other. The baseline keeps the identity
        // that the full index has always used.
        std::string BaseIdentityName = "WinGetTestSourceIndex";
        std::string DeltaIdentityName = "WinGetTestSourceIndexDelta";

        std::string BaseFamilyName;
        std::string DeltaFamilyName;

        TestPreIndexedSource(const std::vector<IndexFields>& packages)
        {
            SQLiteIndex index = SQLiteIndex::CreateNew(WorkingFile.GetPath().u8string(), SQLiteVersion{ 2, 1 });
            index.SetProperty(SQLiteIndex::Property::PackageUpdateTrackingBaseTime, "0");

            for (const auto& fields : packages)
            {
                auto manifest = CreateManifest(fields);
                index.AddManifest(manifest, fields.Path);
            }
        }

        void AddPackage(const IndexFields& fields)
        {
            SQLiteIndex index = SQLiteIndex::Open(WorkingFile.GetPath().u8string(), SQLiteStorageBase::OpenDisposition::ReadWrite);

            if (!m_baseTimeReset)
            {
                index.SetProperty(SQLiteIndex::Property::PackageUpdateTrackingBaseTime, "");
                m_baseTimeReset = true;
            }

            auto manifest = CreateManifest(fields);
            index.AddManifest(manifest, fields.Path);
        }

        // Prepares a copy of the working index, designating it as a baseline in the same prepare,
        // and publishes it where the delta will say it is. Everything the working index does after
        // this is what a delta will describe.
        void PublishBaseline()
        {
            fs::copy_file(WorkingFile.GetPath(), BaselineFile.GetPath(), fs::copy_options::overwrite_existing);

            {
                SQLiteIndex index = SQLiteIndex::Open(BaselineFile.GetPath().u8string(), SQLiteStorageBase::OpenDisposition::ReadWrite);

                index.SetProperty(SQLiteIndex::Property::DeltaMarkAsBaseline, "true");
                index.SetProperty(SQLiteIndex::Property::DeltaOutputPath, BaselineDeltaFile.GetPath().u8string());
                index.SetProperty(SQLiteIndex::Property::DeltaBaselineRelativeSourcePath, std::string{ s_BaselineRelativePath });
                index.SetProperty(SQLiteIndex::Property::DeltaBaselinePackageVersion, std::string{ s_BaselineVersion });

                index.PrepareForPackaging();
            }

            BaseFamilyName = PublishPackage(BaselineFile.GetPath(), s_BaselineRelativePath, BaseIdentityName, std::string{ s_BaselineVersion });
        }

        // Prepares a copy of the working index and publishes it as the source's full index,
        // optionally publishing the delta that preparing produces alongside it.
        void Publish(std::string_view fullVersion, std::optional<std::string_view> deltaVersion = {})
        {
            TempFile prepared{ "pips_prepared"s, ".db"s };
            TempFile delta{ "pips_delta"s, ".db"s };

            fs::copy_file(WorkingFile.GetPath(), prepared.GetPath(), fs::copy_options::overwrite_existing);

            {
                SQLiteIndex index = SQLiteIndex::Open(prepared.GetPath().u8string(), SQLiteStorageBase::OpenDisposition::ReadWrite);

                if (deltaVersion)
                {
                    index.SetProperty(SQLiteIndex::Property::DeltaBaselineIndexPath, BaselineFile.GetPath().u8string());
                    index.SetProperty(SQLiteIndex::Property::DeltaOutputPath, delta.GetPath().u8string());
                    index.SetProperty(SQLiteIndex::Property::DeltaBaselineRelativeSourcePath, std::string{ s_BaselineRelativePath });
                    index.SetProperty(SQLiteIndex::Property::DeltaBaselinePackageVersion, std::string{ s_BaselineVersion });
                }

                index.PrepareForPackaging();
            }

            BaseFamilyName = PublishPackage(prepared.GetPath(), "source2.msix"sv, BaseIdentityName, std::string{ fullVersion });

            if (deltaVersion)
            {
                DeltaFamilyName = PublishPackage(delta.GetPath(), s_DeltaMsixName, DeltaIdentityName, std::string{ deltaVersion.value() });
            }
        }

        // Replaces the published baseline with one at a version the delta does not name.
        void RepublishBaselineAtVersion(std::string_view version)
        {
            PublishPackage(BaselineFile.GetPath(), s_BaselineRelativePath, BaseIdentityName, std::string{ version });
        }

        SourceDetails MakeDetails(std::string name = "TestName") const
        {
            SourceDetails details;
            details.Name = std::move(name);
            details.Type = AppInstaller::Repository::Microsoft::PreIndexedPackageSourceFactory::Type();
            details.Arg = Remote.GetPath().u8string();
            return details;
        }

        fs::path StatePath() const
        {
            REQUIRE(!BaseFamilyName.empty());
            return GetStatePathFor(BaseFamilyName);
        }

    private:
        std::string PublishPackage(
            const fs::path& indexFile,
            std::string_view relativePath,
            const std::string& identityName,
            const std::string& version)
        {
            fs::path target = Remote.GetPath() / std::string{ relativePath };
            fs::create_directories(target.parent_path());
            fs::remove(target);

            TestIndexPackageDefinition definition;
            definition.Name = identityName;
            definition.Version = version;

            return CreateTestIndexPackage(target, indexFile, definition);
        }

        bool m_baseTimeReset = false;
    };

    // The identifiers the named source presents, read back through the source itself rather than
    // out of the files, so that what is asserted is what a user would see.
    std::set<std::string> GetSourcePackageIds(std::string_view sourceName)
    {
        Source source{ sourceName };
        TestProgress callback;
        source.Open(callback);

        std::set<std::string> result;

        for (const auto& match : source.Search({}).Matches)
        {
            result.insert(match.Package->GetProperty(PackageProperty::Id).get());
        }

        return result;
    }
}

TEST_CASE("PIPS_LocalFile_FullIndex", "[pips][localfile]")
{
    TestHook::SetSourcePackageTrustValidation_Override trustOverride;

    TestPreIndexedSource source{ { MakeIndexFields("Publisher1.Id", "Package 1"), MakeIndexFields("Publisher2.Id", "Package 2") } };
    source.Publish("1.0.0.0"sv);

    CleanSourcesFor(source.BaseFamilyName);

    SourceDetails details = source.MakeDetails();
    TestProgress callback;

    REQUIRE(AddSource(details, callback));

    fs::path state = source.StatePath();
    REQUIRE(fs::exists(state / s_IndexMsixName));

    // The delta's slots are untouched by a source that is not using one.
    REQUIRE(!fs::exists(state / s_DeltaMsixName));
    REQUIRE(!fs::exists(state / s_BaselineMsixName));

    REQUIRE(GetSourcePackageIds(details.Name) == std::set<std::string>{ "Publisher1.Id", "Publisher2.Id" });

    // A newer full index is acquired and replaces what is held.
    source.AddPackage(MakeIndexFields("Publisher3.Id", "Package 3"));
    source.Publish("2.0.0.0"sv);

    bool progressCalled = false;
    callback.m_OnProgress = [&](uint64_t, uint64_t, ProgressType) { progressCalled = true; };

    REQUIRE(UpdateSource(details.Name, callback));
    REQUIRE(progressCalled);

    REQUIRE(GetSourcePackageIds(details.Name) == std::set<std::string>{ "Publisher1.Id", "Publisher2.Id", "Publisher3.Id" });

    REQUIRE(RemoveSource(details.Name, callback));
    REQUIRE(!fs::exists(state));
}

TEST_CASE("PIPS_LocalFile_Delta_Add", "[pips][localfile][delta]")
{
    auto settings = TestUserSettings::EnableExperimentalFeature(ExperimentalFeature::Feature::DeltaIndex);
    TestHook::SetSourcePackageTrustValidation_Override trustOverride;

    TestPreIndexedSource source{ { MakeIndexFields("Publisher1.Id", "Package 1"), MakeIndexFields("Publisher2.Id", "Package 2") } };
    source.PublishBaseline();

    source.AddPackage(MakeIndexFields("Publisher3.Id", "Package 3"));
    source.Publish("2.0.0.0"sv, "2.0.0.0"sv);

    CleanSourcesFor(source.BaseFamilyName);

    SourceDetails details = source.MakeDetails();
    TestProgress callback;

    REQUIRE(AddSource(details, callback));

    SourceDetails stored = GetStoredDetails(details.Name);

    // Adding is the only operation that can learn what a source's packages are, so it is the only
    // one that records both identities.
    SourceData data{ stored.Data };
    REQUIRE(data.BaseIdentity() == source.BaseFamilyName);
    REQUIRE(data.DeltaIdentity() == source.DeltaFamilyName);

    // The source's own identity stays the one it has always been known by, since the local state
    // directory and the cross process lock are derived from it.
    REQUIRE(stored.Identifier == source.BaseFamilyName);

    fs::path state = source.StatePath();
    REQUIRE(fs::exists(state / s_DeltaMsixName));
    REQUIRE(fs::exists(state / s_BaselineMsixName));

    // Nothing acquired the full index, which is the whole point.
    REQUIRE(!fs::exists(state / s_IndexMsixName));

    // The package added after the baseline was captured is only in the delta, so finding it proves
    // that the two were merged rather than that either was read on its own.
    REQUIRE(GetSourcePackageIds(details.Name) == std::set<std::string>{ "Publisher1.Id", "Publisher2.Id", "Publisher3.Id" });
}

TEST_CASE("PIPS_LocalFile_Delta_UpdateKeepsBaseline", "[pips][localfile][delta]")
{
    auto settings = TestUserSettings::EnableExperimentalFeature(ExperimentalFeature::Feature::DeltaIndex);
    TestHook::SetSourcePackageTrustValidation_Override trustOverride;

    TestPreIndexedSource source{ { MakeIndexFields("Publisher1.Id", "Package 1") } };
    source.PublishBaseline();

    source.AddPackage(MakeIndexFields("Publisher2.Id", "Package 2"));
    source.Publish("2.0.0.0"sv, "2.0.0.0"sv);

    CleanSourcesFor(source.BaseFamilyName);

    SourceDetails details = source.MakeDetails();
    TestProgress callback;

    REQUIRE(AddSource(details, callback));

    fs::path baselinePackage = source.StatePath() / s_BaselineMsixName;
    REQUIRE(fs::exists(baselinePackage));
    auto baselineWriteTime = fs::last_write_time(baselinePackage);

    // A newer delta against the same baseline. This is the common case, and the saving it buys is
    // that no baseline traffic occurs at all.
    source.AddPackage(MakeIndexFields("Publisher3.Id", "Package 3"));
    source.Publish("3.0.0.0"sv, "3.0.0.0"sv);

    REQUIRE(UpdateSource(details.Name, callback));

    REQUIRE(fs::last_write_time(baselinePackage) == baselineWriteTime);

    REQUIRE(GetSourcePackageIds(details.Name) == std::set<std::string>{ "Publisher1.Id", "Publisher2.Id", "Publisher3.Id" });
}

TEST_CASE("PIPS_LocalFile_Delta_FallsBackWhenNoDeltaPublished", "[pips][localfile][delta]")
{
    auto settings = TestUserSettings::EnableExperimentalFeature(ExperimentalFeature::Feature::DeltaIndex);
    TestHook::SetSourcePackageTrustValidation_Override trustOverride;

    TestPreIndexedSource source{ { MakeIndexFields("Publisher1.Id", "Package 1") } };
    source.Publish("1.0.0.0"sv);

    CleanSourcesFor(source.BaseFamilyName);

    SourceDetails details = source.MakeDetails();
    TestProgress callback;

    REQUIRE(AddSource(details, callback));

    // A source that publishes no delta is an ordinary answer rather than a failure, so the added
    // source records no delta identity and holds the full index.
    SourceData data{ GetStoredDetails(details.Name).Data };
    REQUIRE(data.BaseIdentity() == source.BaseFamilyName);
    REQUIRE(!data.HasDeltaIdentity());

    fs::path state = source.StatePath();
    REQUIRE(fs::exists(state / s_IndexMsixName));
    REQUIRE(!fs::exists(state / s_DeltaMsixName));

    REQUIRE(GetSourcePackageIds(details.Name) == std::set<std::string>{ "Publisher1.Id" });
}

TEST_CASE("PIPS_LocalFile_Delta_FallsBackWhenBaselineVersionDiffers", "[pips][localfile][delta]")
{
    auto settings = TestUserSettings::EnableExperimentalFeature(ExperimentalFeature::Feature::DeltaIndex);
    TestHook::SetSourcePackageTrustValidation_Override trustOverride;

    TestPreIndexedSource source{ { MakeIndexFields("Publisher1.Id", "Package 1") } };
    source.PublishBaseline();

    source.AddPackage(MakeIndexFields("Publisher2.Id", "Package 2"));
    source.Publish("2.0.0.0"sv, "2.0.0.0"sv);

    // Whatever is at the location the delta names is not the baseline it was computed against, and
    // the version header says so before anything is downloaded.
    source.RepublishBaselineAtVersion("9.9.9.9"sv);

    CleanSourcesFor(source.BaseFamilyName);

    SourceDetails details = source.MakeDetails();
    TestProgress callback;

    REQUIRE(AddSource(details, callback));

    // The delta cannot serve the source, so the full index does.
    fs::path state = source.StatePath();
    REQUIRE(fs::exists(state / s_IndexMsixName));
    REQUIRE(!fs::exists(state / s_BaselineMsixName));

    SourceData data{ GetStoredDetails(details.Name).Data };
    REQUIRE(!data.HasDeltaIdentity());

    REQUIRE(GetSourcePackageIds(details.Name) == std::set<std::string>{ "Publisher1.Id", "Publisher2.Id" });
}

TEST_CASE("PIPS_LocalFile_Delta_RemoveClearsEverySlot", "[pips][localfile][delta]")
{
    auto settings = TestUserSettings::EnableExperimentalFeature(ExperimentalFeature::Feature::DeltaIndex);
    TestHook::SetSourcePackageTrustValidation_Override trustOverride;

    TestPreIndexedSource source{ { MakeIndexFields("Publisher1.Id", "Package 1") } };
    source.PublishBaseline();

    source.AddPackage(MakeIndexFields("Publisher2.Id", "Package 2"));
    source.Publish("2.0.0.0"sv, "2.0.0.0"sv);

    CleanSourcesFor(source.BaseFamilyName);

    SourceDetails details = source.MakeDetails();
    TestProgress callback;

    REQUIRE(AddSource(details, callback));

    fs::path state = source.StatePath();
    REQUIRE(fs::exists(state / s_DeltaMsixName));
    REQUIRE(fs::exists(state / s_BaselineMsixName));

    REQUIRE(RemoveSource(details.Name, callback));

    REQUIRE(!fs::exists(state));
}
