// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "pch.h"
#include "ConfigurationUnitSettingDetails.h"

namespace winrt::Microsoft::Management::Configuration::Processor::DscV3::implementation
{
    using namespace winrt::Microsoft::Management::Configuration;

    winrt::hstring ConfigurationUnitSettingDetails::Identifier() const
    {
        return m_identifier;
    }

    void ConfigurationUnitSettingDetails::Identifier(const winrt::hstring& identifier)
    {
        m_identifier = identifier;
    }

    winrt::hstring ConfigurationUnitSettingDetails::Title() const
    {
        return m_title;
    }

    void ConfigurationUnitSettingDetails::Title(const winrt::hstring& title)
    {
        m_title = title;
    }

    winrt::hstring ConfigurationUnitSettingDetails::Description() const
    {
        return m_description;
    }

    void ConfigurationUnitSettingDetails::Description(const winrt::hstring& description)
    {
        m_description = description;
    }

    bool ConfigurationUnitSettingDetails::IsKey() const
    {
        return m_isKey;
    }

    void ConfigurationUnitSettingDetails::IsKey(bool isKey)
    {
        m_isKey = isKey;
    }

    bool ConfigurationUnitSettingDetails::IsInformational() const
    {
        return m_isInformational;
    }

    void ConfigurationUnitSettingDetails::IsInformational(bool isInformational)
    {
        m_isInformational = isInformational;
    }

    bool ConfigurationUnitSettingDetails::IsRequired() const
    {
        return m_isRequired;
    }

    void ConfigurationUnitSettingDetails::IsRequired(bool isRequired)
    {
        m_isRequired = isRequired;
    }

    Windows::Foundation::PropertyType ConfigurationUnitSettingDetails::Type()
    {
        return m_type;
    }

    void ConfigurationUnitSettingDetails::Type(Windows::Foundation::PropertyType type)
    {
        m_type = type;
    }

    winrt::hstring ConfigurationUnitSettingDetails::Schema() const
    {
        return m_schema;
    }

    void ConfigurationUnitSettingDetails::Schema(const winrt::hstring& schema)
    {
        m_schema = schema;
    }
}
