// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "pch.h"
#include "ConfigurationVariable.h"
#include "ConfigurationVariable.g.cpp"
#include "ArgumentValidation.h"

namespace winrt::Microsoft::Management::Configuration::implementation
{
    namespace
    {
        // Ensures that the value object is an appropriate type.
        void EnsureObjectType(Windows::Foundation::IInspectable const& value)
        {
            // It must be a ValueSet or an IPropertyValue
            if (value.try_as<Windows::Foundation::Collections::ValueSet>())
            {
                return;
            }

            // If it wasn't a ValueSet, it must be an IPropertyValue
            auto propertyValue = value.try_as<Windows::Foundation::IPropertyValue>();
            THROW_HR_IF(E_INVALIDARG, !propertyValue);

            // If it is an IPropertyValue, it must have a supported type
            EnsureSupportedType(propertyValue.Type());
        }
    }

    hstring ConfigurationVariable::Name()
    {
        return m_name;
    }

    void ConfigurationVariable::Name(hstring const& value)
    {
        m_name = value;
    }

    winrt::Windows::Foundation::IInspectable ConfigurationVariable::Value()
    {
        return m_value;
    }

    void ConfigurationVariable::Value(winrt::Windows::Foundation::IInspectable const& value)
    {
        if (value)
        {
            EnsureObjectType(value);
        }

        m_value = value;
    }

    HRESULT STDMETHODCALLTYPE ConfigurationVariable::SetLifetimeWatcher(IUnknown* watcher)
    {
        return AppInstaller::WinRT::LifetimeWatcherBase::SetLifetimeWatcher(watcher);
    }
}
