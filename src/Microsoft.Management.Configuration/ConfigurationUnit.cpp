// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "pch.h"
#include "ConfigurationUnit.h"
#include "ConfigurationUnit.g.cpp"
#include "ConfigurationSetParser.h"

namespace winrt::Microsoft::Management::Configuration::implementation
{
    namespace
    {
        using ValueSet = Windows::Foundation::Collections::ValueSet;

        ValueSet Clone(const ValueSet& source)
        {
            ValueSet result;

            for (const auto& entry : source)
            {
                ValueSet child = entry.Value().try_as<ValueSet>();

                if (child)
                {
                    result.Insert(entry.Key(), Clone(child));
                }
                else
                {
                    result.Insert(entry.Key(), entry.Value());
                }
            }

            return result;
        }
    }

    ConfigurationUnit::ConfigurationUnit()
    {
        GUID instanceIdentifier;
        THROW_IF_FAILED(CoCreateGuid(&instanceIdentifier));
        m_instanceIdentifier = instanceIdentifier;
    }

    ConfigurationUnit::ConfigurationUnit(const guid& instanceIdentifier) :
        m_instanceIdentifier(instanceIdentifier)
    {
    }

    hstring ConfigurationUnit::Type()
    {
        return m_type;
    }

    void ConfigurationUnit::Type(const hstring& value)
    {
        m_type = value;
    }

    guid ConfigurationUnit::InstanceIdentifier()
    {
        return m_instanceIdentifier;
    }

    hstring ConfigurationUnit::Identifier()
    {
        return m_identifier;
    }

    void ConfigurationUnit::Identifier(const hstring& value)
    {
        m_identifier = value;
    }

    Windows::Foundation::Collections::IVector<hstring> ConfigurationUnit::Dependencies()
    {
        return m_dependencies;
    }

    void ConfigurationUnit::Dependencies(const Windows::Foundation::Collections::IVector<hstring>& value)
    {
        std::vector<hstring> temp{ value.Size() };
        value.GetMany(0, temp);
        m_dependencies = winrt::single_threaded_vector<hstring>(std::move(temp));
    }

    void ConfigurationUnit::Dependencies(std::vector<hstring>&& value)
    {
        m_dependencies = winrt::single_threaded_vector<hstring>(std::move(value));
    }

    Windows::Foundation::Collections::ValueSet ConfigurationUnit::Metadata()
    {
        return m_metadata;
    }

    bool ConfigurationUnit::IsGroup()
    {
        return m_isGroup;
    }

    void ConfigurationUnit::IsGroup(bool value)
    {
        m_isGroup = value;

        if (value)
        {
            if (!m_units)
            {
                m_units = winrt::single_threaded_vector<Configuration::ConfigurationUnit>();
            }
        }
        else
        {
            m_units = nullptr;
        }
    }

    Windows::Foundation::Collections::ValueSet ConfigurationUnit::Settings()
    {
        return m_settings;
    }

    Windows::Foundation::Collections::IVector<Configuration::ConfigurationUnit> ConfigurationUnit::Units()
    {
        return m_units;
    }

    void ConfigurationUnit::Units(const Windows::Foundation::Collections::IVector<Configuration::ConfigurationUnit>& value)
    {
        std::vector<Configuration::ConfigurationUnit> temp{ value.Size() };
        value.GetMany(0, temp);
        m_units = winrt::single_threaded_vector<Configuration::ConfigurationUnit>(std::move(temp));
    }

    void ConfigurationUnit::Units(std::vector<Configuration::ConfigurationUnit>&& value)
    {
        m_units = winrt::single_threaded_vector<Configuration::ConfigurationUnit>(std::move(value));
    }

    IConfigurationUnitProcessorDetails ConfigurationUnit::Details()
    {
        return m_details;
    }

    void ConfigurationUnit::Details(IConfigurationUnitProcessorDetails&& details)
    {
        m_details = std::move(details);
    }

    ConfigurationUnitState ConfigurationUnit::State()
    {
        return ConfigurationUnitState::Unknown;
    }

    IConfigurationUnitResultInformation ConfigurationUnit::ResultInformation()
    {
        return nullptr;
    }

    bool ConfigurationUnit::IsActive()
    {
        return m_isActive;
    }

    void ConfigurationUnit::IsActive(bool value)
    {
        m_isActive = value;
    }

    HRESULT STDMETHODCALLTYPE ConfigurationUnit::SetLifetimeWatcher(IUnknown* watcher)
    {
        return AppInstaller::WinRT::LifetimeWatcherBase::SetLifetimeWatcher(watcher);
    }

    Configuration::ConfigurationUnit ConfigurationUnit::Copy()
    {
        auto result = make_self<wil::details::module_count_wrapper<ConfigurationUnit>>();

        result->m_type = m_type;
        result->Dependencies(m_dependencies);
        result->m_metadata = Clone(m_metadata);
        result->m_isGroup = m_isGroup;
        result->m_settings = Clone(m_settings);
        if (m_units)
        {
            result->Units(m_units);
        }
        result->m_details = m_details;

        return *result;
    }
}
