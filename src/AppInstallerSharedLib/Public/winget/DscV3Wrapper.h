// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include <filesystem>
#include <string>
#include <utility>

namespace AppInstaller::DSC
{
    // Provides functionality for interfacing with dsc.exe (DSC v3).
    struct DscV3Wrapper
    {
        DscV3Wrapper();
        DscV3Wrapper(std::filesystem::path executablePath);

        DscV3Wrapper(const DscV3Wrapper&) = default;
        DscV3Wrapper& operator=(const DscV3Wrapper&) = default;
        DscV3Wrapper(DscV3Wrapper&&) = default;
        DscV3Wrapper& operator=(DscV3Wrapper&&) = default;

        // Get or set the path the dsc.exe
        const std::filesystem::path& ExecutablePath() const;
        void ExecutablePath(const std::filesystem::path& path);

        // Finds dsc.exe, returning the path to it.
        // Returns an empty path if it is not found.
        static std::filesystem::path FindDscExecutablePath();

        // Determines if the given path represents a secure dsc.exe binary.
        static bool ValidateDscExecutablePath(const std::filesystem::path& path);

        // The result from running a dsc.exe command.
        struct ExecuteResult
        {
            std::string StdOut;
            std::string StdErr;
            DWORD ExitCode = 0;
        };

        // Calls `list` without a filter to get all resources.
        // Successful output is JSON lines with each line containing resource metadata.
        // https://learn.microsoft.com/en-us/powershell/dsc/reference/schemas/outputs/resource/list?view=dsc-3.0
        ExecuteResult ExecuteResourceList() const;

        // Calls `list` for the given resource.
        // Successful output is a single JSON line containing resource metadata.
        // https://learn.microsoft.com/en-us/powershell/dsc/reference/schemas/outputs/resource/list?view=dsc-3.0
        ExecuteResult ExecuteResourceList(const std::string& resource) const;

        // Calls `schema` for the given resource.
        // Successful output is a JSON schema document for the resource.
        ExecuteResult ExecuteResourceSchema(const std::string& resource) const;

        // Calls `get` for the given resource and input.
        // Successful output is a JSON object with the current properties of the resource.
        // https://learn.microsoft.com/en-us/powershell/dsc/reference/schemas/outputs/resource/get?view=dsc-3.0
        ExecuteResult ExecuteResourceGet(const std::string& resource, const std::string& input) const;

        // Calls `set` for the given resource and input.
        // Successful output is a JSON object with the previous, new, and changed properties of the resource.
        // https://learn.microsoft.com/en-us/powershell/dsc/reference/schemas/outputs/resource/set?view=dsc-3.0
        ExecuteResult ExecuteResourceSet(const std::string& resource, const std::string& input) const;

        // Calls `test` for the given resource and input.
        // Successful output is a JSON object with the result of testing the current state of the object.
        // https://learn.microsoft.com/en-us/powershell/dsc/reference/schemas/outputs/resource/test?view=dsc-3.0
        ExecuteResult ExecuteResourceTest(const std::string& resource, const std::string& input) const;

        // Calls `export` for the given resource.
        // Successful output is a JSON configuration document containing the exported resource instances.
        ExecuteResult ExecuteResourceExport(const std::string& resource) const;

        // Map a dsc.exe exit code to an HRESULT.
        static HRESULT MapExitCode(DWORD exitCode);
        static HRESULT MapExitCode(const ExecuteResult& result);

        // Gets the error message to use for a given error state.
        static std::string GetErrorMessage(HRESULT error, const std::string& errorOutput);

        // Gets the HRESULT and error message if it indicates failure.
        static std::pair<HRESULT, std::string> GetErrorInfo(const ExecuteResult& result);

        // Contains a structured version of the data from a `list` command.
        struct ResourceListItem
        {

        };

        // Processes the successful output from a `list` command.
        static std::vector<ResourceListItem> ProcessResourceListResult(const std::string& output);
        static std::vector<ResourceListItem> ProcessResourceListResult(const ExecuteResult& result);

        // Contains a structured version of the data from a `schema` command.
        struct ResourceSchema
        {

        };

        // Processes the successful output from a `schema` command.
        static ResourceSchema ProcessResourceSchemaResult(const std::string& output);
        static ResourceSchema ProcessResourceSchemaResult(const ExecuteResult& result);

        // Contains a structured version of the data from a `get` command.
        struct ResourceGetResponse
        {

        };

        // Processes the successful output from a `get` command.
        static ResourceGetResponse ProcessResourceGetResult(const std::string& output);
        static ResourceGetResponse ProcessResourceGetResult(const ExecuteResult& result);

    private:
        // Executes an arbitrary command.
        ExecuteResult ExecuteCommand(const std::string& arguments, const std::string& input, bool sendInput = true) const;

        std::filesystem::path m_executablePath;
    };
}
