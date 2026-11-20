// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "ExecutionContext.h"
#include <WinGetDeploymentClient.h>
#include <wil/resource.h>

namespace AppInstaller::CLI::Workflow
{
    struct LimitedTokenDeployment
    {
        // The session, including its deleting destructor, must be released before its DLL.
        wil::unique_hmodule Module;
        std::unique_ptr<Deployment::Client::IDeploymentSession> Session;
        Deployment::Client::Operation Operation = Deployment::Client::Operation::Install;
        bool Started = false;
    };

#ifndef AICLI_DISABLE_TEST_HOOKS
    using LimitedTokenSessionFactory = std::function<std::unique_ptr<Deployment::Client::IDeploymentSession>(const Deployment::Client::OperationRequest&)>;
    LimitedTokenSessionFactory* TestHook_SetLimitedTokenSessionFactory(LimitedTokenSessionFactory* factory);
#endif

    void PrepareLimitedTokenInstall(Execution::Context& context);
    bool PrepareLimitedTokenUninstall(Execution::Context& context);
    bool PrepareLimitedTokenRepair(Execution::Context& context);
    void ExecuteLimitedTokenOperation(Execution::Context& context);
}
