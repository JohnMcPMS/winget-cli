// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "ExecutionContext.h"
#include <WinGetDeploymentClient.h>
#include <wil/resource.h>

namespace AppInstaller::CLI::Workflow
{
    struct LimitedTokenUninstall
    {
        // The deleting destructor must execute before the helper DLL is released.
        wil::unique_hmodule Module;
        std::unique_ptr<Deployment::Client::IUninstallSession> Session;
        bool Started = false;
    };

#ifndef AICLI_DISABLE_TEST_HOOKS
    using LimitedTokenUninstallFactory = std::function<std::unique_ptr<Deployment::Client::IUninstallSession>()>;
    LimitedTokenUninstallFactory* TestHook_SetLimitedTokenUninstallFactory(LimitedTokenUninstallFactory* factory);
#endif

    wil::unique_hmodule LoadDeploymentClient();

    bool PrepareLimitedTokenUninstall(Execution::Context& context);
    void ExecuteLimitedTokenUninstall(Execution::Context& context);
}
