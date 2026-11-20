// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include <guiddef.h>
#include <functional>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#ifdef WINGET_DEPLOYMENT_CLIENT_BUILD
#define WINGET_DEPLOYMENT_CLIENT_API __declspec(dllexport)
#else
#define WINGET_DEPLOYMENT_CLIENT_API __declspec(dllimport)
#endif

namespace AppInstaller::Deployment::Client
{
    enum class Operation
    {
        Install,
        Upgrade,
        Uninstall,
        Repair,
    };

    struct InstalledPackage
    {
        std::string Id;
        std::string Version;
        std::string Channel;
        std::string Scope;
        std::string Architecture;
        std::string InstallerType;
        std::string InstallLocation;
        std::vector<std::string> ProductCodes;
        std::vector<std::string> PackageFamilyNames;
    };

    struct OperationRequest
    {
        struct Catalog
        {
            std::string Name;
            std::string Identifier;
        };

        Operation Type = Operation::Install;
        InstalledPackage Installed;
        std::string SourceName;
        std::string SourceIdentifier;
        std::vector<Catalog> CorrelatedCatalogs;
        std::string PackageId;
        std::string Version;
        std::string Channel;
        std::string Scope;
        std::string Locale;
        std::vector<std::string> AllowedArchitectures;
        std::string InstallerType;
        std::string Rename;
        std::string LogOutputPath;
        std::string InstallLocation;
        std::string ReplacementArguments;
        std::string AdditionalArguments;
        std::string AuthenticationMode;
        std::string AuthenticationAccount;
        std::string SourceHeader;
        std::string Proxy;
        std::string CorrelationData;
        bool Interactive = false;
        bool NoProxy = false;
        bool AcceptSourceAgreements = false;
        bool AllowUnknownScope = false;
        bool UseDefaultScopePreference = false;
        bool Silent = false;
        bool Force = false;
        bool UninstallPrevious = false;
        bool Purge = false;
        bool Preserve = false;
        bool AllowUpgradeToUnknownVersion = false;
        bool IgnoreLocalArchiveMalwareScan = false;
        bool BypassIsStoreClientBlockedPolicyCheck = false;
    };

    enum class OperationStage
    {
        Queued,
        Download,
        Execution,
        PostExecution,
    };

    struct OperationProgress
    {
        OperationStage Stage = OperationStage::Queued;
        uint64_t Current = 0;
        uint64_t Maximum = 0;
        bool Bytes = false;
    };

    struct OperationCallbacks
    {
        std::function<void(const OperationProgress&)> Progress;
        std::function<bool()> IsCancelled;
    };

    struct OperationResult
    {
        long ExtendedError = 0;
        uint32_t ReturnCode = 0;
        bool Succeeded = false;
        bool RebootRequired = false;
    };

    // Owns all projected objects; only native values cross this package-internal DLL boundary.
    struct IDeploymentSession
    {
        virtual ~IDeploymentSession() = default;
        virtual OperationResult Execute(const OperationRequest& request, const OperationCallbacks& callbacks) = 0;
    };

    using CreateDeploymentSession = IDeploymentSession* (__cdecl*)(const OperationRequest&);
    inline constexpr char CreateDeploymentSessionExport[] = "WinGetCreateDeploymentSession";

    extern "C" IDeploymentSession* __cdecl WinGetCreateDeploymentSession(const OperationRequest& request);

    // Callback arguments are valid only until the callback returns.
    WINGET_DEPLOYMENT_CLIENT_API void RunPackageManagerActivationPlayground(
        const std::function<void(std::wstring_view, const std::vector<GUID>&)>& callback);
}
