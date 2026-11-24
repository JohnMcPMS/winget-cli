// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include <guiddef.h>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace AppInstaller::Deployment::Client
{
    struct UninstallRequest
    {
        std::string PackageId;
        std::string Scope;
        std::string SourceName;
        std::string SourceIdentifier;
        std::string SourceHeader;
        std::string AuthenticationMode;
        std::string AuthenticationAccount;
        std::string LogOutputPath;
        std::string CorrelationData;
        bool AcceptSourceAgreements = false;
        bool Interactive = false;
        bool Silent = false;
        bool Force = false;
    };

    struct UninstallCallbacks
    {
        std::function<void(bool queued, double completion)> Progress;
        std::function<bool()> IsCancelled;
    };

    struct UninstallResult
    {
        long ExtendedError = 0;
        unsigned int ReturnCode = 0;
        bool Succeeded = false;
        bool RebootRequired = false;
    };

    // Owns projected objects; only native values cross this package-internal DLL boundary.
    struct IUninstallSession
    {
        virtual ~IUninstallSession() = default;
        virtual UninstallResult Execute(const UninstallRequest& request, const UninstallCallbacks& callbacks) = 0;
    };

    using CreateUninstallSession = IUninstallSession* (__cdecl*)();
    inline constexpr char CreateUninstallSessionExport[] = "WinGetCreateUninstallSession";
    extern "C" IUninstallSession* __cdecl WinGetCreateUninstallSession();

    struct ActivationDiagnosticsResult
    {
        std::wstring RuntimeClassName;
        std::vector<GUID> InterfaceIds;
    };

    struct IActivationDiagnosticsSession
    {
        virtual ~IActivationDiagnosticsSession() = default;
        virtual ActivationDiagnosticsResult Execute() = 0;
    };

    using CreateActivationDiagnosticsSession = IActivationDiagnosticsSession* (__cdecl*)();
    inline constexpr char CreateActivationDiagnosticsSessionExport[] = "WinGetCreateActivationDiagnosticsSession";
    extern "C" IActivationDiagnosticsSession* __cdecl WinGetCreateActivationDiagnosticsSession();
}
