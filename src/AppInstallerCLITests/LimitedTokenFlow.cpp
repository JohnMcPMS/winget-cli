// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "pch.h"
#include "TestHooks.h"
#include "WorkflowCommon.h"
#include <Workflows/LimitedTokenFlow.h>
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
    void AddInstalledPackage(TestContext& context, std::string_view type = "Exe", std::string_view scope = "User")
    {
        auto manifest = AppInstaller::Manifest::YamlParser::CreateFromPath(TestDataFile("InstallFlowTest_Exe.yaml"));
        context.Add<Data::InstalledPackageVersion>(TestPackageVersion::Make(manifest, TestPackageVersion::MetadataMap{
            { PackageVersionMetadata::InstalledType, std::string{ type } },
            { PackageVersionMetadata::InstalledScope, std::string{ scope } },
            { PackageVersionMetadata::StandardUninstallCommand, "C:\\Test\\uninstall.exe" },
        }));
    }

    struct TestSession : Client::IUninstallSession
    {
        std::function<Client::UninstallResult(const Client::UninstallRequest&, const Client::UninstallCallbacks&)> Run;

        Client::UninstallResult Execute(const Client::UninstallRequest& request, const Client::UninstallCallbacks& callbacks) override
        {
            return Run(request, callbacks);
        }
    };
}

TEST_CASE("LimitedTokenUninstall_OwnsClassicUninstall", "[LimitedTokenFlow][UninstallFlow][workflow]")
{
    const auto type = GENERATE("Exe", "Burn", "Inno", "Nullsoft", "Msi", "Wix");
    CAPTURE(type);
    TestHook::SetIsRunningWithNonDefaultFullToken_Override token(true);
    std::ostringstream output;
    TestContext context{ output, std::cin };
    AddInstalledPackage(context, type);
    context.Args.AddArg(Args::Type::InstallScope, "user");
    context.Args.AddArg(Args::Type::Interactive);
    context.Args.AddArg(Args::Type::Force);
    context.Args.AddArg(Args::Type::ProductCode, "test-product");
    size_t executions = 0;
    TestHook::SetLimitedTokenUninstallFactory_Override factory([&](const Client::UninstallRequest& request)
    {
        REQUIRE(request.Scope == "user");
        REQUIRE(request.ProductCode == "test-product");
        REQUIRE(request.Interactive);
        REQUIRE_FALSE(request.Silent);
        REQUIRE(request.Force);
        auto session = std::make_unique<TestSession>();
        session->Run = [&](const Client::UninstallRequest& finalRequest, const Client::UninstallCallbacks& callbacks)
        {
            ++executions;
            REQUIRE(finalRequest.LogOutputPath == "C:\\Test\\uninstall.log");
            REQUIRE_FALSE(callbacks.IsCancelled());
            callbacks.Progress(true, 0);
            callbacks.Progress(false, 0.5);
            return Client::UninstallResult{ S_OK, 0, true, false };
        };
        return session;
    });
    context.Override({ ForceInstalledCacheUpdate, [](TestContext&) {}, 1 });
    context << GetUninstallInfo;
    REQUIRE(context.Contains(Data::LimitedTokenUninstall));
    REQUIRE_FALSE(context.Contains(Data::UninstallString));
    REQUIRE_FALSE(context.Contains(Data::ProductCodes));
    context.Args.AddArg(Args::Type::Log, "C:\\Test\\uninstall.log");
    // No Package is present: local RecordUninstall would fail instead of duplicating worker tracking.
    context << ExecuteUninstaller << RecordUninstall;
    REQUIRE_FALSE(context.IsTerminated());
    REQUIRE(executions == 1);
    REQUIRE(context.Get<Data::OperationReturnCode>() == 0);
}

TEST_CASE("LimitedTokenUninstall_PreservesDiscoveryContext", "[LimitedTokenFlow][workflow]")
{
    TestHook::SetIsRunningWithNonDefaultFullToken_Override token(true);
    std::ostringstream output;
    TestContext context{ output, std::cin };
    AddInstalledPackage(context);
    auto manifest = context.Get<Data::InstalledPackageVersion>()->GetManifest();
    auto source = std::make_shared<TestSource>();
    context.Add<Data::Package>(TestCompositePackage::Make(std::vector{ manifest }, source));
    context.Args.AddArg(Args::Type::CustomHeader, "test-header");
    context.Args.AddArg(Args::Type::AuthenticationMode, "interactive");
    context.Args.AddArg(Args::Type::AuthenticationAccount, "test-account");
    context.Args.AddArg(Args::Type::AcceptSourceAgreements);
    context.Args.AddArg(Args::Type::Silent);
    TestHook::SetLimitedTokenUninstallFactory_Override factory([&](const Client::UninstallRequest& request)
    {
        REQUIRE(request.PackageId == manifest.Id);
        REQUIRE(request.Scope.empty());
        REQUIRE(request.Catalogs.size() == 1);
        REQUIRE(request.Catalogs[0].Name == source->Details.Name);
        REQUIRE(request.Catalogs[0].Identifier == source->Details.Identifier);
        REQUIRE(request.SourceHeader == "test-header");
        REQUIRE(request.AuthenticationMode == "interactive");
        REQUIRE(request.AuthenticationAccount == "test-account");
        REQUIRE(request.AcceptSourceAgreements);
        REQUIRE(request.Silent);
        return std::make_unique<TestSession>();
    });
    REQUIRE(PrepareLimitedTokenUninstall(context));
}

TEST_CASE("LimitedTokenUninstall_UnsupportedRequestsRemainBlocked", "[LimitedTokenFlow][workflow]")
{
    const auto argument = GENERATE(Args::Type::TargetVersion, Args::Type::AllVersions, Args::Type::Channel,
        Args::Type::Proxy, Args::Type::NoProxy);
    CAPTURE(argument);
    TestHook::SetIsRunningWithNonDefaultFullToken_Override token(true);
    TestHook::SetLimitedTokenUninstallFactory_Override factory([](const Client::UninstallRequest&) -> std::unique_ptr<Client::IUninstallSession>
    {
        FAIL("Unsupported requests must not activate the helper.");
        return {};
    });
    std::ostringstream output;
    TestContext context{ output, std::cin };
    AddInstalledPackage(context);
    context.Args.AddArg(argument, "test");
    context << GetUninstallInfo;
    REQUIRE_TERMINATED_WITH(context, APPINSTALLER_CLI_ERROR_ADMIN_CONTEXT_ACTION_PROHIBITED);
    REQUIRE_FALSE(context.Contains(Data::UninstallString));
    REQUIRE_FALSE(context.Contains(Data::LimitedTokenUninstall));
}

TEST_CASE("LimitedTokenUninstall_LeavesOtherOperationsAlone", "[LimitedTokenFlow][workflow]")
{
    const auto type = GENERATE("Portable", "Font", "Msix", "MSStore", "Unknown");
    TestHook::SetIsRunningWithNonDefaultFullToken_Override token(true);
    std::ostringstream output;
    TestContext context{ output, std::cin };
    AddInstalledPackage(context, type);
    REQUIRE_FALSE(PrepareLimitedTokenUninstall(context));
}

TEST_CASE("LimitedTokenUninstall_LeavesOtherTokensScopesAndNestedOperationsAlone", "[LimitedTokenFlow][workflow]")
{
    const int scenario = GENERATE(0, 1, 2, 3, 4);
    CAPTURE(scenario);
    TestHook::SetIsRunningWithNonDefaultFullToken_Override token(scenario != 0);
    std::ostringstream output;
    TestContext context{ output, std::cin };
    AddInstalledPackage(context, "Exe", scenario == 1 ? "Machine" : "User");
    if (scenario == 2)
    {
        context.Args.AddArg(Args::Type::InstallScope, "machine");
    }
    if (scenario >= 3)
    {
        context.SetFlags(scenario == 3 ? ContextFlag::InstallerExecutionUseUpdate : ContextFlag::InstallerExecutionUseRepair);
    }
    REQUIRE_FALSE(PrepareLimitedTokenUninstall(context));
}

TEST_CASE("LimitedTokenUninstall_MissingHelperRetainsBlock", "[LimitedTokenFlow][workflow]")
{
    TestHook::SetIsRunningWithNonDefaultFullToken_Override token(true);
    TestHook::SetLimitedTokenUninstallFactory_Override factory({});
    std::ostringstream output;
    TestContext context{ output, std::cin };
    AddInstalledPackage(context);
    context << GetUninstallInfo;
    REQUIRE_TERMINATED_WITH(context, APPINSTALLER_CLI_ERROR_ADMIN_CONTEXT_ACTION_PROHIBITED);
    REQUIRE_FALSE(context.Contains(Data::UninstallString));
}

TEST_CASE("LimitedTokenUninstall_BrokenHelperNeverFallsBack", "[LimitedTokenFlow][workflow]")
{
    TestHook::SetIsRunningWithNonDefaultFullToken_Override token(true);
    TestHook::SetLimitedTokenUninstallFactory_Override factory([](const Client::UninstallRequest&) -> std::unique_ptr<Client::IUninstallSession>
    {
        THROW_HR(REGDB_E_CLASSNOTREG);
    });
    std::ostringstream output;
    TestContext context{ output, std::cin };
    AddInstalledPackage(context);
    REQUIRE_THROWS_HR(context << GetUninstallInfo, REGDB_E_CLASSNOTREG);
    REQUIRE_FALSE(context.Contains(Data::UninstallString));
}

TEST_CASE("LimitedTokenUninstall_PreservesFailureRebootAndNeverReplays", "[LimitedTokenFlow][workflow]")
{
    const bool disconnect = GENERATE(false, true);
    TestHook::SetIsRunningWithNonDefaultFullToken_Override token(true);
    std::ostringstream output;
    TestContext context{ output, std::cin };
    AddInstalledPackage(context);
    size_t executions = 0;
    TestHook::SetLimitedTokenUninstallFactory_Override factory([&](const Client::UninstallRequest&)
    {
        auto session = std::make_unique<TestSession>();
        session->Run = [&](const Client::UninstallRequest&, const Client::UninstallCallbacks&) -> Client::UninstallResult
        {
            ++executions;
            if (disconnect)
            {
                THROW_HR(RPC_E_DISCONNECTED);
            }
            return { E_FAIL, 1603, false, true };
        };
        return session;
    });
    REQUIRE(PrepareLimitedTokenUninstall(context));
    if (disconnect)
    {
        REQUIRE_THROWS_HR(ExecuteLimitedTokenUninstall(context), RPC_E_DISCONNECTED);
    }
    else
    {
        context.Override({ ForceInstalledCacheUpdate, [](TestContext&) {}, 1 });
        ExecuteLimitedTokenUninstall(context);
        REQUIRE_TERMINATED_WITH(context, E_FAIL);
        REQUIRE(context.Get<Data::OperationReturnCode>() == 1603);
        REQUIRE(WI_IsFlagSet(context.GetFlags(), ContextFlag::RebootRequired));
    }
    REQUIRE_THROWS_HR(ExecuteLimitedTokenUninstall(context), E_UNEXPECTED);
    REQUIRE(executions == 1);
}

TEST_CASE("LimitedTokenUninstall_PropagatesCancellation", "[LimitedTokenFlow][workflow]")
{
    TestHook::SetIsRunningWithNonDefaultFullToken_Override token(true);
    std::ostringstream output;
    TestContext context{ output, std::cin };
    AddInstalledPackage(context);
    TestHook::SetLimitedTokenUninstallFactory_Override factory([&](const Client::UninstallRequest&)
    {
        auto session = std::make_unique<TestSession>();
        session->Run = [&](const Client::UninstallRequest&, const Client::UninstallCallbacks& callbacks)
        {
            REQUIRE_FALSE(callbacks.IsCancelled());
            context.SetTerminationHR(E_ABORT);
            REQUIRE(callbacks.IsCancelled());
            return Client::UninstallResult{ E_ABORT, 0, false, false };
        };
        return session;
    });
    REQUIRE(PrepareLimitedTokenUninstall(context));
    ExecuteLimitedTokenUninstall(context);
    REQUIRE_TERMINATED_WITH(context, E_ABORT);
}
