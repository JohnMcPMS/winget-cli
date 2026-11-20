// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "pch.h"
#include "LimitedTokenFlow.h"
#include "SourceFlow.h"
#include "WorkflowBase.h"
#include "COMContext.h"
#include <AppInstallerRuntime.h>
#include <cmath>

namespace AppInstaller::CLI::Workflow
{
    using namespace Execution;
    using namespace Manifest;
    using namespace Repository;
    namespace Client = AppInstaller::Deployment::Client;

    namespace
    {
#ifndef AICLI_DISABLE_TEST_HOOKS
        LimitedTokenUninstallFactory* s_factoryOverride = nullptr;
#endif

        bool IsSupported(Context& context)
        {
            if (!Runtime::IsRunningWithNonDefaultFullToken() ||
                WI_IsAnyFlagSet(context.GetFlags(), ContextFlag::InstallerExecutionUseUpdate | ContextFlag::InstallerExecutionUseRepair) ||
                context.Args.Contains(Args::Type::TargetVersion) || context.Args.Contains(Args::Type::AllVersions) ||
                context.Args.Contains(Args::Type::Channel) ||
                context.Args.Contains(Args::Type::Proxy) || context.Args.Contains(Args::Type::NoProxy) ||
                !context.Contains(Data::InstalledPackageVersion) || !context.Get<Data::InstalledPackageVersion>())
            {
                return false;
            }
            auto metadata = context.Get<Data::InstalledPackageVersion>()->GetMetadata();
            if (ConvertToScopeEnum(metadata[PackageVersionMetadata::InstalledScope]) != ScopeEnum::User)
            {
                return false;
            }
            auto scope = ConvertToScopeEnum(context.Args.GetArg(Args::Type::InstallScope));
            if (scope != ScopeEnum::Unknown && scope != ScopeEnum::User)
            {
                return false;
            }
            switch (ConvertToInstallerTypeEnum(metadata[PackageVersionMetadata::InstalledType]))
            {
            case InstallerTypeEnum::Exe:
            case InstallerTypeEnum::Burn:
            case InstallerTypeEnum::Inno:
            case InstallerTypeEnum::Nullsoft:
            case InstallerTypeEnum::Msi:
            case InstallerTypeEnum::Wix:
                return true;
            default:
                return false;
            }
        }

        Client::UninstallRequest CreateRequest(Context& context)
        {
            THROW_HR_IF(APPINSTALLER_CLI_ERROR_ADMIN_CONTEXT_ACTION_PROHIBITED, !IsSupported(context));
            Client::UninstallRequest result;
            auto installed = context.Get<Data::InstalledPackageVersion>();
            result.PackageId = installed->GetProperty(PackageVersionProperty::Id);
            auto source = installed->GetSource();
            if (source && source.ContainsAvailablePackages())
            {
                const auto& details = source.GetDetails();
                result.SourceName = details.Name;
                result.SourceIdentifier = details.Identifier;
            }
            auto arg = [&](Args::Type type) { return std::string{ context.Args.GetArg(type) }; };
            result.ProductCode = arg(Args::Type::ProductCode);
            result.Scope = arg(Args::Type::InstallScope);
            result.SourceHeader = arg(Args::Type::CustomHeader);
            auto authentication = GetAuthenticationArguments(context);
            result.AuthenticationMode = Authentication::AuthenticationModeToString(authentication.Mode);
            result.AuthenticationAccount = authentication.AuthenticationAccount;
            result.AcceptSourceAgreements = context.Args.Contains(Args::Type::AcceptSourceAgreements);
            result.Interactive = context.Args.Contains(Args::Type::Interactive);
            result.Silent = context.Args.Contains(Args::Type::Silent);
            result.Force = context.Args.Contains(Args::Type::Force);
            result.LogOutputPath = arg(Args::Type::Log);
            if (auto comContext = dynamic_cast<COMContext*>(&context))
            {
                result.CorrelationData = Utility::ConvertToUTF8(comContext->GetCorrelationJson());
            }
            return result;
        }
    }

#ifndef AICLI_DISABLE_TEST_HOOKS
    LimitedTokenUninstallFactory* TestHook_SetLimitedTokenUninstallFactory(LimitedTokenUninstallFactory* factory)
    {
        return std::exchange(s_factoryOverride, factory);
    }
#endif

    bool PrepareLimitedTokenUninstall(Context& context)
    {
        if (!IsSupported(context))
        {
            return false;
        }
        if (context.Contains(Data::LimitedTokenUninstall))
        {
            return true;
        }
        auto state = std::make_shared<LimitedTokenUninstall>();
#ifndef AICLI_DISABLE_TEST_HOOKS
        if (s_factoryOverride)
        {
            if (!*s_factoryOverride)
            {
                return false;
            }
            state->Session = (*s_factoryOverride)(CreateRequest(context));
        }
        else
#endif
        {
            if (!Runtime::IsRunningInPackagedContext())
            {
                return false;
            }
            auto path = Runtime::GetPathTo(Runtime::PathName::SelfPackageRoot) / L"WinGetDeploymentClient.dll";
            std::error_code error;
            bool exists = std::filesystem::exists(path, error);
            if (error)
            {
                THROW_HR(HRESULT_FROM_WIN32(error.value()));
            }
            if (!exists)
            {
                return false;
            }
            state->Module.reset(LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32));
            THROW_LAST_ERROR_IF(!state->Module);
            auto create = reinterpret_cast<Client::CreateUninstallSession>(GetProcAddress(state->Module.get(), Client::CreateUninstallSessionExport));
            THROW_LAST_ERROR_IF(!create);
            state->Session.reset(create(CreateRequest(context)));
        }
        THROW_HR_IF_NULL(E_UNEXPECTED, state->Session.get());
        context.Add<Data::LimitedTokenUninstall>(std::move(state));
        return true;
    }

    void ExecuteLimitedTokenUninstall(Context& context)
    {
        auto state = context.Get<Data::LimitedTokenUninstall>();
        THROW_HR_IF_NULL(E_UNEXPECTED, state.get());
        THROW_HR_IF_NULL(E_UNEXPECTED, state->Session.get());
        THROW_HR_IF(E_UNEXPECTED, state->Started);
        state->Started = true;
        auto result = context.Reporter.ExecuteWithProgress([&](IProgressCallback& progress)
        {
            Client::UninstallCallbacks callbacks;
            callbacks.IsCancelled = [&]() { return context.IsTerminated() || progress.IsCancelledBy(CancelReason::Any); };
            callbacks.Progress = [&](bool queued, double completion)
            {
                THROW_HR_IF(E_UNEXPECTED, !std::isfinite(completion) || completion < 0 || completion > 1);
                if (queued)
                {
                    progress.SetProgressMessage(Resource::String::InstallWaitingOnAnother());
                }
                else
                {
                    progress.SetProgressMessage({});
                    progress.OnProgress(static_cast<uint64_t>(completion * 100), 100, ProgressType::Percent);
                }
            };
            return state->Session->Execute(CreateRequest(context), callbacks);
        });
        context.Add<Data::OperationReturnCode>(result.ReturnCode);
        context << ForceInstalledCacheUpdate;
        if (result.RebootRequired)
        {
            context.SetFlags(ContextFlag::RebootRequired);
        }
        if (!result.Succeeded || FAILED(result.ExtendedError))
        {
            HRESULT error = FAILED(result.ExtendedError) ? result.ExtendedError : E_UNEXPECTED;
            if (result.ReturnCode)
            {
                context.Reporter.Error() << Resource::String::UninstallFailedWithCode(result.ReturnCode) << std::endl;
            }
            else
            {
                context.Reporter.Error() << Utility::LocIndView{ GetUserPresentableMessage(error) } << std::endl;
            }
            AICLI_TERMINATE_CONTEXT(error);
        }
        context.SetExecutionStage(ExecutionStage::PostExecution);
        context.Reporter.Info() << Resource::String::UninstallFlowUninstallSuccess << std::endl;
    }
}
