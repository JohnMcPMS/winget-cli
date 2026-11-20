// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include <guiddef.h>
#include <functional>
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
    struct UninstallRequest
    {
        struct Catalog
        {
            std::string Name;
            std::string Identifier;
        };

        std::string PackageId;
        std::string ProductCode;
        std::string Scope;
        std::vector<Catalog> Catalogs;
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

    using CreateUninstallSession = IUninstallSession* (__cdecl*)(const UninstallRequest&);
    inline constexpr char CreateUninstallSessionExport[] = "WinGetCreateUninstallSession";
    extern "C" IUninstallSession* __cdecl WinGetCreateUninstallSession(const UninstallRequest& request);

    // Callback arguments are valid only until the callback returns.
    WINGET_DEPLOYMENT_CLIENT_API void RunPackageManagerActivationPlayground(
        const std::function<void(std::wstring_view, const std::vector<GUID>&)>& callback);
}
