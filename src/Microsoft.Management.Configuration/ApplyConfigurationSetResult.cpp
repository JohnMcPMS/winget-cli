// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "pch.h"
#include "ApplyConfigurationSetResult.h"
#include "ApplyConfigurationSetResult.g.cpp"

namespace winrt::Microsoft::Management::Configuration::implementation
{
    ApplyConfigurationSetResult::ApplyConfigurationSetResult() :
        m_unitResults(single_threaded_vector<ApplyConfigurationUnitResult>())
    {}

    Windows::Foundation::Collections::IVectorView<ApplyConfigurationUnitResult> ApplyConfigurationSetResult::UnitResults() const
    {
        return m_unitResults.GetView();
    }

    void ApplyConfigurationSetResult::UnitResults(std::vector<ApplyConfigurationUnitResult>&& value)
    {
        m_unitResults = single_threaded_vector<ApplyConfigurationUnitResult>(std::move(value));
    }

    hresult ApplyConfigurationSetResult::ResultCode() const
    {
        return m_resultCode;
    }

    void ApplyConfigurationSetResult::ResultCode(hresult value)
    {
        m_resultCode = value;
    }
}
