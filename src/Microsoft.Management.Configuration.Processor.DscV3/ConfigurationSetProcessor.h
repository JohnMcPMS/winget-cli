// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include <winrt/Microsoft.Management.Configuration.h>
#include "DscV3ConfigurationSetProcessorFactory.h"

namespace winrt::Microsoft::Management::Configuration::Processor::DscV3::implementation
{
    struct ConfigurationSetProcessor : winrt::implements<ConfigurationSetProcessor, winrt::Microsoft::Management::Configuration::IConfigurationSetProcessor>
    {
        ConfigurationSetProcessor(winrt::Microsoft::Management::Configuration::ConfigurationSet const& configurationSet, winrt::weak_ref<DscV3ConfigurationSetProcessorFactory> const& weakFactory);

        winrt::Microsoft::Management::Configuration::IConfigurationUnitProcessorDetails GetUnitProcessorDetails(
            const winrt::Microsoft::Management::Configuration::ConfigurationUnit& unit,
            winrt::Microsoft::Management::Configuration::ConfigurationUnitDetailFlags detailFlags);

        winrt::Microsoft::Management::Configuration::IConfigurationUnitProcessor CreateUnitProcessor(
            const winrt::Microsoft::Management::Configuration::ConfigurationUnit& unit);

    private:
        winrt::Microsoft::Management::Configuration::ConfigurationSet m_configurationSet;
        winrt::weak_ref<DscV3ConfigurationSetProcessorFactory> m_weakFactory;
    };
}
