// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "pch.h"
#include "DscV3ConfigurationSetProcessorFactory.h"
#include "ConfigurationUnitProcessor.h"
#include "TestSettingsResult.h"
#include "GetSettingsResult.h"
#include "ApplySettingsResult.h"
#include "GetAllSettingsResult.h"

namespace winrt::Microsoft::Management::Configuration::Processor::DscV3::implementation
{
    using namespace winrt::Microsoft::Management::Configuration;

    ConfigurationUnitProcessor::ConfigurationUnitProcessor(const ConfigurationUnit& unit, winrt::weak_ref<DscV3ConfigurationSetProcessorFactory> const& weakFactory)
        : m_unit(unit), m_weakFactory(weakFactory)
    {
    }

    ITestSettingsResult ConfigurationUnitProcessor::TestSettings()
    {
        return winrt::make<TestSettingsResult>(m_unit);
    }

    IGetSettingsResult ConfigurationUnitProcessor::GetSettings()
    {
        return winrt::make<GetSettingsResult>(m_unit);
    }

    IApplySettingsResult ConfigurationUnitProcessor::ApplySettings()
    {
        return winrt::make<ApplySettingsResult>(m_unit);
    }

    IGetAllSettingsResult ConfigurationUnitProcessor::GetAllSettings()
    {
        return winrt::make<GetAllSettingsResult>(m_unit);
    }

    ConfigurationUnitProcessorGroup::ConfigurationUnitProcessorGroup(const ConfigurationUnit& unit, winrt::weak_ref<DscV3ConfigurationSetProcessorFactory> const& weakFactory)
        : m_unit(unit), m_weakFactory(weakFactory)
    {
    }

    ITestSettingsResult ConfigurationUnitProcessorGroup::TestSettings()
    {
        return winrt::make<TestSettingsResult>(m_unit);
    }

    IGetSettingsResult ConfigurationUnitProcessorGroup::GetSettings()
    {
        return winrt::make<GetSettingsResult>(m_unit);
    }

    IApplySettingsResult ConfigurationUnitProcessorGroup::ApplySettings()
    {
        return winrt::make<ApplySettingsResult>(m_unit);
    }

    IGetAllSettingsResult ConfigurationUnitProcessorGroup::GetAllSettings()
    {
        return winrt::make<GetAllSettingsResult>(m_unit);
    }

    ConfigurationUnitProcessorPowerShellGroup::ConfigurationUnitProcessorPowerShellGroup(const ConfigurationUnit& unit, winrt::weak_ref<DscV3ConfigurationSetProcessorFactory> const& weakFactory)
        : m_unit(unit), m_weakFactory(weakFactory)
    {
        // TODO: Ensure install modules.
        //   This needs to go through all the m_unit.Units
        //   Create a new thing that creates a PowerShellGroup with PowerShellGet\MSFT_PSModule (and hope PowerShellGet v3 ships with something similar)
        //   Then one the first Test/Get/Apply/GetAll/Export call Set on that.
        //   Then call the method.
    }

    ITestSettingsResult ConfigurationUnitProcessorPowerShellGroup::TestSettings()
    {
        return winrt::make<TestSettingsResult>(m_unit);
    }

    IGetSettingsResult ConfigurationUnitProcessorPowerShellGroup::GetSettings()
    {
        return winrt::make<GetSettingsResult>(m_unit);
    }

    IApplySettingsResult ConfigurationUnitProcessorPowerShellGroup::ApplySettings()
    {
        return winrt::make<ApplySettingsResult>(m_unit);
    }

    IGetAllSettingsResult ConfigurationUnitProcessorPowerShellGroup::GetAllSettings()
    {
        return winrt::make<GetAllSettingsResult>(m_unit);
    }
}
