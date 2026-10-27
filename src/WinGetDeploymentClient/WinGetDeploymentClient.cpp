// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include <Windows.h>
#include "Public\WinGetDeploymentClient.h"
#include <objidl.h>
#include <winrt/Microsoft.Management.Deployment.h>

namespace AppInstaller::Deployment::Client
{
    void RunPackageManagerActivationPlayground(
        const std::function<void(std::wstring_view, const std::vector<GUID>&)>& callback)
    {
        winrt::com_ptr<IGlobalOptions> options;
        winrt::check_hresult(CoCreateInstance(
            CLSID_GlobalOptions, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IGlobalOptions), options.put_void()));
        winrt::check_hresult(options->Set(COMGLB_UNMARSHALING_POLICY, COMGLB_UNMARSHALING_POLICY_STRONG));

        winrt::Microsoft::Management::Deployment::PackageManager instance;
        const auto runtimeClassName = winrt::get_class_name(instance);
        if (runtimeClassName != winrt::name_of<winrt::Microsoft::Management::Deployment::PackageManager>())
        {
            throw winrt::hresult_error(E_UNEXPECTED, L"Activation did not return a PackageManager.");
        }

        const auto interfaces = winrt::get_interfaces(instance);
        std::vector<GUID> interfaceIds;
        interfaceIds.reserve(interfaces.size());
        for (const auto& interfaceId : interfaces)
        {
            interfaceIds.push_back(interfaceId);
        }

        callback(runtimeClassName, interfaceIds);
    }
}
