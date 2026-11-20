// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "pch.h"
#include "LimitedTokenFlow.h"
#include "FontFlow.h"
#include "PortableFlow.h"
#include "SourceFlow.h"
#include "WorkflowBase.h"
#include "COMContext.h"
#include <AppInstallerRuntime.h>

namespace AppInstaller::CLI::Workflow
{
    using namespace Execution;
    using namespace Manifest;
    using namespace Repository;
    namespace Client = AppInstaller::Deployment::Client;

    namespace
    {
#ifndef AICLI_DISABLE_TEST_HOOKS
        LimitedTokenSessionFactory* s_sessionFactoryOverride = nullptr;
#endif
        bool InstalledOperationNeedsDelegation(Context& context)
        {
            if (!context.Contains(Data::InstalledPackageVersion) || !context.Get<Data::InstalledPackageVersion>())
            {
                return false;
            }
            auto metadata = context.Get<Data::InstalledPackageVersion>()->GetMetadata();
            return ConvertToScopeEnum(metadata[PackageVersionMetadata::InstalledScope]) == ScopeEnum::User &&
                InstallerDataWritableUnelevatedForUserScope(ConvertToInstallerTypeEnum(metadata[PackageVersionMetadata::InstalledType]));
        }

        bool UninstallOperationNeedsDelegation(Context& context)
        {
            if (context.Contains(Data::InstalledPackageVersion) && context.Get<Data::InstalledPackageVersion>())
            {
                auto metadata = context.Get<Data::InstalledPackageVersion>()->GetMetadata();
                if (ConvertToInstallerTypeEnum(metadata[PackageVersionMetadata::InstalledType]) == InstallerTypeEnum::Font)
                {
                    auto installedScope = ConvertToScopeEnum(metadata[PackageVersionMetadata::InstalledScope]);
                    auto effectiveScope = context.Args.Contains(Args::Type::InstallScope) ?
                        ConvertToScopeEnum(context.Args.GetArg(Args::Type::InstallScope)) : installedScope;
                    return installedScope != ScopeEnum::Machine || effectiveScope != ScopeEnum::Machine;
                }
            }
            return InstalledOperationNeedsDelegation(context);
        }

        bool IsLocalManifestOperation(Context& context, Client::Operation operation)
        {
            return operation != Client::Operation::Uninstall &&
                (context.Args.Contains(Args::Type::Manifest) ||
                    (context.Contains(Data::Manifest) &&
                        (!context.Contains(Data::PackageVersion) || !context.Get<Data::PackageVersion>())));
        }

        Client::OperationRequest CreateRequest(Context& context, Client::Operation operation)
        {
            THROW_HR_IF(APPINSTALLER_CLI_ERROR_ADMIN_CONTEXT_ACTION_PROHIBITED, IsLocalManifestOperation(context, operation));
            Client::OperationRequest result;
            result.Type = operation;
            if (context.Contains(Data::InstalledPackageVersion) && context.Get<Data::InstalledPackageVersion>())
            {
                auto installed = context.Get<Data::InstalledPackageVersion>();
                auto metadata = installed->GetMetadata();
                result.Installed.Id = installed->GetProperty(PackageVersionProperty::Id);
                result.Installed.Version = installed->GetProperty(PackageVersionProperty::Version);
                result.Installed.Channel = installed->GetProperty(PackageVersionProperty::Channel);
                result.Installed.Scope = metadata[PackageVersionMetadata::InstalledScope];
                result.Installed.Architecture = metadata[PackageVersionMetadata::InstalledArchitecture];
                result.Installed.InstallerType = metadata[PackageVersionMetadata::InstalledType];
                result.Installed.InstallLocation = metadata[PackageVersionMetadata::InstalledLocation];
                for (const auto& code : installed->GetMultiProperty(PackageVersionMultiProperty::ProductCode))
                {
                    result.Installed.ProductCodes.emplace_back(code);
                }
                for (const auto& name : installed->GetMultiProperty(PackageVersionMultiProperty::PackageFamilyName))
                {
                    result.Installed.PackageFamilyNames.emplace_back(name);
                }
                result.PackageId = result.Installed.Id;
            }
            if (context.Contains(Data::Package))
            {
                auto package = context.Get<Data::Package>();
                result.PackageId = package->GetProperty(PackageProperty::Id);
                if (operation != Client::Operation::Install)
                {
                    for (const auto& available : package->GetAvailable())
                    {
                        auto source = available->GetLatestVersion()->GetSource();
                        const auto& details = source.GetDetails();
                        if (std::none_of(result.CorrelatedCatalogs.begin(), result.CorrelatedCatalogs.end(),
                            [&](const auto& catalog) { return catalog.Identifier == details.Identifier; }))
                        {
                            result.CorrelatedCatalogs.push_back({ details.Name, details.Identifier });
                        }
                    }
                }
            }
            if (context.Contains(Data::PackageVersion))
            {
                auto version = context.Get<Data::PackageVersion>();
                auto source = version->GetSource();
                const auto& details = source.GetDetails();
                result.SourceName = details.Name;
                result.SourceIdentifier = details.Identifier;
            }
            if (operation != Client::Operation::Uninstall && context.Contains(Data::Manifest))
            {
                const auto& manifest = context.Get<Data::Manifest>();
                result.PackageId = manifest.Id;
                result.Version = manifest.Version;
                result.Channel = manifest.Channel;
            }
            const auto arg = [&](Args::Type type) { return std::string{ context.Args.GetArg(type) }; };
            result.Scope = arg(Args::Type::InstallScope);
            result.Locale = arg(Args::Type::Locale);
            if (context.Contains(Data::AllowedArchitectures))
            {
                for (auto architecture : context.Get<Data::AllowedArchitectures>())
                {
                    result.AllowedArchitectures.emplace_back(Utility::ToString(architecture));
                }
            }
            else if (context.Args.Contains(Args::Type::InstallArchitecture))
            {
                result.AllowedArchitectures.emplace_back(
                    Utility::ToString(Utility::ConvertToArchitectureEnum(context.Args.GetArg(Args::Type::InstallArchitecture))));
            }
            if (context.Args.Contains(Args::Type::InstallerType))
            {
                result.InstallerType = InstallerTypeToString(ConvertToInstallerTypeEnum(std::string{ context.Args.GetArg(Args::Type::InstallerType) }));
            }
            result.Rename = arg(Args::Type::Rename);
            result.LogOutputPath = arg(Args::Type::Log);
            result.InstallLocation = arg(Args::Type::InstallLocation);
            result.ReplacementArguments = arg(Args::Type::Override);
            result.AdditionalArguments = arg(Args::Type::CustomSwitches);
            auto authentication = GetAuthenticationArguments(context);
            result.AuthenticationMode = Authentication::AuthenticationModeToString(authentication.Mode);
            result.AuthenticationAccount = authentication.AuthenticationAccount;
            result.SourceHeader = arg(Args::Type::CustomHeader);
            result.Proxy = arg(Args::Type::Proxy);
            result.NoProxy = context.Args.Contains(Args::Type::NoProxy);
            result.AcceptSourceAgreements = context.Args.Contains(Args::Type::AcceptSourceAgreements);
            result.Interactive = context.Args.Contains(Args::Type::Interactive);
            result.AllowUnknownScope = context.Contains(Data::AllowUnknownScope) && context.Get<Data::AllowUnknownScope>();
            result.UseDefaultScopePreference = !context.Contains(Data::AllowUnknownScope);
            result.Silent = context.Args.Contains(Args::Type::Silent);
            result.Force = context.Args.Contains(Args::Type::Force);
            result.UninstallPrevious = context.Args.Contains(Args::Type::UninstallPrevious);
            result.Purge = context.Args.Contains(Args::Type::Purge);
            result.Preserve = context.Args.Contains(Args::Type::Preserve);
            result.AllowUpgradeToUnknownVersion = context.Args.Contains(Args::Type::IncludeUnknown);
            result.IgnoreLocalArchiveMalwareScan = context.Args.Contains(Args::Type::IgnoreLocalArchiveMalwareScan);
            result.BypassIsStoreClientBlockedPolicyCheck = WI_IsFlagSet(context.GetFlags(), ContextFlag::BypassIsStoreClientBlockedPolicyCheck);
            if (auto comContext = dynamic_cast<COMContext*>(&context))
            {
                result.CorrelationData = Utility::ConvertToUTF8(comContext->GetCorrelationJson());
            }
            return result;
        }

        bool PrepareOperation(Context& context, Client::Operation operation)
        {
            if (context.Contains(Data::LimitedTokenDeployment))
            {
                THROW_HR_IF(E_UNEXPECTED, context.Get<Data::LimitedTokenDeployment>()->Operation != operation);
                return true;
            }
            if (!Runtime::IsRunningWithNonDefaultFullToken())
            {
                return false;
            }
            if (IsLocalManifestOperation(context, operation))
            {
                return false;
            }
            auto deployment = std::make_shared<LimitedTokenDeployment>();
#ifndef AICLI_DISABLE_TEST_HOOKS
            if (s_sessionFactoryOverride)
            {
                if (!*s_sessionFactoryOverride)
                {
                    return false;
                }
                deployment->Session = (*s_sessionFactoryOverride)(CreateRequest(context, operation));
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
                deployment->Module.reset(LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32));
                THROW_LAST_ERROR_IF(!deployment->Module);
                auto create = reinterpret_cast<Client::CreateDeploymentSession>(
                    GetProcAddress(deployment->Module.get(), Client::CreateDeploymentSessionExport));
                THROW_LAST_ERROR_IF(!create);
                deployment->Session.reset(create(CreateRequest(context, operation)));
            }
            THROW_HR_IF_NULL(E_UNEXPECTED, deployment->Session.get());
            deployment->Operation = operation;
            context.Add<Data::LimitedTokenDeployment>(std::move(deployment));
            return true;
        }
    }

#ifndef AICLI_DISABLE_TEST_HOOKS
    LimitedTokenSessionFactory* TestHook_SetLimitedTokenSessionFactory(LimitedTokenSessionFactory* factory)
    {
        return std::exchange(s_sessionFactoryOverride, factory);
    }
#endif

    void PrepareLimitedTokenInstall(Context& context)
    {
        if (!Runtime::IsRunningWithNonDefaultFullToken() ||
            WI_IsFlagSet(context.GetFlags(), ContextFlag::InstallerDownloadOnly) ||
            WI_IsFlagSet(context.GetFlags(), ContextFlag::InstallerExecutionUseRepair))
        {
            return;
        }
        const auto& installer = context.Get<Data::Installer>().value();
        bool update = WI_IsFlagSet(context.GetFlags(), ContextFlag::InstallerExecutionUseUpdate);
        bool uninstallPrevious = update &&
            (installer.UpdateBehavior == UpdateBehaviorEnum::UninstallPrevious || context.Args.Contains(Args::Type::UninstallPrevious));
        if ((uninstallPrevious && UninstallOperationNeedsDelegation(context)) ||
            (installer.EffectiveInstallerType() == InstallerTypeEnum::Portable && GetPortableInstallScope(context) != ScopeEnum::Machine) ||
            (installer.EffectiveInstallerType() == InstallerTypeEnum::Font && GetFontInstallScope(context) != ScopeEnum::Machine))
        {
            PrepareOperation(context, update ? Client::Operation::Upgrade : Client::Operation::Install);
        }
    }

    bool PrepareLimitedTokenUninstall(Context& context)
    {
        if (!Runtime::IsRunningWithNonDefaultFullToken())
        {
            return false;
        }
        return UninstallOperationNeedsDelegation(context) && PrepareOperation(context, Client::Operation::Uninstall);
    }

    bool PrepareLimitedTokenRepair(Context& context)
    {
        return Runtime::IsRunningWithNonDefaultFullToken() && InstalledOperationNeedsDelegation(context) &&
            PrepareOperation(context, Client::Operation::Repair);
    }

    void ExecuteLimitedTokenOperation(Context& context)
    {
        auto deployment = context.Get<Data::LimitedTokenDeployment>();
        THROW_HR_IF_NULL(E_UNEXPECTED, deployment.get());
        THROW_HR_IF_NULL(E_UNEXPECTED, deployment->Session.get());
        THROW_HR_IF(E_UNEXPECTED, deployment->Started);
        deployment->Started = true;
        auto result = context.Reporter.ExecuteWithProgress([&](IProgressCallback& progress)
        {
            Client::OperationCallbacks callbacks;
            callbacks.IsCancelled = [&]() { return context.IsTerminated() || progress.IsCancelledBy(CancelReason::Any); };
            callbacks.Progress = [&](const Client::OperationProgress& value)
            {
                auto stage = context.GetExecutionStage();
                switch (value.Stage)
                {
                case Client::OperationStage::Queued:
                    progress.SetProgressMessage(Resource::String::InstallWaitingOnAnother());
                    break;
                case Client::OperationStage::Download:
                    progress.SetProgressMessage({});
                    stage = ExecutionStage::Download;
                    break;
                case Client::OperationStage::Execution:
                    progress.SetProgressMessage({});
                    stage = ExecutionStage::Execution;
                    break;
                case Client::OperationStage::PostExecution:
                    progress.SetProgressMessage({});
                    stage = ExecutionStage::PostExecution;
                    break;
                }
                if (stage > context.GetExecutionStage())
                {
                    context.SetExecutionStage(stage);
                }
                progress.OnProgress(value.Current, value.Maximum, value.Bytes ? ProgressType::Bytes : ProgressType::Percent);
            };
            return deployment->Session->Execute(CreateRequest(context, deployment->Operation), callbacks);
        });
        context.Add<Data::OperationReturnCode>(result.ReturnCode);
        context << ForceInstalledCacheUpdate;
        if (result.RebootRequired)
        {
            context.SetFlags(ContextFlag::RebootRequired);
        }
        if (result.ExtendedError == APPINSTALLER_CLI_ERROR_INSTALL_REBOOT_REQUIRED_FOR_INSTALL)
        {
            context.SetFlags(ContextFlag::RegisterResume);
        }
        if (!result.Succeeded || FAILED(result.ExtendedError))
        {
            HRESULT error = FAILED(result.ExtendedError) ? result.ExtendedError : E_UNEXPECTED;
            if (error == APPINSTALLER_CLI_ERROR_INSTALLER_HASH_MISMATCH)
            {
                context.Reporter.Error() << Resource::String::InstallerHashMismatchAdminBlock << std::endl;
            }
            context.Reporter.Error() << Utility::LocIndView{ GetUserPresentableMessage(error) } << std::endl;
            AICLI_TERMINATE_CONTEXT(error);
        }
        context.SetExecutionStage(ExecutionStage::PostExecution);
        switch (deployment->Operation)
        {
        case Client::Operation::Install:
        case Client::Operation::Upgrade:
            context.Reporter.Info() << Resource::String::InstallFlowInstallSuccess << std::endl;
            break;
        case Client::Operation::Uninstall:
            context.Reporter.Info() << Resource::String::UninstallFlowUninstallSuccess << std::endl;
            break;
        case Client::Operation::Repair:
            context.Reporter.Info() << Resource::String::RepairFlowRepairSuccess << std::endl;
            break;
        }
    }
}
