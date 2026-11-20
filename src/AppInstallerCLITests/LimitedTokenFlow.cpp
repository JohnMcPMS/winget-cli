// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "pch.h"
#include "TestHooks.h"
#include "WorkflowCommon.h"
#include <Workflows/DownloadFlow.h>
#include <Workflows/InstallFlow.h>
#include <Workflows/LimitedTokenFlow.h>
#include <Workflows/RepairFlow.h>
#include <Workflows/SourceFlow.h>
#include <Workflows/UninstallFlow.h>
#include <winget/ManifestYamlParser.h>

using namespace TestCommon;
using namespace AppInstaller::CLI;
using namespace AppInstaller::CLI::Execution;
using namespace AppInstaller::CLI::Workflow;
using namespace AppInstaller::Repository;
namespace Client = AppInstaller::Deployment::Client;

namespace
{
    struct TestSession : Client::IDeploymentSession
    {
        std::function<Client::OperationResult(const Client::OperationCallbacks&)> Run;
        std::function<void(const Client::OperationRequest&)> ValidateRequest;

        explicit TestSession(std::function<Client::OperationResult(const Client::OperationCallbacks&)> run) :
            Run(std::move(run))
        {
        }

        Client::OperationResult Execute(const Client::OperationRequest& request, const Client::OperationCallbacks& callbacks) override
        {
            if (ValidateRequest)
            {
                ValidateRequest(request);
            }
            return Run(callbacks);
        }
    };

    void AddInstalledPackage(TestContext& context, std::string_view type = "Exe", std::string_view scope = "User")
    {
        auto manifest = AppInstaller::Manifest::YamlParser::CreateFromPath(TestDataFile("InstallFlowTest_Exe.yaml"));
        context.Add<Data::InstalledPackageVersion>(TestPackageVersion::Make(manifest, TestPackageVersion::MetadataMap{
            { PackageVersionMetadata::InstalledType, std::string{ type } },
            { PackageVersionMetadata::InstalledScope, std::string{ scope } },
            { PackageVersionMetadata::InstalledArchitecture, "X64" },
            { PackageVersionMetadata::InstalledLocation, "C:\\Test\\Installed" },
            { PackageVersionMetadata::StandardUninstallCommand, "C:\\Test\\uninstall.exe" },
        }));
    }

    std::unique_ptr<TestSession> SuccessfulSession(size_t& executions)
    {
        return std::make_unique<TestSession>([&](const Client::OperationCallbacks& callbacks)
        {
            ++executions;
            REQUIRE_FALSE(callbacks.IsCancelled());
            callbacks.Progress({ Client::OperationStage::Execution, 50, 100, false });
            return Client::OperationResult{ S_OK, 0, true, false };
        });
    }
}

TEST_CASE("LimitedTokenFlow_UninstallOwnsPreparationExecutionAndTracking", "[LimitedTokenFlow][UninstallFlow][workflow]")
{
    TestHook::SetIsRunningWithNonDefaultFullToken_Override token(true);
    std::ostringstream output;
    TestContext context{ output, std::cin };
    AddInstalledPackage(context);
    context.Args.AddArg(Args::Type::Purge);
    context.Args.AddArg(Args::Type::Silent);
    context.Args.AddArg(Args::Type::AuthenticationMode, "interactive");
    size_t executions = 0;
    TestHook::SetLimitedTokenSessionFactory_Override factory([&](const Client::OperationRequest& request)
    {
        REQUIRE(request.Type == Client::Operation::Uninstall);
        REQUIRE(request.Installed.Architecture == "X64");
        REQUIRE(request.Installed.InstallLocation == "C:\\Test\\Installed");
        REQUIRE(request.Purge);
        REQUIRE(request.Silent);
        REQUIRE(request.AuthenticationMode == "interactive");
        return SuccessfulSession(executions);
    });
    context.Override({ ForceInstalledCacheUpdate, [](TestContext&) {}, 1 });
    context << GetUninstallInfo;
    REQUIRE_FALSE(context.IsTerminated());
    REQUIRE_FALSE(context.Contains(Data::UninstallString));
    REQUIRE_FALSE(context.Contains(Data::PortableInstaller));

    // No Package is present: local RecordUninstall would fail instead of updating the worker's tracking again.
    context << ExecuteUninstaller << RecordUninstall;
    REQUIRE_FALSE(context.IsTerminated());
    REQUIRE(executions == 1);
    REQUIRE(context.Get<Data::OperationReturnCode>() == 0);
}

TEST_CASE("LimitedTokenFlow_MissingHelperRetainsExistingBlock", "[LimitedTokenFlow][UninstallFlow][workflow]")
{
    TestHook::SetIsRunningWithNonDefaultFullToken_Override token(true);
    TestHook::SetLimitedTokenSessionFactory_Override factory({});
    std::ostringstream output;
    TestContext context{ output, std::cin };
    AddInstalledPackage(context);
    context << GetUninstallInfo;
    REQUIRE_TERMINATED_WITH(context, APPINSTALLER_CLI_ERROR_ADMIN_CONTEXT_ACTION_PROHIBITED);
    REQUIRE_FALSE(context.Contains(Data::UninstallString));
    REQUIRE_FALSE(context.Contains(Data::LimitedTokenDeployment));
}

TEST_CASE("LimitedTokenFlow_ActivationFailureNeverFallsBack", "[LimitedTokenFlow][UninstallFlow][workflow]")
{
    TestHook::SetIsRunningWithNonDefaultFullToken_Override token(true);
    TestHook::SetLimitedTokenSessionFactory_Override factory([](const Client::OperationRequest&) -> std::unique_ptr<Client::IDeploymentSession>
    {
        THROW_HR(REGDB_E_CLASSNOTREG);
    });
    std::ostringstream output;
    TestContext context{ output, std::cin };
    AddInstalledPackage(context);
    REQUIRE_THROWS_HR(context << GetUninstallInfo, REGDB_E_CLASSNOTREG);
    REQUIRE_FALSE(context.Contains(Data::UninstallString));
    REQUIRE_FALSE(context.Contains(Data::LimitedTokenDeployment));
}

TEST_CASE("LimitedTokenFlow_NonFullTokensRemainLocal", "[LimitedTokenFlow][UninstallFlow][workflow]")
{
    TestHook::SetIsRunningWithNonDefaultFullToken_Override token(false);
    TestHook::SetLimitedTokenSessionFactory_Override factory([](const Client::OperationRequest&) -> std::unique_ptr<Client::IDeploymentSession>
    {
        FAIL("A non-Full token must not activate a delegation session.");
        return {};
    });
    std::ostringstream output;
    TestContext context{ output, std::cin };
    AddInstalledPackage(context);
    context << GetUninstallInfo;
    REQUIRE_FALSE(context.IsTerminated());
    REQUIRE(context.Contains(Data::UninstallString));
    REQUIRE_FALSE(context.Contains(Data::LimitedTokenDeployment));
}

TEST_CASE("LimitedTokenFlow_InstallDoesNotDownloadOrRunLocalSuffix", "[LimitedTokenFlow][InstallFlow][workflow]")
{
    using InstallerType = AppInstaller::Manifest::InstallerTypeEnum;
    const auto type = GENERATE(InstallerType::Portable, InstallerType::Font);
    CAPTURE(type);
    TestHook::SetIsRunningWithNonDefaultFullToken_Override token(true);
    std::ostringstream output;
    TestContext context{ output, std::cin };
    auto manifest = AppInstaller::Manifest::YamlParser::CreateFromPath(TestDataFile("InstallFlowTest_Exe.yaml"));
    auto installer = manifest.Installers.at(0);
    installer.BaseInstallerType = type;
    installer.Scope = AppInstaller::Manifest::ScopeEnum::User;
    manifest.Installers.front() = installer;
    auto source = std::make_shared<TestSource>();
    context.Add<Data::Manifest>(manifest);
    context.Add<Data::Installer>(installer);
    context.Add<Data::PackageVersion>(TestPackageVersion::Make(manifest, source));
    context.Add<Data::Package>(TestCompositePackage::Make(std::vector{ manifest }));
    context.Args.AddArg(Args::Type::InstallScope, "user");
    context.Args.AddArg(Args::Type::Rename, "renamed");
    context.Args.AddArg(Args::Type::HashOverride);
    context.Args.AddArg(Args::Type::Locale, "en-US");
    context.Args.AddArg(Args::Type::InstallArchitecture, "x86");
    context.Args.AddArg(Args::Type::InstallerType, AppInstaller::Manifest::InstallerTypeToString(type));
    size_t executions = 0;
    TestHook::SetLimitedTokenSessionFactory_Override factory([&](const Client::OperationRequest& request)
    {
        REQUIRE(request.Type == Client::Operation::Install);
        REQUIRE(request.Rename == "renamed");
        REQUIRE(request.Locale == "en-US");
        REQUIRE(request.AllowedArchitectures == std::vector<std::string>{ "X86" });
        REQUIRE(request.InstallerType == AppInstaller::Manifest::InstallerTypeToString(type));
        REQUIRE(request.Scope == "user");
        REQUIRE_FALSE(request.AllowUnknownScope);
        REQUIRE(request.UseDefaultScopePreference);
        REQUIRE(request.CorrelatedCatalogs.empty());
        REQUIRE(request.SourceName == source->Details.Name);
        REQUIRE(request.SourceIdentifier == source->Details.Identifier);
        return SuccessfulSession(executions);
    });
    context.Override({ ForceInstalledCacheUpdate, [](TestContext&) {}, 1 });
    context.Override({ DisplayInstallationNotes, [](TestContext&) {}, 1 });
    context << EnsureSupportForInstall << DownloadInstaller << ReverifyInstallerHash << InstallPackageInstaller;
    REQUIRE_FALSE(context.IsTerminated());
    REQUIRE(executions == 1);
    REQUIRE_FALSE(context.Contains(Data::InstallerPath));
    REQUIRE_FALSE(context.Contains(Data::ARPCorrelationData));
    REQUIRE_FALSE(context.Contains(Data::PortableInstaller));
}

TEST_CASE("LimitedTokenFlow_RepairDoesNotPrepareLocalCommands", "[LimitedTokenFlow][RepairFlow][workflow]")
{
    TestHook::SetIsRunningWithNonDefaultFullToken_Override token(true);
    std::ostringstream output;
    TestContext context{ output, std::cin };
    AddInstalledPackage(context, "Msi");
    size_t executions = 0;
    TestHook::SetLimitedTokenSessionFactory_Override factory([&](const Client::OperationRequest& request)
    {
        REQUIRE(request.Type == Client::Operation::Repair);
        return SuccessfulSession(executions);
    });
    context.Override({ ForceInstalledCacheUpdate, [](TestContext&) {}, 1 });
    context << RepairSinglePackage;
    REQUIRE_FALSE(context.IsTerminated());
    REQUIRE(executions == 1);
    REQUIRE_FALSE(context.Contains(Data::RepairString));
    REQUIRE_FALSE(context.Contains(Data::ProductCodes));
}

TEST_CASE("LimitedTokenFlow_UninstallPreviousDelegatesTheWholeUpgrade", "[LimitedTokenFlow][InstallFlow][workflow]")
{
    TestHook::SetIsRunningWithNonDefaultFullToken_Override token(true);
    std::ostringstream output;
    TestContext context{ output, std::cin };
    AddInstalledPackage(context);
    auto manifest = AppInstaller::Manifest::YamlParser::CreateFromPath(TestDataFile("InstallFlowTest_Exe.yaml"));
    manifest.Version = "2.0";
    auto installer = manifest.Installers.at(0);
    installer.UpdateBehavior = AppInstaller::Manifest::UpdateBehaviorEnum::UninstallPrevious;
    manifest.Installers.front() = installer;
    auto source = std::make_shared<TestSource>();
    context.Add<Data::Manifest>(manifest);
    context.Add<Data::Installer>(installer);
    context.Add<Data::PackageVersion>(TestPackageVersion::Make(manifest, source));
    context.SetFlags(ContextFlag::InstallerExecutionUseUpdate);
    size_t executions = 0;
    TestHook::SetLimitedTokenSessionFactory_Override factory([&](const Client::OperationRequest& request)
    {
        REQUIRE(request.Type == Client::Operation::Upgrade);
        REQUIRE(request.Installed.Version != request.Version);
        auto session = SuccessfulSession(executions);
        session->ValidateRequest = [](const Client::OperationRequest& finalRequest)
        {
            REQUIRE(finalRequest.LogOutputPath == "C:\\Test\\installer.log");
        };
        return session;
    });
    context.Override({ ForceInstalledCacheUpdate, [](TestContext&) {}, 1 });
    context.Override({ DisplayInstallationNotes, [](TestContext&) {}, 1 });
    context << EnsureSupportForInstall;
    context.Args.AddArg(Args::Type::Log, "C:\\Test\\installer.log");
    context << InstallPackageInstaller;
    REQUIRE_FALSE(context.IsTerminated());
    REQUIRE(executions == 1);
    REQUIRE_FALSE(context.Contains(Data::UninstallString));
    REQUIRE_FALSE(context.Contains(Data::UninstalledPackageMetadata));
}

TEST_CASE("LimitedTokenFlow_LocalManifestRetainsElevationBlock", "[LimitedTokenFlow][workflow]")
{
    const auto operation = GENERATE(Client::Operation::Install, Client::Operation::Upgrade, Client::Operation::Repair);
    const auto manifestContext = GENERATE(std::pair{ true, false }, std::pair{ true, true }, std::pair{ false, false });
    CAPTURE(operation, manifestContext.first, manifestContext.second);
    TestHook::SetIsRunningWithNonDefaultFullToken_Override token(true);
    TestHook::SetLimitedTokenSessionFactory_Override factory([](const Client::OperationRequest&) -> std::unique_ptr<Client::IDeploymentSession>
    {
        FAIL("Local-manifest operations must not activate the COM helper.");
        return {};
    });
    std::ostringstream output;
    TestContext context{ output, std::cin };
    auto manifest = AppInstaller::Manifest::YamlParser::CreateFromPath(TestDataFile("InstallFlowTest_Exe.yaml"));
    auto installer = manifest.Installers.at(0);
    installer.BaseInstallerType = operation == Client::Operation::Repair ?
        AppInstaller::Manifest::InstallerTypeEnum::Exe : AppInstaller::Manifest::InstallerTypeEnum::Portable;
    installer.Scope = AppInstaller::Manifest::ScopeEnum::User;
    manifest.Installers.front() = installer;
    context.Add<Data::Manifest>(manifest);
    context.Add<Data::Installer>(installer);
    if (manifestContext.first)
    {
        context.Args.AddArg(Args::Type::Manifest, "local.yaml");
    }
    auto source = std::make_shared<TestSource>();
    if (manifestContext.second)
    {
        context.Add<Data::PackageVersion>(TestPackageVersion::Make(manifest, source));
    }
    if (operation == Client::Operation::Upgrade)
    {
        AddInstalledPackage(context, "Portable");
        context.SetFlags(ContextFlag::InstallerExecutionUseUpdate);
    }
    if (operation == Client::Operation::Repair)
    {
        AddInstalledPackage(context);
        context << RepairSinglePackage;
    }
    else
    {
        context << EnsureSupportForInstall;
    }
    REQUIRE_TERMINATED_WITH(context, APPINSTALLER_CLI_ERROR_ADMIN_CONTEXT_ACTION_PROHIBITED);
    REQUIRE_FALSE(context.Contains(Data::LimitedTokenDeployment));
}

TEST_CASE("LimitedTokenFlow_ForwardsSelectionInputsWithoutPinningDefaults", "[LimitedTokenFlow][workflow]")
{
    const auto operation = GENERATE(Client::Operation::Install, Client::Operation::Upgrade, Client::Operation::Repair);
    const int architectureInput = GENERATE(0, 1, 2, 3);
    CAPTURE(operation, architectureInput);
    TestHook::SetIsRunningWithNonDefaultFullToken_Override token(true);
    std::ostringstream output;
    TestContext context{ output, std::cin };
    auto manifest = AppInstaller::Manifest::YamlParser::CreateFromPath(TestDataFile("InstallFlowTest_Exe.yaml"));
    auto type = operation == Client::Operation::Repair ? AppInstaller::Manifest::InstallerTypeEnum::Exe :
        operation == Client::Operation::Install ? AppInstaller::Manifest::InstallerTypeEnum::Font :
        AppInstaller::Manifest::InstallerTypeEnum::Portable;
    manifest.Installers.front().BaseInstallerType = type;
    manifest.Installers.front().Scope = AppInstaller::Manifest::ScopeEnum::User;
    auto source = std::make_shared<TestSource>();
    context.Add<Data::Manifest>(manifest);
    context.Add<Data::Installer>(manifest.Installers.front());
    context.Add<Data::PackageVersion>(TestPackageVersion::Make(manifest, source));
    context.Args.AddArg(Args::Type::Locale, "fr-FR");
    context.Args.AddArg(Args::Type::InstallerType, AppInstaller::Manifest::InstallerTypeToString(type));
    if (operation != Client::Operation::Install)
    {
        AddInstalledPackage(context, AppInstaller::Manifest::InstallerTypeToString(type));
        context.SetFlags(operation == Client::Operation::Upgrade ?
            ContextFlag::InstallerExecutionUseUpdate : ContextFlag::InstallerExecutionUseRepair);
    }
    if (architectureInput != 0)
    {
        context.Args.AddArg(Args::Type::InstallArchitecture, "x86");
        context.Args.AddArg(Args::Type::InstallScope, "user");
    }
    if (architectureInput >= 2)
    {
        context.Add<Data::AllowedArchitectures>(std::vector{
            AppInstaller::Utility::Architecture::Neutral, AppInstaller::Utility::Architecture::X64 });
        context.Add<Data::AllowUnknownScope>(architectureInput == 3);
    }
    size_t executions = 0;
    TestHook::SetLimitedTokenSessionFactory_Override factory([&](const Client::OperationRequest& request)
    {
        REQUIRE(request.Type == operation);
        REQUIRE(request.PackageId == manifest.Id);
        REQUIRE(request.Version == manifest.Version);
        REQUIRE(request.Channel == manifest.Channel);
        REQUIRE(request.Locale == "fr-FR");
        REQUIRE(request.InstallerType == AppInstaller::Manifest::InstallerTypeToString(type));
        if (architectureInput == 0)
        {
            REQUIRE(request.AllowedArchitectures.empty());
            REQUIRE(request.Scope.empty());
        }
        else if (architectureInput == 1)
        {
            REQUIRE(request.AllowedArchitectures == std::vector<std::string>{ "X86" });
            REQUIRE(request.Scope == "user");
        }
        else
        {
            REQUIRE(request.AllowedArchitectures == (std::vector<std::string>{ "Neutral", "X64" }));
            REQUIRE(request.Scope == "user");
        }
        REQUIRE(request.AllowUnknownScope == (architectureInput == 3));
        REQUIRE(request.UseDefaultScopePreference == (architectureInput < 2));
        return SuccessfulSession(executions);
    });
    if (operation == Client::Operation::Repair)
    {
        REQUIRE(PrepareLimitedTokenRepair(context));
    }
    else
    {
        PrepareLimitedTokenInstall(context);
    }
    REQUIRE(context.Contains(Data::LimitedTokenDeployment));
    REQUIRE_FALSE(context.IsTerminated());
    REQUIRE(executions == 0);
}

TEST_CASE("LimitedTokenFlow_FontUninstallChecksSelectedAndEffectiveScopes", "[LimitedTokenFlow][UninstallFlow][workflow]")
{
    auto scopes = GENERATE(std::pair{ "Machine", "user" }, std::pair{ "User", "machine" }, std::pair{ "Unknown", "" });
    CAPTURE(scopes.first, scopes.second);
    TestHook::SetIsRunningWithNonDefaultFullToken_Override token(true);
    std::ostringstream output;
    TestContext context{ output, std::cin };
    AddInstalledPackage(context, "Font", scopes.first);
    if (*scopes.second)
    {
        context.Args.AddArg(Args::Type::InstallScope, scopes.second);
    }
    size_t executions = 0;
    TestHook::SetLimitedTokenSessionFactory_Override factory([&](const Client::OperationRequest& request)
    {
        REQUIRE(request.Type == Client::Operation::Uninstall);
        return SuccessfulSession(executions);
    });
    context.Override({ ForceInstalledCacheUpdate, [](TestContext&) {}, 1 });
    context << GetUninstallInfo << ExecuteUninstaller;
    REQUIRE_FALSE(context.IsTerminated());
    REQUIRE(executions == 1);
}

TEST_CASE("LimitedTokenFlow_RemoteFailurePreservesCodesAndCannotReplay", "[LimitedTokenFlow][workflow]")
{
    TestHook::SetIsRunningWithNonDefaultFullToken_Override token(true);
    std::ostringstream output;
    TestContext context{ output, std::cin };
    AddInstalledPackage(context);
    size_t executions = 0;
    TestHook::SetLimitedTokenSessionFactory_Override factory([&](const Client::OperationRequest&)
    {
        return std::make_unique<TestSession>([&](const Client::OperationCallbacks&)
        {
            ++executions;
            return Client::OperationResult{ APPINSTALLER_CLI_ERROR_EXEC_UNINSTALL_COMMAND_FAILED, 123, false, true };
        });
    });
    context.Override({ ForceInstalledCacheUpdate, [](TestContext&) {}, 1 });
    context << GetUninstallInfo << ExecuteUninstaller;
    REQUIRE_TERMINATED_WITH(context, APPINSTALLER_CLI_ERROR_EXEC_UNINSTALL_COMMAND_FAILED);
    REQUIRE(context.Get<Data::OperationReturnCode>() == 123);
    REQUIRE(WI_IsFlagSet(context.GetFlags(), ContextFlag::RebootRequired));
    REQUIRE(executions == 1);
    REQUIRE_THROWS_HR(ExecuteLimitedTokenOperation(context), E_UNEXPECTED);
}

TEST_CASE("LimitedTokenFlow_DisconnectedOperationCannotReplay", "[LimitedTokenFlow][workflow]")
{
    TestHook::SetIsRunningWithNonDefaultFullToken_Override token(true);
    std::ostringstream output;
    TestContext context{ output, std::cin };
    AddInstalledPackage(context);
    size_t executions = 0;
    TestHook::SetLimitedTokenSessionFactory_Override factory([&](const Client::OperationRequest&)
    {
        return std::make_unique<TestSession>([&](const Client::OperationCallbacks&) -> Client::OperationResult
        {
            ++executions;
            THROW_HR(RPC_E_DISCONNECTED);
        });
    });
    context << GetUninstallInfo;
    REQUIRE_THROWS_HR(ExecuteLimitedTokenOperation(context), RPC_E_DISCONNECTED);
    REQUIRE_THROWS_HR(ExecuteLimitedTokenOperation(context), E_UNEXPECTED);
    REQUIRE(executions == 1);
    REQUIRE_FALSE(context.Contains(Data::UninstallString));
}

TEST_CASE("LimitedTokenFlow_ProgressCannotMoveCoordinatorBackwards", "[LimitedTokenFlow][workflow]")
{
    TestHook::SetIsRunningWithNonDefaultFullToken_Override token(true);
    std::ostringstream output;
    TestContext context{ output, std::cin };
    AddInstalledPackage(context);
    TestHook::SetLimitedTokenSessionFactory_Override factory([&](const Client::OperationRequest&)
    {
        return std::make_unique<TestSession>([&](const Client::OperationCallbacks& callbacks)
        {
            callbacks.Progress({ Client::OperationStage::Execution, 50, 100, false });
            callbacks.Progress({ Client::OperationStage::Download, 10, 100, true });
            REQUIRE(context.GetExecutionStage() == ExecutionStage::Execution);
            return Client::OperationResult{ S_OK, 0, true, false };
        });
    });
    context.Override({ ForceInstalledCacheUpdate, [](TestContext&) {}, 1 });
    context << GetUninstallInfo << ExecuteUninstaller;
    REQUIRE_FALSE(context.IsTerminated());
    REQUIRE(context.GetExecutionStage() == ExecutionStage::PostExecution);
    REQUIRE(executions == 1);
}
