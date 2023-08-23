// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "ConfigurationSetParser.h"
#include "ConfigurationIntent.h"
#include <ConfigurationParameter.h>
#include <ConfigurationVariable.h>

#include <winget/Yaml.h>

namespace winrt::Microsoft::Management::Configuration::implementation
{
    // Parser for schema version 0.3
    struct ConfigurationSetParser_0_3 : public ConfigurationSetParser
    {
        ConfigurationSetParser_0_3(AppInstaller::YAML::Node&& document) : m_document(std::move(document)) {}

        virtual ~ConfigurationSetParser_0_3() noexcept = default;

        ConfigurationSetParser_0_3(const ConfigurationSetParser_0_3&) = delete;
        ConfigurationSetParser_0_3& operator=(const ConfigurationSetParser_0_3&) = delete;
        ConfigurationSetParser_0_3(ConfigurationSetParser_0_3&&) = default;
        ConfigurationSetParser_0_3& operator=(ConfigurationSetParser_0_3&&) = default;

        // Retrieve the configuration units from the parser.
        void Parse() override;

        // Retrieves the schema version of the parser.
        hstring GetSchemaVersion() override;

    protected:
        void ParseParameters(ConfigurationSetParser::ConfigurationSetPtr& set);
        void ParseParameter(ConfigurationParameter* unit, const AppInstaller::YAML::Node& node);

        void ParseVariables(ConfigurationSetParser::ConfigurationSetPtr& set);
        void ParseVariable(ConfigurationVariable* unit, const AppInstaller::YAML::Node& node);

        void ParseConfigurationUnitsFromField(const AppInstaller::YAML::Node& document, FieldName field, std::vector<Configuration::ConfigurationUnit>& result);
        virtual void ParseConfigurationUnit(ConfigurationUnit* unit, const AppInstaller::YAML::Node& unitNode);

        AppInstaller::YAML::Node m_document;
    };
}
