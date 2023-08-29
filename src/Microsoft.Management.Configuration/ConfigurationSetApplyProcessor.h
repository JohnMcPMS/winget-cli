// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "ConfigurationSet.h"
#include "ConfigurationUnit.h"
#include "ApplyConfigurationSetResult.h"
#include "ApplyConfigurationUnitResult.h"
#include "ConfigurationUnitResultInformation.h"
#include "Telemetry/Telemetry.h"
#include <winget/AsyncTokens.h>

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace winrt::Microsoft::Management::Configuration::implementation
{
    namespace details
    {
        // Separate out the unit info portions to make the processing logic less cluttered.
        struct UnitInfoBase
        {
            using ConfigurationSet = Configuration::ConfigurationSet;
            using ConfigurationUnit = Configuration::ConfigurationUnit;

        protected:
            UnitInfoBase(const ConfigurationSet& configurationSet);

            // Contains all of the relevant data for a configuration unit.
            struct UnitInfo
            {
                UnitInfo(ConfigurationUnit&& unit);

                ConfigurationUnit Unit;
                std::vector<std::reference_wrapper<UnitInfo>> DependencyUnitInfos;
                decltype(make_self<wil::details::module_count_wrapper<ApplyConfigurationUnitResult>>()) Result;
                decltype(make_self<wil::details::module_count_wrapper<ConfigurationUnitResultInformation>>()) ResultInformation;
                bool PreProcessed = false;
                bool Processed = false;
                ConfigurationIntent LastActionIntent = ConfigurationIntent::Assert;
            };

            // Gets all of the unit results.
            std::vector<Configuration::ApplyConfigurationUnitResult> GetUnitResults();

            // Gets a count of UnitInfos with the identifier.
            size_t Count(const hstring& identifier);

            // Gets a UnitInfo by the unit identifier.
            std::optional<std::reference_wrapper<UnitInfo>> TryGetUnitInfo(const hstring& identifier);

            // Gets a UnitInfo by the unit instance identifier.
            UnitInfo& GetUnitInfo(const guid& instanceIdentifier);

            // Gets all UnitInfos.
            std::vector<UnitInfo>& GetAllUnitInfos();

            // Gets all UnitInfos.
            const std::vector<UnitInfo>& GetAllUnitInfos() const;

            // Gets all of the UnitInfos for units of the original configuration set.
            std::vector<std::reference_wrapper<UnitInfo>> GetAllTopLevelUnitInfoReferences();

        private:
            // Adds the set of units to our collections.
            void AddUnitInfos(const Windows::Foundation::Collections::IVector<ConfigurationUnit>& units, bool isTopLevel = false);

            // Adds the given unit to the maps.
            void AddToMaps(const UnitInfo& info, size_t index);

            // Gets a UnitInfo by index.
            UnitInfo& GetUnitInfo(size_t index);

            std::vector<UnitInfo> m_unitInfo;
            std::vector<size_t> m_topLevelUnitInfos;
            std::multimap<std::string, size_t> m_idToUnitInfoIndex;
            std::map<winrt::guid, size_t> m_instanceIdToUnitInfoIndex;
        };
    }

    // A helper to better organize the configuration set Apply.
    struct ConfigurationSetApplyProcessor : protected details::UnitInfoBase
    {
        using ApplyConfigurationSetResult = Configuration::ApplyConfigurationSetResult;
        using ConfigurationSet = Configuration::ConfigurationSet;
        using ConfigurationUnit = Configuration::ConfigurationUnit;
        using ConfigurationSetChangeData = Configuration::ConfigurationSetChangeData;

        using result_type = decltype(make_self<wil::details::module_count_wrapper<implementation::ApplyConfigurationSetResult>>());

        ConfigurationSetApplyProcessor(const ConfigurationSet& configurationSet, const TelemetryTraceLogger& telemetry, IConfigurationSetProcessor&& setProcessor, AppInstaller::WinRT::AsyncProgress<ApplyConfigurationSetResult, ConfigurationSetChangeData>&& progress);

        // Processes the apply for the configuration set.
        void Process(bool preProcessOnly = false);

        // Gets the result object.
        ApplyConfigurationSetResult Result() const;

    private:
        // Builds out some data used during processing and validates the set along the way.
        bool PreProcess();

        // Checks the dependency; returns true to indicate that the dependency is satisfied, false if not.
        using CheckDependencyPtr = bool (*)(const UnitInfo&);

        // Processes the unit; returns true if successful, false if not.
        using ProcessUnitPtr = bool (ConfigurationSetApplyProcessor::*)(UnitInfo&);

        // Runs the processing using the given functions.
        bool ProcessInternal(CheckDependencyPtr checkDependencyFunction, ProcessUnitPtr processUnitFunction, bool sendProgress = false);

        // Determines if the given unit has the given intent and all of its dependencies are satisfied
        bool HasSatisfiedDependencies(
            const UnitInfo& unitInfo,
            CheckDependencyPtr checkDependencyFunction) const;

        // Checks a dependency for preprocessing.
        static bool HasPreprocessed(const UnitInfo& unitInfo);

        // Marks a unit as preprocessed.
        bool MarkPreprocessed(UnitInfo& unitInfo);

        // Checks a dependency for having processed successfully.
        static bool HasProcessedSuccessfully(const UnitInfo& unitInfo);

        // Processes a configuration unit.
        bool ProcessUnit(UnitInfo& unitInfo);

        // Processes a configuration unit.
        bool ProcessUnit(UnitInfo& unitInfo, IConfigurationUnitProcessor& processor);

        // QI or create a group processor for the given unit processor.
        IConfigurationGroupProcessor CreateGroupProcessor(const ConfigurationUnit& unit, IConfigurationUnitProcessor& processor);

        // Processes a configuration group.
        bool ProcessGroup(UnitInfo& unitInfo, IConfigurationGroupProcessor& processor);

        // Records the relevant unit results from the group test operation.
        void RecordUnitResults(const Windows::Foundation::Collections::IVector<ITestSettingsResult>& results);

        // Records the unit result from the group apply operation.
        void RecordUnitResult(const IApplySettingsResult& result);

        // Records any missing unit results from the group apply operation.
        void RecordUnitResults(const Windows::Foundation::Collections::IVector<IApplySettingsResult>& results);

        // Records any uprocessed skipped units.
        void RecordSkippedUnits(const Windows::Foundation::Collections::IVector<ConfigurationUnit>& units);

        // Sends progress
        // TODO: Eventually these functions/call sites will be used for history
        void SendProgress(ConfigurationSetState state);
        void SendProgress(ConfigurationUnitState state, const UnitInfo& unitInfo);
        void SendProgressIfNotComplete(ConfigurationUnitState state, const UnitInfo& unitInfo);

        // For exception telemetry, get our internal status
        TelemetryTraceLogger::ProcessingSummaryForIntent GetProcessingSummaryFor(ConfigurationIntent intent) const;

        ConfigurationSet m_configurationSet;
        IConfigurationSetProcessor m_setProcessor;
        const TelemetryTraceLogger& m_telemetry;
        AppInstaller::WinRT::AsyncProgress<ApplyConfigurationSetResult, ConfigurationSetChangeData> m_progress;
        result_type m_result;
        hresult m_resultCode;
    };
}
