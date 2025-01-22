// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "pch.h"
#include "DscV3ConfigurationSetProcessorFactory.h"
#include "DscV3ConfigurationSetProcessorFactory.g.cpp"
#include "ConfigurationSetProcessor.h"

namespace winrt::Microsoft::Management::Configuration::Processor::DscV3::implementation
{
    using namespace winrt::Microsoft::Management::Configuration;
    using namespace winrt::Windows::Foundation;

    IConfigurationSetProcessor DscV3ConfigurationSetProcessorFactory::CreateSetProcessor(ConfigurationSet const& configurationSet)
    {
        return winrt::make<ConfigurationSetProcessor>(configurationSet, get_weak());
    }

    winrt::event_token DscV3ConfigurationSetProcessorFactory::Diagnostics(EventHandler<IDiagnosticInformation> const& handler)
    {
        return m_diagnostics.add(handler);
    }

    void DscV3ConfigurationSetProcessorFactory::Diagnostics(winrt::event_token const& token) noexcept
    {
        m_diagnostics.remove(token);
    }

    DiagnosticLevel DscV3ConfigurationSetProcessorFactory::MinimumLevel()
    {
        return m_minimumLevel;
    }

    void DscV3ConfigurationSetProcessorFactory::MinimumLevel(winrt::Microsoft::Management::Configuration::DiagnosticLevel const& minimumLevel)
    {
        m_minimumLevel = minimumLevel;
    }
}
