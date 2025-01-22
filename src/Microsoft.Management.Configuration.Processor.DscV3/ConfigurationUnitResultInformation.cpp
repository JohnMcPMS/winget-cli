// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "pch.h"
#include "ConfigurationUnitResultInformation.h"

namespace winrt::Microsoft::Management::Configuration::Processor::DscV3::implementation
{
    using namespace winrt::Microsoft::Management::Configuration;

    winrt::hresult ConfigurationUnitResultInformation::ResultCode() const
    {
        return m_resultCode;
    }

    void ConfigurationUnitResultInformation::ResultCode(winrt::hresult resultCode)
    {
        m_resultCode = resultCode;
    }

    winrt::hstring ConfigurationUnitResultInformation::Description() const
    {
        return m_description;
    }

    void ConfigurationUnitResultInformation::Description(const winrt::hstring& description)
    {
        m_description = description;
    }

    winrt::hstring ConfigurationUnitResultInformation::Details() const
    {
        return m_details;
    }

    void ConfigurationUnitResultInformation::Details(const winrt::hstring& details)
    {
        m_details = details;
    }

    ConfigurationUnitResultSource ConfigurationUnitResultInformation::ResultSource() const
    {
        return m_resultSource;
    }

    void ConfigurationUnitResultInformation::ResultSource(ConfigurationUnitResultSource resultSource)
    {
        m_resultSource = resultSource;
    }
}
