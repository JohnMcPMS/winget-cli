// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "pch.h"
#include "ConfigurationSetProcessor.h"
#include "ConfigurationUnitProcessorDetails.h"
#include "ConfigurationUnitProcessor.h"

namespace winrt::Microsoft::Management::Configuration::Processor::DscV3::implementation
{
    using namespace winrt::Microsoft::Management::Configuration;

    namespace
    {
        constexpr std::wstring_view s_PowerShellGroup = L"DSC/PowerShellGroup";
    }

    ConfigurationSetProcessor::ConfigurationSetProcessor(ConfigurationSet const& configurationSet, winrt::weak_ref<DscV3ConfigurationSetProcessorFactory> const& weakFactory)
        : m_configurationSet(configurationSet), m_weakFactory(weakFactory) {}

    IConfigurationUnitProcessorDetails ConfigurationSetProcessor::GetUnitProcessorDetails(const ConfigurationUnit& unit, ConfigurationUnitDetailFlags detailFlags)
    {
        return winrt::make<ConfigurationUnitProcessorDetails>(unit, detailFlags);
    }

    IConfigurationUnitProcessor ConfigurationSetProcessor::CreateUnitProcessor(const ConfigurationUnit& unit)
    {
        // TODO: check schema version.

        if (unit.IsGroup())
        {
            // TODO: see if this is case-sensitive in dsc.
            if (unit.Type() == s_PowerShellGroup)
            {
                return winrt::make<ConfigurationUnitProcessorPowerShellGroup>(unit, m_weakFactory);
            }

            return winrt::make<ConfigurationUnitProcessorGroup>(unit, m_weakFactory);
        }

        return winrt::make<ConfigurationUnitProcessor>(unit, m_weakFactory);
    }
}
