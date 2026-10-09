// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "pch.h"
#include "Microsoft/PreIndexed/IndexForm.h"
#include "Microsoft/PreIndexed/RemotePackage.h"
#include "Microsoft/PreIndexed/SourceData.h"

#include <AppInstallerDateTime.h>
#include <AppInstallerDownloader.h>

namespace AppInstaller::Repository::Microsoft::PreIndexed
{
    namespace anon
    {
        // A source composed of a delta and the baseline that the delta names.
        struct DeltaIndexForm : public IIndexForm
        {
            DeltaIndexForm(const SourceDetails& details) : m_details(details)
            {
                SourceData data{ details.Data };
                m_baselineIdentity = data.BaseIdentity();
                m_deltaIdentity = data.DeltaIdentity();
            }

            std::optional<std::string> DiscoverIdentities(IProgressCallback& progress) override
            {
                if (m_deltaIdentity.empty())
                {
                    if (!m_baselineIdentity.empty())
                    {
                        // The source is already configured, and what it is configured with names
                        // no delta. Probing here would be a migration, which is deliberately not
                        // performed: the identity a probe found could not be written back anyway,
                        // since an update is given a const SourceDetails and only the metadata
                        // fields are persisted afterwards. A third party source that starts
                        // publishing a delta can be re-added.
                        return std::nullopt;
                    }

                    // Nothing is known about this source yet, so it is being added. That is the
                    // one operation that can both probe for a delta and store what it finds.
                    std::optional<std::string> deltaIdentity = ProbeDeltaIdentity(progress);

                    if (!deltaIdentity)
                    {
                        // The source publishes no delta. That is an ordinary answer, and the caller
                        // falls back to the full index rather than treating it as a failure.
                        return std::nullopt;
                    }

                    m_deltaIdentity = deltaIdentity.value();

                    // A baseline is a full index that has been designated as one, published under
                    // the identity that the source has always used, so discovering the full index
                    // discovers the baseline.
                    PreIndexedPackageInfo packageInfo(GetFullIndexPackageLocations(m_details), [](const std::string& packageLocation)
                        {
                            THROW_HR_IF(APPINSTALLER_CLI_ERROR_SOURCE_NOT_SECURE, Utility::IsUrlRemote(packageLocation) && !Utility::IsUrlSecure(packageLocation));
                        });

                    THROW_HR_IF(APPINSTALLER_CLI_ERROR_PACKAGE_IS_BUNDLE, packageInfo.MsixInfo().GetIsBundle());

                    m_baselineIdentity = Msix::GetPackageFamilyNameFromFullName(packageInfo.MsixInfo().GetPackageFullName());
                }

                return SerializeIdentities();
            }

            std::vector<PackageKey> GetPackages() const override
            {
                return { GetDeltaKey(), GetBaselineKey() };
            }

            bool HasIdentities() const override
            {
                return !m_deltaIdentity.empty() && !m_baselineIdentity.empty();
            }

            bool IsHeld(const IPackageStore& store) const override
            {
                return store.GetVersion(GetDeltaKey()).has_value() && store.GetVersion(GetBaselineKey()).has_value();
            }

            bool IsUsable(IPackageStore& store, IProgressCallback& progress) override
            {
                std::optional<Msix::PackageVersion> heldBaselineVersion = store.GetVersion(GetBaselineKey());

                if (!heldBaselineVersion)
                {
                    return false;
                }

                auto locator = ReadBaselineLocator(store, progress);

                if (!locator)
                {
                    // A delta that cannot be read, or that names no baseline, cannot be paired
                    // with anything.
                    return false;
                }

                // The version is the whole of the question: a package identity and version name
                // one set of contents, so a baseline held at the version the delta names is the
                // baseline the delta was computed against.
                if (heldBaselineVersion.value() != Msix::PackageVersion{ locator->PackageVersion })
                {
                    AICLI_LOG(Repo, Warning, << "Delta for source `" << m_details.Name << "` names baseline version " <<
                        locator->PackageVersion << ", but the baseline held is version " << heldBaselineVersion.value().ToString());
                    return false;
                }

                return true;
            }

            std::optional<Msix::PackageVersion> GetHeldVersion(const IPackageStore& store) const override
            {
                return store.GetVersion(GetDeltaKey());
            }

            UpdateResult Update(IPackageStore& store, bool isBackground, IProgressCallback& progress, UpdateReport& report) override
            {
                report.UsedDeltaDownload = true;

                std::optional<Msix::PackageVersion> currentDeltaVersion = store.GetVersion(GetDeltaKey());

                if (currentDeltaVersion)
                {
                    report.PreviousIndexPublishedAt = Utility::GetTimePointFromVersion(currentDeltaVersion.value());
                }

                // The delta is published at a fixed name beside the full index, and is probed by
                // exactly the same mechanism.
                PreIndexedPackageUpdateCheck deltaCheck(GetDeltaPackageLocations(m_details));

                report.NewIndexPublishedAt = Utility::GetTimePointFromVersion(deltaCheck.AvailableVersion());

                bool deltaIsCurrent = currentDeltaVersion && currentDeltaVersion.value() >= deltaCheck.AvailableVersion();

                std::optional<SQLiteIndex::DeltaBaselineLocator> locator;

                if (deltaIsCurrent)
                {
                    // The delta we hold is current, but we may still be missing the baseline that
                    // it names, so being up to date is not on its own a reason to stop.
                    locator = ReadBaselineLocator(store, progress);

                    if (locator)
                    {
                        auto heldBaselineVersion = store.GetVersion(GetBaselineKey());

                        if (heldBaselineVersion && heldBaselineVersion.value() == Msix::PackageVersion{ locator->PackageVersion })
                        {
                            AICLI_LOG(Repo, Verbose, << "Remote delta (" << deltaCheck.AvailableVersion().ToString() <<
                                ") was not newer than existing (" << currentDeltaVersion.value().ToString() <<
                                ") and its baseline is held, no update needed");

                            report.PreviousBaselinePublishedAt = Utility::GetTimePointFromVersion(heldBaselineVersion.value());
                            report.NewBaselinePublishedAt = report.PreviousBaselinePublishedAt;
                            return UpdateResult::Success;
                        }
                    }
                }

                // Re-acquire the delta when it is not current, and also when we could not read the
                // one we hold -- in that case what we hold is unusable whatever its version says.
                bool acquireDelta = !deltaIsCurrent || !locator;

                if (progress.IsCancelledBy(CancelReason::Any))
                {
                    AICLI_LOG(Repo, Info, << "Cancelling update upon request");
                    return UpdateResult::Aborted;
                }

                auto lock = store.Lock(progress, isBackground);
                if (!lock)
                {
                    // The delta may well have been usable, so falling back here would acquire a
                    // full index that the source did not need.
                    return UpdateResult::Aborted;
                }

                std::optional<uint64_t> deltaBytes;

                if (acquireDelta)
                {
                    auto acquired = store.Acquire(GetDeltaKey(), deltaCheck.PackageLocation(), progress);
                    if (!acquired)
                    {
                        // Acquisition reports nothing only when it was cancelled.
                        return UpdateResult::Aborted;
                    }

                    deltaBytes = acquired->DownloadedBytes;

                    store.Persist(std::move(acquired.value()), progress);

                    locator = ReadBaselineLocator(store, progress);
                }

                if (!locator)
                {
                    AICLI_LOG(Repo, Warning, << "Delta for source `" << m_details.Name << "` did not name a baseline");
                    return UpdateResult::Unusable;
                }

                AICLI_LOG(Repo, Info, << "Delta for source `" << m_details.Name << "` names baseline " << locator->Identifier <<
                    " at `" << locator->RelativeSourcePath << "` version " << locator->PackageVersion);

                Msix::PackageVersion requiredBaselineVersion{ locator->PackageVersion };
                std::optional<Msix::PackageVersion> currentBaselineVersion = store.GetVersion(GetBaselineKey());

                if (currentBaselineVersion)
                {
                    report.PreviousBaselinePublishedAt = Utility::GetTimePointFromVersion(currentBaselineVersion.value());
                }

                report.NewBaselinePublishedAt = Utility::GetTimePointFromVersion(requiredBaselineVersion);

                std::optional<uint64_t> baselineBytes;

                if (currentBaselineVersion && currentBaselineVersion.value() == requiredBaselineVersion)
                {
                    AICLI_LOG(Repo, Verbose, << "Already holding baseline version " << requiredBaselineVersion.ToString());
                }
                else
                {
                    PreIndexedPackageUpdateCheck baselineCheck(GetBaselinePackageLocations(m_details, locator->RelativeSourcePath));

                    // The delta told us which version it was computed against; anything else at
                    // that location is not the baseline this delta can be paired with.
                    if (baselineCheck.AvailableVersion() != requiredBaselineVersion)
                    {
                        AICLI_LOG(Repo, Warning, << "Baseline at `" << baselineCheck.PackageLocation() << "` was version " <<
                            baselineCheck.AvailableVersion().ToString() << ", but the delta named " << requiredBaselineVersion.ToString());
                        return UpdateResult::Unusable;
                    }

                    auto acquired = store.Acquire(GetBaselineKey(), baselineCheck.PackageLocation(), progress);
                    if (!acquired)
                    {
                        // Acquisition reports nothing only when it was cancelled.
                        return UpdateResult::Aborted;
                    }

                    baselineBytes = acquired->DownloadedBytes;

                    store.Persist(std::move(acquired.value()), progress);

                    report.BaselineUpdated = true;
                }

                if (deltaBytes || baselineBytes)
                {
                    report.DownloadedBytes = deltaBytes.value_or(0) + baselineBytes.value_or(0);
                    report.Reportable = true;
                }

                return UpdateResult::Success;
            }

            SQLiteIndex Open(IPackageStore& store, IProgressCallback& progress) override
            {
                auto delta = store.GetIndex(GetDeltaKey(), progress);
                auto baseline = store.GetIndex(GetBaselineKey(), progress);

                if (!delta || !baseline)
                {
                    THROW_HR(APPINSTALLER_CLI_ERROR_SOURCE_DATA_MISSING);
                }

                return SQLiteIndex::OpenWithBaseline(
                    delta->Path.u8string(),
                    baseline->Path.u8string(),
                    SQLiteIndex::OpenDisposition::Immutable,
                    std::move(delta->TemporaryFile),
                    std::move(baseline->TemporaryFile));
            }

        private:
            PackageKey GetDeltaKey() const
            {
                THROW_HR_IF(E_NOT_VALID_STATE, m_deltaIdentity.empty());
                return PackageKey{ PackageSlot::Delta, m_deltaIdentity };
            }

            PackageKey GetBaselineKey() const
            {
                THROW_HR_IF(E_NOT_VALID_STATE, m_baselineIdentity.empty());
                return PackageKey{ PackageSlot::Baseline, m_baselineIdentity };
            }

            std::string SerializeIdentities() const
            {
                SourceData data;
                data.BaseIdentity(m_baselineIdentity);
                data.DeltaIdentity(m_deltaIdentity);
                return data.Serialize();
            }

            // Learns the delta's identity by reading the published package.
            //
            // Nothing when the source publishes no delta at that location, which is how a source
            // that is not delta capable is recognized.
            std::optional<std::string> ProbeDeltaIdentity(IProgressCallback&)
            {
                try
                {
                    PreIndexedPackageInfo packageInfo(GetDeltaPackageLocations(m_details), [](const std::string& packageLocation)
                        {
                            THROW_HR_IF(APPINSTALLER_CLI_ERROR_SOURCE_NOT_SECURE, Utility::IsUrlRemote(packageLocation) && !Utility::IsUrlSecure(packageLocation));
                        });

                    THROW_HR_IF(APPINSTALLER_CLI_ERROR_PACKAGE_IS_BUNDLE, packageInfo.MsixInfo().GetIsBundle());

                    return Msix::GetPackageFamilyNameFromFullName(packageInfo.MsixInfo().GetPackageFullName());
                }
                catch (...)
                {
                    LOG_CAUGHT_EXCEPTION_MSG("No delta found for source: %hs", m_details.Name.c_str());
                    return std::nullopt;
                }
            }

            // Reads the baseline that the delta we hold names, from the delta itself.
            //
            // Nothing when the delta cannot be read or does not carry a locator; the caller falls
            // back to the full index rather than guessing at a baseline of its own.
            std::optional<SQLiteIndex::DeltaBaselineLocator> ReadBaselineLocator(IPackageStore& store, IProgressCallback& progress)
            {
                try
                {
                    auto extracted = store.GetIndex(GetDeltaKey(), progress);
                    if (!extracted)
                    {
                        return std::nullopt;
                    }

                    SQLiteIndex deltaIndex = SQLiteIndex::Open(extracted->Path.u8string(), SQLiteIndex::OpenDisposition::Immutable);
                    return deltaIndex.GetDeltaBaselineLocator();
                }
                catch (...)
                {
                    if (progress.IsCancelledBy(CancelReason::Any))
                    {
                        throw;
                    }

                    LOG_CAUGHT_EXCEPTION_MSG("Could not read the baseline locator from the held delta");
                    return std::nullopt;
                }
            }

            SourceDetails m_details;
            std::string m_deltaIdentity;
            std::string m_baselineIdentity;
        };
    }

    std::unique_ptr<IIndexForm> CreateDeltaIndexForm(const SourceDetails& details)
    {
        return std::make_unique<anon::DeltaIndexForm>(details);
    }
}
