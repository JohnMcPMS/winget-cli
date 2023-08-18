// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "ConfigurationVariable.g.h"
#include <winget/ILifetimeWatcher.h>
#include <winrt/Windows.Foundation.h>

namespace winrt::Microsoft::Management::Configuration::implementation
{
    struct ConfigurationVariable : ConfigurationVariableT<ConfigurationVariable, winrt::cloaked<AppInstaller::WinRT::ILifetimeWatcher>>, AppInstaller::WinRT::LifetimeWatcherBase
    {
        ConfigurationVariable() = default;

        hstring Name();
        void Name(hstring const& value);

        Windows::Foundation::IInspectable Value();
        void Value(Windows::Foundation::IInspectable const& value);

        HRESULT STDMETHODCALLTYPE SetLifetimeWatcher(IUnknown* watcher);

#if !defined(INCLUDE_ONLY_INTERFACE_METHODS)
    private:
        hstring m_name;
        Windows::Foundation::IInspectable m_value;
#endif
    };
}

#if !defined(INCLUDE_ONLY_INTERFACE_METHODS)
namespace winrt::Microsoft::Management::Configuration::factory_implementation
{
    struct ConfigurationVariable : ConfigurationVariableT<ConfigurationVariable, implementation::ConfigurationVariable>
    {
    };
}
#endif
