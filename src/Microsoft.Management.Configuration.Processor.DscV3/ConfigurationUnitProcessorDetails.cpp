// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "pch.h"
#include "ConfigurationUnitProcessorDetails.h"

namespace winrt::Microsoft::Management::Configuration::Processor::DscV3::implementation
{
    using namespace winrt::Microsoft::Management::Configuration;
    using namespace winrt::Windows::Foundation;
    using namespace winrt::Windows::Foundation::Collections;

    ConfigurationUnitProcessorDetails::ConfigurationUnitProcessorDetails(const ConfigurationUnit& unit, ConfigurationUnitDetailFlags /*detailFlags*/)
        : m_unit(unit) {}

    winrt::hstring ConfigurationUnitProcessorDetails::UnitType() const
    {
        return m_unitType;
    }

    winrt::hstring ConfigurationUnitProcessorDetails::UnitDescription() const {
        return m_unitDescription;
    }

    Uri ConfigurationUnitProcessorDetails::UnitDocumentationUri() const {
        return m_unitDocumentationUri;
    }

    Uri ConfigurationUnitProcessorDetails::UnitIconUri() const {
        return m_unitDocumentationUri;
    }

    winrt::hstring ConfigurationUnitProcessorDetails::ModuleName() const {
        return m_moduleName;
    }

    winrt::hstring ConfigurationUnitProcessorDetails::ModuleType() const {
        return m_moduleType;
    }

    winrt::hstring ConfigurationUnitProcessorDetails::ModuleSource() const {
        return m_moduleSource;
    }

    winrt::hstring ConfigurationUnitProcessorDetails::ModuleDescription() const {
        return m_moduleDescription;
    }

    Uri ConfigurationUnitProcessorDetails::ModuleDocumentationUri() const {
        return m_moduleDocumentationUri;
    }

    Uri ConfigurationUnitProcessorDetails::PublishedModuleUri() const {
        return m_publishedModuleUri;
    }

    winrt::hstring ConfigurationUnitProcessorDetails::Version() const {
        return m_version;
    }

    DateTime ConfigurationUnitProcessorDetails::PublishedDate() const {
        return m_publishedDate;
    }

    bool ConfigurationUnitProcessorDetails::IsLocal() const {
        return m_isLocal;
    }

    winrt::hstring ConfigurationUnitProcessorDetails::Author() const {
        return m_author;
    }

    winrt::hstring ConfigurationUnitProcessorDetails::Publisher() const {
        return m_publisher;
    }

    IVectorView<IInspectable> ConfigurationUnitProcessorDetails::SigningInformation() const
    {
        return m_signingInformation.GetView();
    }

    IVectorView<IConfigurationUnitSettingDetails> ConfigurationUnitProcessorDetails::Settings() const
    {
        return (m_settings ? m_settings.GetView() : nullptr);
    }

    bool ConfigurationUnitProcessorDetails::IsPublic() const
    {
        return m_isPublic;
    }

    bool ConfigurationUnitProcessorDetails::IsGroup() const
    {
        return m_isGroup;
    }
}
