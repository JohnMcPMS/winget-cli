// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "pch.h"
#include "Microsoft/PreIndexedPackageSourceFactory.h"
#include "Microsoft/SQLiteIndex.h"
#include "Microsoft/SQLiteIndexSource.h"
#include "SourceUpdateChecks.h"

#include <AppInstallerDateTime.h>
#include <AppInstallerDeployment.h>
#include <AppInstallerDownloader.h>
#include <AppInstallerMsixInfo.h>
#include <winget/ManagedFile.h>
#include <winget/ExperimentalFeature.h>

using namespace std::string_literals;
using namespace std::string_view_literals;

namespace AppInstaller::Repository::Microsoft
{
    namespace
    {
        static constexpr std::string_view s_PreIndexedPackageSourceFactory_PackageFileName = "source.msix"sv;
        static constexpr std::string_view s_PreIndexedPackageSourceFactory_V2_PackageFileName = "source2.msix"sv;
        static constexpr std::string_view s_PreIndexedPackageSourceFactory_DeltaPackageFileName = "delta.msix"sv;
        static constexpr std::string_view s_PreIndexedPackageSourceFactory_LocalBaselineFileName = "baseline.msix"sv;
        static constexpr std::string_view s_PreIndexedPackageSourceFactory_PackageVersionHeader = "x-ms-meta-sourceversion"sv;
        static constexpr std::string_view s_PreIndexedPackageSourceFactory_IndexFileName = "index.db"sv;
        // TODO: This being hard coded to force using the Public directory name is not ideal.
        static constexpr std::string_view s_PreIndexedPackageSourceFactory_IndexFilePath = "Public\\index.db"sv;

        // Construct the package location from the given details.
        // Currently expects that the arg is an https uri pointing to the root of the data.
        std::string GetPackageLocation(const std::string& basePath, std::string_view fileName)
        {
            std::string result = basePath;
            if (result.back() != '/')
            {
                result += '/';
            }
            result += fileName;
            return result;
        }

        // Gets the set of package locations that should be tried, in order.
        // Every relative location is tried against the primary arg before any is tried against the
        // alternate, so that a source stays on its primary location whenever that location can
        // serve the request at all.
        std::vector<std::string> GetPackageLocations(const SourceDetails& details, const std::vector<std::string_view>& relativeLocations)
        {
            THROW_HR_IF(E_INVALIDARG, details.Arg.empty());

            std::vector<std::string> result;

            for (std::string_view relativeLocation : relativeLocations)
            {
                result.emplace_back(GetPackageLocation(details.Arg, relativeLocation));
            }

            if (!details.AlternateArg.empty())
            {
                for (std::string_view relativeLocation : relativeLocations)
                {
                    result.emplace_back(GetPackageLocation(details.AlternateArg, relativeLocation));
                }
            }

            return result;
        }

        // The locations of the full index, which is what a client that is not using a delta acquires.
        std::vector<std::string> GetFullIndexPackageLocations(const SourceDetails& details)
        {
            return GetPackageLocations(details, {
                s_PreIndexedPackageSourceFactory_V2_PackageFileName,
                s_PreIndexedPackageSourceFactory_PackageFileName });
        }

        // The locations of the delta, which is published at a fixed name beside the full index.
        std::vector<std::string> GetDeltaPackageLocations(const SourceDetails& details)
        {
            return GetPackageLocations(details, { s_PreIndexedPackageSourceFactory_DeltaPackageFileName });
        }

        // The locations of the baseline that a delta names.
        //
        // The path is relative to the source base location and travels inside the delta, so the
        // service can version and relocate baselines without a client change, and a client can
        // never pair a delta with a baseline chosen by a stale assumption of its own.
        std::vector<std::string> GetBaselinePackageLocations(const SourceDetails& details, const std::string& relativeSourcePath)
        {
            THROW_HR_IF(E_INVALIDARG, relativeSourcePath.empty());

            return GetPackageLocations(details, { relativeSourcePath });
        }

        // Abstracts the fallback for package location when the MsixInfo is needed.
        struct PreIndexedPackageInfo
        {
            template <typename LocationCheck>
            PreIndexedPackageInfo(std::vector<std::string> potentialLocations, LocationCheck&& locationCheck)
            {
                for (const auto& location : potentialLocations)
                {
                    locationCheck(location);
                }

                std::exception_ptr primaryException;

                for (const auto& location : potentialLocations)
                {
                    try
                    {
                        m_msixInfo = std::make_unique<Msix::MsixInfo>(location);
                        m_packageLocation = location;
                        return;
                    }
                    catch (...)
                    {
                        LOG_CAUGHT_EXCEPTION_MSG("PreIndexedPackageInfo failed on location: %hs", location.c_str());
                        if (!primaryException)
                        {
                            primaryException = std::current_exception();
                        }
                    }
                }

                std::rethrow_exception(primaryException);
            }

            const std::string& PackageLocation() const { return m_packageLocation; }
            Msix::MsixInfo& MsixInfo() { return *m_msixInfo; }

        private:
            std::string m_packageLocation;
            std::unique_ptr<Msix::MsixInfo> m_msixInfo;
        };

        // Abstracts the fallback for package location when an update is being done.
        struct PreIndexedPackageUpdateCheck
        {
            PreIndexedPackageUpdateCheck(std::vector<std::string> potentialLocations)
            {
                std::exception_ptr primaryException;

                for (const auto& location : potentialLocations)
                {
                    try
                    {
                        m_availableVersion = GetAvailableVersionFrom(location);
                        m_packageLocation = location;
                        return;
                    }
                    catch (...)
                    {
                        LOG_CAUGHT_EXCEPTION_MSG("PreIndexedPackageUpdateCheck failed on location: %hs", location.c_str());
                        if (!primaryException)
                        {
                            primaryException = std::current_exception();
                        }
                    }
                }

                std::rethrow_exception(primaryException);
            }

            const std::string& PackageLocation() const { return m_packageLocation; }
            const Msix::PackageVersion& AvailableVersion() const { return m_availableVersion; }

        private:
            std::string m_packageLocation;
            Msix::PackageVersion m_availableVersion;

            Msix::PackageVersion GetAvailableVersionFrom(const std::string& packageLocation)
            {
                if (Utility::IsUrlRemote(packageLocation))
                {
                    std::map<std::string, std::string> headers = Utility::GetHeaders(packageLocation);
                    auto itr = headers.find(std::string{ s_PreIndexedPackageSourceFactory_PackageVersionHeader });
                    if (itr != headers.end())
                    {
                        AICLI_LOG(Repo, Verbose, << "Header indicates version is: " << itr->second);
                        return { itr->second };
                    }

                    // We did not find the header we were looking for, log the ones we did find
                    AICLI_LOG(Repo, Verbose, << "Did not find " << s_PreIndexedPackageSourceFactory_PackageVersionHeader << " in:\n" << [&]()
                        {
                            std::ostringstream headerLog;
                            for (const auto& header : headers)
                            {
                                headerLog << "  " << header.first << " : " << header.second << '\n';
                            }
                            return std::move(headerLog).str();
                        }());
                }

                AICLI_LOG(Repo, Verbose, << "Reading package data to determine version");
                Msix::MsixInfo info{ packageLocation };
                auto manifest = info.GetAppPackageManifests();

                THROW_HR_IF(APPINSTALLER_CLI_ERROR_PACKAGE_IS_BUNDLE, manifest.size() > 1);
                THROW_HR_IF(E_UNEXPECTED, manifest.size() == 0);

                return manifest[0].GetIdentity().GetVersion();
            }
        };

        // Gets the package family name from the details.
        std::string GetPackageFamilyNameFromDetails(const SourceDetails& details)
        {
            THROW_HR_IF(E_UNEXPECTED, details.Data.empty());
            return details.Data;
        }

        // Creates a name for the cross process reader-writer lock given the details.
        std::string CreateNameForCPL(const SourceDetails& details)
        {
            // The only relevant data is the package family name
            return "PreIndexedSourceCPL_"s + GetPackageFamilyNameFromDetails(details);
        }

        // The packages that a source can hold locally.
        //
        // A delta and the baseline it names must both be present at the same time, so each of
        // these occupies its own slot rather than replacing another. In particular the baseline is
        // kept apart from the full index even though the two are the same format: falling back to
        // a full index update must not overwrite the baseline that an already acquired delta is
        // paired with.
        enum class PackageSlot
        {
            // The complete index, as acquired by a client that is not using a delta.
            FullIndex,

            // The changes since the baseline.
            Delta,

            // The index that the delta's changes apply to.
            Baseline,
        };

        // A short token for a slot, used in diagnostics and in temporary file names.
        std::string_view GetSlotName(PackageSlot slot)
        {
            switch (slot)
            {
            case PackageSlot::FullIndex: return "fullIndex"sv;
            case PackageSlot::Delta: return "delta"sv;
            case PackageSlot::Baseline: return "baseline"sv;
            }

            THROW_HR(E_UNEXPECTED);
        }

        // Selects between the two mechanisms for holding the index package locally.
        bool UseDeployedPackage()
        {
            return Runtime::IsRunningInPackagedContext();
        }

        // The file name that a slot's package is stored under by the local file mechanism.
        // We choose these names ourselves, so this mechanism needs no package identity at all.
        std::string_view GetLocalFileNameForSlot(PackageSlot slot)
        {
            switch (slot)
            {
            case PackageSlot::FullIndex: return s_PreIndexedPackageSourceFactory_PackageFileName;
            case PackageSlot::Delta: return s_PreIndexedPackageSourceFactory_DeltaPackageFileName;
            case PackageSlot::Baseline: return s_PreIndexedPackageSourceFactory_LocalBaselineFileName;
            }

            THROW_HR(E_UNEXPECTED);
        }

        // The package family name that identifies a slot's package.
        //
        // Nothing when it is not known, which the deployed mechanism needs it to be: the platform
        // keys installed packages on their identity, so a package whose identity we cannot name
        // cannot be found again in a later process.
        //
        // TODO: Delta acquisition needs a richer SourceDetails::Data syntax, and a migration to it.
        //
        //       Data holds exactly one package family name. That was sufficient while a source had
        //       one package; it is not now. The delta is published under its own identity while the
        //       baseline keeps the identity the full index has always used, so a delta capable
        //       source has two to name and Data can carry only one of them.
        //
        //       The ordering is what makes this awkward. The delta is the package that names its
        //       baseline, so it must be acquired and opened first -- which means the delta's
        //       identity is the one needed before any network access, and it is precisely the one
        //       that Data does not carry.
        //
        //       Three things have to be settled before this can be relied upon:
        //
        //         1. A syntax for Data that carries both identities and can be distinguished from
        //            the bare family name that every client written to date has stored.
        //         2. A migration for sources already configured with the old syntax. Our own
        //            sources can be special cased, since we publish them and know both identities,
        //            but a third party pre-indexed source cannot be.
        //         3. A way to persist the new value. Add is the only operation that writes Data
        //            today: ISourceFactory::Update takes a const SourceDetails&, and the update
        //            path writes only the metadata fields back afterwards, so a value learned
        //            during an update is discarded. That is changeable, but not from this file.
        //
        //       Until then the deployed mechanism can name only the full index and the baseline.
        //       The delta is found within a single acquisition pass, from the package that was
        //       just downloaded, and cannot be found again in a later process. The local file
        //       mechanism is unaffected, because it does not depend on identity.
        std::optional<std::string> GetPackageFamilyNameForSlot(const SourceDetails& details, PackageSlot slot)
        {
            switch (slot)
            {
            case PackageSlot::FullIndex:
            case PackageSlot::Baseline:
                // A baseline is a full index that has been designated as one, published under the
                // identity that the source has always used, so the stored name identifies both.
                return GetPackageFamilyNameFromDetails(details);
            case PackageSlot::Delta:
                return std::nullopt;
            }

            THROW_HR(E_UNEXPECTED);
        }

        // The optimistic deployed open calls this without the cross process lock, and retries under the lock on failure.
        std::optional<Deployment::Extension> GetExtensionForPackageFamilyName(const std::string& packageFamilyName)
        {
            Deployment::ExtensionCatalog catalog(Deployment::SourceExtensionName);
            return catalog.FindByPackageFamilyAndId(packageFamilyName, Deployment::IndexDBId);
        }

        std::optional<Deployment::Extension> GetExtensionFromDetails(const SourceDetails& details)
        {
            return GetExtensionForPackageFamilyName(GetPackageFamilyNameFromDetails(details));
        }

        // Nothing when the slot's identity is not known; see GetPackageFamilyNameForSlot.
        std::optional<Deployment::Extension> GetExtensionForSlot(const SourceDetails& details, PackageSlot slot)
        {
            auto packageFamilyName = GetPackageFamilyNameForSlot(details, slot);
            return packageFamilyName ? GetExtensionForPackageFamilyName(packageFamilyName.value()) : std::nullopt;
        }

        std::optional<Msix::PackageVersion> GetDeployedPackageVersion(const SourceDetails& details, PackageSlot slot)
        {
            auto extension = GetExtensionForSlot(details, slot);

            if (extension)
            {
                auto version = extension->GetPackageVersion();
                return Msix::PackageVersion{ version.Major, version.Minor, version.Build, version.Revision };
            }
            else
            {
                return std::nullopt;
            }
        }

        // Constructs the location that we will write files to.
        std::filesystem::path GetStatePathFromDetails(const SourceDetails& details)
        {
            std::filesystem::path result = Runtime::GetPathTo(Runtime::PathName::LocalState);
            result /= PreIndexedPackageSourceFactory::Type();
            result /= GetPackageFamilyNameFromDetails(details);
            return result;
        }

        std::filesystem::path GetLocalFilePathForSlot(const SourceDetails& details, PackageSlot slot)
        {
            return GetStatePathFromDetails(details) / GetLocalFileNameForSlot(slot);
        }

        std::optional<Msix::PackageVersion> GetLocalFilePackageVersion(const SourceDetails& details, PackageSlot slot)
        {
            std::filesystem::path packagePath = GetLocalFilePathForSlot(details, slot);

            if (std::filesystem::exists(packagePath))
            {
                // If we already have a trusted index package, use it to determine if we need to update or not.
                Msix::WriteLockedMsixFile indexPackage{ packagePath };
                if (indexPackage.ValidateTrustInfo(WI_IsFlagSet(details.TrustLevel, SourceTrustLevel::StoreOrigin)))
                {
                    Msix::MsixInfo msixInfo{ packagePath };
                    auto manifest = msixInfo.GetAppPackageManifests();

                    if (manifest.size() == 1)
                    {
                        return manifest[0].GetIdentity().GetVersion();
                    }
                }
            }

            return std::nullopt;
        }

        // Retrieves the currently held version of a slot's package, or nothing when there is none.
        std::optional<Msix::PackageVersion> GetCurrentVersion(const SourceDetails& details, PackageSlot slot = PackageSlot::FullIndex)
        {
            return UseDeployedPackage() ? GetDeployedPackageVersion(details, slot) : GetLocalFilePackageVersion(details, slot);
        }

        // Describes a package to acquire, and which of the source's slots to put it in.
        struct AcquisitionTarget
        {
            // Which of the source's packages this is.
            PackageSlot Slot = PackageSlot::FullIndex;

            // The remote or local location to acquire it from.
            std::string PackageLocation;

            // The family name that the acquired package is required to carry, when that is known.
            // A delta's identity is not known until it has been acquired, so it is not always.
            std::optional<std::string> ExpectedPackageFamilyName;
        };

        // What acquiring a package produced.
        struct AcquisitionResult
        {
            // The family name of the package that was acquired.
            std::string PackageFamilyName;

            // The bytes transferred, when the package came from a remote location.
            std::optional<uint64_t> DownloadedBytes;
        };

        // Makes the index database of a package that we hold locally readable.
        //
        // The deployed mechanism already has it on disk inside the installed package; the local
        // file mechanism has to extract it from the package file, so the result may own a
        // temporary file that must outlive any read of the path.
        struct ExtractedIndex
        {
            std::filesystem::path Path;
            Utility::ManagedFile TemporaryFile;
        };

        std::optional<ExtractedIndex> ExtractIndexForSlot(const SourceDetails& details, PackageSlot slot, const std::string& packageFamilyName, IProgressCallback& progress)
        {
            ExtractedIndex result;

            if (UseDeployedPackage())
            {
                auto extension = GetExtensionForPackageFamilyName(packageFamilyName);
                if (!extension)
                {
                    AICLI_LOG(Repo, Info, << "Deployed " << GetSlotName(slot) << " package not found: " << packageFamilyName);
                    return std::nullopt;
                }

                // See the note in OpenDeployedIndex on constructing this location ourselves.
                result.Path = extension->GetPackagePath();
                result.Path /= s_PreIndexedPackageSourceFactory_IndexFilePath;
            }
            else
            {
                std::filesystem::path packagePath = GetLocalFilePathForSlot(details, slot);
                if (!std::filesystem::exists(packagePath))
                {
                    AICLI_LOG(Repo, Info, << "Data not found at " << packagePath);
                    return std::nullopt;
                }

                auto tempIndexFilePath = Runtime::GetNewTempFilePath();
                auto tempIndexFile = Utility::ManagedFile::CreateWriteLockedFile(tempIndexFilePath, GENERIC_WRITE, true);

                Msix::MsixInfo packageInfo{ packagePath };
                packageInfo.WriteToFileHandle(s_PreIndexedPackageSourceFactory_IndexFilePath, tempIndexFile.GetFileHandle(), progress);

                result.Path = tempIndexFile.GetFilePath();
                result.TemporaryFile = std::move(tempIndexFile);
            }

            return std::optional<ExtractedIndex>{ std::move(result) };
        }

        // Reads the baseline that a delta we hold names, from the delta itself.
        //
        // Nothing when the delta cannot be read or does not carry a complete locator; the caller
        // falls back to the full index rather than guessing at a baseline of its own.
        std::optional<SQLiteIndex::DeltaBaselineLocator> ReadBaselineLocatorFromDelta(
            const SourceDetails& details,
            const std::string& deltaPackageFamilyName,
            IProgressCallback& progress)
        {
            auto extracted = ExtractIndexForSlot(details, PackageSlot::Delta, deltaPackageFamilyName, progress);
            if (!extracted)
            {
                return std::nullopt;
            }

            // Opened as an index rather than as a bare database so that the schema version selects
            // the interface that knows what a delta records; reading the metadata here would be
            // this layer asserting a specific schema version's layout.
            SQLiteIndex deltaIndex = SQLiteIndex::Open(extracted->Path.u8string(), SQLiteIndex::OpenDisposition::Immutable);

            return deltaIndex.GetDeltaBaselineLocator();
        }

        struct SourceOpenTimer
        {
            using clock = std::chrono::steady_clock;

            struct SingleTimer
            {
                SingleTimer(long long& durationMs) : m_durationMs(durationMs), m_start(clock::now()) {}

                ~SingleTimer()
                {
                    Stop();
                }

                void Stop()
                {
                    if (!m_stopped)
                    {
                        m_durationMs += std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - m_start).count();
                        m_stopped = true;
                    }
                }

            private:
                long long& m_durationMs;
                clock::time_point m_start;
                bool m_stopped = false;
            };

            SourceOpenTimer(const std::string& sourceName) : m_sourceName(sourceName), m_start(clock::now()) {}

            ~SourceOpenTimer()
            {
                const auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - m_start).count();
                AICLI_LOG(Repo, Info, << "Packaged source open for '" << m_sourceName << "' " << (m_succeeded ? "succeeded" : "failed") <<
                    " in " << totalMs << " ms [extensionLookup=" << m_extensionLookupMs <<
                    " ms, verifyContentIntegrity=" << m_verifyContentIntegrityMs <<
                    " ms, sqliteOpen=" << m_sqliteOpenMs << " ms, mode=" << (m_usedLockFallback ? "fallbackLocked" : "optimistic") << "]");
            }

            SingleTimer MeasureExtensionLookup() { return SingleTimer{ m_extensionLookupMs }; }
            SingleTimer MeasureVerifyContentIntegrity() { return SingleTimer{ m_verifyContentIntegrityMs }; }
            SingleTimer MeasureSQLiteOpen() { return SingleTimer{ m_sqliteOpenMs }; }
            void MarkFallbackLocked() { m_usedLockFallback = true; }
            void MarkSucceeded() { m_succeeded = true; }

        private:
            const std::string& m_sourceName;
            clock::time_point m_start;
            bool m_succeeded = false;
            bool m_usedLockFallback = false;
            long long m_extensionLookupMs = 0;
            long long m_verifyContentIntegrityMs = 0;
            long long m_sqliteOpenMs = 0;
        };

        SQLiteIndex OpenDeployedIndex(const SourceDetails& details, IProgressCallback& progress, SourceOpenTimer& openTimer)
        {
            auto extensionLookupTimer = openTimer.MeasureExtensionLookup();
            auto extension = GetExtensionFromDetails(details);
            extensionLookupTimer.Stop();
            if (!extension)
            {
                AICLI_LOG(Repo, Info, << "Package not found " << details.Data);
                THROW_HR(APPINSTALLER_CLI_ERROR_SOURCE_DATA_MISSING);
            }

            auto verifyTimer = openTimer.MeasureVerifyContentIntegrity();
            THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NEEDS_REMEDIATION), !extension->VerifyContentIntegrity(progress));
            verifyTimer.Stop();

            // To work around an issue with accessing the public folder, we are temporarily
            // constructing the location ourself.  This was already the case for the non-packaged
            // runtime, and we can fix both in the future.  The only problem with this is that
            // the directory in the extension *must* be Public, rather than one set by the creator.
            std::filesystem::path indexLocation = extension->GetPackagePath();
            indexLocation /= s_PreIndexedPackageSourceFactory_IndexFilePath;

            auto sqliteOpenTimer = openTimer.MeasureSQLiteOpen();
            auto index = SQLiteIndex::Open(indexLocation.u8string(), SQLiteIndex::OpenDisposition::Immutable);
            sqliteOpenTimer.Stop();

            return index;
        }

        // A reference to a preindexed package source.
        struct PreIndexedSourceReference : public ISourceReference
        {
            PreIndexedSourceReference(const SourceDetails& details) : m_details(details)
            {
                if (!m_details.Data.empty())
                {
                    m_details.Identifier = GetPackageFamilyNameFromDetails(details);
                }
            }

            std::string GetIdentifier() override { return m_details.Identifier; }

            SourceDetails& GetDetails() override { return m_details; };

            bool ShouldUpdateBeforeOpen(const std::optional<TimeSpan>& requestedUpdateInterval) override
            {
                auto currentVersion = GetCurrentVersion(m_details);

                // If we can't find a good package, then we have to update to operate
                if (!currentVersion)
                {
                    AICLI_LOG(Repo, Verbose, << "Source `" << m_details.Name << "` has no data");
                    return true;
                }

                using namespace std::chrono_literals;
                using clock = std::chrono::system_clock;

                // Attempt to convert the package version to a time_point
                clock::time_point versionTime = Utility::GetTimePointFromVersion(currentVersion.value());

                // Since we expect that the version time indicates creation time, don't let it be far in the future.
                auto now = clock::now();
                if (versionTime > now && versionTime - now > 24h)
                {
                    versionTime = clock::time_point::min();
                }

                // Use the later of the version and last update times
                clock::time_point timeToCheck = (versionTime > m_details.LastUpdateTime ? versionTime : m_details.LastUpdateTime);

                return IsAfterUpdateCheckTime(m_details.Name, timeToCheck, requestedUpdateInterval);
            }

            std::shared_ptr<ISource> Open(IProgressCallback& progress) override
            {
                // TODO: Open still reads only the full index. Pairing an acquired delta with its
                //       baseline through SQLiteIndex::OpenWithBaseline, including validating that
                //       the baseline carries the identifier the delta names, is the next step.
                return UseDeployedPackage() ? OpenDeployed(progress) : OpenLocalFile(progress);
            }

        private:
            SourceDetails m_details;

            // Reads the index out of the location that the deployed package was installed to.
            std::shared_ptr<ISource> OpenDeployed(IProgressCallback& progress)
            {
                SourceOpenTimer openTimer{ m_details.Name };
                auto completeOpen = [&](SQLiteIndex index)
                    {
                        // We didn't use to store the source identifier, so we compute it here in case it's
                        // missing from the details.
                        m_details.Identifier = GetPackageFamilyNameFromDetails(m_details);
                        openTimer.MarkSucceeded();
                        return std::make_shared<SQLiteIndexSource>(m_details, std::move(index), false, true);
                    };

                std::optional<SQLiteIndex> index;
                bool retryUnderLock = false;

                try
                {
                    index.emplace(OpenDeployedIndex(m_details, progress, openTimer));
                }
                catch (...)
                {
                    if (progress.IsCancelledBy(CancelReason::Any))
                    {
                        throw;
                    }

                    LOG_CAUGHT_EXCEPTION_MSG("Optimistic packaged source open failed, retrying under lock for source: %hs", m_details.Name.c_str());
                    retryUnderLock = true;
                }

                if (retryUnderLock)
                {
                    openTimer.MarkFallbackLocked();
                    Synchronization::CrossProcessLock lock(CreateNameForCPL(m_details));
                    if (!lock.Acquire(progress))
                    {
                        return {};
                    }

                    index.emplace(OpenDeployedIndex(m_details, progress, openTimer));
                }

                return completeOpen(std::move(index.value()));
            }

            // Extracts the index out of the package file held in local state.
            std::shared_ptr<ISource> OpenLocalFile(IProgressCallback& progress)
            {
                Synchronization::CrossProcessLock lock(CreateNameForCPL(m_details));
                if (!lock.Acquire(progress))
                {
                    return {};
                }

                std::filesystem::path packageLocation = GetLocalFilePathForSlot(m_details, PackageSlot::FullIndex);

                if (!std::filesystem::exists(packageLocation))
                {
                    AICLI_LOG(Repo, Info, << "Data not found at " << packageLocation);
                    THROW_HR(APPINSTALLER_CLI_ERROR_SOURCE_DATA_MISSING);
                }

                // Put a write exclusive lock on the index package.
                Msix::WriteLockedMsixFile indexPackage{ packageLocation };

                // Validate index package trust info.
                THROW_HR_IF(APPINSTALLER_CLI_ERROR_SOURCE_DATA_INTEGRITY_FAILURE, !indexPackage.ValidateTrustInfo(WI_IsFlagSet(m_details.TrustLevel, SourceTrustLevel::StoreOrigin)));

                // Create a temp lock exclusive index file.
                auto tempIndexFilePath = Runtime::GetNewTempFilePath();
                auto tempIndexFile = Utility::ManagedFile::CreateWriteLockedFile(tempIndexFilePath, GENERIC_WRITE, true);

                // Populate temp index file.
                Msix::MsixInfo packageInfo(packageLocation);
                packageInfo.WriteToFileHandle(s_PreIndexedPackageSourceFactory_IndexFilePath, tempIndexFile.GetFileHandle(), progress);

                if (progress.IsCancelledBy(CancelReason::Any))
                {
                    AICLI_LOG(Repo, Info, << "Cancelling open upon request");
                    return {};
                }

                SQLiteIndex index = SQLiteIndex::Open(tempIndexFile.GetFilePath().u8string(), SQLiteIndex::OpenDisposition::Immutable, std::move(tempIndexFile));

                // We didn't use to store the source identifier, so we compute it here in case it's
                // missing from the details.
                m_details.Identifier = GetPackageFamilyNameFromDetails(m_details);
                return std::make_shared<SQLiteIndexSource>(m_details, std::move(index), false, true);
            }
        };

        // The factory for a preindexed package source.
        struct PreIndexedFactory : public ISourceFactory
        {
            std::string_view TypeName() const override
            {
                return PreIndexedPackageSourceFactory::Type();
            }

            std::shared_ptr<ISourceReference> Create(const SourceDetails& details) override
            {
                // With more than one source implementation, we will probably need to probe first
                THROW_HR_IF(E_INVALIDARG, !details.Type.empty() && details.Type != PreIndexedPackageSourceFactory::Type());

                return std::make_shared<PreIndexedSourceReference>(details);
            }

            bool Add(SourceDetails& details, IProgressCallback& progress) override
            {
                if (details.Type.empty())
                {
                    // With more than one source implementation, we will probably need to probe first
                    details.Type = PreIndexedPackageSourceFactory::Type();
                    AICLI_LOG(Repo, Info, << "Initializing source type: " << details.Name << " => " << details.Type);
                }
                else
                {
                    THROW_HR_IF(E_INVALIDARG, details.Type != PreIndexedPackageSourceFactory::Type());
                }

                PreIndexedPackageInfo packageInfo(GetFullIndexPackageLocations(details), [](const std::string& packageLocation)
                    {
                        THROW_HR_IF(APPINSTALLER_CLI_ERROR_SOURCE_NOT_SECURE, Utility::IsUrlRemote(packageLocation) && !Utility::IsUrlSecure(packageLocation));
                    });

                AICLI_LOG(Repo, Info, << "Initializing source from: " << details.Name << " => " << packageInfo.PackageLocation());

                THROW_HR_IF(APPINSTALLER_CLI_ERROR_PACKAGE_IS_BUNDLE, packageInfo.MsixInfo().GetIsBundle());

                auto fullName = packageInfo.MsixInfo().GetPackageFullName();
                AICLI_LOG(Repo, Info, << "Found package full name: " << details.Name << " => " << fullName);

                // TODO: Add remains on the full index, because it is the only operation that can
                //       establish details.Data and the data syntax that can name a delta as well
                //       has not been settled. See GetPackageFamilyNameForSlot. Until it
                //       is, a newly added source acquires the full index and only later updates
                //       pick up a delta.
                details.Data = Msix::GetPackageFamilyNameFromFullName(fullName);
                details.Identifier = Msix::GetPackageFamilyNameFromFullName(fullName);

                auto lock = LockExclusive(details, progress);
                if (!lock)
                {
                    return false;
                }

                AcquisitionResult acquisitionResult;
                bool result = UpdateInternal({ PackageSlot::FullIndex, packageInfo.PackageLocation(), GetPackageFamilyNameForSlot(details, PackageSlot::FullIndex) }, details, progress, acquisitionResult);

                if (acquisitionResult.DownloadedBytes)
                {
                    try
                    {
                        auto manifests = packageInfo.MsixInfo().GetAppPackageManifests();
                        if (!manifests.empty())
                        {
                            Logging::Telemetry().LogPreindexedPackageUpdate(
                                details.Identifier,
                                std::nullopt,
                                Utility::GetTimePointFromVersion(manifests[0].GetIdentity().GetVersion()),
                                false,
                                std::nullopt,
                                std::nullopt,
                                false,
                                acquisitionResult.DownloadedBytes.value(),
                                true);
                        }
                    }
                    CATCH_LOG();
                }

                return result;
            }

            bool Update(const SourceDetails& details, IProgressCallback& progress) override
            {
                return UpdateBase(details, false, progress);
            }

            bool BackgroundUpdate(const SourceDetails& details, IProgressCallback& progress) override
            {
                return UpdateBase(details, true, progress);
            }

            // Places the acquired package into whichever store the mechanism in use reads from.
            bool UpdateInternal(const AcquisitionTarget& target, const SourceDetails& details, IProgressCallback& progress, AcquisitionResult& result)
            {
                return UseDeployedPackage() ?
                    UpdateDeployedPackage(target, details, progress, result) :
                    UpdateLocalFilePackage(target, details, progress, result);
            }

            bool Remove(const SourceDetails& details, IProgressCallback& progress) override
            {
                THROW_HR_IF(E_INVALIDARG, details.Type != PreIndexedPackageSourceFactory::Type());
                auto lock = LockExclusive(details, progress);
                if (!lock)
                {
                    return false;
                }

                return UseDeployedPackage() ? RemoveDeployedPackage(details, progress) : RemoveLocalFilePackage(details, progress);
            }

        private:
            Synchronization::CrossProcessLock LockExclusive(const SourceDetails& details, IProgressCallback& progress, bool isBackground = false)
            {
                Synchronization::CrossProcessLock result(CreateNameForCPL(details));

                if (isBackground)
                {
                    // If this is a background update, don't wait on the lock.
                    result.TryAcquireNoWait();
                }
                else
                {
                    result.Acquire(progress);
                }

                return result;
            }

            bool UpdateBase(const SourceDetails& details, bool isBackground, IProgressCallback& progress)
            {
                THROW_HR_IF(E_INVALIDARG, details.Type != PreIndexedPackageSourceFactory::Type());

                if (Settings::ExperimentalFeature::IsEnabled(Settings::ExperimentalFeature::Feature::DeltaIndex))
                {
                    try
                    {
                        if (UpdateViaDelta(details, isBackground, progress))
                        {
                            return true;
                        }
                    }
                    catch (...)
                    {
                        if (progress.IsCancelledBy(CancelReason::Any))
                        {
                            throw;
                        }

                        LOG_CAUGHT_EXCEPTION_MSG("Delta update failed for source: %hs", details.Name.c_str());
                    }

                    if (progress.IsCancelledBy(CancelReason::Any))
                    {
                        AICLI_LOG(Repo, Info, << "Cancelling update upon request");
                        return false;
                    }

                    // Every delta failure falls back to the full index; the delta is an
                    // optimization, never a correctness dependency.
                    AICLI_LOG(Repo, Info, << "Falling back to the full index for source: " << details.Name);
                }

                return UpdateFullIndex(details, isBackground, progress);
            }

            bool UpdateFullIndex(const SourceDetails& details, bool isBackground, IProgressCallback& progress)
            {
                std::optional<Msix::PackageVersion> currentVersion = GetCurrentVersion(details);
                PreIndexedPackageUpdateCheck updateCheck(GetFullIndexPackageLocations(details));

                if (currentVersion)
                {
                    if (currentVersion.value() >= updateCheck.AvailableVersion())
                    {
                        AICLI_LOG(Repo, Verbose, << "Remote source data (" << updateCheck.AvailableVersion().ToString() <<
                            ") was not newer than existing (" << currentVersion.value().ToString() << "), no update needed");
                        return true;
                    }
                    else
                    {
                        AICLI_LOG(Repo, Verbose, << "Remote source data (" << updateCheck.AvailableVersion().ToString() <<
                            ") was newer than existing (" << currentVersion.value().ToString() << "), updating");
                    }
                }

                if (progress.IsCancelledBy(CancelReason::Any))
                {
                    AICLI_LOG(Repo, Info, << "Cancelling update upon request");
                    return false;
                }

                auto lock = LockExclusive(details, progress, isBackground);
                if (!lock)
                {
                    return false;
                }

                AcquisitionResult acquisitionResult;
                bool result = UpdateInternal({ PackageSlot::FullIndex, updateCheck.PackageLocation(), GetPackageFamilyNameForSlot(details, PackageSlot::FullIndex) }, details, progress, acquisitionResult);

                {
                    std::optional<std::chrono::system_clock::time_point> previousIndexPublishedAt;
                    if (currentVersion)
                    {
                        previousIndexPublishedAt = Utility::GetTimePointFromVersion(currentVersion.value());
                    }

                    Logging::Telemetry().LogPreindexedPackageUpdate(
                        details.Identifier,
                        previousIndexPublishedAt,
                        Utility::GetTimePointFromVersion(updateCheck.AvailableVersion()),
                        false,
                        std::nullopt,
                        std::nullopt,
                        false,
                        acquisitionResult.DownloadedBytes.value_or(0),
                        !isBackground);
                }

                return result;
            }

            // Acquires the delta and, when the one we hold is not the one it names, its baseline.
            //
            // Returns true when the source holds a usable delta and baseline pair afterwards, and
            // false when the caller must fall back to the full index.
            bool UpdateViaDelta(const SourceDetails& details, bool isBackground, IProgressCallback& progress)
            {
                // TODO: With the deployed mechanism this is always nothing, because the delta's
                //       identity is not stored anywhere that survives the process. Until the
                //       details data syntax carries it, a deployed source re-acquires the delta on
                //       every update rather than recognizing the one it already holds.
                std::optional<Msix::PackageVersion> currentDeltaVersion = GetCurrentVersion(details, PackageSlot::Delta);

                // The delta is published at a fixed name beside the full index, and is probed by
                // exactly the same mechanism.
                PreIndexedPackageUpdateCheck deltaCheck(GetDeltaPackageLocations(details));

                bool deltaIsCurrent = currentDeltaVersion && currentDeltaVersion.value() >= deltaCheck.AvailableVersion();

                // The delta names the baseline that its changes apply to; a client never chooses
                // one for itself.
                std::optional<SQLiteIndex::DeltaBaselineLocator> locator;

                if (deltaIsCurrent)
                {
                    // The delta we hold is current, but we may still be missing the baseline that
                    // it names, so being up to date is not on its own a reason to stop.
                    locator = ReadBaselineLocatorFromDelta(details, {}, progress);

                    if (locator)
                    {
                        auto heldBaselineVersion = GetCurrentVersion(details, PackageSlot::Baseline);

                        if (heldBaselineVersion && heldBaselineVersion.value() == Msix::PackageVersion{ locator->PackageVersion })
                        {
                            AICLI_LOG(Repo, Verbose, << "Remote delta (" << deltaCheck.AvailableVersion().ToString() <<
                                ") was not newer than existing (" << currentDeltaVersion.value().ToString() <<
                                ") and its baseline is held, no update needed");
                            return true;
                        }
                    }
                }

                // Re-acquire the delta when it is not current, and also when we could not read the
                // one we hold -- in that case what we hold is unusable whatever its version says.
                bool acquireDelta = !deltaIsCurrent || !locator;

                if (progress.IsCancelledBy(CancelReason::Any))
                {
                    AICLI_LOG(Repo, Info, << "Cancelling update upon request");
                    return false;
                }

                auto lock = LockExclusive(details, progress, isBackground);
                if (!lock)
                {
                    return false;
                }

                AcquisitionResult deltaResult;

                if (acquireDelta)
                {
                    if (!UpdateInternal(
                        { PackageSlot::Delta, deltaCheck.PackageLocation(), GetPackageFamilyNameForSlot(details, PackageSlot::Delta) },
                        details, progress, deltaResult))
                    {
                        return false;
                    }

                    locator = ReadBaselineLocatorFromDelta(details, deltaResult.PackageFamilyName, progress);
                }

                if (!locator)
                {
                    AICLI_LOG(Repo, Warning, << "Delta for source `" << details.Name << "` did not name a baseline");
                    return false;
                }

                AICLI_LOG(Repo, Info, << "Delta for source `" << details.Name << "` names baseline " << locator->Identifier <<
                    " at `" << locator->RelativeSourcePath << "` version " << locator->PackageVersion);

                Msix::PackageVersion requiredBaselineVersion{ locator->PackageVersion };
                std::optional<Msix::PackageVersion> currentBaselineVersion = GetCurrentVersion(details, PackageSlot::Baseline);

                AcquisitionResult baselineResult;
                bool baselineUpdated = false;

                if (currentBaselineVersion && currentBaselineVersion.value() == requiredBaselineVersion)
                {
                    // The common case: the baseline changes far less often than the delta, so most
                    // updates involve no baseline traffic at all.
                    AICLI_LOG(Repo, Verbose, << "Already holding baseline version " << requiredBaselineVersion.ToString());
                }
                else
                {
                    // Probe for the baseline the same way the full index is probed, so that the
                    // Arg / AlternateArg fallback applies to it as well.
                    PreIndexedPackageUpdateCheck baselineCheck(GetBaselinePackageLocations(details, locator->RelativeSourcePath));

                    // The delta told us which version it was computed against; anything else at
                    // that location is not the baseline this delta can be paired with.
                    if (baselineCheck.AvailableVersion() != requiredBaselineVersion)
                    {
                        AICLI_LOG(Repo, Warning, << "Baseline at `" << baselineCheck.PackageLocation() << "` was version " <<
                            baselineCheck.AvailableVersion().ToString() << ", but the delta named " << requiredBaselineVersion.ToString());
                        return false;
                    }

                    if (!UpdateInternal(
                        { PackageSlot::Baseline, baselineCheck.PackageLocation(), GetPackageFamilyNameForSlot(details, PackageSlot::Baseline) },
                        details, progress, baselineResult))
                    {
                        return false;
                    }

                    baselineUpdated = true;
                }

                // TODO: Validate that the acquired baseline carries the identifier that the delta
                //       names, and open the two as a pair. Pairing happens in the open path, which
                //       still opens only the full index; until it does, this acquisition has no
                //       consumer and the feature gate keeps it unreachable.

                std::optional<uint64_t> downloadedBytes;
                if (deltaResult.DownloadedBytes || baselineResult.DownloadedBytes)
                {
                    downloadedBytes = deltaResult.DownloadedBytes.value_or(0) + baselineResult.DownloadedBytes.value_or(0);
                }

                if (downloadedBytes)
                {
                    std::optional<std::chrono::system_clock::time_point> previousIndexPublishedAt;
                    if (currentDeltaVersion)
                    {
                        previousIndexPublishedAt = Utility::GetTimePointFromVersion(currentDeltaVersion.value());
                    }

                    std::optional<std::chrono::system_clock::time_point> previousBaselinePublishedAt;
                    if (currentBaselineVersion)
                    {
                        previousBaselinePublishedAt = Utility::GetTimePointFromVersion(currentBaselineVersion.value());
                    }

                    Logging::Telemetry().LogPreindexedPackageUpdate(
                        details.Identifier,
                        previousIndexPublishedAt,
                        Utility::GetTimePointFromVersion(deltaCheck.AvailableVersion()),
                        true,
                        previousBaselinePublishedAt,
                        Utility::GetTimePointFromVersion(requiredBaselineVersion),
                        baselineUpdated,
                        downloadedBytes.value(),
                        !isBackground);
                }

                return true;
            }

            bool UpdateDeployedPackage(const AcquisitionTarget& target, const SourceDetails& details, IProgressCallback& progress, AcquisitionResult& result)
            {
                // Due to complications with deployment, download the file and deploy from
                // a local source while we investigate further.
                bool download = Utility::IsUrlRemote(target.PackageLocation);
                std::filesystem::path localFile;

                if (download)
                {
                    localFile = Runtime::GetPathTo(Runtime::PathName::Temp);
                    localFile /= GetPackageFamilyNameFromDetails(details) + "." + std::string{ GetSlotName(target.Slot) } + ".msix";

                    auto downloadResult = Utility::Download(target.PackageLocation, localFile, Utility::DownloadType::Index, progress);
                    result.DownloadedBytes = downloadResult.SizeInBytes;
                }
                else
                {
                    localFile = Utility::ConvertToUTF16(target.PackageLocation);
                }

                // Verify the local file
                Msix::WriteLockedMsixFile fileLock{ localFile };
                Msix::MsixInfo localMsixInfo{ localFile };

                // The package should not be a bundle
                THROW_HR_IF(APPINSTALLER_CLI_ERROR_PACKAGE_IS_BUNDLE, localMsixInfo.GetIsBundle());

                result.PackageFamilyName = Msix::GetPackageFamilyNameFromFullName(localMsixInfo.GetPackageFullName());

                // Ensure that the family name is the one we expected, when we knew what to expect
                THROW_HR_IF(APPINSTALLER_CLI_ERROR_SOURCE_DATA_INTEGRITY_FAILURE,
                    target.ExpectedPackageFamilyName && target.ExpectedPackageFamilyName.value() != result.PackageFamilyName);

                if (!fileLock.ValidateTrustInfo(WI_IsFlagSet(details.TrustLevel, SourceTrustLevel::StoreOrigin)))
                {
                    AICLI_LOG(Repo, Error, << "Source update failed. Source package failed trust validation.");
                    THROW_HR(APPINSTALLER_CLI_ERROR_SOURCE_DATA_INTEGRITY_FAILURE);
                }

                winrt::Windows::Foundation::Uri uri = winrt::Windows::Foundation::Uri(localFile.c_str());
                Deployment::AddPackage(
                    uri,
                    Deployment::Options{ WI_IsFlagSet(details.TrustLevel, SourceTrustLevel::Trusted) },
                    progress);

                if (download)
                {
                    try
                    {
                        // If successful, delete the file
                        std::filesystem::remove(localFile);
                    }
                    CATCH_LOG();
                }

                return true;
            }

            bool RemoveDeployedPackage(const SourceDetails& details, IProgressCallback& callback)
            {
                // TODO: This removes only the identity that details.Data names, which covers the
                //       full index and the baseline but not the delta. Removing a delta capable
                //       source will leave its deployed delta package behind until the data syntax
                //       carries that identity; see GetPackageFamilyNameForSlot.
                auto fullName = Msix::GetPackageFullNameFromFamilyName(GetPackageFamilyNameFromDetails(details));

                if (!fullName)
                {
                    AICLI_LOG(Repo, Info, << "No full name found for family name: " << GetPackageFamilyNameFromDetails(details));
                }
                else
                {
                    AICLI_LOG(Repo, Info, << "Removing package: " << *fullName);
                    Deployment::RemovePackage(*fullName, winrt::Windows::Management::Deployment::RemovalOptions::None, callback);
                }

                return true;
            }

            bool UpdateLocalFilePackage(const AcquisitionTarget& target, const SourceDetails& details, IProgressCallback& progress, AcquisitionResult& result)
            {
                // We will extract the manifest and index files directly to this location
                std::filesystem::path packageState = GetStatePathFromDetails(details);
                std::filesystem::create_directories(packageState);

                std::filesystem::path packagePath = packageState / GetLocalFileNameForSlot(target.Slot);

                std::filesystem::path tempPackagePath = packagePath.u8string() + ".dnld.msix";
                auto removeTempFileOnExit = wil::scope_exit([&]()
                    {
                        try
                        {
                            std::filesystem::remove(tempPackagePath);
                        }
                        catch (...)
                        {
                            AICLI_LOG(Repo, Info, << "Failed to remove temp index file at: " << tempPackagePath);
                        }
                    });

                if (Utility::IsUrlRemote(target.PackageLocation))
                {
                    auto downloadResult = AppInstaller::Utility::Download(target.PackageLocation, tempPackagePath, AppInstaller::Utility::DownloadType::Index, progress);
                    result.DownloadedBytes = downloadResult.SizeInBytes;
                }
                else
                {
                    std::filesystem::copy(target.PackageLocation, tempPackagePath);
                    progress.OnProgress(100, 100, ProgressType::Percent);
                }

                if (progress.IsCancelledBy(CancelReason::Any))
                {
                    AICLI_LOG(Repo, Info, << "Cancelling update upon request");
                    return false;
                }

                {
                    // Extra scope to release the file lock right after trust validation.
                    Msix::WriteLockedMsixFile tempIndexPackage{ tempPackagePath };
                    Msix::MsixInfo tempMsixInfo{ tempPackagePath };

                    // The package should not be a bundle
                    THROW_HR_IF(APPINSTALLER_CLI_ERROR_PACKAGE_IS_BUNDLE, tempMsixInfo.GetIsBundle());

                    result.PackageFamilyName = Msix::GetPackageFamilyNameFromFullName(tempMsixInfo.GetPackageFullName());

                    // Ensure that the family name is the one we expected, when we knew what to expect
                    THROW_HR_IF(APPINSTALLER_CLI_ERROR_SOURCE_DATA_INTEGRITY_FAILURE,
                        target.ExpectedPackageFamilyName && target.ExpectedPackageFamilyName.value() != result.PackageFamilyName);

                    if (!tempIndexPackage.ValidateTrustInfo(WI_IsFlagSet(details.TrustLevel, SourceTrustLevel::StoreOrigin)))
                    {
                        AICLI_LOG(Repo, Error, << "Source update failed. Source package failed trust validation.");
                        THROW_HR(APPINSTALLER_CLI_ERROR_SOURCE_DATA_INTEGRITY_FAILURE);
                    }
                }

                std::filesystem::rename(tempPackagePath, packagePath);
                AICLI_LOG(Repo, Info, << "Source update success.");

                removeTempFileOnExit.release();

                return true;
            }

            bool RemoveLocalFilePackage(const SourceDetails& details, IProgressCallback&)
            {
                std::filesystem::path packageState = GetStatePathFromDetails(details);

                if (!std::filesystem::exists(packageState))
                {
                    AICLI_LOG(Repo, Info, << "No state found for source: " << packageState.u8string());
                }
                else
                {
                    AICLI_LOG(Repo, Info, << "Removing state found for source: " << packageState.u8string());
                    std::filesystem::remove_all(packageState);
                }

                return true;
            }
        };
    }

    std::unique_ptr<ISourceFactory> PreIndexedPackageSourceFactory::Create()
    {
        return std::make_unique<PreIndexedFactory>();
    }
}
