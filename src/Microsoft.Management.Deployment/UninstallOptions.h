// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "UninstallOptions.g.h"
#include "Public/ComClsids.h"
#include <winget/ModuleCountBase.h>

namespace winrt::Microsoft::Management::Deployment::implementation
{
    [uuid(WINGET_OUTOFPROC_COM_CLSID_UninstallOptions)]
    struct UninstallOptions : UninstallOptionsT<UninstallOptions>
    {
        UninstallOptions();

        winrt::Microsoft::Management::Deployment::PackageVersionId PackageVersionId();
        void PackageVersionId(winrt::Microsoft::Management::Deployment::PackageVersionId const& value);
        winrt::Microsoft::Management::Deployment::PackageUninstallMode PackageUninstallMode();
        void PackageUninstallMode(winrt::Microsoft::Management::Deployment::PackageUninstallMode const& value);
        hstring LogOutputPath();
        void LogOutputPath(hstring const& value);
        hstring CorrelationData();
        void CorrelationData(hstring const& value);
        bool Force();
        void Force(bool value);
        winrt::Microsoft::Management::Deployment::PackageUninstallScope PackageUninstallScope();
        void PackageUninstallScope(winrt::Microsoft::Management::Deployment::PackageUninstallScope const& value);
        winrt::Microsoft::Management::Deployment::PackageVersionInfo InstalledVersion();
        void InstalledVersion(winrt::Microsoft::Management::Deployment::PackageVersionInfo const& value);
        bool Purge();
        void Purge(bool value);
        bool Preserve();
        void Preserve(bool value);

#if !defined(INCLUDE_ONLY_INTERFACE_METHODS)
    private:
        winrt::Microsoft::Management::Deployment::PackageVersionId m_packageVersionId{ nullptr };
        winrt::Microsoft::Management::Deployment::PackageUninstallMode m_packageUninstallMode = winrt::Microsoft::Management::Deployment::PackageUninstallMode::Default;
        std::wstring m_logOutputPath = L"";
        std::wstring m_correlationData = L"";
        bool m_force = false;
        winrt::Microsoft::Management::Deployment::PackageUninstallScope m_packageUninstallScope = winrt::Microsoft::Management::Deployment::PackageUninstallScope::Any;
        winrt::Microsoft::Management::Deployment::PackageVersionInfo m_installedVersion{ nullptr };
        bool m_purge = false;
        bool m_preserve = false;
#endif
    };
}

#if !defined(INCLUDE_ONLY_INTERFACE_METHODS)
namespace winrt::Microsoft::Management::Deployment::factory_implementation
{
    struct UninstallOptions : UninstallOptionsT<UninstallOptions, implementation::UninstallOptions>, AppInstaller::WinRT::ModuleCountBase
    {
    };
}
#endif
