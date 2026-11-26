// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "winget/PortableARPEntry.h"
#include "winget/PortableFileEntry.h"
#include <Workflows/WorkflowBase.h>
#include <filesystem>
#include <set>
#include <utility>

using namespace AppInstaller::Registry::Portable;

namespace AppInstaller::Repository::Microsoft
{
    struct PortableIndex;
}

namespace AppInstaller::CLI::Portable
{
    std::filesystem::path GetPortableLinksLocation(Manifest::ScopeEnum scope);

    std::filesystem::path GetPortableInstallRoot(Manifest::ScopeEnum scope, Utility::Architecture arch);

    // Object representation of the metadata and functionality required for installing a Portable package. 
    struct PortableInstaller
    {
        // These values are initialized from the existing ARP entry.
        std::string DisplayName;
        std::string DisplayVersion;
        std::string HelpLink;
        std::string InstallDate;
        std::filesystem::path InstallLocation;
        std::filesystem::path PortableSymlinkFullPath;
        std::filesystem::path PortableTargetFullPath;
        std::string Publisher;
        std::string SHA256;
        std::string URLInfoAbout;
        std::string UninstallString;
        std::string WinGetInstallerType;
        std::string WinGetPackageIdentifier;
        std::string WinGetSourceIdentifier;
        bool InstallDirectoryCreated = false;
        bool BinariesDependOnPath = false;
        // True when any alias requires its executable directory on PATH.
        bool InstallDirectoryAddedToPath = false;

        bool Purge = false;
        bool RecordToIndex = false;

        // This is the incoming target install location determined from the context args.
        std::filesystem::path TargetInstallLocation;

        PortableInstaller(Manifest::ScopeEnum scope, Utility::Architecture arch, const std::string& productCode, Workflow::OperationType operation);

        bool IsUpdate() const { return m_operation == Workflow::OperationType::Upgrade; }

        bool VerifyExpectedState();

        void SetDesiredState(const std::vector<AppInstaller::Portable::PortableFileEntry>& desiredEntries)
        {
            m_desiredEntries = desiredEntries;
        };

        void PrepareForCleanUp()
        {
            m_expectedEntries = std::move(m_cleanupEntries);
            m_desiredEntries = {};
            InstallLocation = TargetInstallLocation;
        }

        void Install();

        void Uninstall();

        template<typename T>
        void CommitToARPEntry(PortableValueName valueName, T value)
        {
            m_portableARPEntry.SetValue(valueName, value);
            m_hasRegistrationWrites = true;
        }

        bool HasRegistrationWrites() const { return m_hasRegistrationWrites; }

        std::filesystem::path GetPortableIndexFileName()
        {
            return Utility::ConvertToUTF16(GetProductCode() + ".db");
        }

        Manifest::ScopeEnum GetScope() { return m_portableARPEntry.GetScope(); };

        Utility::Architecture GetArch() { return m_portableARPEntry.GetArchitecture(); };

        std::string GetProductCode() { return m_portableARPEntry.GetProductCode(); };

        bool ARPEntryExists() { return m_portableARPEntry.Exists(); };

        bool CanRecoverARPEntry()
        {
            return m_operation == Workflow::OperationType::Install && m_portableARPEntry.Exists() && m_portableARPEntry.IsEmpty();
        }

        std::string GetOutputMessage()
        {
            return m_stream.str();
        }

        void SetAppsAndFeaturesMetadata(
            const Manifest::Manifest& manifest,
            const std::vector<AppInstaller::Manifest::AppsAndFeaturesEntry>& entries);

        AppInstaller::Manifest::AppsAndFeaturesEntry GetAppsAndFeaturesEntry();

    private:
        Workflow::OperationType m_operation;
        PortableARPEntry m_portableARPEntry;
        bool m_hasRegistrationWrites = false;
        std::vector<AppInstaller::Portable::PortableFileEntry> m_desiredEntries;
        std::vector<AppInstaller::Portable::PortableFileEntry> m_expectedEntries;
        // Rollback includes validated targets whose installation was attempted.
        std::vector<AppInstaller::Portable::PortableFileEntry> m_cleanupEntries;
        std::set<std::wstring> m_requiredPathDirectoryKeys;
        bool m_symlinkCreated = false;
        std::stringstream m_stream;

        bool SupportsInstall() const { return m_operation == Workflow::OperationType::Install || IsUpdate(); }
        bool SupportsUninstall() const { return m_operation == Workflow::OperationType::Install || m_operation == Workflow::OperationType::Uninstall; }

        std::string GetStringValue(PortableValueName valueName);
        std::filesystem::path GetPathValue(PortableValueName valueName);
        bool GetBoolValue(PortableValueName valueName);

        void SetExpectedState();
        void RegisterARPEntry();

        void ApplyDesiredState();
        void RemoveExpectedState();
        void InstallFile(AppInstaller::Portable::PortableFileEntry& desiredState, Repository::Microsoft::PortableIndex* index);
        void RemoveFile(AppInstaller::Portable::PortableFileEntry& desiredState);

        bool CreateTargetInstallDirectory();
        void RemoveInstallDirectory();

        void AddToPathVariable(std::filesystem::path value);
        void AddInstallDirectoryToPath(const std::filesystem::path& value);
        void RemoveFromPathVariable(std::filesystem::path value, bool onlyIfEmpty);
    };
}
