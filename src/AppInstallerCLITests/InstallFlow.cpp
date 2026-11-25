// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "pch.h"
#include "WorkflowCommon.h"
#include "TestHooks.h"
#include "AppInstallerRuntime.h"
#include <AppInstallerFileLogger.h>
#include <AppInstallerProgress.h>
#include <winget/MSStore.h>
#include <AppInstallerStrings.h>
#include <AppInstallerSynchronization.h>
#include <Commands/InstallCommand.h>
#include <Commands/UninstallCommand.h>
#include <winget/AdminSettings.h>
#include <winget/PathVariable.h>
#include <winget/PortableIndex.h>
#include <winget/Settings.h>
#include <winget/ManifestYamlParser.h>
#include <Workflows/ArchiveFlow.h>
#include <Workflows/DownloadFlow.h>
#include <Workflows/FontFlow.h>
#include <Workflows/MsiInstallFlow.h>
#include <Workflows/InstallFlow.h>
#include <Workflows/PortableFlow.h>
#include <Workflows/UninstallFlow.h>
#include <Workflows/ShellExecuteInstallerHandler.h>
#include <PortableInstaller.h>

using namespace winrt::Windows::Foundation;
using namespace TestCommon;
using namespace AppInstaller::CLI;
using namespace AppInstaller::CLI::Execution;
using namespace AppInstaller::CLI::Workflow;
using namespace AppInstaller::Logging;
using namespace AppInstaller::Manifest;
using namespace AppInstaller::Settings;
using namespace AppInstaller::Utility;
using namespace AppInstaller::Utility::literals;

namespace
{
    struct InstallerLogDirectoryOverride
    {
        InstallerLogDirectoryOverride(size_t length) :
            Original(AppInstaller::Runtime::GetPathDetailsFor(AppInstaller::Runtime::PathName::DefaultLogLocation))
        {
            AppInstaller::Filesystem::PathDetails details;
            details.Path = L"C:\\" + std::wstring(length - 3, L'a');
            details.Create = false;
            AppInstaller::Runtime::TestHook_SetPathOverride(AppInstaller::Runtime::PathName::DefaultLogLocation, details);
        }

        ~InstallerLogDirectoryOverride()
        {
            AppInstaller::Runtime::TestHook_SetPathOverride(AppInstaller::Runtime::PathName::DefaultLogLocation, Original);
        }

        AppInstaller::Filesystem::PathDetails Original;
    };

    std::string WritePortableArchivePayload(const std::filesystem::path& path, std::string_view contents)
    {
        std::filesystem::create_directories(path.parent_path());
        {
            std::ofstream file(path, std::ios::binary);
            REQUIRE(file.is_open());
            file << contents;
            REQUIRE(file.good());
        }
        return SHA256::ConvertToString(SHA256::ComputeHashFromFile(path));
    }
}

void OverrideForDirectMsi(TestContext& context)
{
    OverrideForCheckExistingInstaller(context);

    context.Override({ DownloadInstallerFile, [](TestContext& context)
    {
        const auto& fileHash = GetTestExeInstallerHash();
        context.Add<Data::DownloadHashInfo>({ fileHash, DownloadResult{ fileHash } });
        // We don't have an msi installer for tests, but we won't execute it anyway
        context.Add<Data::InstallerPath>(TestDataFile("AppInstallerTestExeInstaller.exe"));
    } });

    context.Override({ RenameDownloadedInstaller, [](TestContext&)
    {
    } });

    OverrideForUpdateInstallerMotw(context);

    context.Override({ DirectMSIInstallImpl, [](TestContext& context)
    {
            // Write out the install command
            std::filesystem::path temp = std::filesystem::temp_directory_path();
            temp /= "TestMsiInstalled.txt";
            std::ofstream file(temp, std::ofstream::out);
            file << context.Get<Execution::Data::InstallerArgs>();
            file.close();

            context.Add<Execution::Data::OperationReturnCode>(0);
        } });
}

TEST_CASE("ExeInstallFlowWithTestManifest", "[InstallFlow][workflow]")
{
    TestCommon::TempFile installResultPath("TestExeInstalled.txt");

    std::ostringstream installOutput;
    TestContext context{ installOutput, std::cin };
    auto previousThreadGlobals = context.SetForCurrentThread();
    OverrideForShellExecute(context);
    context.Args.AddArg(Execution::Args::Type::Manifest, TestDataFile("InstallFlowTest_Exe.yaml").GetPath().u8string());

    InstallCommand install({});
    install.Execute(context);
    INFO(installOutput.str());

    // Verify Installer is called and parameters are passed in.
    REQUIRE(std::filesystem::exists(installResultPath.GetPath()));
    std::ifstream installResultFile(installResultPath.GetPath());
    REQUIRE(installResultFile.is_open());
    std::string installResultStr;
    std::getline(installResultFile, installResultStr);
    REQUIRE(installResultStr.find("/custom") != std::string::npos);
    REQUIRE(installResultStr.find("/silentwithprogress") != std::string::npos);
}

TEST_CASE("InstallFlow_RenameFromEncodedUrl", "[InstallFlow][workflow]")
{
    TestCommon::TempFile installResultPath("TestExeInstalled.txt");

    std::ostringstream installOutput;
    TestContext context{ installOutput, std::cin };
    auto previousThreadGlobals = context.SetForCurrentThread();
    OverrideForCheckExistingInstaller(context);
    context.Override({ DownloadInstallerFile, [](TestContext& context)
    {
        const auto& fileHash = GetTestExeInstallerHash();
        context.Add<Data::DownloadHashInfo>({ fileHash, DownloadResult{ fileHash } });
        auto installerPath = std::filesystem::temp_directory_path();
        installerPath /= "EncodedUrlTest.exe";
        std::filesystem::copy(TestDataFile("AppInstallerTestExeInstaller.exe"), installerPath, std::filesystem::copy_options::overwrite_existing);
        context.Add<Data::InstallerPath>(installerPath);
    } });
    OverrideForUpdateInstallerMotw(context);
    context.Args.AddArg(Execution::Args::Type::Manifest, TestDataFile("InstallFlowTest_EncodedUrl.yaml").GetPath().u8string());

    InstallCommand install({});
    install.Execute(context);
    INFO(installOutput.str());

    // Verify Installer is called and parameters are passed in.
    REQUIRE(std::filesystem::exists(installResultPath.GetPath()));
    std::ifstream installResultFile(installResultPath.GetPath());
    REQUIRE(installResultFile.is_open());
    std::string installResultStr;
    std::getline(installResultFile, installResultStr);
    REQUIRE(installResultStr.find("/encodedUrl") != std::string::npos);
}

TEST_CASE("InstallFlow_RenameFromInvalidFileCharacterUrl", "[InstallFlow][workflow]")
{
    TestCommon::TempFile installResultPath("TestExeInstalled.txt");

    std::ostringstream installOutput;
    TestContext context{ installOutput, std::cin };
    auto previousThreadGlobals = context.SetForCurrentThread();
    OverrideForCheckExistingInstaller(context);
    context.Override({ DownloadInstallerFile, [](TestContext& context)
    {
        const auto& fileHash = GetTestExeInstallerHash();
        context.Add<Data::DownloadHashInfo>({ fileHash, DownloadResult{ fileHash } });
        auto installerPath = std::filesystem::temp_directory_path();
        installerPath /= "InvalidFileCharacterUrlTest.exe";
        std::filesystem::copy(TestDataFile("AppInstallerTestExeInstaller.exe"), installerPath, std::filesystem::copy_options::overwrite_existing);
        context.Add<Data::InstallerPath>(installerPath);
    } });
    OverrideForUpdateInstallerMotw(context);
    context.Args.AddArg(Execution::Args::Type::Manifest, TestDataFile("InstallFlowTest_InvalidFileCharacterUrl.yaml").GetPath().u8string());

    InstallCommand install({});
    install.Execute(context);
    INFO(installOutput.str());

    // Verify Installer is called and parameters are passed in.
    REQUIRE(std::filesystem::exists(installResultPath.GetPath()));
    std::ifstream installResultFile(installResultPath.GetPath());
    REQUIRE(installResultFile.is_open());
    std::string installResultStr;
    std::getline(installResultFile, installResultStr);
    REQUIRE(installResultStr.find("/invalidFileCharacterUrl") != std::string::npos);
}

TEST_CASE("InstallFlowNonZeroExitCode", "[InstallFlow][workflow]")
{
    TestCommon::TempFile installResultPath("TestExeInstalled.txt");

    std::ostringstream installOutput;
    TestContext context{ installOutput, std::cin };
    auto previousThreadGlobals = context.SetForCurrentThread();
    OverrideForShellExecute(context);
    context.Args.AddArg(Execution::Args::Type::Manifest, TestDataFile("InstallFlowTest_NonZeroExitCode.yaml").GetPath().u8string());

    InstallCommand install({});
    install.Execute(context);
    INFO(installOutput.str());

    // Verify Installer is called and parameters are passed in.
    REQUIRE(context.GetTerminationHR() == S_OK);
    REQUIRE(std::filesystem::exists(installResultPath.GetPath()));
    std::ifstream installResultFile(installResultPath.GetPath());
    REQUIRE(installResultFile.is_open());
    std::string installResultStr;
    std::getline(installResultFile, installResultStr);
    REQUIRE(installResultStr.find("/ExitCode 0x80070005") != std::string::npos);
    REQUIRE(installResultStr.find("/silentwithprogress") != std::string::npos);
}

TEST_CASE("InstallFlow_InstallationNotes", "[InstallFlow][workflow]")
{
    TestCommon::TempFile installResultPath("TestExeInstalled.txt");

    std::ostringstream installOutput;
    TestContext context{ installOutput, std::cin };
    auto previousThreadGlobals = context.SetForCurrentThread();
    OverrideForShellExecute(context);
    context.Args.AddArg(Execution::Args::Type::Manifest, TestDataFile("InstallFlowTest_InstallationNotes.yaml").GetPath().u8string());

    InstallCommand install({});
    install.Execute(context);
    INFO(installOutput.str());

    // Verify installation notes are displayed
    REQUIRE(context.GetTerminationHR() == S_OK);
    REQUIRE(std::filesystem::exists(installResultPath.GetPath()));
    REQUIRE(installOutput.str().find("testInstallationNotes") != std::string::npos);
}

TEST_CASE("InstallFlow_UnsupportedArguments_Warn", "[InstallFlow][workflow]")
{
    TestCommon::TempFile installResultPath("TestExeInstalled.txt");
    TestCommon::TempDirectory tempDirectory("TempDirectory", false);

    std::ostringstream installOutput;
    TestContext context{ installOutput, std::cin };
    auto previousThreadGlobals = context.SetForCurrentThread();
    OverrideForShellExecute(context);
    context.Args.AddArg(Execution::Args::Type::Manifest, TestDataFile("InstallFlowTest_UnsupportedArguments.yaml").GetPath().u8string());
    context.Args.AddArg(Execution::Args::Type::Log, tempDirectory);

    InstallCommand install({});
    context.SetExecutingCommand(&install);
    install.Execute(context);
    INFO(installOutput.str());

    // Verify unsupported arguments warn message is shown
    REQUIRE(context.GetTerminationHR() == S_OK);
    REQUIRE(std::filesystem::exists(installResultPath.GetPath()));
    REQUIRE(installOutput.str().find(Resource::LocString(Resource::String::UnsupportedArgument).get()) != std::string::npos);
    REQUIRE(installOutput.str().find("-o,--log") != std::string::npos);
}

TEST_CASE("InstallFlow_UnsupportedArguments_Error", "[InstallFlow][workflow]")
{
    TestCommon::TempFile installResultPath("TestExeInstalled.txt");
    TestCommon::TempDirectory tempDirectory("TempDirectory", false);

    std::ostringstream installOutput;
    TestContext context{ installOutput, std::cin };
    auto previousThreadGlobals = context.SetForCurrentThread();
    context.Args.AddArg(Execution::Args::Type::Manifest, TestDataFile("InstallFlowTest_UnsupportedArguments.yaml").GetPath().u8string());
    context.Args.AddArg(Execution::Args::Type::InstallLocation, tempDirectory);

    InstallCommand install({});
    context.SetExecutingCommand(&install);
    install.Execute(context);
    INFO(installOutput.str());

    // Verify unsupported arguments error message is shown
    REQUIRE(context.GetTerminationHR() == APPINSTALLER_CLI_ERROR_UNSUPPORTED_ARGUMENT);
    REQUIRE(!std::filesystem::exists(installResultPath.GetPath()));
    REQUIRE(installOutput.str().find(Resource::LocString(Resource::String::UnsupportedArgument).get()) != std::string::npos);
    REQUIRE(installOutput.str().find("-l,--location") != std::string::npos);
}

TEST_CASE("InstallFlow_UnsupportedArguments_NotProvided")
{
    TestCommon::TempFile installResultPath("TestExeInstalled.txt");

    std::ostringstream installOutput;
    TestContext context{ installOutput, std::cin };
    auto previousThreadGlobals = context.SetForCurrentThread();
    OverrideForShellExecute(context);
    context.Args.AddArg(Execution::Args::Type::Manifest, TestDataFile("InstallFlowTest_UnsupportedArguments.yaml").GetPath().u8string());

    InstallCommand install({});
    context.SetExecutingCommand(&install);
    install.Execute(context);
    INFO(installOutput.str());

    // Verify unsupported arguments error message is not shown when not provided
    REQUIRE(context.GetTerminationHR() == S_OK);
    REQUIRE(std::filesystem::exists(installResultPath.GetPath()));
    REQUIRE(installOutput.str().find(Resource::LocString(Resource::String::UnsupportedArgument).get()) == std::string::npos);
    REQUIRE(installOutput.str().find("-o,--log") == std::string::npos);
    REQUIRE(installOutput.str().find("-l,--location") == std::string::npos);
}

TEST_CASE("InstallFlow_ExpectedReturnCodes", "[InstallFlow][workflow]")
{
    TestCommon::TempFile installResultPath("TestExeInstalled.txt");

    std::ostringstream installOutput;
    TestContext context{ installOutput, std::cin };
    auto previousThreadGlobals = context.SetForCurrentThread();
    OverrideForShellExecute(context);
    context.Args.AddArg(Execution::Args::Type::Manifest, TestDataFile("InstallFlowTest_ExpectedReturnCodes.yaml").GetPath().u8string());
    context.Args.AddArg(Execution::Args::Type::Override, "/ExitCode 8"sv);

    InstallCommand install({});
    install.Execute(context);
    INFO(installOutput.str());

    // Verify install failed with the right message
    REQUIRE_TERMINATED_WITH(context, APPINSTALLER_CLI_ERROR_INSTALL_CONTACT_SUPPORT);
    REQUIRE(std::filesystem::exists(installResultPath.GetPath()));
    REQUIRE(installOutput.str().find(Resource::LocString(Resource::String::InstallFlowReturnCodeContactSupport).get()) != std::string::npos);
    REQUIRE(installOutput.str().find("https://TestReturnResponseUrl") != std::string::npos);
}

TEST_CASE("InstallFlowWithNonApplicableArchitecture", "[InstallFlow][workflow]")
{
    TestCommon::TempFile installResultPath("TestExeInstalled.txt");

    std::ostringstream installOutput;
    TestContext context{ installOutput, std::cin };
    auto previousThreadGlobals = context.SetForCurrentThread();
    context.Args.AddArg(Execution::Args::Type::Manifest, TestDataFile("InstallFlowTest_NoApplicableArchitecture.yaml").GetPath().u8string());

    InstallCommand install({});
    install.Execute(context);
    INFO(installOutput.str());

    REQUIRE_TERMINATED_WITH(context, APPINSTALLER_CLI_ERROR_NO_APPLICABLE_INSTALLER);

    // Verify Installer was not called
    REQUIRE(!std::filesystem::exists(installResultPath.GetPath()));
}

TEST_CASE("InstallFlow_Zip_Exe", "[InstallFlow][workflow]")
{
    TestCommon::TempFile installResultPath("TestExeInstalled.txt");

    std::ostringstream installOutput;
    TestContext context{ installOutput, std::cin };
    auto previousThreadGlobals = context.SetForCurrentThread();
    OverrideForShellExecute(context);
    OverrideForExtractInstallerFromArchive(context);
    OverrideForVerifyAndSetNestedInstaller(context);
    context.Args.AddArg(Execution::Args::Type::Manifest, TestDataFile("InstallFlowTest_Zip_Exe.yaml").GetPath().u8string());

    TestHook::SetScanArchiveResult_Override scanArchiveResultOverride(true);

    InstallCommand install({});
    install.Execute(context);
    INFO(installOutput.str());

    // Verify Installer is called and parameters are passed in.
    REQUIRE(std::filesystem::exists(installResultPath.GetPath()));
    std::ifstream installResultFile(installResultPath.GetPath());
    REQUIRE(installResultFile.is_open());
    std::string installResultStr;
    std::getline(installResultFile, installResultStr);
    REQUIRE(installResultStr.find("/custom") != std::string::npos);
    REQUIRE(installResultStr.find("/silentwithprogress") != std::string::npos);
}

TEST_CASE("InstallFlow_Zip_BadRelativePath", "[InstallFlow][workflow]")
{
    TestCommon::TempFile installResultPath("TestExeInstalled.txt");

    std::ostringstream installOutput;
    TestContext context{ installOutput, std::cin };
    auto previousThreadGlobals = context.SetForCurrentThread();
    OverrideForShellExecute(context);
    OverrideForExtractInstallerFromArchive(context);
    context.Args.AddArg(Execution::Args::Type::Manifest, TestDataFile("InstallFlowTest_Zip_Exe.yaml").GetPath().u8string());

    TestHook::SetScanArchiveResult_Override scanArchiveResultOverride(true);

    InstallCommand install({});
    install.Execute(context);
    INFO(installOutput.str());

    REQUIRE_TERMINATED_WITH(context, APPINSTALLER_CLI_ERROR_NESTEDINSTALLER_NOT_FOUND);

    // Verify Installer was not called
    REQUIRE(!std::filesystem::exists(installResultPath.GetPath()));
    auto relativePath = context.Get<Execution::Data::InstallerPath>().parent_path() / "extracted" / "relativeFilePath";
    auto expectedMessage = Resource::String::NestedInstallerNotFound(AppInstaller::Utility::LocIndString{ relativePath.u8string()});
    REQUIRE(installOutput.str().find(Resource::LocString(expectedMessage).get()) != std::string::npos);
}

TEST_CASE("InstallFlow_Zip_MissingNestedInstaller", "[InstallFlow][workflow]")
{
    TestCommon::TempFile installResultPath("TestExeInstalled.txt");

    std::ostringstream installOutput;
    TestContext context{ installOutput, std::cin };
    auto previousThreadGlobals = context.SetForCurrentThread();
    context.Args.AddArg(Execution::Args::Type::Manifest, TestDataFile("InstallFlowTest_Zip_MissingNestedInstaller.yaml").GetPath().u8string());

    InstallCommand install({});
    install.Execute(context);
    INFO(installOutput.str());

    REQUIRE_TERMINATED_WITH(context, APPINSTALLER_CLI_ERROR_INVALID_MANIFEST);

    // Verify Installer was not called
    REQUIRE(!std::filesystem::exists(installResultPath.GetPath()));
    REQUIRE(installOutput.str().find(Resource::LocString(Resource::String::NestedInstallerNotSpecified).get()) != std::string::npos);
}

TEST_CASE("InstallFlow_Zip_UnsupportedNestedInstaller", "[InstallFlow][workflow]")
{
    TestCommon::TempFile installResultPath("TestExeInstalled.txt");

    std::ostringstream installOutput;
    TestContext context{ installOutput, std::cin };
    auto previousThreadGlobals = context.SetForCurrentThread();
    context.Args.AddArg(Execution::Args::Type::Manifest, TestDataFile("InstallFlowTest_Zip_UnsupportedNestedInstaller.yaml").GetPath().u8string());

    InstallCommand install({});
    install.Execute(context);
    INFO(installOutput.str());

    REQUIRE_TERMINATED_WITH(context, ERROR_NOT_SUPPORTED);

    // Verify Installer was not called
    REQUIRE(!std::filesystem::exists(installResultPath.GetPath()));
    REQUIRE(installOutput.str().find(Resource::LocString(Resource::String::NestedInstallerNotSupported).get()) != std::string::npos);
}

TEST_CASE("InstallFlow_Zip_MultipleNonPortableNestedInstallers", "[InstallFlow][workflow]")
{
    TestCommon::TempFile installResultPath("TestExeInstalled.txt");

    std::ostringstream installOutput;
    TestContext context{ installOutput, std::cin };
    auto previousThreadGlobals = context.SetForCurrentThread();
    context.Args.AddArg(Execution::Args::Type::Manifest, TestDataFile("InstallFlowTest_Zip_MultipleNonPortableNestedInstallers.yaml").GetPath().u8string());

    InstallCommand install({});
    install.Execute(context);
    INFO(installOutput.str());

    REQUIRE_TERMINATED_WITH(context, APPINSTALLER_CLI_ERROR_INVALID_MANIFEST);

    // Verify Installer was not called
    REQUIRE(!std::filesystem::exists(installResultPath.GetPath()));
    REQUIRE(installOutput.str().find(Resource::LocString(Resource::String::MultipleUnsupportedNestedInstallersSpecified).get()) != std::string::npos);
}

TEST_CASE("InstallFlow_Zip_ArchiveScanFailed", "[InstallFlow][workflow]")
{
    TestCommon::TempFile installResultPath("TestExeInstalled.txt");

    std::ostringstream installOutput;
    TestContext context{ installOutput, std::cin };
    auto previousThreadGlobals = context.SetForCurrentThread();
    OverrideForShellExecute(context);
    context.Args.AddArg(Execution::Args::Type::Manifest, TestDataFile("InstallFlowTest_Zip_Exe.yaml").GetPath().u8string());

    TestHook::SetScanArchiveResult_Override scanArchiveResultOverride(false);

    InstallCommand install({});
    install.Execute(context);
    INFO(installOutput.str());

    REQUIRE_TERMINATED_WITH(context, APPINSTALLER_CLI_ERROR_ARCHIVE_SCAN_FAILED);

    // Verify Installer was not called
    REQUIRE(!std::filesystem::exists(installResultPath.GetPath()));
    REQUIRE(installOutput.str().find(Resource::LocString(Resource::String::ArchiveFailedMalwareScan).get()) != std::string::npos);
}

TEST_CASE("InstallFlow_Zip_ArchiveScanOverride_AdminSettingDisabled", "[InstallFlow][workflow]")
{
    TestCommon::TempFile installResultPath("TestExeInstalled.txt");

    std::ostringstream installOutput;
    TestContext context{ installOutput, std::cin };
    auto previousThreadGlobals = context.SetForCurrentThread();
    OverrideForShellExecute(context);
    context.Args.AddArg(Execution::Args::Type::Manifest, TestDataFile("InstallFlowTest_Zip_Exe.yaml").GetPath().u8string());
    context.Args.AddArg(Execution::Args::Type::IgnoreLocalArchiveMalwareScan);

    DisableAdminSetting(AppInstaller::Settings::BoolAdminSetting::LocalArchiveMalwareScanOverride);

    TestHook::SetScanArchiveResult_Override scanArchiveResultOverride(false);

    InstallCommand install({});
    install.Execute(context);
    INFO(installOutput.str());

    REQUIRE_TERMINATED_WITH(context, APPINSTALLER_CLI_ERROR_ARCHIVE_SCAN_FAILED);

    // Verify Installer was not called
    REQUIRE(!std::filesystem::exists(installResultPath.GetPath()));
    REQUIRE(installOutput.str().find(Resource::LocString(Resource::String::ArchiveFailedMalwareScan).get()) != std::string::npos);
}

TEST_CASE("InstallFlow_Zip_ArchiveScanOverride_AdminSettingEnabled", "[InstallFlow][workflow]")
{
    TestCommon::TempFile installResultPath("TestExeInstalled.txt");

    std::ostringstream installOutput;
    TestContext context{ installOutput, std::cin };
    auto previousThreadGlobals = context.SetForCurrentThread();
    OverrideForShellExecute(context);
    OverrideForExtractInstallerFromArchive(context);
    OverrideForVerifyAndSetNestedInstaller(context);
    context.Args.AddArg(Execution::Args::Type::Manifest, TestDataFile("InstallFlowTest_Zip_Exe.yaml").GetPath().u8string());
    context.Args.AddArg(Execution::Args::Type::IgnoreLocalArchiveMalwareScan);

    EnableAdminSetting(AppInstaller::Settings::BoolAdminSetting::LocalArchiveMalwareScanOverride);

    TestHook::SetScanArchiveResult_Override scanArchiveResultOverride(false);
    TestHook::SetScanArchiveCalled_Signal scanArchiveCalled;

    InstallCommand install({});
    install.Execute(context);
    INFO(installOutput.str());

    // Verify the scan is skipped and the user is notified.
    REQUIRE_FALSE(scanArchiveCalled.WasCalled());
    REQUIRE(installOutput.str().find(Resource::LocString(Resource::String::ArchiveMalwareScanSkipped).get()) != std::string::npos);

    // Verify Installer is called and parameters are passed in.
    REQUIRE(std::filesystem::exists(installResultPath.GetPath()));
    std::ifstream installResultFile(installResultPath.GetPath());
    REQUIRE(installResultFile.is_open());
    std::string installResultStr;
    std::getline(installResultFile, installResultStr);
    REQUIRE(installResultStr.find("/custom") != std::string::npos);
    REQUIRE(installResultStr.find("/silentwithprogress") != std::string::npos);
}

TEST_CASE("ExtractInstallerFromArchive_InvalidZip", "[InstallFlow][workflow]")
{
    std::ostringstream installOutput;
    TestContext context{ installOutput, std::cin };
    auto previousThreadGlobals = context.SetForCurrentThread();
    auto manifest = YamlParser::CreateFromPath(TestDataFile("InstallFlowTest_Zip_Exe.yaml"));
    context.Add<Data::Manifest>(manifest);
    context.Add<Data::Installer>(manifest.Installers.at(0));

    // Provide an invalid zip file which should be handled appropriately.
    context.Add<Data::InstallerPath>(TestDataFile("AppInstallerTestExeInstaller.exe"));
    context << ExtractFilesFromArchive;
    REQUIRE_TERMINATED_WITH(context, APPINSTALLER_CLI_ERROR_EXTRACT_ARCHIVE_FAILED);
    REQUIRE(installOutput.str().find(Resource::LocString(Resource::String::ExtractArchiveFailed).get()) != std::string::npos);
}

TEST_CASE("ExtractInstallerFromArchiveWithTar", "[InstallFlow][workflow]")
{
    TestCommon::TestUserSettings testSettings;
    testSettings.Set<Setting::ArchiveExtractionMethod>(AppInstaller::Archive::ExtractionMethod::Tar);

    TestCommon::TempFile installResultPath("TestExeInstalled.txt");

    std::ostringstream installOutput;
    TestContext context{ installOutput, std::cin };
    auto previousThreadGlobals = context.SetForCurrentThread();

    OverrideForShellExecute(context);
    OverrideForVerifyAndSetNestedInstaller(context);
    context.Args.AddArg(Execution::Args::Type::Manifest, TestDataFile("InstallFlowTest_Zip_Exe.yaml").GetPath().u8string());

    TestHook::SetScanArchiveResult_Override scanArchiveResultOverride(true);
    TestHook::SetExtractArchiveWithTarResult_Override setExtractArchiveWithTarResultOverride(ERROR_SUCCESS);

    InstallCommand install({});
    install.Execute(context);
    INFO(installOutput.str());
    REQUIRE(installOutput.str().find(Resource::LocString(Resource::String::ExtractArchiveSucceeded).get()) != std::string::npos);

    // Verify Installer is called and parameters are passed in.
    REQUIRE(std::filesystem::exists(installResultPath.GetPath()));
    std::ifstream installResultFile(installResultPath.GetPath());
    REQUIRE(installResultFile.is_open());
    std::string installResultStr;
    std::getline(installResultFile, installResultStr);
    REQUIRE(installResultStr.find("/custom") != std::string::npos);
    REQUIRE(installResultStr.find("/silentwithprogress") != std::string::npos);
}

TEST_CASE("ExtractInstallerFromArchiveWithTar_InvalidZip", "[InstallFlow][workflow]")
{
    TestCommon::TestUserSettings testSettings;
    testSettings.Set<Setting::ArchiveExtractionMethod>(AppInstaller::Archive::ExtractionMethod::Tar);

    std::ostringstream installOutput;
    TestContext context{ installOutput, std::cin };
    auto previousThreadGlobals = context.SetForCurrentThread();
    auto manifest = YamlParser::CreateFromPath(TestDataFile("InstallFlowTest_Zip_Exe.yaml"));
    context.Add<Data::Manifest>(manifest);
    context.Add<Data::Installer>(manifest.Installers.at(0));

    // Provide an invalid zip file which should be handled appropriately.
    context.Add<Data::InstallerPath>(TestDataFile("AppInstallerTestExeInstaller.exe"));
    context << ExtractFilesFromArchive;
    REQUIRE_TERMINATED_WITH(context, APPINSTALLER_CLI_ERROR_EXTRACT_ARCHIVE_FAILED);
    REQUIRE(installOutput.str().find(Resource::LocString(Resource::String::ExtractArchiveFailed).get()) != std::string::npos);
}

TEST_CASE("MSStoreInstallFlowWithTestManifest", "[InstallFlow][workflow]")
{
    TestCommon::TempFile installResultPath("TestMSStoreInstalled.txt");

    std::ostringstream installOutput;
    TestContext context{ installOutput, std::cin };
    auto previousThreadGlobals = context.SetForCurrentThread();
    OverrideForMSStore(context, false);
    context.Args.AddArg(Execution::Args::Type::Manifest, TestDataFile("InstallFlowTest_MSStore.yaml").GetPath().u8string());

    InstallCommand install({});
    install.Execute(context);
    INFO(installOutput.str());

    // Verify Installer is called and parameters are passed in.
    REQUIRE(std::filesystem::exists(installResultPath.GetPath()));
    std::ifstream installResultFile(installResultPath.GetPath());
    REQUIRE(installResultFile.is_open());
    std::string installResultStr;
    std::getline(installResultFile, installResultStr);
    REQUIRE(installResultStr.find("9WZDNCRFJ364") != std::string::npos);
}

TEST_CASE("MSStoreInstallFlow_MachineScopeProvision", "[InstallFlow][MSStore]")
{
    if (!AppInstaller::Runtime::IsRunningAsAdmin() || AppInstaller::Runtime::IsRunningAsSystem())
    {
        WARN("Test requires running as admin but not SYSTEM. Skipped.");
        return;
    }

    TestHook::SetForceProvisionAfterInstall_Override forceProvisionOverride(true);

    AppInstaller::ProgressCallback progress;
    AppInstaller::MSStore::MSStoreOperation installOperation(
        AppInstaller::MSStore::MSStoreOperationType::Install,
        L"9NVTPZWRC6KQ",
        AppInstaller::Manifest::ScopeEnum::User,
        true,
        false);

    HRESULT hr = installOperation.StartAndWaitForOperation(progress);
    INFO("Store install HRESULT: 0x" << SetHRFormat << hr);
    REQUIRE(SUCCEEDED(hr));

    // Verify the package is now provisioned.
    winrt::Windows::Management::Deployment::PackageManager packageManager;
    bool isProvisioned = false;
    for (auto const& pkg : packageManager.FindProvisionedPackages())
    {
        if (pkg.Id().FamilyName() == L"Microsoft.DesiredStateConfiguration_8wekyb3d8bbwe")
        {
            isProvisioned = true;
            break;
        }
    }
    REQUIRE(isProvisioned);
}

TEST_CASE("MsixInstallFlow_DownloadFlow", "[InstallFlow][workflow]")
{
    TestCommon::TempFile installResultPath("TestMsixInstalled.txt");

    std::ostringstream installOutput;
    TestContext context{ installOutput, std::cin };
    auto previousThreadGlobals = context.SetForCurrentThread();
    OverrideForMSIX(context);
    OverrideForUpdateInstallerMotw(context);
    // Todo: point to files from our repo when the repo goes public
    context.Args.AddArg(Execution::Args::Type::Manifest, TestDataFile("InstallFlowTest_Msix_DownloadFlow.yaml").GetPath().u8string());

    InstallCommand install({});
    install.Execute(context);
    INFO(installOutput.str());

    // Verify Installer is called and a local file is used as package Uri.
    REQUIRE(std::filesystem::exists(installResultPath.GetPath()));
    std::ifstream installResultFile(installResultPath.GetPath());
    REQUIRE(installResultFile.is_open());
    std::string installResultStr;
    std::getline(installResultFile, installResultStr);
    Uri uri = Uri(ConvertToUTF16(installResultStr));
    REQUIRE(uri.SchemeName() == L"file");
}

TEST_CASE("MsixInstallFlow_StreamingFlow", "[InstallFlow][workflow]")
{
    TestCommon::TempFile installResultPath("TestMsixInstalled.txt");

    std::ostringstream installOutput;
    TestContext context{ installOutput, std::cin };
    auto previousThreadGlobals = context.SetForCurrentThread();
    OverrideForMSIX(context);
    OverrideForCheckExistingInstaller(context);
    // Todo: point to files from our repo when the repo goes public
    context.Args.AddArg(Execution::Args::Type::Manifest, TestDataFile("InstallFlowTest_Msix_StreamingFlow.yaml").GetPath().u8string());

    InstallCommand install({});
    install.Execute(context);
    INFO(installOutput.str());

    // Verify Installer is called and a http address is used as package Uri.
    REQUIRE(std::filesystem::exists(installResultPath.GetPath()));
    std::ifstream installResultFile(installResultPath.GetPath());
    REQUIRE(installResultFile.is_open());
    std::string installResultStr;
    std::getline(installResultFile, installResultStr);
    Uri uri = Uri(ConvertToUTF16(installResultStr));
    REQUIRE(uri.SchemeName() == L"https");
}

TEST_CASE("MsiInstallFlow_DirectMsi", "[InstallFlow][workflow]")
{
    TestCommon::TempFile installResultPath("TestMsiInstalled.txt");

    TestCommon::TestUserSettings testSettings;
    testSettings.Set<Setting::EFDirectMSI>(true);

    std::ostringstream installOutput;
    TestContext context{ installOutput, std::cin };
    auto previousThreadGlobals = context.SetForCurrentThread();
    OverrideForDirectMsi(context);
    context.Args.AddArg(Execution::Args::Type::Manifest, TestDataFile("InstallerArgTest_Msi_NoSwitches.yaml").GetPath().u8string());
    context.Args.AddArg(Execution::Args::Type::Silent);

    InstallCommand install({});
    install.Execute(context);
    INFO(installOutput.str());

    // Verify Installer is called and parameters are passed in.
    REQUIRE(std::filesystem::exists(installResultPath.GetPath()));
    std::ifstream installResultFile(installResultPath.GetPath());
    REQUIRE(installResultFile.is_open());
    std::string installResultStr;
    std::getline(installResultFile, installResultStr);
    REQUIRE(installResultStr.find("/quiet") != std::string::npos);
}

TEST_CASE("InstallFlow_Portable", "[InstallFlow][workflow]")
{
    TestHook::SetIsRunningWithNonDefaultFullToken_Override tokenOverride(false);

    TestCommon::TempDirectory tempDirectory("TestPortableInstallRoot", false);
    TestCommon::TempFile portableInstallResultPath("TestPortableInstalled.txt");

    std::ostringstream installOutput;
    TestContext context{ installOutput, std::cin };
    auto previousThreadGlobals = context.SetForCurrentThread();
    OverrideForPortableInstallFlow(context);
    context.Args.AddArg(Execution::Args::Type::Manifest, TestDataFile("InstallFlowTest_Portable.yaml").GetPath().u8string());
    context.Args.AddArg(Execution::Args::Type::InstallLocation, tempDirectory);

    InstallCommand install({});
    install.Execute(context);
    INFO(installOutput.str());

    REQUIRE(std::filesystem::exists(portableInstallResultPath.GetPath()));
}

TEST_CASE("PortableInstallFlow_ArchiveAliasHashes", "[InstallFlow][workflow][PortableAliasRegression]")
{
    TestHook::SetPortableTestEnvironment_Override testEnvironment;
    const bool executableInSubdirectory = GENERATE(false, true);
    const bool binariesDependOnPath = GENERATE(false, true);
    CAPTURE(executableInSubdirectory, binariesDependOnPath);
    TestHook::SetIsRunningWithNonDefaultFullToken_Override tokenOverride(false);
    TempDirectory extractedDirectory("PortableArchiveAliasSource");
    TempDirectory targetDirectory("PortableArchiveAliasTarget", false);
    TempDirectory linksDirectory("PortableArchiveAliasLinks");

    const std::string relativeFilePath = executableInSubdirectory ? "bin\\tool.exe" : "tool.exe";
    std::vector<NestedInstallerFile> nestedInstallerFiles;

    SECTION("Single alias")
    {
        nestedInstallerFiles = { { relativeFilePath, "primary" } };
    }

    SECTION("Multiple aliases for equivalent source paths")
    {
        const auto prefix = GENERATE(".\\", "./");
        const bool prefixedPathFirst = GENERATE(false, true);
        nestedInstallerFiles = { { relativeFilePath, "primary" }, { std::string{ prefix } + relativeFilePath, "secondary" } };
        if (prefixedPathFirst)
        {
            std::reverse(nestedInstallerFiles.begin(), nestedInstallerFiles.end());
        }
    }
    CAPTURE(nestedInstallerFiles.front().RelativeFilePath, nestedInstallerFiles.back().RelativeFilePath);

    const auto sourcePath = extractedDirectory.GetPath() / ConvertToUTF16(relativeFilePath);
    std::filesystem::create_directories(sourcePath.parent_path());
    {
        std::ofstream sourceFile(sourcePath, std::ios::binary);
        REQUIRE(sourceFile.is_open());
        sourceFile << "portable archive executable";
        REQUIRE(sourceFile.good());
    }
    const auto expectedHash = SHA256::ConvertToString(SHA256::ComputeHashFromFile(sourcePath));
    const auto targetPath = targetDirectory.GetPath() / ConvertToUTF16(relativeFilePath);

    const auto originalLinks = AppInstaller::Runtime::GetPathDetailsFor(AppInstaller::Runtime::PathName::PortableLinksUserLocation);
    const auto productCode = targetDirectory.GetPath().filename().u8string();
    AppInstaller::Registry::Portable::PortableARPEntry arpEntry(ScopeEnum::User, Architecture::X64, productCode);
    auto cleanup = wil::scope_exit([&]()
    {
        AppInstaller::Runtime::TestHook_SetPathOverride(AppInstaller::Runtime::PathName::PortableLinksUserLocation, originalLinks);
        auto pathVariable = AppInstaller::Registry::Environment::PathVariable(ScopeEnum::User);
        pathVariable.Remove(targetPath.parent_path());
        pathVariable.Remove(linksDirectory.GetPath());
        arpEntry.Delete();
    });
    AppInstaller::Runtime::TestHook_SetPathOverride(AppInstaller::Runtime::PathName::PortableLinksUserLocation, linksDirectory.GetPath());

    ManifestInstaller installer;
    installer.BaseInstallerType = InstallerTypeEnum::Zip;
    installer.NestedInstallerType = InstallerTypeEnum::Portable;
    installer.ArchiveBinariesDependOnPath = binariesDependOnPath;
    installer.NestedInstallerFiles = nestedInstallerFiles;

    AppInstaller::CLI::Portable::PortableInstaller portableInstaller{
        ScopeEnum::User, Architecture::X64, productCode, OperationType::Install };
    portableInstaller.TargetInstallLocation = targetDirectory.GetPath();
    portableInstaller.BinariesDependOnPath = binariesDependOnPath;

    std::ostringstream output;
    TestContext context{ output, std::cin };
    auto previousThreadGlobals = context.SetForCurrentThread();
    context.Add<Data::Installer>(installer);
    context.Add<Data::InstallerPath>(extractedDirectory.GetPath());
    context.Add<Data::PortableInstaller>(std::move(portableInstaller));

    context << PortableInstallImpl;

    INFO(output.str());
    REQUIRE_FALSE(context.IsTerminated());
    REQUIRE(context.Get<Data::OperationReturnCode>() == ERROR_SUCCESS);
    REQUIRE(std::filesystem::exists(targetPath));
    CHECK(SHA256::ConvertToString(SHA256::ComputeHashFromFile(targetPath)) == expectedHash);

    const auto indexPath = targetDirectory.GetPath() / context.Get<Data::PortableInstaller>().GetPortableIndexFileName();
    auto index = AppInstaller::Repository::Microsoft::PortableIndex::Open(
        indexPath.u8string(), AppInstaller::SQLite::SQLiteStorageBase::OpenDisposition::Read);
    const auto installedEntries = index.GetAllPortableFiles();
    for (const auto& nestedInstallerFile : nestedInstallerFiles)
    {
        const auto aliasPath = targetPath.parent_path() / ConvertToUTF16(nestedInstallerFile.PortableCommandAlias + ".exe");
        CAPTURE(aliasPath);
        REQUIRE(std::filesystem::exists(aliasPath));
        CHECK(SHA256::ConvertToString(SHA256::ComputeHashFromFile(aliasPath)) == expectedHash);
        const auto aliasEntry = std::find_if(installedEntries.begin(), installedEntries.end(), [&](const auto& entry)
        {
            return entry.FileType == AppInstaller::Portable::PortableFileType::Hardlink &&
                entry.GetFilePath() == std::filesystem::weakly_canonical(aliasPath);
        });
        REQUIRE(aliasEntry != installedEntries.end());
        CHECK(aliasEntry->SHA256 == expectedHash);
    }
}

TEST_CASE("PortableInstallFlow_CaseOnlyAlias", "[InstallFlow][workflow][PortableAliasRegression]")
{
    TestHook::SetPortableTestEnvironment_Override testEnvironment;
    const auto aliasKind = GENERATE("case-only", "distinct", "unicode-distinct");
    const bool caseOnlyAlias = std::string_view{ aliasKind } == "case-only";
    const bool unicodeDistinctAlias = std::string_view{ aliasKind } == "unicode-distinct";
    CAPTURE(aliasKind);
    TestHook::SetIsRunningWithNonDefaultFullToken_Override tokenOverride(false);
    TestHook::SetCreateSymlinkResult_Override symlinkOverride(false);
    TempDirectory sourceDirectory("PortableCaseOnlyAliasSource");
    TempDirectory targetDirectory("PortableCaseOnlyAliasTarget", false);
    TempDirectory linksDirectory("PortableCaseOnlyAliasLinks");
    const std::string commandAlias = unicodeDistinctAlias ? "strasse" : (caseOnlyAlias ? "tool" : "command");
    std::filesystem::path relativeFilePath = unicodeDistinctAlias ? std::filesystem::path{ L"stra\u00DFe.exe" } : std::filesystem::path{ "Tool.exe" };
    ManifestInstaller installer;
    installer.BaseInstallerType = InstallerTypeEnum::Portable;
    bool useRename = false;

    SECTION("Standalone Commands")
    {
        installer.Commands = { commandAlias };
    }

    SECTION("Standalone rename")
    {
        installer.Commands = { "unused" };
        useRename = true;
    }

    SECTION("Archive root executable")
    {
        installer.BaseInstallerType = InstallerTypeEnum::Zip;
    }

    SECTION("Archive nested executable")
    {
        installer.BaseInstallerType = InstallerTypeEnum::Zip;
        relativeFilePath = std::filesystem::path{ "bin" } / relativeFilePath;
    }

    const bool isArchive = installer.BaseInstallerType == InstallerTypeEnum::Zip;
    if (isArchive)
    {
        installer.NestedInstallerType = InstallerTypeEnum::Portable;
        installer.NestedInstallerFiles = { { relativeFilePath.u8string(), commandAlias } };
    }

    const auto sourcePath = sourceDirectory.GetPath() / relativeFilePath;
    std::filesystem::create_directories(sourcePath.parent_path());
    {
        std::ofstream sourceFile(sourcePath, std::ios::binary);
        REQUIRE(sourceFile.is_open());
        sourceFile << "case-only alias executable";
        REQUIRE(sourceFile.good());
    }
    const auto expectedHash = SHA256::ConvertToString(SHA256::ComputeHashFromFile(sourcePath));
    const auto targetPath = targetDirectory.GetPath() / relativeFilePath;
    const auto aliasPath = targetPath.parent_path() / ConvertToUTF16(commandAlias + ".exe");
    const auto productCode = targetDirectory.GetPath().filename().u8string();
    const auto originalLinks = AppInstaller::Runtime::GetPathDetailsFor(AppInstaller::Runtime::PathName::PortableLinksUserLocation);
    AppInstaller::Registry::Portable::PortableARPEntry arpEntry(ScopeEnum::User, Architecture::X64, productCode);
    auto cleanup = wil::scope_exit([&]()
    {
        AppInstaller::Runtime::TestHook_SetPathOverride(AppInstaller::Runtime::PathName::PortableLinksUserLocation, originalLinks);
        auto pathVariable = AppInstaller::Registry::Environment::PathVariable(ScopeEnum::User);
        pathVariable.Remove(targetPath.parent_path());
        pathVariable.Remove(linksDirectory.GetPath());
        arpEntry.Delete();
    });
    AppInstaller::Runtime::TestHook_SetPathOverride(AppInstaller::Runtime::PathName::PortableLinksUserLocation, linksDirectory.GetPath());

    AppInstaller::CLI::Portable::PortableInstaller portableInstaller{
        ScopeEnum::User, Architecture::X64, productCode, OperationType::Install };
    portableInstaller.TargetInstallLocation = targetDirectory.GetPath();

    std::ostringstream output;
    TestContext context{ output, std::cin };
    auto previousThreadGlobals = context.SetForCurrentThread();
    if (useRename)
    {
        context.Args.AddArg(Execution::Args::Type::Rename, commandAlias);
    }
    context.Add<Data::Installer>(installer);
    context.Add<Data::InstallerPath>(isArchive ? sourceDirectory.GetPath() : sourcePath);
    context.Add<Data::PortableInstaller>(std::move(portableInstaller));

    context << PortableInstallImpl;

    INFO(output.str());
    REQUIRE_FALSE(context.IsTerminated());
    REQUIRE(context.Get<Data::OperationReturnCode>() == ERROR_SUCCESS);
    REQUIRE(std::filesystem::exists(targetPath));
    REQUIRE(std::filesystem::exists(aliasPath));
    CHECK(SHA256::ConvertToString(SHA256::ComputeHashFromFile(targetPath)) == expectedHash);
    CHECK(SHA256::ConvertToString(SHA256::ComputeHashFromFile(aliasPath)) == expectedHash);
    CHECK(AppInstaller::Registry::Environment::PathVariable(ScopeEnum::User).Contains(targetPath.parent_path()));

    if (isArchive)
    {
        const auto indexPath = targetDirectory.GetPath() / context.Get<Data::PortableInstaller>().GetPortableIndexFileName();
        auto index = AppInstaller::Repository::Microsoft::PortableIndex::Open(
            indexPath.u8string(), AppInstaller::SQLite::SQLiteStorageBase::OpenDisposition::Read);
        const auto entries = index.GetAllPortableFiles();
        CHECK(std::count_if(entries.begin(), entries.end(), [](const auto& entry)
        {
            return entry.FileType == AppInstaller::Portable::PortableFileType::Hardlink;
        }) == (caseOnlyAlias ? 0 : 1));
    }

    AppInstaller::CLI::Portable::PortableInstaller uninstaller{
        ScopeEnum::User, Architecture::X64, productCode, OperationType::Uninstall };
    REQUIRE(uninstaller.VerifyExpectedState());
    uninstaller.Uninstall();
    CHECK_FALSE(std::filesystem::exists(targetPath));
    CHECK_FALSE(std::filesystem::exists(aliasPath));
    CHECK_FALSE(std::filesystem::exists(targetDirectory.GetPath()));
}

TEST_CASE("PortableInstallFlow_ArchiveAliasCollisions", "[InstallFlow][workflow][PortableAliasRegression]")
{
    TestHook::SetPortableTestEnvironment_Override testEnvironment;
    using AppInstaller::CLI::Portable::PortableInstaller;
    using AppInstaller::Portable::PortableFileEntry;
    using namespace AppInstaller::Registry::Portable;

    const bool isUpgrade = GENERATE(false, true);
    const bool executableInSubdirectory = GENERATE(false, true);
    const bool binariesDependOnPath = GENERATE(false, true);
    CAPTURE(isUpgrade, executableInSubdirectory, binariesDependOnPath);
    TestHook::SetIsRunningWithNonDefaultFullToken_Override tokenOverride(false);
    TestHook::SetCreateSymlinkResult_Override symlinkOverride(false);
    TempDirectory sourceDirectory("PortableCollisionSource");
    TempDirectory targetDirectory("PortableCollisionTarget", false);
    TempDirectory linksDirectory("PortableCollisionLinks");
    TempFile previousSource("PortableCollisionPrevious.exe");
    const std::filesystem::path relativeDirectory = executableInSubdirectory ? "bin" : "";
    const auto relativeExecutable = relativeDirectory / "tool.exe";
    std::vector<std::pair<std::filesystem::path, std::string>> sourceHashes;
    std::vector<std::filesystem::path> sourceDirectories;
    auto addPayload = [&](const std::filesystem::path& relativePath, std::string_view contents)
    {
        sourceHashes.emplace_back(relativePath, WritePortableArchivePayload(sourceDirectory.GetPath() / relativePath, contents));
    };
    addPayload(relativeExecutable, "new executable");
    addPayload("untouched.txt", "unrelated archive payload");
    std::vector<NestedInstallerFile> nestedInstallerFiles{ { relativeExecutable.u8string(), "command" } };

    SECTION("Alias collides with a different payload file")
    {
        addPayload(relativeDirectory / "COMMAND.exe", "different payload executable");
    }

    SECTION("Alias collides with a payload directory")
    {
        const bool directoryHasContents = GENERATE(false, true);
        CAPTURE(directoryHasContents);
        const auto relativePath = relativeDirectory / "command.exe";
        std::filesystem::create_directories(sourceDirectory.GetPath() / relativePath);
        sourceDirectories.push_back(relativePath);
        if (directoryHasContents)
        {
            addPayload(relativePath / "keep.txt", "payload directory contents");
        }
    }

    SECTION("Different sources claim the same in-package alias after extension and case mapping")
    {
        const auto otherExecutable = relativeDirectory / "other.exe";
        addPayload(otherExecutable, "different aliased executable");
        nestedInstallerFiles.push_back({ otherExecutable.u8string(), "COMMAND.exe" });
    }

    SECTION("Different executable directories claim the same Links alias")
    {
        const auto otherExecutable = relativeDirectory / "other" / "other.exe";
        addPayload(otherExecutable, "different published executable");
        nestedInstallerFiles.push_back({ otherExecutable.u8string(), "COMMAND.exe" });
    }

    const auto productCode = targetDirectory.GetPath().filename().u8string();
    const auto originalLinks = AppInstaller::Runtime::GetPathDetailsFor(AppInstaller::Runtime::PathName::PortableLinksUserLocation);
    PortableARPEntry arpEntry(ScopeEnum::User, Architecture::X64, productCode);
    auto cleanup = wil::scope_exit([&]()
    {
        AppInstaller::Runtime::TestHook_SetPathOverride(AppInstaller::Runtime::PathName::PortableLinksUserLocation, originalLinks);
        auto pathVariable = AppInstaller::Registry::Environment::PathVariable(ScopeEnum::User);
        pathVariable.Remove(targetDirectory.GetPath());
        pathVariable.Remove(targetDirectory.GetPath() / relativeDirectory);
        pathVariable.Remove(targetDirectory.GetPath() / relativeDirectory / "other");
        pathVariable.Remove(linksDirectory.GetPath());
        arpEntry.Delete();
    });
    AppInstaller::Runtime::TestHook_SetPathOverride(AppInstaller::Runtime::PathName::PortableLinksUserLocation, linksDirectory.GetPath());

    const auto previousPath = targetDirectory.GetPath() / "previous.exe";
    std::string previousHash;
    if (isUpgrade)
    {
        previousHash = WritePortableArchivePayload(previousSource.GetPath(), "previous installed executable");
        PortableInstaller previousInstaller{ ScopeEnum::User, Architecture::X64, productCode, OperationType::Install };
        previousInstaller.TargetInstallLocation = targetDirectory.GetPath();
        previousInstaller.RecordToIndex = true;
        previousInstaller.DisplayVersion = "1.0";
        previousInstaller.SetDesiredState({ PortableFileEntry::CreateFileEntry(previousSource.GetPath(), previousPath, previousHash) });
        previousInstaller.Install();
    }

    PortableInstaller portableInstaller{
        ScopeEnum::User, Architecture::X64, productCode, isUpgrade ? OperationType::Upgrade : OperationType::Install };
    portableInstaller.TargetInstallLocation = targetDirectory.GetPath();
    portableInstaller.BinariesDependOnPath = binariesDependOnPath;
    portableInstaller.DisplayVersion = "2.0";
    const auto indexPath = targetDirectory.GetPath() / portableInstaller.GetPortableIndexFileName();
    const std::string previousIndexHash = isUpgrade ? SHA256::ConvertToString(SHA256::ComputeHashFromFile(indexPath)) : "";

    ManifestInstaller installer;
    installer.BaseInstallerType = InstallerTypeEnum::Zip;
    installer.NestedInstallerType = InstallerTypeEnum::Portable;
    installer.ArchiveBinariesDependOnPath = binariesDependOnPath;
    installer.NestedInstallerFiles = nestedInstallerFiles;

    std::ostringstream output;
    TestContext context{ output, std::cin };
    auto previousThreadGlobals = context.SetForCurrentThread();
    context.Add<Data::Installer>(installer);
    context.Add<Data::InstallerPath>(sourceDirectory.GetPath());
    context.Add<Data::PortableInstaller>(std::move(portableInstaller));

    context << PortableInstallImpl;

    INFO(output.str());
    CHECK(context.IsTerminated());
    CHECK(context.GetTerminationHR() == APPINSTALLER_CLI_ERROR_INVALID_MANIFEST);
    for (const auto& relativePath : sourceDirectories)
    {
        CAPTURE(relativePath);
        CHECK(std::filesystem::is_directory(sourceDirectory.GetPath() / relativePath));
    }
    for (const auto& [relativePath, expectedHash] : sourceHashes)
    {
        CAPTURE(relativePath);
        const auto sourcePath = sourceDirectory.GetPath() / relativePath;
        const bool sourceExists = std::filesystem::is_regular_file(sourcePath);
        CHECK(sourceExists);
        if (sourceExists)
        {
            CHECK(SHA256::ConvertToString(SHA256::ComputeHashFromFile(sourcePath)) == expectedHash);
        }
    }

    if (isUpgrade)
    {
        const bool previousExists = std::filesystem::is_regular_file(previousPath);
        CHECK(previousExists);
        if (previousExists)
        {
            CHECK(SHA256::ConvertToString(SHA256::ComputeHashFromFile(previousPath)) == previousHash);
        }
        const bool indexExists = std::filesystem::is_regular_file(indexPath);
        CHECK(indexExists);
        if (indexExists)
        {
            CHECK(SHA256::ConvertToString(SHA256::ComputeHashFromFile(indexPath)) == previousIndexHash);
        }
        const auto version = arpEntry[PortableValueName::DisplayVersion];
        REQUIRE(version.has_value());
        CHECK(version->GetValue<AppInstaller::Registry::Value::Type::UTF16String>() == L"1.0");
    }
    else
    {
        CHECK_FALSE(std::filesystem::exists(targetDirectory.GetPath()));
    }
}

TEST_CASE("PortableInstallFlow_ArchiveAliasCompatibility", "[InstallFlow][workflow][PortableAliasRegression]")
{
    TestHook::SetPortableTestEnvironment_Override testEnvironment;
    using AppInstaller::CLI::Portable::PortableInstaller;

    const bool executableInSubdirectory = GENERATE(false, true);
    const bool binariesDependOnPath = GENERATE(false, true);
    CAPTURE(executableInSubdirectory, binariesDependOnPath);
    TestHook::SetIsRunningWithNonDefaultFullToken_Override tokenOverride(false);
    TestHook::SetCreateSymlinkResult_Override symlinkOverride(false);
    TempDirectory sourceDirectory("PortableAliasCompatibilitySource");
    TempDirectory targetDirectory("PortableAliasCompatibilityTarget", false);
    TempDirectory linksDirectory("PortableAliasCompatibilityLinks");
    const std::filesystem::path relativeDirectory = executableInSubdirectory ? "bin" : "";
    std::vector<std::pair<std::filesystem::path, std::string>> expectedFiles;
    auto addPayload = [&](const std::filesystem::path& relativePath, std::string_view contents)
    {
        auto hash = WritePortableArchivePayload(sourceDirectory.GetPath() / relativePath, contents);
        expectedFiles.emplace_back(relativePath, hash);
        return hash;
    };
    ManifestInstaller installer;
    installer.BaseInstallerType = InstallerTypeEnum::Zip;
    installer.NestedInstallerType = InstallerTypeEnum::Portable;
    installer.ArchiveBinariesDependOnPath = binariesDependOnPath;

    SECTION("Equivalent source paths request the same alias after extension and case mapping")
    {
        const auto relativePath = relativeDirectory / "tool.exe";
        const auto hash = addPayload(relativePath, "shared alias executable");
        installer.NestedInstallerFiles = { { relativePath.u8string(), "command" }, { ".\\" + relativePath.u8string(), "COMMAND.exe" } };
        expectedFiles.emplace_back(relativeDirectory / "command.exe", hash);
    }

    SECTION("Different sources use aliases that Windows keeps distinct")
    {
        const auto firstExecutable = relativeDirectory / "first.exe";
        const auto secondExecutable = relativeDirectory / "second.exe";
        const auto firstHash = addPayload(firstExecutable, "first Windows-distinct alias executable");
        const auto secondHash = addPayload(secondExecutable, "second Windows-distinct alias executable");
        installer.NestedInstallerFiles = {
            { firstExecutable.u8string(), ConvertToUTF8(L"stra\u00DFe") },
            { secondExecutable.u8string(), "strasse" },
        };
        expectedFiles.emplace_back(relativeDirectory / L"stra\u00DFe.exe", firstHash);
        expectedFiles.emplace_back(relativeDirectory / "strasse.exe", secondHash);
    }

    const auto productCode = targetDirectory.GetPath().filename().u8string();
    const auto originalLinks = AppInstaller::Runtime::GetPathDetailsFor(AppInstaller::Runtime::PathName::PortableLinksUserLocation);
    AppInstaller::Registry::Portable::PortableARPEntry arpEntry(ScopeEnum::User, Architecture::X64, productCode);
    auto cleanup = wil::scope_exit([&]()
    {
        AppInstaller::Runtime::TestHook_SetPathOverride(AppInstaller::Runtime::PathName::PortableLinksUserLocation, originalLinks);
        auto pathVariable = AppInstaller::Registry::Environment::PathVariable(ScopeEnum::User);
        pathVariable.Remove(targetDirectory.GetPath() / relativeDirectory);
        pathVariable.Remove(linksDirectory.GetPath());
        arpEntry.Delete();
    });
    AppInstaller::Runtime::TestHook_SetPathOverride(AppInstaller::Runtime::PathName::PortableLinksUserLocation, linksDirectory.GetPath());

    PortableInstaller portableInstaller{ ScopeEnum::User, Architecture::X64, productCode, OperationType::Install };
    portableInstaller.TargetInstallLocation = targetDirectory.GetPath();
    portableInstaller.BinariesDependOnPath = binariesDependOnPath;

    std::ostringstream output;
    TestContext context{ output, std::cin };
    auto previousThreadGlobals = context.SetForCurrentThread();
    context.Add<Data::Installer>(installer);
    context.Add<Data::InstallerPath>(sourceDirectory.GetPath());
    context.Add<Data::PortableInstaller>(std::move(portableInstaller));

    context << PortableInstallImpl;

    INFO(output.str());
    REQUIRE_FALSE(context.IsTerminated());
    REQUIRE(context.Get<Data::OperationReturnCode>() == ERROR_SUCCESS);
    for (const auto& [relativePath, expectedHash] : expectedFiles)
    {
        CAPTURE(relativePath);
        const auto targetPath = targetDirectory.GetPath() / relativePath;
        REQUIRE(std::filesystem::is_regular_file(targetPath));
        CHECK(SHA256::ConvertToString(SHA256::ComputeHashFromFile(targetPath)) == expectedHash);
    }

    PortableInstaller uninstaller{ ScopeEnum::User, Architecture::X64, productCode, OperationType::Uninstall };
    REQUIRE(uninstaller.VerifyExpectedState());
    uninstaller.Uninstall();
    CHECK_FALSE(std::filesystem::exists(targetDirectory.GetPath()));
}

TEST_CASE("PortableInstallFlow_PreparationFailurePreservesRegistrationState", "[InstallFlow][workflow][PortableAliasRegression]")
{
    TestHook::SetPortableTestEnvironment_Override testEnvironment;
    const HKEY arpRoot = testEnvironment.GetARPRegistryRoot(ScopeEnum::User);
    using AppInstaller::CLI::Portable::PortableInstaller;
    using AppInstaller::Portable::PortableFileEntry;

    const bool existingRegistration = GENERATE(false, true);
    CAPTURE(existingRegistration);
    TestHook::SetIsRunningWithNonDefaultFullToken_Override tokenOverride(false);
    TestHook::SetCreateSymlinkResult_Override symlinkOverride(false);
    TempDirectory sourceDirectory("PortablePreparationFailureSource");
    TempDirectory previousSourceDirectory("PortablePreparationFailurePrevious");
    TempDirectory targetDirectory("PortablePreparationFailureTarget", false);
    TempDirectory linksDirectory("PortablePreparationFailureLinks");
    const auto sourcePath = sourceDirectory.GetPath() / "tool.exe";
    const auto sourceHash = WritePortableArchivePayload(sourcePath, "unapplied executable");
    const auto productCode = targetDirectory.GetPath().filename().u8string();
    const auto subKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" + ConvertToUTF16(productCode);
    REQUIRE(!AppInstaller::Registry::Key::OpenIfExists(arpRoot, subKey));
    const auto originalLinks = AppInstaller::Runtime::GetPathDetailsFor(AppInstaller::Runtime::PathName::PortableLinksUserLocation);
    auto cleanup = wil::scope_exit([&]()
    {
        AppInstaller::Runtime::TestHook_SetPathOverride(AppInstaller::Runtime::PathName::PortableLinksUserLocation, originalLinks);
        auto pathVariable = AppInstaller::Registry::Environment::PathVariable(ScopeEnum::User);
        pathVariable.Remove(targetDirectory.GetPath());
        pathVariable.Remove(linksDirectory.GetPath());
        AppInstaller::Registry::Key::Delete(arpRoot, subKey, KEY_WOW64_64KEY);
    });
    AppInstaller::Runtime::TestHook_SetPathOverride(AppInstaller::Runtime::PathName::PortableLinksUserLocation, linksDirectory.GetPath());

    const auto previousPath = targetDirectory.GetPath() / "previous.exe";
    std::string previousHash;
    if (existingRegistration)
    {
        const auto previousSource = previousSourceDirectory.GetPath() / "previous.exe";
        previousHash = WritePortableArchivePayload(previousSource, "previous installed executable");
        PortableInstaller previousInstaller{ ScopeEnum::User, Architecture::X64, productCode, OperationType::Install };
        previousInstaller.TargetInstallLocation = targetDirectory.GetPath();
        previousInstaller.RecordToIndex = true;
        previousInstaller.BinariesDependOnPath = true;
        previousInstaller.DisplayVersion = "1.0";
        previousInstaller.WinGetPackageIdentifier = productCode;
        previousInstaller.WinGetSourceIdentifier = "*DefaultSource";
        previousInstaller.SetDesiredState({
            PortableFileEntry::CreateFileEntry(previousSource, previousPath, previousHash),
            PortableFileEntry::CreateSymlinkEntry(linksDirectory.GetPath() / "previous.exe", previousPath),
        });
        previousInstaller.Install();
        REQUIRE(AppInstaller::Registry::Environment::PathVariable(ScopeEnum::User).Contains(targetDirectory.GetPath()));
    }

    ManifestInstaller installer;
    std::filesystem::path installerPath;
    bool planningException = false;

    SECTION("Validation terminates before installation")
    {
        installer.BaseInstallerType = InstallerTypeEnum::Portable;
        installer.Commands = { "C:\\escape" };
        installerPath = sourcePath;
    }

    SECTION("Alias collision terminates before installation")
    {
        WritePortableArchivePayload(sourceDirectory.GetPath() / "command.exe", "conflicting payload");
        installer.BaseInstallerType = InstallerTypeEnum::Zip;
        installer.NestedInstallerType = InstallerTypeEnum::Portable;
        installer.NestedInstallerFiles = { { "tool.exe", "command" } };
        installerPath = sourceDirectory.GetPath();
    }

    SECTION("Desired-state construction throws before installation")
    {
        installer.BaseInstallerType = InstallerTypeEnum::Zip;
        installer.NestedInstallerType = InstallerTypeEnum::Portable;
        installer.NestedInstallerFiles = { { "missing.exe", "command" } };
        installerPath = sourceDirectory.GetPath();
        planningException = true;
    }

    PortableInstaller portableInstaller{ ScopeEnum::User, Architecture::X64, productCode, OperationType::Install };
    portableInstaller.TargetInstallLocation = targetDirectory.GetPath();
    const auto indexPath = targetDirectory.GetPath() / portableInstaller.GetPortableIndexFileName();
    const auto previousIndexHash = existingRegistration ? SHA256::ConvertToString(SHA256::ComputeHashFromFile(indexPath)) : "";

    std::ostringstream output;
    TestContext context{ output, std::cin };
    auto previousThreadGlobals = context.SetForCurrentThread();
    context.Add<Data::Installer>(installer);
    context.Add<Data::InstallerPath>(installerPath);
    context.Add<Data::PortableInstaller>(std::move(portableInstaller));

    context << PortableInstallImpl;

    INFO(output.str());
    if (planningException)
    {
        REQUIRE(context.Contains(Data::OperationReturnCode));
        CHECK(context.Get<Data::OperationReturnCode>() != ERROR_SUCCESS);
    }
    else
    {
        CHECK(context.IsTerminated());
        CHECK(context.GetTerminationHR() == APPINSTALLER_CLI_ERROR_INVALID_MANIFEST);
    }
    REQUIRE(std::filesystem::is_regular_file(sourcePath));
    CHECK(SHA256::ConvertToString(SHA256::ComputeHashFromFile(sourcePath)) == sourceHash);

    const auto key = AppInstaller::Registry::Key::OpenIfExists(arpRoot, subKey);
    CHECK(static_cast<bool>(key) == existingRegistration);
    if (existingRegistration)
    {
        REQUIRE(key);
        const auto version = key[std::wstring{ L"DisplayVersion" }];
        REQUIRE(version.has_value());
        CHECK(version->GetValue<AppInstaller::Registry::Value::Type::String>() == "1.0");
        REQUIRE(std::filesystem::is_regular_file(previousPath));
        CHECK(SHA256::ConvertToString(SHA256::ComputeHashFromFile(previousPath)) == previousHash);
        REQUIRE(std::filesystem::is_regular_file(indexPath));
        CHECK(SHA256::ConvertToString(SHA256::ComputeHashFromFile(indexPath)) == previousIndexHash);
        CHECK(AppInstaller::Registry::Environment::PathVariable(ScopeEnum::User).Contains(targetDirectory.GetPath()));
    }
    else
    {
        CHECK_FALSE(std::filesystem::exists(targetDirectory.GetPath()));
        CHECK_FALSE(std::filesystem::exists(indexPath));
    }
}

TEST_CASE("PortableInstallFlow_RejectsEscapingPathsAtPointOfUse", "[InstallFlow][workflow]")
{
    TestCommon::TempDirectory targetDirectory("TestPortableInstallRoot", false);
    TestCommon::TempDirectory extractedDirectory("TestPortableExtractedRoot", true);
    TestCommon::TempFile installerFile("TestPortableInstaller.exe");

    std::ostringstream installOutput;
    TestContext context{ installOutput, std::cin };
    auto previousThreadGlobals = context.SetForCurrentThread();

    ManifestInstaller installer;
    std::filesystem::path installerPath = installerFile.GetPath();

    SECTION("Command alias")
    {
        installer.BaseInstallerType = InstallerTypeEnum::Portable;
        installer.Commands = { "C:\\escape" };
    }

    SECTION("Nested installer relative path")
    {
        installer.BaseInstallerType = InstallerTypeEnum::Zip;
        installer.NestedInstallerFiles = { { "C:\\escape", {} } };
        installerPath = extractedDirectory.GetPath();
    }

    SECTION("Nested installer command alias")
    {
        installer.BaseInstallerType = InstallerTypeEnum::Zip;
        installer.NestedInstallerFiles = { { "installer.exe", "C:\\escape" } };
        installerPath = extractedDirectory.GetPath();
    }

    AppInstaller::CLI::Portable::PortableInstaller portableInstaller{
        ScopeEnum::User, Architecture::X64, "TestProductCode", OperationType::Install };
    portableInstaller.TargetInstallLocation = targetDirectory.GetPath();

    context.Add<Execution::Data::Installer>(installer);
    context.Add<Execution::Data::InstallerPath>(installerPath);
    context.Add<Execution::Data::PortableInstaller>(std::move(portableInstaller));

    PortableInstallImpl(context);

    REQUIRE_TERMINATED_WITH(context, APPINSTALLER_CLI_ERROR_INVALID_MANIFEST);
}

TEST_CASE("InstallFlow_Portable_SymlinkCreationFail", "[InstallFlow][workflow]")
{
    TestHook::SetIsRunningWithNonDefaultFullToken_Override tokenOverride(false);

    TestCommon::TempDirectory tempDirectory("TestPortableInstallRoot", false);
    std::ostringstream installOutput;
    TestContext installContext{ installOutput, std::cin };
    auto PreviousThreadGlobals = installContext.SetForCurrentThread();
    OverridePortableInstaller(installContext);
    TestHook::SetCreateSymlinkResult_Override createSymlinkResultOverride(false);
    const auto& targetDirectory = tempDirectory.GetPath();
    const auto& portableTargetPath = targetDirectory / "AppInstallerTestExeInstaller.exe";
    installContext.Args.AddArg(Execution::Args::Type::Manifest, TestDataFile("InstallFlowTest_Portable.yaml").GetPath().u8string());
    installContext.Args.AddArg(Execution::Args::Type::InstallLocation, targetDirectory.u8string());
    installContext.Args.AddArg(Execution::Args::Type::InstallScope, "user"sv);

    InstallCommand install({});
    install.Execute(installContext);

    {
        INFO(installOutput.str());

        // Use CHECK to allow the uninstall to still occur
        CHECK(std::filesystem::exists(portableTargetPath));
        CHECK(AppInstaller::Registry::Environment::PathVariable(AppInstaller::Manifest::ScopeEnum::User).Contains(targetDirectory));
    }

    // Perform uninstall
    std::ostringstream uninstallOutput;
    TestContext uninstallContext{ uninstallOutput, std::cin };
    auto previousThreadGlobals = uninstallContext.SetForCurrentThread();
    uninstallContext.Args.AddArg(Execution::Args::Type::Name, "AppInstaller Test Portable Exe"sv);
    uninstallContext.Args.AddArg(Execution::Args::Type::AcceptSourceAgreements);

    UninstallCommand uninstall({});
    uninstall.Execute(uninstallContext);
    INFO(uninstallOutput.str());
    REQUIRE_FALSE(std::filesystem::exists(portableTargetPath));
}

TEST_CASE("InstallFlow_AdminContextWithUserScopeUninstallPrevious", "[InstallFlow][workflow]")
{
    using namespace AppInstaller::Manifest;
    using namespace AppInstaller::Repository;

    const bool explicitUninstallPrevious = GENERATE(false, true);
    CAPTURE(explicitUninstallPrevious);
    TestHook::SetIsRunningWithNonDefaultFullToken_Override tokenOverride(true);

    auto manifest = YamlParser::CreateFromPath(TestDataFile("InstallFlowTest_Exe.yaml"));
    auto installer = manifest.Installers.at(0);
    installer.UpdateBehavior = explicitUninstallPrevious ? UpdateBehaviorEnum::Install : UpdateBehaviorEnum::UninstallPrevious;

    std::ostringstream output;
    TestContext context{ output, std::cin };
    context.SetFlags(Execution::ContextFlag::InstallerExecutionUseUpdate);
    context.Add<Execution::Data::Installer>(installer);
    context.Add<Execution::Data::InstalledPackageVersion>(TestPackageVersion::Make(
        manifest,
        TestPackageVersion::MetadataMap{
            { PackageVersionMetadata::InstalledType, "Msi" },
            { PackageVersionMetadata::InstalledScope, "User" },
        }));
    if (explicitUninstallPrevious)
    {
        context.Args.AddArg(Execution::Args::Type::UninstallPrevious);
    }

    context << Workflow::EnsureSupportForInstall;

    REQUIRE_TERMINATED_WITH(context, APPINSTALLER_CLI_ERROR_ADMIN_CONTEXT_ACTION_PROHIBITED);
}

TEST_CASE("InstallFlow_UninstallPreviousPreservesMetadata", "[InstallFlow][workflow][UninstallPrevious]")
{
    using namespace AppInstaller::Repository;
    const auto installerType = GENERATE(InstallerTypeEnum::Exe, InstallerTypeEnum::Portable);
    const bool uninstallPreviousArg = GENERATE(false, true);
    const bool uninstallFails = GENERATE(false, true);
    CAPTURE(installerType, uninstallPreviousArg, uninstallFails);
    TestHook::SetIsRunningWithNonDefaultFullToken_Override tokenOverride(false);
    TestUserSettings settings;
    settings.Set<Setting::InstallScopePreference>(ScopeEnum::Machine);
    auto manifest = YamlParser::CreateFromPath(TestDataFile("InstallFlowTest_Portable.yaml"));
    manifest.Id = "Portable.UninstallPreviousMetadata";
    auto installer = manifest.Installers.at(0);
    installer.BaseInstallerType = installerType;
    installer.UpdateBehavior = uninstallPreviousArg ? UpdateBehaviorEnum::Install : UpdateBehaviorEnum::UninstallPrevious;
    const IPackageVersion::Metadata metadata{
        { PackageVersionMetadata::InstalledType, std::string(InstallerTypeToString(installerType)) },
        { PackageVersionMetadata::InstalledScope, "User" },
        { PackageVersionMetadata::InstalledArchitecture, "x64" },
        { PackageVersionMetadata::UserIntentLocale, "en-US" },
    };
    const auto productCode = MakeSuitablePathPart(manifest.Id + "_*DefaultSource");
    AppInstaller::Registry::Portable::PortableARPEntry arpEntry(ScopeEnum::User, Architecture::X64, productCode);
    auto cleanup = wil::scope_exit([&]() { arpEntry.Delete(); });
    TempDirectory target("PortablePreviousMetadata", false);

    std::ostringstream output;
    TestContext context{ output, std::cin };
    auto previousThreadGlobals = context.SetForCurrentThread();
    context.Add<Data::Manifest>(manifest);
    context.Add<Data::Installer>(installer);
    context.Add<Data::InstalledPackageVersion>(TestPackageVersion::Make(manifest, metadata));
    context.SetFlags(Execution::ContextFlag::InstallerExecutionUseUpdate);
    context.Args.AddArg(Execution::Args::Type::InstallLocation, target);
    context.Args.AddArg(Execution::Args::Type::InstallScope, "machine"sv);
    if (uninstallPreviousArg)
    {
        context.Args.AddArg(Execution::Args::Type::UninstallPrevious);
    }

    context.Override({ GetUninstallInfo, [](TestContext&) {}, 1 });
    context.Override({ ExecuteUninstaller, [&](TestContext& context)
        {
            REQUIRE(context.Get<Data::UninstalledPackageMetadata>() == metadata);
            context.Add<Data::InstalledPackageVersion>(std::shared_ptr<IPackageVersion>{});
            if (uninstallFails)
            {
                AICLI_TERMINATE_CONTEXT(E_ABORT);
            }
        }, 1 });
    if (!uninstallFails)
    {
        auto verifyReplacement = [&](TestContext& context)
            {
                REQUIRE_FALSE(WI_IsFlagSet(context.GetFlags(), Execution::ContextFlag::InstallerExecutionUseUpdate));
                REQUIRE(context.Get<Data::UninstalledPackageMetadata>() == metadata);
                if (installerType == InstallerTypeEnum::Portable)
                {
                    context << InitializePortableInstaller;
                    REQUIRE_FALSE(context.IsTerminated());
                    REQUIRE(context.Get<Data::PortableInstaller>().GetScope() == ScopeEnum::User);
                    REQUIRE_FALSE(context.Get<Data::PortableInstaller>().IsUpdate());
                }
            };
        context.Override({ installerType == InstallerTypeEnum::Portable ?
            Workflow::details::PortableInstall : Workflow::details::ShellExecuteInstall, verifyReplacement, 1 });
    }

    context << ExecuteInstallerForType(installerType);
    INFO(output.str());
    REQUIRE(context.Get<Data::UninstalledPackageMetadata>() == metadata);
    if (uninstallFails)
    {
        REQUIRE_TERMINATED_WITH(context, E_ABORT);
        REQUIRE(WI_IsFlagSet(context.GetFlags(), Execution::ContextFlag::InstallerExecutionUseUpdate));
    }
    else
    {
        REQUIRE_FALSE(context.IsTerminated());
    }
}

TEST_CASE("PortableInstallFlow_UninstallPreviousPreservesElevatedMachineScope", "[InstallFlow][workflow][UninstallPrevious]")
{
    using namespace AppInstaller::Repository;
    const bool uninstallPreviousArg = GENERATE(false, true);
    CAPTURE(uninstallPreviousArg);
    TestHook::SetIsRunningWithNonDefaultFullToken_Override tokenOverride(true);
    TestUserSettings settings;
    settings.Set<Setting::InstallScopePreference>(ScopeEnum::User);
    ManifestInstaller installer;
    installer.BaseInstallerType = InstallerTypeEnum::Portable;
    installer.UpdateBehavior = uninstallPreviousArg ? UpdateBehaviorEnum::Install : UpdateBehaviorEnum::UninstallPrevious;
    std::ostringstream output;
    TestContext context{ output, std::cin };
    auto previousThreadGlobals = context.SetForCurrentThread();
    context.Add<Data::Installer>(installer);
    context.Add<Data::InstalledPackageVersion>(TestPackageVersion::Make(
        Manifest{}, TestPackageVersion::MetadataMap{
            { PackageVersionMetadata::InstalledType, "Portable" },
            { PackageVersionMetadata::InstalledScope, "Machine" } }));
    context.SetFlags(Execution::ContextFlag::InstallerExecutionUseUpdate);
    if (uninstallPreviousArg)
    {
        context.Args.AddArg(Execution::Args::Type::UninstallPrevious);
    }
    TempDirectory links("PortablePreviousMachineLinks", true);
    const auto originalLinks = AppInstaller::Runtime::GetPathDetailsFor(AppInstaller::Runtime::PathName::PortableLinksMachineLocation);
    auto restoreLinks = wil::scope_exit([&]()
        {
            AppInstaller::Runtime::TestHook_SetPathOverride(AppInstaller::Runtime::PathName::PortableLinksMachineLocation, originalLinks);
        });
    AppInstaller::Filesystem::PathDetails linksDetails;
    linksDetails.Path = links.GetPath();
    linksDetails.Create = false;
    AppInstaller::Runtime::TestHook_SetPathOverride(AppInstaller::Runtime::PathName::PortableLinksMachineLocation, linksDetails);
    context.Override({ GetUninstallInfo, [](TestContext&) {}, 1 });
    context.Override({ ExecuteUninstaller, [](TestContext&) {}, 1 });
    context.Override({ Workflow::details::PortableInstall, [](TestContext& context)
        {
            REQUIRE_FALSE(WI_IsFlagSet(context.GetFlags(), Execution::ContextFlag::InstallerExecutionUseUpdate));
            REQUIRE_FALSE(context.Args.Contains(Execution::Args::Type::InstallScope));
            context << EnsureSupportForPortableInstall;
            REQUIRE_FALSE(context.IsTerminated());
        }, 1 });

    context << EnsureSupportForPortableInstall << ExecuteInstallerForType(InstallerTypeEnum::Portable);
    INFO(output.str());
    REQUIRE_FALSE(context.IsTerminated());
}

TEST_CASE("PortableInstallFlow_PreservedMetadataRetainsElevationProtection", "[InstallFlow][workflow][UninstallPrevious]")
{
    using namespace AppInstaller::Repository;
    const bool scopePresent = GENERATE(false, true);
    CAPTURE(scopePresent);
    TestHook::SetIsRunningWithNonDefaultFullToken_Override tokenOverride(true);
    IPackageVersion::Metadata metadata;
    if (scopePresent)
    {
        metadata.emplace(PackageVersionMetadata::InstalledScope, "User");
    }

    std::ostringstream output;
    TestContext context{ output, std::cin };
    context.Add<Data::UninstalledPackageMetadata>(std::move(metadata));
    context.Args.AddArg(Execution::Args::Type::InstallScope, "machine"sv);
    context << InitializePortableInstaller;

    REQUIRE_TERMINATED_WITH(context, APPINSTALLER_CLI_ERROR_ADMIN_CONTEXT_ACTION_PROHIBITED);
    REQUIRE_FALSE(context.Contains(Data::PortableInstaller));
}

TEST_CASE("FontInstallFlow_ScopeAndElevation", "[InstallFlow][workflow]")
{
    const bool fullToken = GENERATE(false, true);
    const auto installerType = GENERATE(InstallerTypeEnum::Font, InstallerTypeEnum::Zip, InstallerTypeEnum::Exe);
    const auto scope = GENERATE(""sv, "user"sv, "machine"sv, "unknown"sv);
    CAPTURE(fullToken, installerType, scope);
    TestHook::SetIsRunningWithNonDefaultFullToken_Override tokenOverride(fullToken);

    ManifestInstaller installer;
    installer.BaseInstallerType = installerType;
    if (installerType == InstallerTypeEnum::Zip)
    {
        installer.NestedInstallerType = InstallerTypeEnum::Font;
    }

    std::ostringstream output;
    TestContext context{ output, std::cin };
    context.Add<Execution::Data::Installer>(installer);
    if (!scope.empty())
    {
        context.Args.AddArg(Execution::Args::Type::InstallScope, scope);
    }

    context << EnsureSupportForFontInstall;

    if (fullToken && installerType != InstallerTypeEnum::Exe && scope != "machine")
    {
        REQUIRE_TERMINATED_WITH(context, APPINSTALLER_CLI_ERROR_ADMIN_CONTEXT_ACTION_PROHIBITED);
    }
    else
    {
        REQUIRE_FALSE(context.IsTerminated());
        REQUIRE(output.str().empty());
    }
}

TEST_CASE("FontInstallFlow_AdminContextRejectsUserScopeBeforePreparation", "[InstallFlow][workflow]")
{
    const bool execute = GENERATE(false, true);
    const bool archive = GENERATE(false, true);
    const bool update = GENERATE(false, true);
    CAPTURE(execute, archive, update);
    TestHook::SetIsRunningWithNonDefaultFullToken_Override tokenOverride(true);

    ManifestInstaller installer;
    installer.BaseInstallerType = archive ? InstallerTypeEnum::Zip : InstallerTypeEnum::Font;
    if (archive)
    {
        installer.NestedInstallerType = InstallerTypeEnum::Font;
    }

    std::ostringstream output;
    TestContext context{ output, std::cin };
    context.Add<Execution::Data::Installer>(installer);
    context.Args.AddArg(Execution::Args::Type::Force);
    if (update)
    {
        context.SetFlags(Execution::ContextFlag::InstallerExecutionUseUpdate);
    }

    if (execute)
    {
        context << FontInstallImpl;
    }
    else
    {
        context << EnsureSupportForInstall;
    }

    REQUIRE_TERMINATED_WITH(context, APPINSTALLER_CLI_ERROR_ADMIN_CONTEXT_ACTION_PROHIBITED);
    REQUIRE_FALSE(context.Contains(Execution::Data::OperationReturnCode));
}

TEST_CASE("PortableInstallFlow_AdminContextRejectsUserScopeUpdateBeforePreparation", "[InstallFlow][workflow]")
{
    using namespace AppInstaller::Manifest;
    using namespace AppInstaller::Repository;
    using namespace AppInstaller::Settings;

    const bool initialize = GENERATE(false, true);
    const auto installedScope = GENERATE("User", "Unknown");
    CAPTURE(initialize, installedScope);
    TestHook::SetIsRunningWithNonDefaultFullToken_Override tokenOverride(true);
    TestUserSettings settings;
    settings.Set<Setting::InstallScopeRequirement>(ScopeEnum::Unknown);
    settings.Set<Setting::InstallScopePreference>(ScopeEnum::Machine);

    std::ostringstream output;
    TestContext context{ output, std::cin };
    context.SetFlags(Execution::ContextFlag::InstallerExecutionUseUpdate);
    context.Args.AddArg(Execution::Args::Type::InstallScope, "machine"sv);
    context.Add<Execution::Data::InstalledPackageVersion>(TestPackageVersion::Make(
        AppInstaller::Manifest::Manifest{},
        TestPackageVersion::MetadataMap{
            { PackageVersionMetadata::InstalledType, "Portable" },
            { PackageVersionMetadata::InstalledScope, installedScope },
        }));

    if (initialize)
    {
        context << Workflow::InitializePortableInstaller;
    }
    else
    {
        auto manifest = YamlParser::CreateFromPath(TestDataFile("InstallFlowTest_Portable.yaml"));
        context.Add<Execution::Data::Installer>(manifest.Installers.at(0));
        context << Workflow::EnsureSupportForPortableInstall;
    }

    REQUIRE_TERMINATED_WITH(context, APPINSTALLER_CLI_ERROR_ADMIN_CONTEXT_ACTION_PROHIBITED);
    REQUIRE_FALSE(context.Contains(Execution::Data::PortableInstaller));
}

TEST_CASE("PortableInstallFlow_ScopeAndElevation", "[InstallFlow][workflow]")
{
    const bool fullToken = GENERATE(false, true);
    const auto scope = GENERATE(ScopeEnum::User, ScopeEnum::Unknown, ScopeEnum::Machine);
    const auto installerType = GENERATE(InstallerTypeEnum::Portable, InstallerTypeEnum::Zip, InstallerTypeEnum::Exe);
    CAPTURE(fullToken, scope, installerType);
    TestHook::SetIsRunningWithNonDefaultFullToken_Override tokenOverride(fullToken);
    TempDirectory machineLinks("PortableElevationMachineLinks", true);
    const auto originalMachineLinks = AppInstaller::Runtime::GetPathDetailsFor(AppInstaller::Runtime::PathName::PortableLinksMachineLocation);
    auto restoreMachineLinks = wil::scope_exit([&]()
        {
            AppInstaller::Runtime::TestHook_SetPathOverride(AppInstaller::Runtime::PathName::PortableLinksMachineLocation, originalMachineLinks);
        });
    AppInstaller::Filesystem::PathDetails machineLinksDetails;
    machineLinksDetails.Path = machineLinks.GetPath();
    machineLinksDetails.Create = false;
    AppInstaller::Runtime::TestHook_SetPathOverride(AppInstaller::Runtime::PathName::PortableLinksMachineLocation, machineLinksDetails);

    ManifestInstaller installer;
    installer.BaseInstallerType = installerType;
    if (installerType == InstallerTypeEnum::Zip)
    {
        installer.NestedInstallerType = InstallerTypeEnum::Portable;
    }

    std::ostringstream output;
    TestContext context{ output, std::cin };
    context.Args.AddArg(Execution::Args::Type::InstallScope, ScopeToString(scope));
    context.Add<Execution::Data::Installer>(installer);
    context << Workflow::EnsureSupportForPortableInstall;

    INFO(output.str());
    if (fullToken && scope != ScopeEnum::Machine && installerType != InstallerTypeEnum::Exe)
    {
        REQUIRE_TERMINATED_WITH(context, APPINSTALLER_CLI_ERROR_ADMIN_CONTEXT_ACTION_PROHIBITED);
    }
    else
    {
        REQUIRE_FALSE(context.IsTerminated());
    }
}

TEST_CASE("PortableInstallFlow_AdminContextRejectsFreshUserScopeBeforePreparation", "[InstallFlow][workflow]")
{
    const bool initialize = GENERATE(false, true);
    const bool archive = GENERATE(false, true);
    const auto scope = GENERATE(ScopeEnum::User, ScopeEnum::Unknown);
    const auto scopeSource = GENERATE("argument", "requirement", "preference");
    CAPTURE(initialize, archive, scope, scopeSource);
    TestHook::SetIsRunningWithNonDefaultFullToken_Override tokenOverride(true);
    TestUserSettings settings;
    settings.Set<Setting::InstallScopeRequirement>(ScopeEnum::Unknown);
    settings.Set<Setting::InstallScopePreference>(ScopeEnum::Machine);

    std::ostringstream output;
    TestContext context{ output, std::cin };
    if (std::string_view{ scopeSource } == "argument")
    {
        context.Args.AddArg(Execution::Args::Type::InstallScope, ScopeToString(scope));
    }
    else if (std::string_view{ scopeSource } == "requirement")
    {
        settings.Set<Setting::InstallScopeRequirement>(scope);
        settings.Set<Setting::InstallScopePreference>(scope);
    }
    else
    {
        settings.Set<Setting::InstallScopePreference>(scope);
    }

    if (initialize)
    {
        context << Workflow::InitializePortableInstaller;
    }
    else
    {
        ManifestInstaller installer;
        installer.BaseInstallerType = archive ? InstallerTypeEnum::Zip : InstallerTypeEnum::Portable;
        installer.NestedInstallerType = archive ? InstallerTypeEnum::Portable : InstallerTypeEnum::Unknown;
        context.Add<Execution::Data::Installer>(installer);
        context << Workflow::EnsureSupportForPortableInstall;
    }

    REQUIRE_TERMINATED_WITH(context, APPINSTALLER_CLI_ERROR_ADMIN_CONTEXT_ACTION_PROHIBITED);
    REQUIRE_FALSE(context.Contains(Execution::Data::PortableInstaller));
    REQUIRE_FALSE(context.Contains(Execution::Data::OperationReturnCode));
}

TEST_CASE("PortableInstallFlow_AdminContextRejectsExecutionBeforeIndexedState", "[InstallFlow][workflow]")
{
    const bool recordToIndex = GENERATE(false, true);
    CAPTURE(recordToIndex);
    TestHook::SetIsRunningWithNonDefaultFullToken_Override tokenOverride(true);
    const std::string productCode = "PortableExecutionElevationTest";
    AppInstaller::Registry::Portable::PortableARPEntry arpEntry(ScopeEnum::User, Architecture::X64, productCode);
    auto cleanup = wil::scope_exit([&]() { arpEntry.Delete(); });
    AppInstaller::CLI::Portable::PortableInstaller installer(ScopeEnum::User, Architecture::X64, productCode, OperationType::Install);
    installer.RecordToIndex = recordToIndex;

    std::ostringstream output;
    TestContext context{ output, std::cin };
    context.Add<Execution::Data::PortableInstaller>(std::move(installer));
    context << Workflow::PortableInstallImpl;

    REQUIRE_TERMINATED_WITH(context, APPINSTALLER_CLI_ERROR_ADMIN_CONTEXT_ACTION_PROHIBITED);
    REQUIRE_FALSE(context.Contains(Execution::Data::OperationReturnCode));
    REQUIRE_FALSE(context.Contains(Execution::Data::CorrelatedAppsAndFeaturesEntries));
}

TEST_CASE("PortableInstallFlow_NonFullContextChecksExistingRegistration", "[InstallFlow][workflow]")
{
    const auto scopeSource = GENERATE("argument", "requirement", "preference");
    const bool identifiersMatch = GENERATE(false, true);
    CAPTURE(scopeSource, identifiersMatch);
    TestHook::SetIsRunningWithNonDefaultFullToken_Override tokenOverride(false);
    TestUserSettings settings;
    settings.Set<Setting::InstallScopeRequirement>(ScopeEnum::Unknown);
    settings.Set<Setting::InstallScopePreference>(ScopeEnum::Machine);
    TempDirectory targetDirectory("PortableFreshElevation", false);

    auto manifest = YamlParser::CreateFromPath(TestDataFile("InstallFlowTest_Portable.yaml"));
    manifest.Id = "Portable.FreshElevation";
    const auto productCode = MakeSuitablePathPart(manifest.Id + "_*DefaultSource");
    AppInstaller::Registry::Portable::PortableARPEntry arpEntry(ScopeEnum::User, Architecture::X64, productCode);
    auto cleanup = wil::scope_exit([&]() { arpEntry.Delete(); });
    arpEntry.SetValue(AppInstaller::Registry::Portable::PortableValueName::WinGetPackageIdentifier,
        identifiersMatch ? "Portable.FreshElevation"sv : "old-package"sv);
    arpEntry.SetValue(AppInstaller::Registry::Portable::PortableValueName::WinGetSourceIdentifier, "*DefaultSource"sv);

    std::ostringstream output;
    TestContext context{ output, std::cin };
    context.Args.AddArg(Execution::Args::Type::InstallLocation, targetDirectory);
    if (std::string_view{ scopeSource } == "argument")
    {
        context.Args.AddArg(Execution::Args::Type::InstallScope, "user"sv);
    }
    else if (std::string_view{ scopeSource } == "requirement")
    {
        settings.Set<Setting::InstallScopeRequirement>(ScopeEnum::User);
    }
    else
    {
        settings.Set<Setting::InstallScopePreference>(ScopeEnum::User);
    }
    context.Add<Execution::Data::Installer>(manifest.Installers.at(0));
    context.Add<Execution::Data::Manifest>(std::move(manifest));

    context <<
        Workflow::EnsureSupportForPortableInstall <<
        Workflow::InitializePortableInstaller <<
        Workflow::VerifyPackageAndSourceMatch;

    INFO(output.str());
    if (!identifiersMatch)
    {
        REQUIRE_TERMINATED_WITH(context, APPINSTALLER_CLI_ERROR_PORTABLE_PACKAGE_ALREADY_EXISTS);
        return;
    }

    REQUIRE_FALSE(context.IsTerminated());
    auto& portableInstaller = context.Get<Execution::Data::PortableInstaller>();
    REQUIRE_FALSE(portableInstaller.IsUpdate());
    REQUIRE(portableInstaller.GetScope() == ScopeEnum::User);
    REQUIRE(portableInstaller.TargetInstallLocation == targetDirectory.GetPath());
    REQUIRE(portableInstaller.WinGetPackageIdentifier == "Portable.FreshElevation");
    REQUIRE(portableInstaller.WinGetSourceIdentifier == "*DefaultSource");
}

TEST_CASE("PortableInstallFlow_UserScope", "[InstallFlow][workflow]")
{
    TestHook::SetIsRunningWithNonDefaultFullToken_Override tokenOverride(false);

    TestCommon::TempDirectory tempDirectory("TestPortableInstallRoot", false);
    TestCommon::TempFile portableInstallResultPath("TestPortableInstalled.txt");

    std::ostringstream installOutput;
    TestContext context{ installOutput, std::cin };
    auto previousThreadGlobals = context.SetForCurrentThread();
    OverrideForPortableInstallFlow(context);
    context.Args.AddArg(Execution::Args::Type::Manifest, TestDataFile("InstallFlowTest_Portable.yaml").GetPath().u8string());
    context.Args.AddArg(Execution::Args::Type::InstallLocation, tempDirectory);
    context.Args.AddArg(Execution::Args::Type::InstallScope, "user"sv);

    InstallCommand install({});
    install.Execute(context);
    INFO(installOutput.str());

    REQUIRE(std::filesystem::exists(portableInstallResultPath.GetPath()));
}

TEST_CASE("PortableInstallFlow_MachineScope", "[InstallFlow][workflow]")
{
    if (!AppInstaller::Runtime::IsRunningAsAdmin())
    {
        WARN("Test requires admin privilege. Skipped.");
        return;
    }

    TestCommon::TempDirectory tempDirectory("TestPortableInstallRoot", false);
    TestCommon::TempFile portableInstallResultPath("TestPortableInstalled.txt");

    std::ostringstream installOutput;
    TestContext context{ installOutput, std::cin };
    auto previousThreadGlobals = context.SetForCurrentThread();
    OverrideForPortableInstallFlow(context);
    context.Args.AddArg(Execution::Args::Type::Manifest, TestDataFile("InstallFlowTest_Portable.yaml").GetPath().u8string());
    context.Args.AddArg(Execution::Args::Type::InstallLocation, tempDirectory);
    context.Args.AddArg(Execution::Args::Type::InstallScope, "machine"sv);

    InstallCommand install({});
    install.Execute(context);
    INFO(installOutput.str());
    REQUIRE(std::filesystem::exists(portableInstallResultPath.GetPath()));
}

TEST_CASE("ShellExecuteHandlerInstallerArgs", "[InstallFlow][workflow][InstallerLogPath]")
{
    InstallerLogDirectoryOverride logDirectory{ 7 };
    TestUserSettings testSettings;
    testSettings.Set<Setting::LoggingFileNameStrategy>(LogNameStrategy::Manifest);

    {
        std::ostringstream installOutput;
        TestContext context{ installOutput, std::cin };
        auto previousThreadGlobals = context.SetForCurrentThread();
        // Default Msi type with no args passed in, no switches specified in manifest
        auto manifest = YamlParser::CreateFromPath(TestDataFile("InstallerArgTest_Msi_NoSwitches.yaml"));
        context.Add<Data::Manifest>(manifest);
        context.Add<Data::Installer>(manifest.Installers.at(0));
        context.Add<Data::InstallerPath>(TestDataFile("AppInstallerTestExeInstaller.exe"));
        context << GetInstallerArgs;
        std::string installerArgs = context.Get<Data::InstallerArgs>();
        REQUIRE(installerArgs.find("/passive") != std::string::npos);
        REQUIRE(installerArgs.find(context.Get<Data::LogPath>().u8string()) != std::string::npos);
        REQUIRE(installerArgs.find(manifest.Id) != std::string::npos);
        REQUIRE(installerArgs.find(manifest.Version) != std::string::npos);
    }

    {
        std::ostringstream installOutput;
        TestContext context{ installOutput, std::cin };
        auto previousThreadGlobals = context.SetForCurrentThread();
        // Msi type with /silent and /log and /custom and /installlocation, no switches specified in manifest
        auto manifest = YamlParser::CreateFromPath(TestDataFile("InstallerArgTest_Msi_NoSwitches.yaml"));
        context.Args.AddArg(Execution::Args::Type::Silent);
        context.Args.AddArg(Execution::Args::Type::Log, "MyLog.log"sv);
        context.Args.AddArg(Execution::Args::Type::InstallLocation, "MyDir"sv);
        context.Add<Data::Manifest>(manifest);
        context.Add<Data::Installer>(manifest.Installers.at(0));
        context << GetInstallerArgs;
        std::string installerArgs = context.Get<Data::InstallerArgs>();
        REQUIRE(installerArgs.find("/quiet") != std::string::npos);
        REQUIRE(installerArgs.find("/log \"MyLog.log\"") != std::string::npos);
        REQUIRE(installerArgs.find("TARGETDIR=\"MyDir\"") != std::string::npos);
    }

    {
        std::ostringstream installOutput;
        TestContext context{ installOutput, std::cin };
        auto previousThreadGlobals = context.SetForCurrentThread();
        // Msi type with /silent and /log and /custom and /installlocation, switches specified in manifest
        auto manifest = YamlParser::CreateFromPath(TestDataFile("InstallerArgTest_Msi_WithSwitches.yaml"));
        context.Args.AddArg(Execution::Args::Type::Silent);
        context.Args.AddArg(Execution::Args::Type::Log, "MyLog.log"sv);
        context.Args.AddArg(Execution::Args::Type::InstallLocation, "MyDir"sv);
        context.Add<Data::Manifest>(manifest);
        context.Add<Data::Installer>(manifest.Installers.at(0));
        context << GetInstallerArgs;
        std::string installerArgs = context.Get<Data::InstallerArgs>();
        REQUIRE(installerArgs.find("/mysilent") != std::string::npos); // Use declaration in manifest
        REQUIRE(installerArgs.find("/mylog=\"MyLog.log\"") != std::string::npos); // Use declaration in manifest
        REQUIRE(installerArgs.find("/mycustom") != std::string::npos); // Use declaration in manifest
        REQUIRE(installerArgs.find("/myinstalldir=\"MyDir\"") != std::string::npos); // Use declaration in manifest
    }

    {
        std::ostringstream installOutput;
        TestContext context{ installOutput, std::cin };
        auto previousThreadGlobals = context.SetForCurrentThread();
        // Default Inno type with no args passed in, no switches specified in manifest
        auto manifest = YamlParser::CreateFromPath(TestDataFile("InstallerArgTest_Inno_NoSwitches.yaml"));
        context.Add<Data::Manifest>(manifest);
        context.Add<Data::Installer>(manifest.Installers.at(0));
        context.Add<Data::InstallerPath>(TestDataFile("AppInstallerTestExeInstaller.exe"));
        context << GetInstallerArgs;
        std::string installerArgs = context.Get<Data::InstallerArgs>();
        REQUIRE(installerArgs.find("/SILENT") != std::string::npos);
        REQUIRE(installerArgs.find(context.Get<Data::LogPath>().u8string()) != std::string::npos);
        REQUIRE(installerArgs.find(manifest.Id) != std::string::npos);
        REQUIRE(installerArgs.find(manifest.Version) != std::string::npos);
    }

    {
        std::ostringstream installOutput;
        TestContext context{ installOutput, std::cin };
        auto previousThreadGlobals = context.SetForCurrentThread();
        // Inno type with /silent and /log and /custom and /installlocation, no switches specified in manifest
        auto manifest = YamlParser::CreateFromPath(TestDataFile("InstallerArgTest_Inno_NoSwitches.yaml"));
        context.Args.AddArg(Execution::Args::Type::Silent);
        context.Args.AddArg(Execution::Args::Type::Log, "MyLog.log"sv);
        context.Args.AddArg(Execution::Args::Type::InstallLocation, "MyDir"sv);
        context.Add<Data::Manifest>(manifest);
        context.Add<Data::Installer>(manifest.Installers.at(0));
        context << GetInstallerArgs;
        std::string installerArgs = context.Get<Data::InstallerArgs>();
        REQUIRE(installerArgs.find("/VERYSILENT") != std::string::npos);
        REQUIRE(installerArgs.find("/LOG=\"MyLog.log\"") != std::string::npos);
        REQUIRE(installerArgs.find("/DIR=\"MyDir\"") != std::string::npos);
    }

    {
        std::ostringstream installOutput;
        TestContext context{ installOutput, std::cin };
        auto previousThreadGlobals = context.SetForCurrentThread();
        // Inno type with /silent and /log and /custom and /installlocation, switches specified in manifest
        auto manifest = YamlParser::CreateFromPath(TestDataFile("InstallerArgTest_Inno_WithSwitches.yaml"));
        context.Args.AddArg(Execution::Args::Type::Silent);
        context.Args.AddArg(Execution::Args::Type::Log, "MyLog.log"sv);
        context.Args.AddArg(Execution::Args::Type::InstallLocation, "MyDir"sv);
        context.Add<Data::Manifest>(manifest);
        context.Add<Data::Installer>(manifest.Installers.at(0));
        context << GetInstallerArgs;
        std::string installerArgs = context.Get<Data::InstallerArgs>();
        REQUIRE(installerArgs.find("/mysilent") != std::string::npos); // Use declaration in manifest
        REQUIRE(installerArgs.find("/mylog=\"MyLog.log\"") != std::string::npos); // Use declaration in manifest
        REQUIRE(installerArgs.find("/mycustom") != std::string::npos); // Use declaration in manifest
        REQUIRE(installerArgs.find("/myinstalldir=\"MyDir\"") != std::string::npos); // Use declaration in manifest
    }

    {
        std::ostringstream installOutput;
        TestContext context{ installOutput, std::cin };
        auto previousThreadGlobals = context.SetForCurrentThread();
        // Inno type with /silent and /log and /custom and /installlocation, switches specified in manifest and --custom argument used in cli
        auto manifest = YamlParser::CreateFromPath(TestDataFile("InstallerArgTest_Inno_WithSwitches.yaml"));
        context.Args.AddArg(Execution::Args::Type::Silent);
        context.Args.AddArg(Execution::Args::Type::Log, "MyLog.log"sv);
        context.Args.AddArg(Execution::Args::Type::InstallLocation, "MyDir"sv);
        context.Args.AddArg(Execution::Args::Type::CustomSwitches, "/MyAppendedSwitch"sv);
        context.Add<Data::Manifest>(manifest);
        context.Add<Data::Installer>(manifest.Installers.at(0));
        context << GetInstallerArgs;
        std::string installerArgs = context.Get<Data::InstallerArgs>();
        REQUIRE(installerArgs.find("/mysilent") != std::string::npos); // Use declaration in manifest
        REQUIRE(installerArgs.find("/mylog=\"MyLog.log\"") != std::string::npos); // Use declaration in manifest
        REQUIRE(installerArgs.find("/mycustom") != std::string::npos); // Use declaration in manifest
        REQUIRE(installerArgs.find("/myinstalldir=\"MyDir\"") != std::string::npos); // Use declaration in manifest
        REQUIRE(installerArgs.find("/MyAppendedSwitch") != std::string::npos); // Use declaration from argument
    }

    {
        std::ostringstream installOutput;
        TestContext context{ installOutput, std::cin };
        auto previousThreadGlobals = context.SetForCurrentThread();
        // Inno type with /silent and /log and /custom and /installlocation, switches specified in manifest and whitespace-only --custom argument used in cli
        auto manifest = YamlParser::CreateFromPath(TestDataFile("InstallerArgTest_Inno_WithSwitches.yaml"));
        context.Args.AddArg(Execution::Args::Type::Silent);
        context.Args.AddArg(Execution::Args::Type::CustomSwitches, "\t"sv);
        context.Add<Data::Manifest>(manifest);
        context.Add<Data::Installer>(manifest.Installers.at(0));
        context << GetInstallerArgs;
        std::string installerArgs = context.Get<Data::InstallerArgs>();
        REQUIRE(installerArgs.find("/mysilent") != std::string::npos); // Use declaration in manifest
        REQUIRE(installerArgs.find("\t") == std::string::npos); // Whitespace only Custom switches should not be appended
    }

    {
        std::ostringstream installOutput;
        TestContext context{ installOutput, std::cin };
        auto previousThreadGlobals = context.SetForCurrentThread();
        // Override switch specified. The whole arg passed to installer is overridden.
        auto manifest = YamlParser::CreateFromPath(TestDataFile("InstallerArgTest_Inno_WithSwitches.yaml"));
        context.Args.AddArg(Execution::Args::Type::Silent);
        context.Args.AddArg(Execution::Args::Type::Log, "MyLog.log"sv);
        context.Args.AddArg(Execution::Args::Type::InstallLocation, "MyDir"sv);
        context.Args.AddArg(Execution::Args::Type::Override, "/OverrideEverything"sv);
        context.Add<Data::Manifest>(manifest);
        context.Add<Data::Installer>(manifest.Installers.at(0));
        context << GetInstallerArgs;
        std::string installerArgs = context.Get<Data::InstallerArgs>();
        REQUIRE(installerArgs == "/OverrideEverything"); // Use value specified in override switch
    }
}

TEST_CASE("ShellExecuteHandlerInstallerArgs_LogNamingStrategy", "[InstallFlow][workflow]")
{
    InstallerLogDirectoryOverride logDirectory{ 7 };

    SECTION("Manifest")
    {
        TestUserSettings testSettings;
        testSettings.Set<Setting::LoggingFileNameStrategy>(LogNameStrategy::Manifest);

        std::ostringstream installOutput;
        TestContext context{ installOutput, std::cin };
        auto previousThreadGlobals = context.SetForCurrentThread();
        auto manifest = YamlParser::CreateFromPath(TestDataFile("InstallerArgTest_Inno_WithSwitches.yaml"));
        context.Add<Data::Manifest>(manifest);
        context.Add<Data::Installer>(manifest.Installers.at(0));
        context << GetInstallerArgs;

        std::string installerArgs = context.Get<Data::InstallerArgs>();
        REQUIRE(installerArgs.find(manifest.Id) != std::string::npos);
        REQUIRE(installerArgs.find(manifest.Version) != std::string::npos);

        REQUIRE(context.Contains(Data::LogPath));
        auto logPath = context.Get<Data::LogPath>();
        REQUIRE(logPath.filename().u8string().find(manifest.Id) != std::string::npos);
    }

    SECTION("Timestamp")
    {
        TestUserSettings testSettings;
        testSettings.Set<Setting::LoggingFileNameStrategy>(LogNameStrategy::Timestamp);

        std::ostringstream installOutput;
        TestContext context{ installOutput, std::cin };
        auto previousThreadGlobals = context.SetForCurrentThread();
        auto manifest = YamlParser::CreateFromPath(TestDataFile("InstallerArgTest_Inno_WithSwitches.yaml"));
        context.Add<Data::Manifest>(manifest);
        context.Add<Data::Installer>(manifest.Installers.at(0));
        context << GetInstallerArgs;

        std::string installerArgs = context.Get<Data::InstallerArgs>();
        REQUIRE(installerArgs.find(manifest.Id) == std::string::npos);
        REQUIRE(installerArgs.find(manifest.Version) == std::string::npos);

        REQUIRE(context.Contains(Data::LogPath));
        auto logPath = context.Get<Data::LogPath>();
        REQUIRE(logPath.extension().u8string() == std::string{ FileLogger::DefaultExt() });
        REQUIRE(logPath.stem().u8string().find(manifest.Id) == std::string::npos);
    }

    SECTION("Guid")
    {
        TestUserSettings testSettings;
        testSettings.Set<Setting::LoggingFileNameStrategy>(LogNameStrategy::Guid);

        std::ostringstream installOutput;
        TestContext context{ installOutput, std::cin };
        auto previousThreadGlobals = context.SetForCurrentThread();
        auto manifest = YamlParser::CreateFromPath(TestDataFile("InstallerArgTest_Inno_WithSwitches.yaml"));
        context.Add<Data::Manifest>(manifest);
        context.Add<Data::Installer>(manifest.Installers.at(0));
        context << GetInstallerArgs;

        std::string installerArgs = context.Get<Data::InstallerArgs>();
        REQUIRE(installerArgs.find(manifest.Id) == std::string::npos);

        REQUIRE(context.Contains(Data::LogPath));
        auto logPath = context.Get<Data::LogPath>();
        REQUIRE(logPath.extension().u8string() == std::string{ FileLogger::DefaultExt() });
        // A GUID string is 36 characters: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
        REQUIRE(logPath.stem().u8string().size() == 36);
    }

    SECTION("ShortGuid")
    {
        TestUserSettings testSettings;
        testSettings.Set<Setting::LoggingFileNameStrategy>(LogNameStrategy::ShortGuid);

        std::ostringstream installOutput;
        TestContext context{ installOutput, std::cin };
        auto previousThreadGlobals = context.SetForCurrentThread();
        auto manifest = YamlParser::CreateFromPath(TestDataFile("InstallerArgTest_Inno_WithSwitches.yaml"));
        context.Add<Data::Manifest>(manifest);
        context.Add<Data::Installer>(manifest.Installers.at(0));
        context << GetInstallerArgs;

        std::string installerArgs = context.Get<Data::InstallerArgs>();
        REQUIRE(installerArgs.find(manifest.Id) == std::string::npos);

        REQUIRE(context.Contains(Data::LogPath));
        auto logPath = context.Get<Data::LogPath>();
        REQUIRE(logPath.extension().u8string() == std::string{ FileLogger::DefaultExt() });
        // A short GUID is the first 8 characters of a full GUID
        REQUIRE(logPath.stem().u8string().size() == 8);
    }
}

TEST_CASE("ShellExecuteHandlerInstallerArgs_LogPathBudget", "[InstallFlow][workflow][InstallerLogPath]")
{
    TestUserSettings testSettings;
    testSettings.Set<Setting::LoggingFileNameStrategy>(LogNameStrategy::Manifest);
    auto manifest = YamlParser::CreateFromPath(TestDataFile("InstallerArgTest_Inno_WithSwitches.yaml"));
    manifest.Id = "Microsoft.DotNet.SDK.10";
    manifest.Version = "10.0.404";
    size_t directoryLength = 95;
    bool shortened = false;
    bool timeOnly = false;
    bool shortGuid = false;
    std::wstring expectedPrefix;

    SECTION("ExactlyPreferredLength")
    {
    }
    SECTION("JustAbovePreferredLength")
    {
        directoryLength = 96;
        shortened = true;
    }
    SECTION("ExactlyPreferredCompactLength")
    {
        directoryLength = 109;
        shortened = true;
    }
    SECTION("TimeOnlyWithoutTruncation")
    {
        directoryLength = 110;
        shortened = timeOnly = true;
        expectedPrefix = L"Microsoft.DotNet.SDK.10";
    }
    SECTION("ReportedNestedSuffixes")
    {
        directoryLength = 119;
        shortened = timeOnly = true;
        expectedPrefix = L"Microsoft.DotNet.SD";
    }
    SECTION("LongVersion")
    {
        manifest.Version = std::string(200, '1');
        shortened = true;
    }
    SECTION("UnicodePackageId")
    {
        std::string id = "Publisher.";
        for (size_t i = 0; i < 30; ++i)
        {
            id += "\xF0\x9F\x98\x80";
        }
        manifest.Id = id + ".SDK";
        directoryLength = 119;
        shortened = timeOnly = true;
        expectedPrefix = L"Publisher.\U0001F600\U0001F600\U0001F600\U0001F600";
    }
    SECTION("LongPackageIdentity")
    {
        manifest.Id = std::string(200, 'a') + ".SDK";
        directoryLength = 119;
        shortened = timeOnly = true;
        expectedPrefix = std::wstring(19, L'a');
    }
    SECTION("NormalizedPackageIdPrefix")
    {
        manifest.Id = "Publisher.e\xCC\x81.SDK";
        directoryLength = 127;
        shortened = timeOnly = true;
        expectedPrefix = L"Publisher.\u00E9";
    }
    SECTION("CombiningCharacterBoundary")
    {
        // Unlike e + acute, x + acute remains two code units after normalization.
        manifest.Id = "Publisher.x\xCC\x81.SDK";
        directoryLength = 127;
        shortened = timeOnly = true;
        expectedPrefix = L"Publisher.";
    }
    SECTION("OneCharacterPrefix")
    {
        directoryLength = 137;
        shortened = timeOnly = true;
        expectedPrefix = L"M";
    }
    SECTION("NoRoomForFirstGrapheme")
    {
        manifest.Id = "\xF0\x9F\x98\x80.SDK";
        directoryLength = 137;
        shortGuid = true;
    }

    InstallerLogDirectoryOverride logDirectory{ directoryLength };
    std::ostringstream installOutput;
    TestContext context{ installOutput, std::cin };
    auto previousThreadGlobals = context.SetForCurrentThread();
    context.SetFlags(ContextFlag::InstallerExecutionUseUpdate);
    context.Add<Data::Manifest>(manifest);
    context.Add<Data::Installer>(manifest.Installers.at(0));
    context << GetInstallerArgs;

    REQUIRE_FALSE(context.IsTerminated());
    REQUIRE(installOutput.str().empty());
    REQUIRE(context.Contains(Data::LogPath));
    const auto& path = context.Get<Data::LogPath>();
    INFO(path.u8string());
    REQUIRE(path.native().size() <= 150);
    REQUIRE(path.extension() == L".log");
    REQUIRE(MakeSuitablePathPart(path.filename().u8string()) == path.filename().u8string());
    const auto stem = path.stem().u8string();
    REQUIRE(ConvertToUTF16(ConvertToUTF8(path.native())) == path.native());
    if (shortGuid)
    {
        REQUIRE(stem.size() == 8);
        REQUIRE(stem.find('-') == std::string::npos);
    }
    else if (shortened)
    {
        const auto timestampStart = stem.rfind('-');
        REQUIRE(timestampStart != std::string::npos);
        const auto timestamp = stem.substr(timestampStart + 1);
        REQUIRE(timestamp.size() == (timeOnly ? 6u : 12u));
        REQUIRE(std::all_of(timestamp.begin(), timestamp.end(), [](char c) { return c >= '0' && c <= '9'; }));
        REQUIRE(stem.find(manifest.Version) == std::string::npos);
        if (timeOnly)
        {
            REQUIRE(stem.substr(0, timestampStart) == ConvertToUTF8(expectedPrefix));
            REQUIRE(path.native().size() == directoryLength + 1 + expectedPrefix.size() + 11);
        }
        else
        {
            REQUIRE(stem.substr(0, timestampStart) == GetPathPart(manifest, '.', PathPartVersionStrategy::NeverIncludeVersion).u8string());
        }
    }
    else
    {
        REQUIRE(path.native().size() == 150);
        REQUIRE(stem.find(manifest.Id + '.' + manifest.Version) == 0);
    }
    const auto& args = context.Get<Data::InstallerArgs>();
    REQUIRE(args.find("/mylog=\"" + path.u8string() + '"') != std::string::npos);
    REQUIRE(args.find("/mycustom") != std::string::npos);
    for (const auto& suffix : {
        L"_013_Microsoft.NET.Sdk.Android.Manifest_6.0.100.31.0.101_preview.9.16_x64.msi",
        L"_018_{106C9C71-2390-4214-9E7E-7BA47FFB1FAB}_010_dotnet_sdk_internal_10.0.401_servicing.26423.113_win_x64.msi" })
    {
        auto derivedPath = path.parent_path() / (path.stem().native() + suffix + path.extension().native());
        REQUIRE(derivedPath.native().size() < MAX_PATH);
    }
}

TEST_CASE("ShellExecuteHandlerInstallerArgs_LogPathFallback", "[InstallFlow][workflow][InstallerLogPath]")
{
    const auto strategy = GENERATE(LogNameStrategy::Manifest, LogNameStrategy::Timestamp, LogNameStrategy::Guid, LogNameStrategy::ShortGuid);
    const size_t directoryLength = GENERATE(137, 138, 246, 247, 300);
    CAPTURE(strategy, directoryLength);
    InstallerLogDirectoryOverride logDirectory{ directoryLength };
    TestUserSettings testSettings;
    testSettings.Set<Setting::LoggingFileNameStrategy>(LogNameStrategy{ strategy });
    std::ostringstream installOutput;
    TestContext context{ installOutput, std::cin };
    auto previousThreadGlobals = context.SetForCurrentThread();
    auto manifest = YamlParser::CreateFromPath(TestDataFile("InstallerArgTest_Inno_WithSwitches.yaml"));
    manifest.Installers.at(0).Switches[InstallerSwitchType::Log] = "/before /myLog=\"<LOGPATH>\" /after";
    manifest.Installers.at(0).Switches[InstallerSwitchType::Custom] = "/myCustom /log=\"<LOGPATH>\"";
    context.Add<Data::Manifest>(manifest);
    context.Add<Data::Installer>(manifest.Installers.at(0));
    context.Args.AddArg(Execution::Args::Type::InstallLocation, "MyDir"sv);
    context.Override({ ShellExecuteInstallImpl, [](TestContext& context)
        {
            REQUIRE(context.Get<Data::InstallerArgs>().find("/myCustom") != std::string::npos);
            context.Add<Data::OperationReturnCode>(0);
        } });
    context << GetInstallerArgs << ShellExecuteInstallImpl;

    REQUIRE_FALSE(context.IsTerminated());
    REQUIRE(context.Get<Data::OperationReturnCode>() == 0);
    const auto& args = context.Get<Data::InstallerArgs>();
    REQUIRE(args.find("<LOGPATH>") == std::string::npos);
    REQUIRE(args.find("/mysilentwithprogress") != std::string::npos);
    REQUIRE(args.find("/myCustom") != std::string::npos);
    REQUIRE(args.find("/myinstalldir=\"MyDir\"") != std::string::npos);
    REQUIRE(context.Contains(Data::LogPath));
    const auto& path = context.Get<Data::LogPath>();
    const size_t stemLength = strategy == LogNameStrategy::Guid ? 36 : (strategy == LogNameStrategy::Timestamp ? 17 : 8);
    const size_t configuredLength = directoryLength + 1 + stemLength + 4;
    REQUIRE(path.native().size() == (configuredLength < MAX_PATH ? configuredLength : directoryLength + 1 + 12));
    REQUIRE(args.find("/before /myLog=\"" + path.u8string() + "\" /after") != std::string::npos);
    REQUIRE(args.find("/myCustom /log=\"" + path.u8string() + '"') != std::string::npos);
    REQUIRE(installOutput.str().empty());
}

TEST_CASE("ShellExecuteHandlerInstallerArgs_LogPathBypass", "[InstallFlow][workflow][InstallerLogPath]")
{
    InstallerLogDirectoryOverride logDirectory{ 300 };
    std::ostringstream installOutput;
    TestContext context{ installOutput, std::cin };
    auto previousThreadGlobals = context.SetForCurrentThread();
    auto manifest = YamlParser::CreateFromPath(TestDataFile("InstallerArgTest_Inno_WithSwitches.yaml"));
    auto installer = manifest.Installers.at(0);
    const std::string explicitPath = "C:\\" + std::string(270, 'a') + ".log";
    bool hasExplicitPath = false;
    bool overridden = false;
    SECTION("ExplicitLog")
    {
        context.Args.AddArg(Execution::Args::Type::Log, explicitPath);
        hasExplicitPath = true;
    }
    SECTION("Override")
    {
        context.Args.AddArg(Execution::Args::Type::Override, "/override"sv);
        overridden = true;
    }
    SECTION("NoLogToken")
    {
        installer.Switches[InstallerSwitchType::Log] = "/fixedLog";
    }
    context.Add<Data::Manifest>(manifest);
    context.Add<Data::Installer>(installer);
    context << GetInstallerArgs;

    REQUIRE_FALSE(context.IsTerminated());
    REQUIRE(context.Contains(Data::LogPath) == hasExplicitPath);
    const auto& args = context.Get<Data::InstallerArgs>();
    if (hasExplicitPath)
    {
        REQUIRE(context.Get<Data::LogPath>().u8string() == explicitPath);
        REQUIRE(args.find("/mylog=\"" + explicitPath + '"') != std::string::npos);
    }
    else if (overridden)
    {
        REQUIRE(args == "/override");
    }
    else
    {
        REQUIRE(args.find("/fixedLog") != std::string::npos);
    }
    REQUIRE(installOutput.str().empty());
}

TEST_CASE("InstallFlow_SearchAndInstall", "[InstallFlow][workflow]")
{
    TestCommon::TempFile installResultPath("TestExeInstalled.txt");

    std::ostringstream installOutput;
    TestContext context{ installOutput, std::cin };
    auto previousThreadGlobals = context.SetForCurrentThread();
    OverrideForOpenSource(context, CreateTestSource({ TSR::TestQuery_ReturnOne }), true);
    OverrideForShellExecute(context);
    context.Args.AddArg(Execution::Args::Type::Query, TSR::TestQuery_ReturnOne.Query);

    InstallCommand install({});
    install.Execute(context);
    INFO(installOutput.str());

    // Verify Installer is called and parameters are passed in.
    REQUIRE(std::filesystem::exists(installResultPath.GetPath()));
    std::ifstream installResultFile(installResultPath.GetPath());
    REQUIRE(installResultFile.is_open());
    std::string installResultStr;
    std::getline(installResultFile, installResultStr);
    REQUIRE(installResultStr.find("/custom") != std::string::npos);
    REQUIRE(installResultStr.find("/silentwithprogress") != std::string::npos);
}

TEST_CASE("InstallFlow_SearchFoundNoApp", "[InstallFlow][workflow]")
{
    std::ostringstream installOutput;
    TestContext context{ installOutput, std::cin };
    auto previousThreadGlobals = context.SetForCurrentThread();
    OverrideForOpenSource(context, CreateTestSource({}), true);
    context.Args.AddArg(Execution::Args::Type::Query, "TestQueryReturnZero"sv);

    InstallCommand install({});
    install.Execute(context);
    INFO(installOutput.str());

    // Verify proper message is printed
    REQUIRE(installOutput.str().find(Resource::LocString(Resource::String::NoPackageFound).get()) != std::string::npos);
}

TEST_CASE("InstallFlow_SearchFoundMultipleApp", "[InstallFlow][workflow]")
{
    std::ostringstream installOutput;
    TestContext context{ installOutput, std::cin };
    auto previousThreadGlobals = context.SetForCurrentThread();
    OverrideForOpenSource(context, CreateTestSource({ TSR::TestQuery_ReturnTwo }), true);
    context.Args.AddArg(Execution::Args::Type::Query, TSR::TestQuery_ReturnTwo.Query);

    InstallCommand install({});
    install.Execute(context);
    INFO(installOutput.str());

    // Verify proper message is printed
    REQUIRE(installOutput.str().find(Resource::LocString(Resource::String::MultiplePackagesFound).get()) != std::string::npos);
}

TEST_CASE("InstallFlow_LicenseAgreement", "[InstallFlow][workflow]")
{
    TestCommon::TempFile installResultPath("TestExeInstalled.txt");

    std::ostringstream installOutput;
    TestContext context{ installOutput, std::cin };
    auto previousThreadGlobals = context.SetForCurrentThread();
    OverrideForShellExecute(context);
    context.Args.AddArg(Execution::Args::Type::Manifest, TestDataFile("InstallFlowTest_LicenseAgreement.yaml").GetPath().u8string());
    context.Args.AddArg(Execution::Args::Type::AcceptPackageAgreements);

    InstallCommand install({});
    install.Execute(context);
    INFO(installOutput.str());

    // Verify agreements are shown
    REQUIRE(installOutput.str().find("Agreement with text") != std::string::npos);
    REQUIRE(installOutput.str().find("This is the text of the agreement.") != std::string::npos);
    REQUIRE(installOutput.str().find("Agreement with URL") != std::string::npos);
    REQUIRE(installOutput.str().find("https://TestAgreementUrl") != std::string::npos);

    // Verify Installer is called.
    REQUIRE(std::filesystem::exists(installResultPath.GetPath()));
}

TEST_CASE("InstallFlow_LicenseAgreement_Prompt", "[InstallFlow][workflow]")
{
    TestCommon::TempFile installResultPath("TestExeInstalled.txt");

    // Accept the agreements by saying "Yes" at the prompt
    std::istringstream installInput{ "y" };

    std::ostringstream installOutput;
    TestContext context{ installOutput, installInput };
    auto previousThreadGlobals = context.SetForCurrentThread();
    OverrideForShellExecute(context);
    context.Args.AddArg(Execution::Args::Type::Manifest, TestDataFile("InstallFlowTest_LicenseAgreement.yaml").GetPath().u8string());

    InstallCommand install({});
    install.Execute(context);
    INFO(installOutput.str());

    // Verify prompt was shown
    REQUIRE(installOutput.str().find(Resource::LocString(Resource::String::PackageAgreementsPrompt).get()) != std::string::npos);

    // Verify agreements are shown
    REQUIRE(installOutput.str().find("Agreement with text") != std::string::npos);
    REQUIRE(installOutput.str().find("This is the text of the agreement.") != std::string::npos);
    REQUIRE(installOutput.str().find("Agreement with URL") != std::string::npos);
    REQUIRE(installOutput.str().find("https://TestAgreementUrl") != std::string::npos);

    // Verify Installer is called.
    REQUIRE(std::filesystem::exists(installResultPath.GetPath()));
}

TEST_CASE("InstallFlow_LicenseAgreement_NotAccepted", "[InstallFlow][workflow]")
{
    TestCommon::TempFile installResultPath("TestExeInstalled.txt");

    // Say "No" at the agreements prompt
    std::istringstream installInput{ "n" };

    std::ostringstream installOutput;
    TestContext context{ installOutput, installInput };
    auto previousThreadGlobals = context.SetForCurrentThread();
    context.Args.AddArg(Execution::Args::Type::Manifest, TestDataFile("InstallFlowTest_LicenseAgreement.yaml").GetPath().u8string());

    InstallCommand install({});
    install.Execute(context);
    INFO(installOutput.str());

    // Verify agreements are shown
    REQUIRE(installOutput.str().find("Agreement with text") != std::string::npos);
    REQUIRE(installOutput.str().find("This is the text of the agreement.") != std::string::npos);
    REQUIRE(installOutput.str().find("Agreement with URL") != std::string::npos);
    REQUIRE(installOutput.str().find("https://TestAgreementUrl") != std::string::npos);

    // Verify installation failed
    REQUIRE_TERMINATED_WITH(context, APPINSTALLER_CLI_ERROR_PACKAGE_AGREEMENTS_NOT_ACCEPTED);
    REQUIRE_FALSE(std::filesystem::exists(installResultPath.GetPath()));
    REQUIRE(installOutput.str().find(Resource::LocString(Resource::String::PackageAgreementsNotAgreedTo).get()) != std::string::npos);
}

TEST_CASE("InstallFlowMultiLocale_RequirementNotSatisfied", "[InstallFlow][workflow]")
{
    TestCommon::TempFile installResultPath("TestExeInstalled.txt");

    std::ostringstream installOutput;
    TestContext context{ installOutput, std::cin };
    auto previousThreadGlobals = context.SetForCurrentThread();
    context.Args.AddArg(Execution::Args::Type::Manifest, TestDataFile("Manifest-Good-MultiLocale.yaml").GetPath().u8string());
    context.Args.AddArg(Execution::Args::Type::Locale, "en-US"sv);

    InstallCommand install({});
    install.Execute(context);
    INFO(installOutput.str());

    REQUIRE_TERMINATED_WITH(context, APPINSTALLER_CLI_ERROR_NO_APPLICABLE_INSTALLER);

    // Verify Installer was not called
    REQUIRE(!std::filesystem::exists(installResultPath.GetPath()));
}

TEST_CASE("InstallFlowMultiLocale_RequirementSatisfied", "[InstallFlow][workflow]")
{
    TestCommon::TempFile installResultPath("TestExeInstalled.txt");

    std::ostringstream installOutput;
    TestContext context{ installOutput, std::cin };
    auto previousThreadGlobals = context.SetForCurrentThread();
    OverrideForShellExecute(context);
    context.Args.AddArg(Execution::Args::Type::Manifest, TestDataFile("Manifest-Good-MultiLocale.yaml").GetPath().u8string());
    context.Args.AddArg(Execution::Args::Type::Locale, "fr-FR"sv);

    InstallCommand install({});
    install.Execute(context);
    INFO(installOutput.str());

    // Verify Installer is called and parameters are passed in.
    REQUIRE(std::filesystem::exists(installResultPath.GetPath()));
    std::ifstream installResultFile(installResultPath.GetPath());
    REQUIRE(installResultFile.is_open());
    std::string installResultStr;
    std::getline(installResultFile, installResultStr);
    REQUIRE(installResultStr.find("/fr-FR") != std::string::npos);
}

TEST_CASE("InstallFlowMultiLocale_PreferenceNoBetterLocale", "[InstallFlow][workflow]")
{
    TestCommon::TempFile installResultPath("TestExeInstalled.txt");

    std::ostringstream installOutput;
    TestContext context{ installOutput, std::cin };
    auto previousThreadGlobals = context.SetForCurrentThread();
    OverrideForShellExecute(context);
    context.Args.AddArg(Execution::Args::Type::Manifest, TestDataFile("Manifest-Good-MultiLocale.yaml").GetPath().u8string());

    TestUserSettings settings;
    settings.Set<AppInstaller::Settings::Setting::InstallLocalePreference>({ "zh-CN" });

    InstallCommand install({});
    install.Execute(context);
    INFO(installOutput.str());

    // Verify Installer is called and parameters are passed in.
    REQUIRE(std::filesystem::exists(installResultPath.GetPath()));
    std::ifstream installResultFile(installResultPath.GetPath());
    REQUIRE(installResultFile.is_open());
    std::string installResultStr;
    std::getline(installResultFile, installResultStr);
    REQUIRE(installResultStr.find("/unknown") != std::string::npos);
}

TEST_CASE("InstallFlowMultiLocale_PreferenceWithBetterLocale", "[InstallFlow][workflow]")
{
    TestCommon::TempFile installResultPath("TestExeInstalled.txt");

    std::ostringstream installOutput;
    TestContext context{ installOutput, std::cin };
    auto previousThreadGlobals = context.SetForCurrentThread();
    OverrideForShellExecute(context);
    context.Args.AddArg(Execution::Args::Type::Manifest, TestDataFile("Manifest-Good-MultiLocale.yaml").GetPath().u8string());

    TestUserSettings settings;
    settings.Set<AppInstaller::Settings::Setting::InstallLocalePreference>({ "en-US" });

    InstallCommand install({});
    install.Execute(context);
    INFO(installOutput.str());

    // Verify Installer is called and parameters are passed in.
    REQUIRE(std::filesystem::exists(installResultPath.GetPath()));
    std::ifstream installResultFile(installResultPath.GetPath());
    REQUIRE(installResultFile.is_open());
    std::string installResultStr;
    std::getline(installResultFile, installResultStr);
    REQUIRE(installResultStr.find("/en-GB") != std::string::npos);
}

TEST_CASE("InstallFlow_InstallMultiple", "[InstallFlow][workflow][MultiQuery]")
{
    TestCommon::TempFile exeInstallResultPath("TestExeInstalled.txt");
    TestCommon::TempFile msixInstallResultPath("TestMsixInstalled.txt");

    std::ostringstream installOutput;
    TestContext context{ installOutput, std::cin };
    auto previousThreadGlobals = context.SetForCurrentThread();
    OverrideForMSIX(context);
    OverrideForShellExecute(context);
    OverrideForOpenSource(context, CreateTestSource({ TSR::TestInstaller_Exe, TSR::TestInstaller_Msix }), true);
    context.Args.AddArg(Execution::Args::Type::MultiQuery, TSR::TestInstaller_Exe.Query);
    context.Args.AddArg(Execution::Args::Type::MultiQuery, TSR::TestInstaller_Msix.Query);

    InstallCommand installCommand({});
    installCommand.Execute(context);
    INFO(installOutput.str());

    // Verify all packages were installed
    REQUIRE(std::filesystem::exists(exeInstallResultPath.GetPath()));
    REQUIRE(std::filesystem::exists(msixInstallResultPath.GetPath()));
}

TEST_CASE("InstallFlow_InstallMultiple_SearchFailed", "[InstallFlow][workflow][MultiQuery]")
{
    std::ostringstream installOutput;
    TestContext context{ installOutput, std::cin };
    auto previousThreadGlobals = context.SetForCurrentThread();
    OverrideForOpenSource(context, CreateTestSource({ TSR::TestInstaller_Exe }), true);
    context.Args.AddArg(Execution::Args::Type::MultiQuery, TSR::TestInstaller_Exe.Query);
    context.Args.AddArg(Execution::Args::Type::MultiQuery, TSR::TestInstaller_Msix.Query);

    InstallCommand installCommand({});
    installCommand.Execute(context);
    INFO(installOutput.str());

    REQUIRE_TERMINATED_WITH(context, APPINSTALLER_CLI_ERROR_NOT_ALL_QUERIES_FOUND_SINGLE);
}

TEST_CASE("InstallFlow_InstallMultiple_IgnoreUnavailable", "[InstallFlow][workflow][MultiQuery]")
{
    TestCommon::TempFile exeInstallResultPath("TestExeInstalled.txt");

    std::ostringstream installOutput;
    TestContext context{ installOutput, std::cin };
    auto previousThreadGlobals = context.SetForCurrentThread();
    OverrideForShellExecute(context);
    OverrideForOpenSource(context, CreateTestSource({ TSR::TestInstaller_Exe }), true);
    context.Args.AddArg(Execution::Args::Type::MultiQuery, TSR::TestInstaller_Exe.Query);
    context.Args.AddArg(Execution::Args::Type::MultiQuery, TSR::TestInstaller_Msix.Query);
    context.Args.AddArg(Execution::Args::Type::IgnoreUnavailable);

    InstallCommand installCommand({});
    installCommand.Execute(context);
    INFO(installOutput.str());

    // Verify the available package was installed despite the missing one,
    // and the unavailable package was reported as not found.
    REQUIRE(std::filesystem::exists(exeInstallResultPath.GetPath()));
    REQUIRE(installOutput.str().find(Resource::LocString(Resource::String::MultiQueryPackageNotFound(LocIndString{ TSR::TestInstaller_Msix.Query })).get()) != std::string::npos);
}

TEST_CASE("InstallFlow_InstallAcquiresLock", "[InstallFlow][workflow]")
{
    TestCommon::TempFile installResultPath("TestExeInstalled.txt");

    std::ostringstream installOutput;
    TestContext context{ installOutput, std::cin };
    auto previousThreadGlobals = context.SetForCurrentThread();
    OverrideForOpenSource(context, CreateTestSource({ TSR::TestQuery_ReturnOne }), true);
    OverrideForShellExecute(context);
    context.Args.AddArg(Execution::Args::Type::Query, TSR::TestQuery_ReturnOne.Query);

    wil::unique_event enteredShellExecute;
    enteredShellExecute.create();
    wil::unique_event canLeaveShellExecute;
    canLeaveShellExecute.create();
    AppInstaller::ProgressCallback progress;

    context.Override({ ShellExecuteInstallImpl, [&](TestContext& context)
        {
            enteredShellExecute.SetEvent();
            canLeaveShellExecute.wait(500);
            ShellExecuteInstallImpl(context);
        }});

    {
        std::thread otherThread([&]() {
            InstallCommand install({});
            install.Execute(context);
            });

        REQUIRE(enteredShellExecute.wait(5000));

        AppInstaller::Synchronization::CrossProcessInstallLock mainThreadLock;
        REQUIRE(!mainThreadLock.TryAcquireNoWait());

        canLeaveShellExecute.SetEvent();
        otherThread.join();
    }

    INFO(installOutput.str());

    // Verify Installer is called and parameters are passed in.
    REQUIRE(std::filesystem::exists(installResultPath.GetPath()));
    std::ifstream installResultFile(installResultPath.GetPath());
    REQUIRE(installResultFile.is_open());
    std::string installResultStr;
    std::getline(installResultFile, installResultStr);
    REQUIRE(installResultStr.find("/custom") != std::string::npos);
    REQUIRE(installResultStr.find("/silentwithprogress") != std::string::npos);
}

TEST_CASE("InstallFlow_InstallWithReboot", "[InstallFlow][workflow][reboot]")
{
    TestCommon::TempFile installResultPath("TestExeInstalled.txt");
    TestCommon::TestUserSettings testSettings;

    std::ostringstream installOutput;
    TestContext context{ installOutput, std::cin };
    auto previousThreadGlobals = context.SetForCurrentThread();
    OverrideForShellExecute(context);

    context.Args.AddArg(Execution::Args::Type::Manifest, TestDataFile("InstallFlowTest_ExpectedReturnCodes.yaml").GetPath().u8string());
    context.Args.AddArg(Execution::Args::Type::AllowReboot);

    context.Override({ ShellExecuteInstallImpl, [&](TestContext& context)
    {
        // APPINSTALLER_CLI_ERROR_INSTALL_REBOOT_REQUIRED_TO_INSTALL (should be treated as an installer error)
        context.Add<Data::OperationReturnCode>(10);
    } });

    SECTION("Reboot success")
    {
        TestHook::SetInitiateRebootResult_Override initiateRebootResultOverride(true);

        InstallCommand install({});
        install.Execute(context);
        INFO(installOutput.str());

        REQUIRE(context.IsTerminated());
        REQUIRE(!std::filesystem::exists(installResultPath.GetPath()));
        REQUIRE(installOutput.str().find(Resource::LocString(Resource::String::InitiatingReboot).get()) != std::string::npos);
        REQUIRE_FALSE(installOutput.str().find(Resource::LocString(Resource::String::FailedToInitiateReboot).get()) != std::string::npos);
    }
    SECTION("Reboot failed")
    {
        TestHook::SetInitiateRebootResult_Override initiateRebootResultOverride(false);

        InstallCommand install({});
        install.Execute(context);
        INFO(installOutput.str());

        REQUIRE(context.IsTerminated());
        REQUIRE(!std::filesystem::exists(installResultPath.GetPath()));
        REQUIRE(installOutput.str().find(Resource::LocString(Resource::String::InitiatingReboot).get()) != std::string::npos);
        REQUIRE(installOutput.str().find(Resource::LocString(Resource::String::FailedToInitiateReboot).get()) != std::string::npos);
    }
}
