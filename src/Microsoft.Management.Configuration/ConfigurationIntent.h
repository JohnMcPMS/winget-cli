// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once

namespace winrt::Microsoft::Management::Configuration::implementation
{
    // The intent being applied to a configuration.
    enum class ConfigurationIntent
    {
        // The configuration will only be used to Test the current system state.
        Assert,
        // The configuration will only be used to Get the current system state.
        Inform,
        // The configuration will be used to Apply the current system state.
        // The configuration will be used to Test (and possibly Get) the current system state as part of that process.
        Apply,
    };
}
