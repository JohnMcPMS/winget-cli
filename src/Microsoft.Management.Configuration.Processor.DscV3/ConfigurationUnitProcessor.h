// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include <winrt/Microsoft.Management.Configuration.h>
#include "DscV3ConfigurationSetProcessorFactory.h"

namespace winrt::Microsoft::Management::Configuration::Processor::DscV3::implementation
{
    struct ConfigurationUnitProcessor : winrt::implements<ConfigurationUnitProcessor, winrt::Microsoft::Management::Configuration::IGetAllSettingsConfigurationUnitProcessor>
    {
        ConfigurationUnitProcessor(const winrt::Microsoft::Management::Configuration::ConfigurationUnit& unit, winrt::weak_ref<DscV3ConfigurationSetProcessorFactory> const& weakFactory);

        winrt::Microsoft::Management::Configuration::ConfigurationUnit Unit() { return m_unit; }

        winrt::Microsoft::Management::Configuration::IGetSettingsResult GetSettings();
        winrt::Microsoft::Management::Configuration::ITestSettingsResult TestSettings();
        winrt::Microsoft::Management::Configuration::IApplySettingsResult ApplySettings();
        winrt::Microsoft::Management::Configuration::IGetAllSettingsResult GetAllSettings();

    private:
        winrt::Microsoft::Management::Configuration::ConfigurationUnit m_unit;
        winrt::weak_ref<DscV3ConfigurationSetProcessorFactory> m_weakFactory;
    };

    struct ConfigurationUnitProcessorGroup : winrt::implements<ConfigurationUnitProcessorGroup, winrt::Microsoft::Management::Configuration::IGetAllSettingsConfigurationUnitProcessor>
    {
        ConfigurationUnitProcessorGroup(const winrt::Microsoft::Management::Configuration::ConfigurationUnit& unit, winrt::weak_ref<DscV3ConfigurationSetProcessorFactory> const& weakFactory);

        winrt::Microsoft::Management::Configuration::ConfigurationUnit Unit() { return m_unit; }

        winrt::Microsoft::Management::Configuration::IGetSettingsResult GetSettings();
        winrt::Microsoft::Management::Configuration::ITestSettingsResult TestSettings();
        winrt::Microsoft::Management::Configuration::IApplySettingsResult ApplySettings();
        winrt::Microsoft::Management::Configuration::IGetAllSettingsResult GetAllSettings();

    private:
        winrt::Microsoft::Management::Configuration::ConfigurationUnit m_unit;
        winrt::weak_ref<DscV3ConfigurationSetProcessorFactory> m_weakFactory;
    };

    struct ConfigurationUnitProcessorPowerShellGroup : winrt::implements<ConfigurationUnitProcessorPowerShellGroup, winrt::Microsoft::Management::Configuration::IGetAllSettingsConfigurationUnitProcessor>
    {
        ConfigurationUnitProcessorPowerShellGroup(const winrt::Microsoft::Management::Configuration::ConfigurationUnit& unit, winrt::weak_ref<DscV3ConfigurationSetProcessorFactory> const& weakFactory);

        winrt::Microsoft::Management::Configuration::ConfigurationUnit Unit() { return m_unit; }

        winrt::Microsoft::Management::Configuration::IGetSettingsResult GetSettings();
        winrt::Microsoft::Management::Configuration::ITestSettingsResult TestSettings();
        winrt::Microsoft::Management::Configuration::IApplySettingsResult ApplySettings();
        winrt::Microsoft::Management::Configuration::IGetAllSettingsResult GetAllSettings();

    private:
        winrt::Microsoft::Management::Configuration::ConfigurationUnit m_unit;
        winrt::weak_ref<DscV3ConfigurationSetProcessorFactory> m_weakFactory;
    };
}
