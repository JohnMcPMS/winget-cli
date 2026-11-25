// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "pch.h"
#include "PortableFlow.h"
#include "PortableInstaller.h"
#include "WorkflowBase.h"
#include <map>
#include <winget/Filesystem.h>
#include <winget/PortableFileEntry.h>
#include <winget/PortableIndex.h>
#include <winget/ManifestValidation.h>

using namespace AppInstaller::Manifest;
using namespace AppInstaller::Repository;
using namespace AppInstaller::Utility;
using namespace AppInstaller::CLI::Portable;
using namespace AppInstaller::Portable;
using namespace AppInstaller::Repository::Microsoft;

namespace AppInstaller::CLI::Workflow
{
    namespace
    {
        constexpr std::string_view s_DefaultSource = "*DefaultSource"sv;

        ScopeEnum GetPortableInstallScope(Execution::Context& context)
        {
            if (context.Contains(Execution::Data::UninstalledPackageMetadata))
            {
                const auto& metadata = context.Get<Execution::Data::UninstalledPackageMetadata>();
                auto scope = metadata.find(PackageVersionMetadata::InstalledScope);
                return scope != metadata.end() ? ConvertToScopeEnum(scope->second) : ScopeEnum::Unknown;
            }

            if (WI_IsFlagSet(context.GetFlags(), Execution::ContextFlag::InstallerExecutionUseUpdate) &&
                context.Contains(Execution::Data::InstalledPackageVersion))
            {
                const auto& installedVersion = context.Get<Execution::Data::InstalledPackageVersion>();
                if (installedVersion)
                {
                    auto metadata = installedVersion->GetMetadata();
                    auto scope = metadata.find(PackageVersionMetadata::InstalledScope);
                    return scope != metadata.end() ? ConvertToScopeEnum(scope->second) : ScopeEnum::Unknown;
                }
            }

            if (context.Args.Contains(Execution::Args::Type::InstallScope))
            {
                return ConvertToScopeEnum(context.Args.GetArg(Execution::Args::Type::InstallScope));
            }

            const auto requiredScope = Settings::User().Get<Settings::Setting::InstallScopeRequirement>();
            return requiredScope != ScopeEnum::Unknown ?
                requiredScope : Settings::User().Get<Settings::Setting::InstallScopePreference>();
        }

        std::string GetPortableProductCode(Execution::Context& context)
        {
            const std::string& packageId = context.Get<Execution::Data::Manifest>().Id;

            std::string source;
            if (context.Contains(Execution::Data::PackageVersion))
            {
                source = context.Get<Execution::Data::PackageVersion>()->GetSource().GetIdentifier();
            }
            else
            {
                source = s_DefaultSource;
            }

            return MakeSuitablePathPart(packageId + "_" + source);
        }

        void EnsureValidArgsForPortableInstall(Execution::Context& context)
        {
            std::string_view renameArg = context.Args.GetArg(Execution::Args::Type::Rename);

            try
            {
                if (MakeSuitablePathPart(renameArg) != renameArg)
                {
                    context.Reporter.Error() << Resource::String::ReservedFilenameError << std::endl;
                    AICLI_TERMINATE_CONTEXT(APPINSTALLER_CLI_ERROR_INVALID_CL_ARGUMENTS);
                }
            }
            catch (...)
            {
                context.Reporter.Error() << Resource::String::ReservedFilenameError << std::endl;
                AICLI_TERMINATE_CONTEXT(APPINSTALLER_CLI_ERROR_INVALID_CL_ARGUMENTS);
            }
        }

        void EnsureVolumeSupportsReparsePoints(Execution::Context& context)
        {
            Manifest::ScopeEnum scope = GetPortableInstallScope(context);
            const std::filesystem::path& symlinkDirectory = GetPortableLinksLocation(scope);

            if (!AppInstaller::Filesystem::SupportsReparsePoints(symlinkDirectory))
            {
                context.Reporter.Error() << Resource::String::ReparsePointsNotSupportedError << std::endl;
                AICLI_TERMINATE_CONTEXT(APPINSTALLER_CLI_ERROR_PORTABLE_REPARSE_POINT_NOT_SUPPORTED);
            }
        }

        void EnsurePathIsRelative(Execution::Context& context, const Manifest::string_t& path, std::string_view field, Resource::StringId errorStringId)
        {
            if (Filesystem::PathEscapesBaseDirectory(path))
            {
                AICLI_LOG(CLI, Error, << "File path for [" << field << "] points to a location outside of its base directory: " << path);
                context.Reporter.Error() << errorStringId << std::endl;
                AICLI_TERMINATE_CONTEXT(APPINSTALLER_CLI_ERROR_INVALID_MANIFEST);
            }
        }

        using PortablePathMap = std::map<std::wstring, std::filesystem::path>;

        bool AddPortableAlias(Execution::Context& context, PortablePathMap& destinations,
            const std::filesystem::path& destination, const std::filesystem::path& source)
        {
            auto [it, inserted] = destinations.emplace(Filesystem::GetPathKey(destination), source);
            if (!inserted && !Filesystem::PathEqualsCaseInsensitive(it->second, source))
            {
                AICLI_LOG(CLI, Error, << "Portable alias destination [" << destination << "] for [" << source <<
                    "] conflicts with [" << it->second << "]");
                context.Reporter.Error() << ManifestError::DuplicatePortableCommandAlias << ' ' << destination.u8string() <<
                    " (" << source.u8string() << ", " << it->second.u8string() << ')' << std::endl;
                AICLI_TERMINATE_CONTEXT_RETURN(APPINSTALLER_CLI_ERROR_INVALID_MANIFEST, false);
            }
            return inserted;
        }
    }

    void VerifyPackageAndSourceMatch(Execution::Context& context)
    {
        const std::string& packageIdentifier = context.Get<Execution::Data::Manifest>().Id;

        std::string sourceIdentifier;
        if (context.Contains(Execution::Data::PackageVersion))
        {
            sourceIdentifier = context.Get<Execution::Data::PackageVersion>()->GetSource().GetIdentifier();
        }
        else
        {
            sourceIdentifier = s_DefaultSource;
        }

        PortableInstaller& portableInstaller = context.Get<Execution::Data::PortableInstaller>();
        if (portableInstaller.ARPEntryExists())
        {
            if (packageIdentifier != portableInstaller.WinGetPackageIdentifier || sourceIdentifier != portableInstaller.WinGetSourceIdentifier)
            {
                if (!context.Args.Contains(Execution::Args::Type::Force))
                {
                    AICLI_LOG(CLI, Error, << "Registry match failed, skipping write to uninstall registry");
                    context.Reporter.Error() << Resource::String::PortablePackageAlreadyExists << std::endl;
                    AICLI_TERMINATE_CONTEXT(APPINSTALLER_CLI_ERROR_PORTABLE_PACKAGE_ALREADY_EXISTS);
                }
                else
                {
                    AICLI_LOG(CLI, Info, << "Overriding registry match check...");
                    context.Reporter.Warn() << Resource::String::PortableRegistryCollisionOverridden << std::endl;
                }
            }
        }

        portableInstaller.WinGetPackageIdentifier = packageIdentifier;
        portableInstaller.WinGetSourceIdentifier = sourceIdentifier;
    }

    void InitializePortableInstaller(Execution::Context& context)
    {
        const bool isUpdate = WI_IsFlagSet(context.GetFlags(), Execution::ContextFlag::InstallerExecutionUseUpdate);
        const auto scope = GetPortableInstallScope(context);
        if (scope != ScopeEnum::Machine)
        {
            AICLI_RETURN_IF_TERMINATED(context << EnsureNotRunningWithNonDefaultFullToken);
        }

        const auto& installer = context.Get<Execution::Data::Installer>().value();
        Utility::Architecture arch = installer.Arch;
        const std::string& productCode = GetPortableProductCode(context);

        PortableInstaller portableInstaller = PortableInstaller(scope, arch, productCode, isUpdate ? OperationType::Upgrade : OperationType::Install);

        if (IsArchiveType(installer.BaseInstallerType) && installer.ArchiveBinariesDependOnPath)
        {
            portableInstaller.BinariesDependOnPath = true;
        }

        // Set target install directory
        std::string_view locationArg = context.Args.GetArg(Execution::Args::Type::InstallLocation);
        std::filesystem::path targetInstallDirectory;

        if (!locationArg.empty())
        {
            targetInstallDirectory = std::filesystem::path{ ConvertToUTF16(locationArg) };
        }
        else
        {
            targetInstallDirectory = GetPortableInstallRoot(scope, arch);
            targetInstallDirectory /= ConvertToUTF16(productCode);
        }

        portableInstaller.TargetInstallLocation = targetInstallDirectory;
        portableInstaller.SetAppsAndFeaturesMetadata(context.Get<Execution::Data::Manifest>(), installer.AppsAndFeaturesEntries);
        context.Add<Execution::Data::PortableInstaller>(std::move(portableInstaller));
    }

    std::vector<PortableFileEntry> GetDesiredStateForPortableInstall(Execution::Context& context)
    {
        std::filesystem::path& installerPath = context.Get<Execution::Data::InstallerPath>();
        PortableInstaller& portableInstaller = context.Get<Execution::Data::PortableInstaller>();
        std::vector<PortableFileEntry> entries;

        const std::filesystem::path& targetInstallDirectory = portableInstaller.TargetInstallLocation;
        const std::filesystem::path& symlinkDirectory = GetPortableLinksLocation(portableInstaller.GetScope());

        // InstallerPath will point to a directory if it is extracted from an archive.
        if (std::filesystem::is_directory(installerPath))
        {
            portableInstaller.RecordToIndex = true;

            // Directory install entries own their subtrees; include those contents in collision checks without flattening the entries.
            PortablePathMap packagePaths;
            for (const auto& entry : std::filesystem::recursive_directory_iterator(installerPath))
            {
                const auto relativePath = entry.path().lexically_relative(installerPath);
                packagePaths.emplace(Filesystem::GetPathKey(relativePath), relativePath);
            }
            PortablePathMap linkPaths;

            // Cache hashes by extracted source path; nested executables are hashed on demand.
            std::map<std::filesystem::path, std::string> fileHashes;

            for (const auto& entry : std::filesystem::directory_iterator(installerPath))
            {
                std::filesystem::path entryPath = entry.path();
                std::filesystem::path relativePath = std::filesystem::relative(entryPath, entryPath.parent_path());
                std::filesystem::path targetPath = targetInstallDirectory / relativePath;

                if (std::filesystem::is_directory(entryPath))
                {
                    entries.emplace_back(PortableFileEntry::CreateDirectoryEntry(entryPath, targetPath));
                }
                else
                {
                    const PortableFileEntry& fileEntry = entries.emplace_back(PortableFileEntry::CreateFileEntry(entryPath, targetPath, {}));
                    fileHashes[std::filesystem::weakly_canonical(entryPath)] = fileEntry.SHA256;
                }
            }

            const std::vector<Manifest::NestedInstallerFile>& nestedInstallerFiles = context.Get<Execution::Data::Installer>()->NestedInstallerFiles;

            for (const auto& nestedInstallerFile : nestedInstallerFiles)
            {
                EnsurePathIsRelative(context, nestedInstallerFile.RelativeFilePath, "RelativeFilePath", ManifestError::RelativeFilePathEscapesDirectory);
                AICLI_RETURN_VALUE_IF_TERMINATED(context, {});

                EnsurePathIsRelative(context, nestedInstallerFile.PortableCommandAlias, "PortableCommandAlias", ManifestError::PortableCommandAliasEscapesDirectory);
                AICLI_RETURN_VALUE_IF_TERMINATED(context, {});

                const std::filesystem::path& relativeFilePath = ConvertToUTF16(nestedInstallerFile.RelativeFilePath);
                const std::filesystem::path& targetPath = targetInstallDirectory / relativeFilePath;
                std::filesystem::path originalFilename = targetPath.filename();

                std::filesystem::path commandAlias;
                if (nestedInstallerFile.PortableCommandAlias.empty())
                {
                    commandAlias = originalFilename;
                }
                else
                {
                    commandAlias = ConvertToUTF16(nestedInstallerFile.PortableCommandAlias);
                }

                Filesystem::AppendExtension(commandAlias, ".exe");

                // If alias differs from original filename, create hardlink
                // Hardlink will be placed in the same directory as the original file to avoid pathing issues and same-volume restrictions
                if (!Filesystem::PathEqualsCaseInsensitive(commandAlias, originalFilename))
                {
                    const bool addHardlink = AddPortableAlias(context, packagePaths, relativeFilePath.parent_path() / commandAlias, relativeFilePath);
                    AICLI_RETURN_VALUE_IF_TERMINATED(context, {});
                    if (addHardlink)
                    {
                        std::filesystem::path hardlinkPath = targetPath.parent_path() / commandAlias;
                        const auto sourcePath = std::filesystem::weakly_canonical(installerPath / relativeFilePath);
                        auto it = fileHashes.find(sourcePath);
                        if (it == fileHashes.end())
                        {
                            THROW_HR_IF_MSG(APPINSTALLER_CLI_ERROR_PORTABLE_INSTALL_FAILED, !std::filesystem::is_regular_file(sourcePath),
                                "Portable hardlink source is not a regular file: %ls", sourcePath.c_str());
                            it = fileHashes.emplace(sourcePath, SHA256::ConvertToString(SHA256::ComputeHashFromFile(sourcePath))).first;
                        }
                        entries.emplace_back(PortableFileEntry::CreateHardlinkEntry(hardlinkPath, targetPath, it->second));
                    }
                }
                const auto linkPath = symlinkDirectory / commandAlias;
                const bool addLink = AddPortableAlias(context, linkPaths, linkPath, relativeFilePath);
                AICLI_RETURN_VALUE_IF_TERMINATED(context, {});
                if (addLink)
                {
                    entries.emplace_back(PortableFileEntry::CreateSymlinkEntry(linkPath, targetPath));
                }
            }
        }
        else
        {
            // Non-archive portable case: single executable file
            std::string_view renameArg = context.Args.GetArg(Execution::Args::Type::Rename);
            const std::vector<string_t>& commands = context.Get<Execution::Data::Installer>()->Commands;

            std::filesystem::path originalFilename = installerPath.filename();
            std::filesystem::path commandAlias = originalFilename;

            // Determine the command alias from rename arg, commands, or use original filename
            if (!commands.empty())
            {
                EnsurePathIsRelative(context, commands[0], "CommandAlias", ManifestError::PortableCommandAliasEscapesDirectory);
                AICLI_RETURN_VALUE_IF_TERMINATED(context, {});

                commandAlias = ConvertToUTF16(commands[0]);
            }

            if (!renameArg.empty())
            {
                commandAlias = ConvertToUTF16(renameArg);
            }

            Filesystem::AppendExtension(commandAlias, ".exe");

            // Target path for the original file (keeps its original name)
            const std::filesystem::path& targetFullPath = targetInstallDirectory / originalFilename;
            
            // Create file entry for original (with original name) - this computes SHA256
            std::string fileSha256 = Utility::SHA256::ConvertToString(Utility::SHA256::ComputeHashFromFile(installerPath));
            entries.emplace_back(PortableFileEntry::CreateFileEntry(installerPath, targetFullPath, fileSha256));

            // If alias differs from original filename, create hardlink
            if (!Filesystem::PathEqualsCaseInsensitive(commandAlias, originalFilename))
            {
                std::filesystem::path hardlinkPath = targetInstallDirectory / commandAlias;
                entries.emplace_back(PortableFileEntry::CreateHardlinkEntry(hardlinkPath, targetFullPath, fileSha256));
            }
            entries.emplace_back(PortableFileEntry::CreateSymlinkEntry(symlinkDirectory / commandAlias, targetFullPath));
        }

        return entries;
    }

    void PortableInstallImpl(Execution::Context& context)
    {
        PortableInstaller& portableInstaller = context.Get<Execution::Data::PortableInstaller>();
        if (portableInstaller.GetScope() != ScopeEnum::Machine)
        {
            AICLI_RETURN_IF_TERMINATED(context << EnsureNotRunningWithNonDefaultFullToken);
        }

        try
        {
            context.Reporter.Info() << Resource::String::InstallFlowStartingPackageInstall << std::endl;

            std::vector<AppInstaller::Portable::PortableFileEntry> desiredState = GetDesiredStateForPortableInstall(context);
            AICLI_RETURN_IF_TERMINATED(context);

            portableInstaller.SetDesiredState(desiredState);

            if (!portableInstaller.VerifyExpectedState())
            {
                if (context.Args.Contains(Execution::Args::Type::Force))
                {
                    context.Reporter.Warn() << Resource::String::PortableHashMismatchOverridden << std::endl;
                }
                else
                {
                    context.Reporter.Warn() << Resource::String::PortableHashMismatchOverrideRequired << std::endl;
                    AICLI_TERMINATE_CONTEXT(APPINSTALLER_CLI_ERROR_PORTABLE_UNINSTALL_FAILED);
                }
            }

            portableInstaller.Install();
            context.Add<Execution::Data::CorrelatedAppsAndFeaturesEntries>({ portableInstaller.GetAppsAndFeaturesEntry() });
            context.Add<Execution::Data::OperationReturnCode>(ERROR_SUCCESS);
            context.Reporter.Warn() << portableInstaller.GetOutputMessage();
        }
        catch (...)
        {
            context.Add<Execution::Data::OperationReturnCode>(Workflow::HandleException(context, std::current_exception()));

            if (!portableInstaller.IsUpdate())
            {
                context.Reporter.Warn() << Resource::String::PortableInstallFailed << std::endl;
                portableInstaller.PrepareForCleanUp();
                portableInstaller.Uninstall();
                AICLI_TERMINATE_CONTEXT(APPINSTALLER_CLI_ERROR_PORTABLE_UNINSTALL_FAILED);
            }
        }
    }

    void PortableUninstallImpl(Execution::Context& context)
    {
        PortableInstaller& portableInstaller = context.Get<Execution::Data::PortableInstaller>();

        try
        {
            context.Reporter.Info() << Resource::String::UninstallFlowStartingPackageUninstall << std::endl;

            if (!portableInstaller.VerifyExpectedState())
            {
                if (context.Args.Contains(Execution::Args::Type::Force))
                {
                    context.Reporter.Warn() << Resource::String::PortableHashMismatchOverridden << std::endl;
                }
                else
                {
                    context.Reporter.Warn() << Resource::String::PortableHashMismatchOverrideRequired << std::endl;
                    AICLI_TERMINATE_CONTEXT(APPINSTALLER_CLI_ERROR_PORTABLE_UNINSTALL_FAILED);
                }
            }

            portableInstaller.Purge = context.Args.Contains(Execution::Args::Type::Purge) ||
                (!portableInstaller.IsUpdate() && Settings::User().Get<Settings::Setting::UninstallPurgePortablePackage>() && !context.Args.Contains(Execution::Args::Type::Preserve));

            portableInstaller.Uninstall();
            context.Add<Execution::Data::OperationReturnCode>(ERROR_SUCCESS);
            context.Reporter.Warn() << portableInstaller.GetOutputMessage();
        }
        catch (...)
        {
            context.Add<Execution::Data::OperationReturnCode>(Workflow::HandleException(context, std::current_exception()));
        }
    }

    void EnsureSupportForPortableInstall(Execution::Context& context)
    {
        auto installerType = context.Get<Execution::Data::Installer>().value().EffectiveInstallerType();

        if (installerType == InstallerTypeEnum::Portable)
        {
            if (GetPortableInstallScope(context) != ScopeEnum::Machine)
            {
                AICLI_RETURN_IF_TERMINATED(context << EnsureNotRunningWithNonDefaultFullToken);
            }

            context <<
                EnsureValidArgsForPortableInstall <<
                EnsureVolumeSupportsReparsePoints;
        }
    }
}
