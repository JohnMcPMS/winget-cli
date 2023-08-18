// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "ConfigurationUnit.g.h"
#include <winget/ILifetimeWatcher.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <vector>

namespace winrt::Microsoft::Management::Configuration::implementation
{
    struct ConfigurationUnit : ConfigurationUnitT<ConfigurationUnit, winrt::cloaked<AppInstaller::WinRT::ILifetimeWatcher>>, AppInstaller::WinRT::LifetimeWatcherBase
    {
        ConfigurationUnit();

#if !defined(INCLUDE_ONLY_INTERFACE_METHODS)
        ConfigurationUnit(const guid& instanceIdentifier);
#endif

        hstring Type();
        void Type(const hstring& value);

        guid InstanceIdentifier();

        hstring Identifier();
        void Identifier(const hstring& value);

        Windows::Foundation::Collections::IVector<hstring> Dependencies();

        Windows::Foundation::Collections::ValueSet Metadata();

        bool IsGroup();
        void IsGroup(bool value);

        Windows::Foundation::Collections::ValueSet Settings();

        Windows::Foundation::Collections::IVector<Configuration::ConfigurationUnit> Units();

        IConfigurationUnitProcessorDetails Details();

        ConfigurationUnitState State();

        IConfigurationUnitResultInformation ResultInformation();

        bool IsActive();
        void IsActive(bool value);

        Configuration::ConfigurationUnit Copy();

        HRESULT STDMETHODCALLTYPE SetLifetimeWatcher(IUnknown* watcher);

#if !defined(INCLUDE_ONLY_INTERFACE_METHODS)
        void Dependencies(const Windows::Foundation::Collections::IVector<hstring>& value);
        void Dependencies(std::vector<hstring>&& value);
        void Units(const Windows::Foundation::Collections::IVector<Configuration::ConfigurationUnit>& value);
        void Units(std::vector<Configuration::ConfigurationUnit>&& value);
        void Details(IConfigurationUnitProcessorDetails&& details);

    private:
        hstring m_type;
        guid m_instanceIdentifier;
        hstring m_identifier;
        Windows::Foundation::Collections::IVector<hstring> m_dependencies{ winrt::single_threaded_vector<hstring>() };
        Windows::Foundation::Collections::ValueSet m_metadata;
        bool m_isGroup = false;
        Windows::Foundation::Collections::ValueSet m_settings;
        Windows::Foundation::Collections::IVector<Configuration::ConfigurationUnit> m_units = nullptr;
        IConfigurationUnitProcessorDetails m_details{ nullptr };
        bool m_isActive = true;
#endif
    };
}

#if !defined(INCLUDE_ONLY_INTERFACE_METHODS)
namespace winrt::Microsoft::Management::Configuration::factory_implementation
{
    struct ConfigurationUnit : ConfigurationUnitT<ConfigurationUnit, implementation::ConfigurationUnit>
    {
    };
}
#endif