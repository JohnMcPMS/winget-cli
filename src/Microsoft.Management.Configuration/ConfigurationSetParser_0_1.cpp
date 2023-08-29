// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "pch.h"
#include "ConfigurationSetParser_0_1.h"
#include "ParsingMacros.h"
#include "AssertionsGroup.h"

#include <AppInstallerErrors.h>
#include <AppInstallerStrings.h>

#include <sstream>

namespace winrt::Microsoft::Management::Configuration::implementation
{
    using namespace AppInstaller::YAML;

    namespace
    {
        bool AssignGroupIdentifierIfNotPresent(const std::vector<Configuration::ConfigurationUnit>& units, std::wstring_view identifier)
        {
            for (size_t i = 1; i < units.size(); ++i)
            {
                if (units[i].Identifier() == identifier)
                {
                    return false;
                }
            }

            units[0].Identifier(identifier);
            return true;
        }

        hstring NewGuid()
        {
            GUID newGuid;
            THROW_IF_FAILED(CoCreateGuid(&newGuid));

            wchar_t buffer[256];

            if (StringFromGUID2(newGuid, buffer, ARRAYSIZE(buffer)))
            {
                return buffer;
            }
            else
            {
                return L"StringFromGUID2/Error";
            }
        }
    }

    void ConfigurationSetParser_0_1::Parse()
    {
        std::vector<Configuration::ConfigurationUnit> units;
        const Node& properties = m_document[GetFieldName(FieldName::Properties)];

        // Get all assertions and place them into a group
        std::vector<Configuration::ConfigurationUnit> assertions;
        bool hasAssertions = false;
        CHECK_ERROR(ParseConfigurationUnitsFromField(properties, FieldName::Assertions, assertions));

        if (!assertions.empty())
        {
            hasAssertions = true;
            auto assertionsGroup = make_self<wil::details::module_count_wrapper<ConfigurationUnit>>();
            // This is a sentinel type name for a group that works like the previous assertions.
            assertionsGroup->Type(hstring{ AssertionsGroup::Type() });
            assertionsGroup->IsGroup(true);
            assertionsGroup->Units(std::move(assertions));
            units.emplace_back(*assertionsGroup);
        }

        // Since we never really supported the `parameters` resources, just don't bother with them in the back compat mode

        CHECK_ERROR(ParseConfigurationUnitsFromField(properties, FieldName::Resources, units));

        // Ensure that the assertions group has a unique identifier
        if (hasAssertions)
        {
            // Determine the identifier for the assertions group
            if (!AssignGroupIdentifierIfNotPresent(units, L"assertions"))
            {
                if (!AssignGroupIdentifierIfNotPresent(units, L"assertionsGroup"))
                {
                    units[0].Identifier(NewGuid());
                }
            }

            // Make everything else dependent on the assertions group
            hstring assertionsGroupIdentifier = units[0].Identifier();
            for (size_t i = 1; i < units.size(); ++i)
            {
                units[i].Dependencies().Append(assertionsGroupIdentifier);
            }
        }

        m_configurationSet = make_self<wil::details::module_count_wrapper<implementation::ConfigurationSet>>();
        m_configurationSet->Units(std::move(units));
        m_configurationSet->SchemaVersion(GetSchemaVersion());
    }

    hstring ConfigurationSetParser_0_1::GetSchemaVersion()
    {
        static hstring s_schemaVersion{ L"0.1" };
        return s_schemaVersion;
    }

    void ConfigurationSetParser_0_1::ParseConfigurationUnitsFromField(const Node& document, FieldName field, std::vector<Configuration::ConfigurationUnit>& result)
    {
        ParseSequence(document, field, false, Node::Type::Mapping, [&](const Node& item)
            {
                auto configurationUnit = make_self<wil::details::module_count_wrapper<ConfigurationUnit>>();
                ParseConfigurationUnit(configurationUnit.get(), item);
                result.emplace_back(*configurationUnit);
            });
    }

    void ConfigurationSetParser_0_1::ParseConfigurationUnit(ConfigurationUnit* unit, const Node& unitNode)
    {
        CHECK_ERROR(GetStringValueForUnit(unitNode, FieldName::Resource, true, unit, &ConfigurationUnit::Type));
        CHECK_ERROR(GetStringValueForUnit(unitNode, FieldName::Id, false, unit, &ConfigurationUnit::Identifier));
        CHECK_ERROR(GetStringArrayForUnit(unitNode, FieldName::DependsOn, false, unit, &ConfigurationUnit::Dependencies));
        CHECK_ERROR(ParseValueSet(unitNode, FieldName::Directives, false, unit->Metadata()));
        CHECK_ERROR(ParseValueSet(unitNode, FieldName::Settings, false, unit->Settings()));
    }
}
