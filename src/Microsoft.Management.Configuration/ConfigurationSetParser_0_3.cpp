// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "pch.h"
#include "ConfigurationSetParser_0_3.h"
#include "ParsingMacros.h"

#include <AppInstallerErrors.h>
#include <AppInstallerStrings.h>

#include <sstream>

namespace winrt::Microsoft::Management::Configuration::implementation
{
    using namespace AppInstaller::YAML;

    namespace
    {
    }

    void ConfigurationSetParser_0_3::Parse()
    {
        auto result = make_self<wil::details::module_count_wrapper<implementation::ConfigurationSet>>();

        CHECK_ERROR(ParseValueSet(m_document, FieldName::Metadata, false, result->Metadata()));
        CHECK_ERROR(ParseParameters(result));
        CHECK_ERROR(ParseVariables(result));

        std::vector<Configuration::ConfigurationUnit> units;
        CHECK_ERROR(ParseConfigurationUnitsFromField(m_document, FieldName::Resources, units));
        result->Initialize(std::move(units));

        result->SchemaVersion(GetSchemaVersion());
        m_configurationSet = std::move(result);
    }

    hstring ConfigurationSetParser_0_3::GetSchemaVersion()
    {
        static hstring s_schemaVersion{ L"0.3" };
        return s_schemaVersion;
    }

    void ConfigurationSetParser_0_3::ParseParameters(ConfigurationSetParser::ConfigurationSetPtr& set)
    {
        std::vector<Configuration::ConfigurationParameter> parameters;

        ParseSequence(m_document, FieldName::Parameters, false, Node::Type::Mapping, [&](const Node& item)
            {
                auto parameter = make_self<wil::details::module_count_wrapper<ConfigurationParameter>>();
                ParseParameter(parameter.get(), item);
                parameters.emplace_back(*parameter);
            });

        set->Parameters(std::move(parameters));
    }

    void ConfigurationSetParser_0_3::ParseParameter(ConfigurationParameter* unit, const AppInstaller::YAML::Node& node)
    {

    }

    void ConfigurationSetParser_0_3::ParseVariables(ConfigurationSetParser::ConfigurationSetPtr& set)
    {
        std::vector<Configuration::ConfigurationVariable> variables;

        ParseSequence(m_document, FieldName::Variables, false, Node::Type::Mapping, [&](const Node& item)
            {
                auto variable = make_self<wil::details::module_count_wrapper<ConfigurationVariable>>();
                ParseVariable(variable.get(), item);
                variables.emplace_back(*variable);
            });

        set->Variables(std::move(variables));
    }

    void ConfigurationSetParser_0_3::ParseVariable(ConfigurationVariable* unit, const AppInstaller::YAML::Node& node)
    {

    }

    void ConfigurationSetParser_0_3::ParseConfigurationUnitsFromField(const Node& document, FieldName field, std::vector<Configuration::ConfigurationUnit>& result)
    {
        ParseSequence(document, field, false, Node::Type::Mapping, [&](const Node& item)
            {
                auto configurationUnit = make_self<wil::details::module_count_wrapper<ConfigurationUnit>>();
                ParseConfigurationUnit(configurationUnit.get(), item);
                result.emplace_back(*configurationUnit);
            });
    }

    void ConfigurationSetParser_0_3::ParseConfigurationUnit(ConfigurationUnit* unit, const Node& unitNode)
    {
        CHECK_ERROR(GetStringValueForUnit(unitNode, GetFieldName(FieldName::Resource), true, unit, &ConfigurationUnit::Type));
        CHECK_ERROR(GetStringValueForUnit(unitNode, "id", false, unit, &ConfigurationUnit::Identifier));
        CHECK_ERROR(GetStringArrayForUnit(unitNode, "dependsOn", unit, &ConfigurationUnit::Dependencies));
        CHECK_ERROR(GetValueSet(unitNode, "directives", false, unit->Metadata()));
        CHECK_ERROR(GetValueSet(unitNode, "settings", false, unit->Settings()));
    }
}
