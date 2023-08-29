// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include <winrt/Microsoft.Management.Configuration.h>

namespace winrt::Microsoft::Management::Configuration::implementation
{
    // The handler for the built-in assertions group.
    struct AssertionsGroup : winrt::implements<AssertionsGroup, IConfigurationGroupProcessor>
    {
        using ConfigurationUnit = Configuration::ConfigurationUnit;

        AssertionsGroup(const ConfigurationUnit& unit);

        // The type name of the group.
        static std::wstring_view Type();

        // The configuration group object (set or unit).
        Windows::Foundation::IInspectable Group();

        // Determines if the system is already in the state described by the configuration unit group.
        Windows::Foundation::IAsyncOperationWithProgress<ITestGroupSettingsResult, ITestSettingsResult> TestGroupSettingsAsync();

        // Gets the current system state for the configuration unit group.
        Windows::Foundation::IAsyncOperationWithProgress<IGetGroupSettingsResult, IGetSettingsResult> GetGroupSettingsAsync();

        // Applies the state described in the configuration unit group.
        Windows::Foundation::IAsyncOperationWithProgress<IApplyGroupSettingsResult, IApplySettingsResult> ApplyGroupSettingsAsync();

    private:
        ConfigurationUnit m_unit;
    };
}
