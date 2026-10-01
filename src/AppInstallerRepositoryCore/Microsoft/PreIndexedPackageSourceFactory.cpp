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
        std::vector<std::string> GetPackageLocations(const SourceDetails& details)
        {
            THROW_HR_IF(E_INVALIDARG, details.Arg.empty());

            std::vector<std::string> result;

            result.emplace_back(GetPackageLocation(details.Arg, s_PreIndexedPackageSourceFactory_V2_PackageFileName));
            result.emplace_back(GetPackageLocation(details.Arg, s_PreIndexedPackageSourceFactory_PackageFileName));

            if (!details.AlternateArg.empty())
            {
                result.emplace_back(GetPackageLocation(details.AlternateArg, s_PreIndexedPackageSourceFactory_V2_PackageFileName));
                result.emplace_back(GetPackageLocation(details.AlternateArg, s_PreIndexedPackageSourceFactory_PackageFileName));
            }

            return result;
        }

        // Abstracts the fallback for package location when the MsixInfo is needed.
        struct PreIndexedPackageInfo
        {
            template <typename LocationCheck>
            PreIndexedPackageInfo(const SourceDetails& details, LocationCheck&& locationCheck)
            {
                std::vector<std::string> potentialLocations = GetPackageLocations(details);

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
            PreIndexedPackageUpdateCheck(const SourceDetails& details)
            {
                std::vector<std::string> potentialLocations = GetPackageLocations(details);

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

        // Selects between the two mechanisms for holding the index package locally.
        bool UseDeployedPackage()
        {
            return Runtime::IsRunningInPackagedContext();
        }

        // The optimistic deployed open calls this without the cross process lock, and retries under the lock on failure.
        std::optional<Deployment::Extension> GetExtensionFromDetails(const SourceDetails& details)
        {
            Deployment::ExtensionCatalog catalog(Deployment::SourceExtensionName);
            return catalog.FindByPackageFamilyAndId(GetPackageFamilyNameFromDetails(details), Deployment::IndexDBId);
        }

        std::optional<Msix::PackageVersion> GetDeployedPackageVersion(const SourceDetails& details)
        {
            auto extension = GetExtensionFromDetails(details);

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

        std::optional<Msix::PackageVersion> GetLocalFilePackageVersion(const SourceDetails& details)
        {
            std::filesystem::path packageState = GetStatePathFromDetails(details);
            std::filesystem::path packagePath = packageState / s_PreIndexedPackageSourceFactory_PackageFileName;

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

        // Retrieves the currently held version of the index package, or nothing when there is none.
        std::optional<Msix::PackageVersion> GetCurrentVersion(const SourceDetails& details)
        {
            return UseDeployedPackage() ? GetDeployedPackageVersion(details) : GetLocalFilePackageVersion(details);
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

                std::filesystem::path packageLocation = GetStatePathFromDetails(m_details);
                packageLocation /= s_PreIndexedPackageSourceFactory_PackageFileName;

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

                PreIndexedPackageInfo packageInfo(details, [](const std::string& packageLocation)
                    {
                        THROW_HR_IF(APPINSTALLER_CLI_ERROR_SOURCE_NOT_SECURE, Utility::IsUrlRemote(packageLocation) && !Utility::IsUrlSecure(packageLocation));
                    });

                AICLI_LOG(Repo, Info, << "Initializing source from: " << details.Name << " => " << packageInfo.PackageLocation());

                THROW_HR_IF(APPINSTALLER_CLI_ERROR_PACKAGE_IS_BUNDLE, packageInfo.MsixInfo().GetIsBundle());

                auto fullName = packageInfo.MsixInfo().GetPackageFullName();
                AICLI_LOG(Repo, Info, << "Found package full name: " << details.Name << " => " << fullName);

                details.Data = Msix::GetPackageFamilyNameFromFullName(fullName);
                details.Identifier = Msix::GetPackageFamilyNameFromFullName(fullName);

                auto lock = LockExclusive(details, progress);
                if (!lock)
                {
                    return false;
                }

                std::optional<uint64_t> downloadedBytes;
                bool result = UpdateInternal(packageInfo.PackageLocation(), details, progress, downloadedBytes);

                if (downloadedBytes)
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
                                downloadedBytes.value(),
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
            bool UpdateInternal(const std::string& packageLocation, const SourceDetails& details, IProgressCallback& progress, std::optional<uint64_t>& downloadedBytes)
            {
                return UseDeployedPackage() ?
                    UpdateDeployedPackage(packageLocation, details, progress, downloadedBytes) :
                    UpdateLocalFilePackage(packageLocation, details, progress, downloadedBytes);
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

                std::optional<Msix::PackageVersion> currentVersion = GetCurrentVersion(details);
                PreIndexedPackageUpdateCheck updateCheck(details);

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

                std::optional<uint64_t> downloadedBytes = 0;
                bool result = UpdateInternal(updateCheck.PackageLocation(), details, progress, downloadedBytes);

                if (downloadedBytes)
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
                        downloadedBytes.value(),
                        !isBackground);
                }

                return result;
            }

            bool UpdateDeployedPackage(const std::string& packageLocation, const SourceDetails& details, IProgressCallback& progress, std::optional<uint64_t>& downloadedBytes)
            {
                // Due to complications with deployment, download the file and deploy from
                // a local source while we investigate further.
                bool download = Utility::IsUrlRemote(packageLocation);
                std::filesystem::path localFile;

                if (download)
                {
                    localFile = Runtime::GetPathTo(Runtime::PathName::Temp);
                    localFile /= GetPackageFamilyNameFromDetails(details) + ".msix";

                    auto downloadResult = Utility::Download(packageLocation, localFile, Utility::DownloadType::Index, progress);
                    downloadedBytes = downloadResult.SizeInBytes;
                }
                else
                {
                    localFile = Utility::ConvertToUTF16(packageLocation);
                }

                // Verify the local file
                Msix::WriteLockedMsixFile fileLock{ localFile };
                Msix::MsixInfo localMsixInfo{ localFile };

                // The package should not be a bundle
                THROW_HR_IF(APPINSTALLER_CLI_ERROR_PACKAGE_IS_BUNDLE, localMsixInfo.GetIsBundle());

                // Ensure that family name has not changed
                THROW_HR_IF(APPINSTALLER_CLI_ERROR_SOURCE_DATA_INTEGRITY_FAILURE,
                    GetPackageFamilyNameFromDetails(details) != Msix::GetPackageFamilyNameFromFullName(localMsixInfo.GetPackageFullName()));

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

            bool UpdateLocalFilePackage(const std::string& packageLocation, const SourceDetails& details, IProgressCallback& progress, std::optional<uint64_t>& downloadedBytes)
            {
                // We will extract the manifest and index files directly to this location
                std::filesystem::path packageState = GetStatePathFromDetails(details);
                std::filesystem::create_directories(packageState);

                std::filesystem::path packagePath = packageState / s_PreIndexedPackageSourceFactory_PackageFileName;

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

                if (Utility::IsUrlRemote(packageLocation))
                {
                    auto downloadResult = AppInstaller::Utility::Download(packageLocation, tempPackagePath, AppInstaller::Utility::DownloadType::Index, progress);
                    downloadedBytes = downloadResult.SizeInBytes;
                }
                else
                {
                    std::filesystem::copy(packageLocation, tempPackagePath);
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

                    // Ensure that family name has not changed
                    THROW_HR_IF(APPINSTALLER_CLI_ERROR_SOURCE_DATA_INTEGRITY_FAILURE,
                        GetPackageFamilyNameFromDetails(details) != Msix::GetPackageFamilyNameFromFullName(tempMsixInfo.GetPackageFullName()));

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
