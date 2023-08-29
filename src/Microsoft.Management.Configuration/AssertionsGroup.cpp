// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "pch.h"
#include "AssertionsGroup.h"

namespace winrt::Microsoft::Management::Configuration::implementation
{
    AssertionsGroup::AssertionsGroup(const Configuration::ConfigurationUnit& unit) :
        m_unit(unit)
    {
    }

    std::wstring_view AssertionsGroup::Type()
    {
        return L"Microsoft.WinGet.Configuration/AssertionsGroup";
    }

    Windows::Foundation::IInspectable AssertionsGroup::Group()
    {
        return m_unit;
    }

    Windows::Foundation::IAsyncOperationWithProgress<ITestGroupSettingsResult, ITestSettingsResult> AssertionsGroup::TestGroupSettingsAsync()
    {
        auto strong_self = get_strong();
        auto progress = co_await get_progress_token();
        co_await resume_background();

        auto result = make_self<wil::details::module_count_wrapper<implementation::TestGroupSettingsResult>>(m_unit);
        progress.Result(*result);

        for (const ConfigurationUnit& unit : m_unit.Units())
        {

        }
    }

    Windows::Foundation::IAsyncOperationWithProgress<IGetGroupSettingsResult, IGetSettingsResult> AssertionsGroup::GetGroupSettingsAsync()
    {

    }

    Windows::Foundation::IAsyncOperationWithProgress<IApplyGroupSettingsResult, IApplySettingsResult> AssertionsGroup::ApplyGroupSettingsAsync()
    {

    }
}
