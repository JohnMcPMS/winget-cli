// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include <guiddef.h>
#include <functional>
#include <string_view>
#include <vector>

#ifdef WINGET_DEPLOYMENT_CLIENT_BUILD
#define WINGET_DEPLOYMENT_CLIENT_API __declspec(dllexport)
#else
#define WINGET_DEPLOYMENT_CLIENT_API __declspec(dllimport)
#endif

namespace AppInstaller::Deployment::Client
{
    // Callback arguments are valid only until the callback returns.
    WINGET_DEPLOYMENT_CLIENT_API void RunPackageManagerActivationPlayground(
        const std::function<void(std::wstring_view, const std::vector<GUID>&)>& callback);
}
