// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "pch.h"
#include "Microsoft/PreIndexed/PackageStore.h"
#include "Microsoft/PreIndexedPackageSourceFactory.h"

#include <AppInstallerDownloader.h>
#include <AppInstallerRuntime.h>
#include <AppInstallerStrings.h>

using namespace std::string_literals;

namespace AppInstaller::Repository::Microsoft::PreIndexed
{
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

    AcquiredPackage::AcquiredPackage(AcquiredPackage&& other) noexcept
    {
        *this = std::move(other);
    }

    AcquiredPackage& AcquiredPackage::operator=(AcquiredPackage&& other) noexcept
    {
        if (this != &other)
        {
            Key = std::move(other.Key);
            Path = std::move(other.Path);
            DownloadedBytes = std::move(other.DownloadedBytes);
            FileLock = std::move(other.FileLock);
            m_isTemporary = other.m_isTemporary;

            // The moved from object must not also remove the file.
            other.m_isTemporary = false;
            other.Path.clear();
        }

        return *this;
    }

    AcquiredPackage::~AcquiredPackage()
    {
        if (m_isTemporary && !Path.empty())
        {
            // Release the lock before attempting to remove the file that it holds.
            FileLock.reset();

            try
            {
                std::filesystem::remove(Path);
            }
            catch (...)
            {
                AICLI_LOG(Repo, Info, << "Failed to remove acquired package file at: " << Path);
            }
        }
    }

    PackageStoreBase::PackageStoreBase(const SourceDetails& details) :
        m_sourceName(details.Name), m_trustLevel(details.TrustLevel)
    {
        // Gets the identity of the source itself, as distinct from the identity of any one of the
        // packages it is composed of.
        //
        // The cross process lock name and the local state directory are both derived from this,
        // which is why it must be stable: changing either one orphans an existing source's local
        // state, and -- worse -- lets an old and a new client take different locks over the same
        // data.
        m_sourceIdentity = details.Identifier.empty() ? details.Data : details.Identifier;
        THROW_HR_IF(E_UNEXPECTED, m_sourceIdentity.empty());
    }

    bool PackageStoreBase::RequireStoreOrigin() const
    {
        return WI_IsFlagSet(m_trustLevel, SourceTrustLevel::StoreOrigin);
    }

    bool PackageStoreBase::IsTrusted() const
    {
        return WI_IsFlagSet(m_trustLevel, SourceTrustLevel::Trusted);
    }

    std::optional<AcquiredPackage> PackageStoreBase::Acquire(const PackageKey& package, const std::string& location, IProgressCallback& progress)
    {
        AcquiredPackage result;
        result.Key = package;

        if (Utility::IsUrlRemote(location))
        {
            std::filesystem::path localFile = Runtime::GetPathTo(Runtime::PathName::Temp);
            localFile /= m_sourceIdentity + "." + std::string{ GetSlotName(package.Slot) } + ".msix";

            // Set the path before downloading so that a partial download is still cleaned up.
            result.Path = localFile;
            result.MarkTemporary();

            auto downloadResult = Utility::Download(location, localFile, Utility::DownloadType::Index, progress);
            result.DownloadedBytes = downloadResult.SizeInBytes;
        }
        else
        {
            // A local location is not ours, so it is used in place and never removed.
            result.Path = Utility::ConvertToUTF16(location);
            progress.OnProgress(100, 100, ProgressType::Percent);
        }

        if (progress.IsCancelledBy(CancelReason::Any))
        {
            AICLI_LOG(Repo, Info, << "Cancelling acquisition upon request");
            return std::nullopt;
        }

        // Hold the file against modification from the moment that it is validated, so that no
        // store can commit to something other than what was checked here.
        Msix::WriteLockedMsixFile fileLock{ result.Path };
        Msix::MsixInfo localMsixInfo{ result.Path };

        // The package should not be a bundle
        THROW_HR_IF(APPINSTALLER_CLI_ERROR_PACKAGE_IS_BUNDLE, localMsixInfo.GetIsBundle());

        std::string packageFamilyName = Msix::GetPackageFamilyNameFromFullName(localMsixInfo.GetPackageFullName());

        // A package whose identity is not the one that was asked for is an integrity failure.
        THROW_HR_IF(APPINSTALLER_CLI_ERROR_SOURCE_DATA_INTEGRITY_FAILURE, package.Identity != packageFamilyName);

        if (!fileLock.ValidateTrustInfo(RequireStoreOrigin()))
        {
            AICLI_LOG(Repo, Error, << "Source update failed. Source package failed trust validation.");
            THROW_HR(APPINSTALLER_CLI_ERROR_SOURCE_DATA_INTEGRITY_FAILURE);
        }

        result.FileLock = std::move(fileLock);

        return std::optional<AcquiredPackage>{ std::move(result) };
    }

    Synchronization::CrossProcessLock PackageStoreBase::Lock(IProgressCallback& progress, bool isBackground)
    {
        Synchronization::CrossProcessLock result("PreIndexedSourceCPL_"s + m_sourceIdentity);

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

    bool CanUseDeployedPackage()
    {
        return Runtime::IsRunningInPackagedContext();
    }

    namespace anon
    {
        // Prefers one mechanism over another, falling back when the preferred one cannot hold a
        // package at all.
        //
        // The direction is one way: an unpackaged process cannot deploy, so the fallback exists
        // for when deployment is not possible rather than as a general retry. Nothing already
        // deployed is ever unregistered in favour of a file copy.
        struct FallbackPackageStore : public IPackageStore
        {
            FallbackPackageStore(std::vector<std::unique_ptr<IPackageStore>>&& stores) : m_stores(std::move(stores))
            {
                THROW_HR_IF(E_UNEXPECTED, m_stores.empty());
            }

            std::optional<AcquiredPackage> Acquire(const PackageKey& package, const std::string& location, IProgressCallback& progress) override
            {
                return m_stores.front()->Acquire(package, location, progress);
            }

            void Persist(AcquiredPackage&& package, IProgressCallback& progress) override
            {
                if (m_latched)
                {
                    m_latched->Persist(std::move(package), progress);
                    return;
                }

                for (size_t i = 0; i < m_stores.size(); ++i)
                {
                    bool isLast = (i == m_stores.size() - 1);

                    try
                    {
                        m_stores[i]->Persist(std::move(package), progress);

                        // The first package of an operation decides where the rest of them go.
                        // Splitting one index form across two stores would leave a delta and a
                        // baseline that cannot be opened together, reported through an operation
                        // that claimed success.
                        m_latched = m_stores[i].get();
                        return;
                    }
                    catch (...)
                    {
                        if (isLast || progress.IsCancelledBy(CancelReason::Any))
                        {
                            throw;
                        }

                        LOG_CAUGHT_EXCEPTION_MSG("Persisting package failed; falling back to the next store");
                    }
                }
            }

            // A source can legitimately be mixed across operations -- deployment may become
            // impossible after a baseline was already deployed -- so reads consult every store.
            std::optional<Msix::PackageVersion> GetVersion(const PackageKey& package) const override
            {
                for (const auto& store : m_stores)
                {
                    auto result = store->GetVersion(package);
                    if (result)
                    {
                        return result;
                    }
                }

                return std::nullopt;
            }

            std::optional<ExtractedIndex> GetIndex(const PackageKey& package, IProgressCallback& progress) override
            {
                for (const auto& store : m_stores)
                {
                    auto result = store->GetIndex(package, progress);
                    if (result)
                    {
                        return result;
                    }
                }

                return std::nullopt;
            }

            void Remove(const std::vector<PackageKey>& packages, IProgressCallback& progress) override
            {
                for (const auto& store : m_stores)
                {
                    store->Remove(packages, progress);
                }
            }

            // Every store derives its lock name from the source identity, so they are all the
            // same lock and taking the first one guards them all.
            Synchronization::CrossProcessLock Lock(IProgressCallback& progress, bool isBackground = false) override
            {
                return m_stores.front()->Lock(progress, isBackground);
            }

            bool AllowsUnlockedRead() const override
            {
                return m_stores.front()->AllowsUnlockedRead();
            }

        private:
            std::vector<std::unique_ptr<IPackageStore>> m_stores;

            // The store that this operation has committed to. A store object lives for exactly
            // one factory operation, so this needs no explicit scope.
            IPackageStore* m_latched = nullptr;
        };
    }

    std::unique_ptr<IPackageStore> CreateStore(const SourceDetails& details)
    {
        if (!CanUseDeployedPackage())
        {
            return CreateLocalFilePackageStore(details);
        }

        std::vector<std::unique_ptr<IPackageStore>> stores;
        stores.emplace_back(CreateDeployedPackageStore(details));
        stores.emplace_back(CreateLocalFilePackageStore(details));

        return std::make_unique<anon::FallbackPackageStore>(std::move(stores));
    }
}
