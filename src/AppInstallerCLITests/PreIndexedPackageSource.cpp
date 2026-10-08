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
#include <Microsoft/PreIndexed/PackageStore.h>
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

namespace
{
    // The identity that the built-in source has always been known by.
    constexpr std::string_view s_WinGetSourceIdentity = "Microsoft.Winget.Source_8wekyb3d8bbwe"sv;

    // Stand in identities for the data format cases, which care only that a value survives.
    constexpr std::string_view s_BaseIdentity = "Base_8wekyb3d8bbwe"sv;
    constexpr std::string_view s_DeltaIdentity = "Delta_8wekyb3d8bbwe"sv;
}

TEST_CASE("PreIndexedSourceData_BareIdentity", "[pips][source_data]")
{
    SourceData data{ s_WinGetSourceIdentity };

    REQUIRE(data.BaseIdentity() == s_WinGetSourceIdentity);
    REQUIRE(!data.HasDeltaIdentity());

    // A source with no delta round trips to exactly what it was read from, so that the stored
    // value does not churn for sources that are not delta capable.
    REQUIRE(data.Serialize() == s_WinGetSourceIdentity);
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
    original.BaseIdentity(std::string{ s_BaseIdentity });
    original.DeltaIdentity(std::string{ s_DeltaIdentity });

    std::string serialized = original.Serialize();
    INFO(serialized);

    SourceData parsed{ serialized };

    REQUIRE(parsed.BaseIdentity() == s_BaseIdentity);
    REQUIRE(parsed.DeltaIdentity() == s_DeltaIdentity);
    REQUIRE(parsed.Serialize() == serialized);
}

TEST_CASE("PreIndexedSourceData_UnknownMembersIgnored", "[pips][source_data]")
{
    // A newer client may add to this value; an older one must still be able to use the source.
    std::string withUnknown = R"({"baseIdentity":")" + std::string{ s_BaseIdentity } + R"(","somethingNew":42})";

    SourceData data{ withUnknown };

    REQUIRE(data.BaseIdentity() == s_BaseIdentity);
    REQUIRE(!data.HasDeltaIdentity());
}

TEST_CASE("PreIndexedSourceData_Malformed", "[pips][source_data]")
{
    // A value that is structured but unreadable is an error, not a package family name.
    std::string truncated = R"({"baseIdentity":)";
    std::string noBase = R"({"deltaIdentity":")" + std::string{ s_DeltaIdentity } + R"("})";
    std::string wrongType = R"({"baseIdentity":42})";

    REQUIRE_THROWS(SourceData{ truncated });
    REQUIRE_THROWS(SourceData{ noBase });
    REQUIRE_THROWS(SourceData{ wrongType });
}

TEST_CASE("PreIndexedSourceData_WellKnownWinGetSourceNamesDelta", "[pips][source_data]")
{
    SourceData data{ GetWellKnownSourceDetails(WellKnownSource::WinGet).Data };

    REQUIRE(data.BaseIdentity() == s_WinGetSourceIdentity);
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
    constexpr std::string_view s_BaselineRelativePath = "baselines/current/baseline.msix"sv;
    constexpr std::string_view s_BaselineVersion = "1.2.3.4"sv;

    constexpr std::string_view s_DeltaMsixName = "delta.msix"sv;
    constexpr std::string_view s_BaselineMsixName = "baseline.msix"sv;

    // The versions that the source publishes its full index and delta at, in the order that the
    // cases publish them.
    constexpr std::string_view s_FirstVersion = "1.0.0.0"sv;
    constexpr std::string_view s_SecondVersion = "2.0.0.0"sv;
    constexpr std::string_view s_ThirdVersion = "3.0.0.0"sv;

    // A baseline version that no delta names, used to stand in for a baseline that has moved on.
    constexpr std::string_view s_UnexpectedBaselineVersion = "9.9.9.9"sv;

    // The packages that the cases publish. Each identifier is written into an index and then read
    // back out of the source, so it is named rather than repeated at both ends.
    const std::string s_Package1Id = "Publisher1.Id";
    const std::string s_Package2Id = "Publisher2.Id";
    const std::string s_Package3Id = "Publisher3.Id";

    IndexFields MakeIndexFields(const std::string& id)
    {
        return IndexFields{
            id,
            id + " Name",
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

    // A deployed package is held outside of our local state entirely, so the store that stands in
    // for that mechanism holds its packages under an identity of its own. That keeps the two as
    // separate as the real ones are.
    constexpr std::string_view s_DeployedIdentitySuffix = ".Deployed"sv;

    // Where the stand in for the deployed store keeps a source's packages.
    fs::path GetDeployedStatePathFor(const std::string& sourceIdentity)
    {
        return GetStatePathFor(sourceIdentity + std::string{ s_DeployedIdentitySuffix });
    }

    void CleanSourcesFor(const std::string& sourceIdentity)
    {
        RemoveSetting(Stream::UserSources);
        RemoveSetting(Stream::SourcesMetadata);
        fs::remove_all(GetStatePathFor(sourceIdentity));
        fs::remove_all(GetDeployedStatePathFor(sourceIdentity));
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
        //
        // The delta is published at the same version as the full index, since the two describe the
        // same contents. Nothing in the client requires that, but there is no reason to differ.
        void Publish(std::string_view version, bool publishDelta = false)
        {
            TempFile prepared{ "pips_prepared"s, ".db"s };
            TempFile delta{ "pips_delta"s, ".db"s };

            fs::copy_file(WorkingFile.GetPath(), prepared.GetPath(), fs::copy_options::overwrite_existing);

            {
                SQLiteIndex index = SQLiteIndex::Open(prepared.GetPath().u8string(), SQLiteStorageBase::OpenDisposition::ReadWrite);

                if (publishDelta)
                {
                    index.SetProperty(SQLiteIndex::Property::DeltaBaselineIndexPath, BaselineFile.GetPath().u8string());
                    index.SetProperty(SQLiteIndex::Property::DeltaOutputPath, delta.GetPath().u8string());
                    index.SetProperty(SQLiteIndex::Property::DeltaBaselineRelativeSourcePath, std::string{ s_BaselineRelativePath });
                    index.SetProperty(SQLiteIndex::Property::DeltaBaselinePackageVersion, std::string{ s_BaselineVersion });
                }

                index.PrepareForPackaging();
            }

            BaseFamilyName = PublishPackage(prepared.GetPath(), "source2.msix"sv, BaseIdentityName, std::string{ version });

            if (publishDelta)
            {
                DeltaFamilyName = PublishPackage(delta.GetPath(), s_DeltaMsixName, DeltaIdentityName, std::string{ version });
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

        // Where the stand in for the deployed store holds this source's packages.
        fs::path DeployedStatePath() const
        {
            REQUIRE(!BaseFamilyName.empty());
            return GetDeployedStatePathFor(BaseFamilyName);
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

    // Stands in for the deployed package store.
    //
    // Packages are held as files, as the local file store does, but under an identity of their
    // own so that the two mechanisms are as separate as the real ones are. Reads are reported as
    // allowed without the lock, which is what the deployed store reports and what the composite
    // store has to carry through correctly.
    struct TestDeployedPackageStore : public IPackageStore
    {
        TestDeployedPackageStore(const SourceDetails& details)
        {
            std::string identity = details.Identifier.empty() ? SourceData{ details.Data }.BaseIdentity() : details.Identifier;
            REQUIRE(!identity.empty());

            SourceDetails deployedDetails = details;
            deployedDetails.Identifier = identity + std::string{ s_DeployedIdentitySuffix };

            m_inner = CreateLocalFilePackageStore(deployedDetails);
        }

        std::optional<AcquiredPackage> Acquire(const PackageKey& package, const std::string& location, IProgressCallback& progress) override
        {
            return m_inner->Acquire(package, location, progress);
        }

        void Persist(AcquiredPackage&& package, IProgressCallback& progress) override
        {
            m_inner->Persist(std::move(package), progress);
        }

        std::optional<Msix::PackageVersion> GetVersion(const PackageKey& package) const override
        {
            return m_inner->GetVersion(package);
        }

        std::optional<ExtractedIndex> GetIndex(const PackageKey& package, IProgressCallback& progress) override
        {
            return m_inner->GetIndex(package, progress);
        }

        void Remove(const std::vector<PackageKey>& packages, IProgressCallback& progress) override
        {
            m_inner->Remove(packages, progress);
        }

        Synchronization::CrossProcessLock Lock(IProgressCallback& progress, bool isBackground = false) override
        {
            return m_inner->Lock(progress, isBackground);
        }

        bool AllowsUnlockedRead() const override { return true; }

    private:
        std::unique_ptr<IPackageStore> m_inner;
    };

    std::unique_ptr<IPackageStore> CreateTestDeployedPackageStore(const SourceDetails& details)
    {
        return std::make_unique<TestDeployedPackageStore>(details);
    }
}

TEST_CASE("PIPS_LocalFile_FullIndex", "[pips][local_file]")
{
    // Held off explicitly, so that this guards the behavior of a source that is not delta capable
    // rather than whatever the machine running the test happens to have enabled.
    TestHook::SetSingleExperimentalFeature_Override deltaDisabled{ ExperimentalFeature::Feature::DeltaIndex, false };
    TestHook::SetSourcePackageTrustValidation_Override trustOverride;

    TestPreIndexedSource source{ { MakeIndexFields(s_Package1Id), MakeIndexFields(s_Package2Id) } };
    source.Publish(s_FirstVersion);

    CleanSourcesFor(source.BaseFamilyName);

    SourceDetails details = source.MakeDetails();
    TestProgress callback;

    REQUIRE(AddSource(details, callback));

    fs::path state = source.StatePath();
    REQUIRE(fs::exists(state / s_IndexMsixName));

    // The delta's slots are untouched by a source that is not using one.
    REQUIRE(!fs::exists(state / s_DeltaMsixName));
    REQUIRE(!fs::exists(state / s_BaselineMsixName));

    REQUIRE(GetSourcePackageIds(details.Name) == std::set<std::string>{ s_Package1Id, s_Package2Id });

    // A newer full index is acquired and replaces what is held.
    source.AddPackage(MakeIndexFields(s_Package3Id));
    source.Publish(s_SecondVersion);

    bool progressCalled = false;
    callback.m_OnProgress = [&](uint64_t, uint64_t, ProgressType) { progressCalled = true; };

    REQUIRE(UpdateSource(details.Name, callback));
    REQUIRE(progressCalled);

    REQUIRE(GetSourcePackageIds(details.Name) == std::set<std::string>{ s_Package1Id, s_Package2Id, s_Package3Id });

    REQUIRE(RemoveSource(details.Name, callback));
    REQUIRE(!fs::exists(state));
}

TEST_CASE("PIPS_LocalFile_Delta_Add", "[pips][local_file][delta]")
{
    TestHook::SetSingleExperimentalFeature_Override deltaEnabled{ ExperimentalFeature::Feature::DeltaIndex };
    TestHook::SetSourcePackageTrustValidation_Override trustOverride;

    TestPreIndexedSource source{ { MakeIndexFields(s_Package1Id), MakeIndexFields(s_Package2Id) } };
    source.PublishBaseline();

    source.AddPackage(MakeIndexFields(s_Package3Id));
    source.Publish(s_SecondVersion, true);

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
    REQUIRE(GetSourcePackageIds(details.Name) == std::set<std::string>{ s_Package1Id, s_Package2Id, s_Package3Id });
}

TEST_CASE("PIPS_LocalFile_Delta_UpdateKeepsBaseline", "[pips][local_file][delta]")
{
    TestHook::SetSingleExperimentalFeature_Override deltaEnabled{ ExperimentalFeature::Feature::DeltaIndex };
    TestHook::SetSourcePackageTrustValidation_Override trustOverride;

    TestPreIndexedSource source{ { MakeIndexFields(s_Package1Id) } };
    source.PublishBaseline();

    source.AddPackage(MakeIndexFields(s_Package2Id));
    source.Publish(s_SecondVersion, true);

    CleanSourcesFor(source.BaseFamilyName);

    SourceDetails details = source.MakeDetails();
    TestProgress callback;

    REQUIRE(AddSource(details, callback));

    fs::path baselinePackage = source.StatePath() / s_BaselineMsixName;
    REQUIRE(fs::exists(baselinePackage));
    auto baselineWriteTime = fs::last_write_time(baselinePackage);

    // A newer delta against the same baseline. This is the common case, and the saving it buys is
    // that no baseline traffic occurs at all.
    source.AddPackage(MakeIndexFields(s_Package3Id));
    source.Publish(s_ThirdVersion, true);

    REQUIRE(UpdateSource(details.Name, callback));

    REQUIRE(fs::last_write_time(baselinePackage) == baselineWriteTime);

    REQUIRE(GetSourcePackageIds(details.Name) == std::set<std::string>{ s_Package1Id, s_Package2Id, s_Package3Id });
}

TEST_CASE("PIPS_LocalFile_Delta_FallsBackWhenNoDeltaPublished", "[pips][local_file][delta]")
{
    TestHook::SetSingleExperimentalFeature_Override deltaEnabled{ ExperimentalFeature::Feature::DeltaIndex };
    TestHook::SetSourcePackageTrustValidation_Override trustOverride;

    TestPreIndexedSource source{ { MakeIndexFields(s_Package1Id) } };
    source.Publish(s_FirstVersion);

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

    REQUIRE(GetSourcePackageIds(details.Name) == std::set<std::string>{ s_Package1Id });
}

TEST_CASE("PIPS_LocalFile_Delta_FallsBackWhenBaselineVersionDiffers", "[pips][local_file][delta]")
{
    TestHook::SetSingleExperimentalFeature_Override deltaEnabled{ ExperimentalFeature::Feature::DeltaIndex };
    TestHook::SetSourcePackageTrustValidation_Override trustOverride;

    TestPreIndexedSource source{ { MakeIndexFields(s_Package1Id) } };
    source.PublishBaseline();

    source.AddPackage(MakeIndexFields(s_Package2Id));
    source.Publish(s_SecondVersion, true);

    // Whatever is at the location the delta names is not the baseline it was computed against, and
    // the version header says so before anything is downloaded.
    source.RepublishBaselineAtVersion(s_UnexpectedBaselineVersion);

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

    REQUIRE(GetSourcePackageIds(details.Name) == std::set<std::string>{ s_Package1Id, s_Package2Id });
}

TEST_CASE("PIPS_LocalFile_Delta_RemoveClearsEverySlot", "[pips][local_file][delta]")
{
    TestHook::SetSingleExperimentalFeature_Override deltaEnabled{ ExperimentalFeature::Feature::DeltaIndex };
    TestHook::SetSourcePackageTrustValidation_Override trustOverride;

    TestPreIndexedSource source{ { MakeIndexFields(s_Package1Id) } };
    source.PublishBaseline();

    source.AddPackage(MakeIndexFields(s_Package2Id));
    source.Publish(s_SecondVersion, true);

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

// The cases below cover a source moving between the two stores it can be held in. Which store a
// process writes to is decided by whether it is running for an interactively logged on user, so
// the same source on the same machine can legitimately be maintained in both: an interactive
// winget deploys, while a scheduled task cannot and falls back to our own local state.

namespace
{
    // A source added by a process that can deploy, holding the overrides that make both stores
    // reachable for as long as the test needs them.
    struct StoreSwapTest
    {
        // Held off explicitly, so that these cover the stores rather than whatever the machine
        // running the test happens to have enabled.
        TestHook::SetSingleExperimentalFeature_Override DeltaDisabled{ ExperimentalFeature::Feature::DeltaIndex, false };
        TestHook::SetSourcePackageTrustValidation_Override TrustOverride;
        TestHook::SetDeployedPackageStore_Override DeployedStore{ CreateTestDeployedPackageStore };
        TestHook::SetIsRunningAsInteractiveUser_Override Interactive{ true };

        TestPreIndexedSource Source{ { MakeIndexFields(s_Package1Id) } };
        SourceDetails Details;
        TestProgress Callback;

        StoreSwapTest()
        {
            Source.Publish(s_FirstVersion);

            CleanSourcesFor(Source.BaseFamilyName);

            Details = Source.MakeDetails();
            REQUIRE(AddSource(Details, Callback));
        }

        // Publishes a full index at a new version, holding one more package than the last one did.
        void PublishNewVersion(std::string_view version, const std::string& addedPackageId)
        {
            Source.AddPackage(MakeIndexFields(addedPackageId));
            Source.Publish(version);
        }

        bool Update()
        {
            return UpdateSource(Details.Name, Callback);
        }

        std::set<std::string> PackageIds()
        {
            return GetSourcePackageIds(Details.Name);
        }
    };
}

TEST_CASE("PIPS_StoreSwap_WriteFollowsInteractiveUser", "[pips][local_file][store_swap]")
{
    StoreSwapTest test;

    // An interactively logged on user can deploy, so the source is held in the deployed store and
    // our own local state is not used at all.
    fs::path deployedIndex = test.Source.DeployedStatePath() / s_IndexMsixName;
    REQUIRE(fs::exists(deployedIndex));
    REQUIRE(!fs::exists(test.Source.StatePath()));

    std::string deployedContents = GetContents(deployedIndex);

    // A process that cannot deploy maintains the same source in the local file store instead,
    // without disturbing what the other store holds.
    test.PublishNewVersion(s_SecondVersion, s_Package2Id);
    test.Interactive.Set(false);

    REQUIRE(test.Update());

    fs::path localIndex = test.Source.StatePath() / s_IndexMsixName;
    REQUIRE(fs::exists(localIndex));
    REQUIRE(GetContents(deployedIndex) == deployedContents);
    REQUIRE(GetContents(localIndex) != deployedContents);
}

TEST_CASE("PIPS_StoreSwap_ReadFindsFreshestStore", "[pips][local_file][store_swap]")
{
    StoreSwapTest test;

    // The local file store moves ahead of the deployed one.
    test.PublishNewVersion(s_SecondVersion, s_Package2Id);
    test.Interactive.Set(false);
    REQUIRE(test.Update());

    REQUIRE(test.PackageIds() == std::set<std::string>{ s_Package1Id, s_Package2Id });

    // A process that can deploy would write to the deployed store, which holds the older copy.
    // The read follows the newer one rather than the one that this process maintains.
    test.Interactive.Set(true);

    REQUIRE(test.PackageIds() == std::set<std::string>{ s_Package1Id, s_Package2Id });

    // The same in the other direction: the deployed store moves ahead, and a process that cannot
    // deploy reads it rather than the copy that it maintains itself.
    test.PublishNewVersion(s_ThirdVersion, s_Package3Id);
    REQUIRE(test.Update());

    test.Interactive.Set(false);

    REQUIRE(test.PackageIds() == std::set<std::string>{ s_Package1Id, s_Package2Id, s_Package3Id });
}

TEST_CASE("PIPS_StoreSwap_ReadKeepsOwnStoreWhenEqual", "[pips][local_file][store_swap]")
{
    StoreSwapTest test;

    // Both stores are brought to the same version, so neither one is newer than the other.
    test.Interactive.Set(false);
    REQUIRE(test.Update());

    REQUIRE(fs::exists(test.Source.StatePath() / s_IndexMsixName));
    REQUIRE(fs::exists(test.Source.DeployedStatePath() / s_IndexMsixName));

    SourceDetails stored = GetStoredDetails(test.Details.Name);

    PackageKey key;
    key.Slot = PackageSlot::FullIndex;
    key.Identity = test.Source.BaseFamilyName;

    Msix::PackageVersion expectedVersion{ std::string{ s_FirstVersion } };

    // Which store answered is not something a source exposes, so these ask the store directly.
    // Whether a read can be served without the lock is the difference between the two mechanisms,
    // and the composite has to report the answer of the store it resolved to rather than its own.
    {
        // A process that cannot deploy stays on the local file store that it maintains, and that
        // store has to extract from the held file, so it cannot serve a read unlocked.
        auto store = CreateCompositeStore(stored);

        REQUIRE(store->GetVersion(key) == expectedVersion);
        REQUIRE(!store->AllowsUnlockedRead());
    }

    test.Interactive.Set(true);

    {
        // A process that can deploy stays on the deployed store for the same reason, and that one
        // is read in place.
        auto store = CreateCompositeStore(stored);

        REQUIRE(store->GetVersion(key) == expectedVersion);
        REQUIRE(store->AllowsUnlockedRead());
    }
}

TEST_CASE("PIPS_StoreSwap_RemoveClearsBothStores", "[pips][local_file][store_swap]")
{
    StoreSwapTest test;

    test.PublishNewVersion(s_SecondVersion, s_Package2Id);
    test.Interactive.Set(false);
    REQUIRE(test.Update());

    REQUIRE(fs::exists(test.Source.DeployedStatePath()));
    REQUIRE(fs::exists(test.Source.StatePath()));

    REQUIRE(RemoveSource(test.Details.Name, test.Callback));

    // Clearing only the store that this process writes to would leave a copy behind that a later
    // read would still find and answer from.
    REQUIRE(!fs::exists(test.Source.StatePath()));
    REQUIRE(!fs::exists(test.Source.DeployedStatePath()));
}
