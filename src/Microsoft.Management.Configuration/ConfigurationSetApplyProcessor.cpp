// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "pch.h"
#include "ConfigurationSetApplyProcessor.h"
#include "ConfigurationSetChangeData.h"
#include "ExceptionResultHelpers.h"
#include "AssertionsGroup.h"

#include <AppInstallerErrors.h>
#include <AppInstallerLogging.h>
#include <AppInstallerStrings.h>

namespace winrt::Microsoft::Management::Configuration::implementation
{
    namespace
    {
        std::string GetNormalizedIdentifier(hstring identifier)
        {
            using namespace AppInstaller::Utility;
            return FoldCase(NormalizedString{ identifier });
        }
    }

    namespace details
    {
        UnitInfoBase::UnitInfo::UnitInfo(Configuration::ConfigurationUnit&& unit) :
            Unit(std::move(unit)),
            Result(make_self<wil::details::module_count_wrapper<implementation::ApplyConfigurationUnitResult>>()),
            ResultInformation(make_self<wil::details::module_count_wrapper<implementation::ConfigurationUnitResultInformation>>())
        {
            Result->Unit(Unit);
            Result->ResultInformation(*ResultInformation);
        }

        UnitInfoBase::UnitInfoBase(const ConfigurationSet& configurationSet)
        {
            AddUnitInfos(configurationSet.Units(), true);
        }

        std::vector<Configuration::ApplyConfigurationUnitResult> UnitInfoBase::GetUnitResults()
        {
            std::vector<Configuration::ApplyConfigurationUnitResult> result;

            for (const UnitInfo& info : m_unitInfo)
            {
                result.emplace_back(*info.Result);
            }

            return result;
        }

        size_t UnitInfoBase::Count(const hstring& identifier)
        {
            return m_idToUnitInfoIndex.count(GetNormalizedIdentifier(identifier));
        }

        std::optional<std::reference_wrapper<UnitInfoBase::UnitInfo>> UnitInfoBase::TryGetUnitInfo(const hstring& identifier)
        {
            auto equalRange = m_idToUnitInfoIndex.equal_range(GetNormalizedIdentifier(identifier));

            // No match, return empty optional
            if (equalRange.first == equalRange.second)
            {
                return std::nullopt;
            }

            std::reference_wrapper<UnitInfo> result = m_unitInfo[equalRange.first->second];
            ++equalRange.first;

            // Only a single value in range, return it
            if (equalRange.first == equalRange.second)
            {
                return result;
            }
            else
            {
                // We should always be checking for this first, but if we mess that up just throw it.
                THROW_HR(WINGET_CONFIG_ERROR_DUPLICATE_IDENTIFIER);
            }
        }

        UnitInfoBase::UnitInfo& UnitInfoBase::GetUnitInfo(const guid& instanceIdentifier)
        {
            auto itr = m_instanceIdToUnitInfoIndex.find(instanceIdentifier);
            THROW_HR_IF(E_NOT_SET, itr == m_instanceIdToUnitInfoIndex.end());
            return m_unitInfo[itr->second];
        }

        std::vector<UnitInfoBase::UnitInfo>& UnitInfoBase::GetAllUnitInfos()
        {
            return m_unitInfo;
        }

        const std::vector<UnitInfoBase::UnitInfo>& UnitInfoBase::GetAllUnitInfos() const
        {
            return m_unitInfo;
        }

        std::vector<std::reference_wrapper<UnitInfoBase::UnitInfo>> UnitInfoBase::GetAllTopLevelUnitInfoReferences()
        {
            std::vector<std::reference_wrapper<UnitInfoBase::UnitInfo>> result;

            for (size_t index : m_topLevelUnitInfos)
            {
                result.emplace_back(m_unitInfo[index]);
            }

            return result;
        }

        void UnitInfoBase::AddUnitInfos(const Windows::Foundation::Collections::IVector<ConfigurationUnit>& units, bool isTopLevel)
        {
            // Create a copy of the set of configuration units
            std::vector<ConfigurationUnit> unitsToProcess{ units.Size() };
            units.GetMany(0, unitsToProcess);

            // Create the unit info vector from these units
            for (auto& unit : unitsToProcess)
            {
                if (unit.IsGroup())
                {
                    AddUnitInfos(unit.Units());
                }

                m_unitInfo.emplace_back(std::move(unit));
                size_t index = m_unitInfo.size() - 1;
                AddToMaps(m_unitInfo.back(), index);
                if (isTopLevel)
                {
                    m_topLevelUnitInfos.emplace_back(index);
                }
            }
        }

        void UnitInfoBase::AddToMaps(const UnitInfo& info, size_t index)
        {
            // Add to instance identifier lookup
            m_instanceIdToUnitInfoIndex.emplace(std::move(info.Unit.InstanceIdentifier()), index);

            // Add to identifier lookup
            hstring originalIdentifier = info.Unit.Identifier();
            if (!originalIdentifier.empty())
            {
                m_idToUnitInfoIndex.emplace(GetNormalizedIdentifier(originalIdentifier), index);
            }
        }

        UnitInfoBase::UnitInfo& UnitInfoBase::GetUnitInfo(size_t index)
        {
            return m_unitInfo[index];
        }
    }

    ConfigurationSetApplyProcessor::ConfigurationSetApplyProcessor(
        const Configuration::ConfigurationSet& configurationSet,
        const TelemetryTraceLogger& telemetry,
        IConfigurationSetProcessor&& setProcessor,
        AppInstaller::WinRT::AsyncProgress<ApplyConfigurationSetResult,
        ConfigurationSetChangeData>&& progress) :
            UnitInfoBase(configurationSet),
            m_configurationSet(configurationSet),
            m_setProcessor(std::move(setProcessor)),
            m_telemetry(telemetry),
            m_result(make_self<wil::details::module_count_wrapper<implementation::ApplyConfigurationSetResult>>()),
            m_progress(std::move(progress))
    {
        m_result->UnitResults(GetUnitResults());
        m_progress.Result(*m_result);
    }

    void ConfigurationSetApplyProcessor::Process(bool preProcessOnly)
    {
        try
        {
            if (PreProcess() && !preProcessOnly)
            {
                // TODO: Send pending when blocked by another configuration run
                //SendProgress(ConfigurationSetState::Pending);

                SendProgress(ConfigurationSetState::InProgress);

                ProcessInternal(HasProcessedSuccessfully, &ConfigurationSetApplyProcessor::ProcessUnit, true);
            }

            SendProgress(ConfigurationSetState::Completed);

            if (!preProcessOnly)
            {
                m_telemetry.LogConfigProcessingSummaryForApply(*winrt::get_self<implementation::ConfigurationSet>(m_configurationSet), *m_result);
            }
        }
        catch (...)
        {
            if (!preProcessOnly)
            {
                const auto& configurationSet = *winrt::get_self<implementation::ConfigurationSet>(m_configurationSet);
                m_telemetry.LogConfigProcessingSummary(
                    configurationSet.InstanceIdentifier(),
                    configurationSet.IsFromHistory(),
                    ConfigurationIntent::Apply,
                    LOG_CAUGHT_EXCEPTION(),
                    ConfigurationUnitResultSource::Internal,
                    GetProcessingSummaryFor(ConfigurationIntent::Assert),
                    GetProcessingSummaryFor(ConfigurationIntent::Inform),
                    GetProcessingSummaryFor(ConfigurationIntent::Apply));
            }

            throw;
        }
    }

    Configuration::ApplyConfigurationSetResult ConfigurationSetApplyProcessor::Result() const
    {
        return *m_result;
    }

    bool ConfigurationSetApplyProcessor::PreProcess()
    {
        bool result = true;

        // Error to indicate that parameters and variables are not yet implemented.
        if (m_configurationSet.Parameters().Size() != 0 || m_configurationSet.Variables().Size() != 0)
        {
            m_result->ResultCode(E_NOTIMPL);
            return false;
        }

        // Check for duplicate identifier values
        for (UnitInfo& unitInfo : GetAllUnitInfos())
        {
            hstring identifier = unitInfo.Unit.Identifier();

            if (Count(identifier) > 1)
            {
                AICLI_LOG(Config, Error, << "Found duplicate identifier: " << AppInstaller::Utility::ConvertToUTF8(identifier));
                unitInfo.ResultInformation->Initialize(WINGET_CONFIG_ERROR_DUPLICATE_IDENTIFIER, ConfigurationUnitResultSource::ConfigurationSet);
                SendProgress(ConfigurationUnitState::Completed, unitInfo);
                result = false;
            }
        }

        if (!result)
        {
            m_result->ResultCode(WINGET_CONFIG_ERROR_DUPLICATE_IDENTIFIER);
            return false;
        }

        // Check for missing dependency references
        for (UnitInfo& unitInfo : GetAllUnitInfos())
        {
            for (hstring dependency : unitInfo.Unit.Dependencies())
            {
                // Throw out empty dependency strings
                if (dependency.empty())
                {
                    continue;
                }

                auto dependencyInfo = TryGetUnitInfo(dependency);
                if (!dependencyInfo)
                {
                    AICLI_LOG(Config, Error, << "Found missing dependency: " << AppInstaller::Utility::ConvertToUTF8(dependency));
                    unitInfo.ResultInformation->Initialize(WINGET_CONFIG_ERROR_MISSING_DEPENDENCY, ConfigurationUnitResultSource::ConfigurationSet);
                    unitInfo.ResultInformation->Details(dependency);
                    SendProgress(ConfigurationUnitState::Completed, unitInfo);
                    result = false;
                    // TODO: Consider collecting all missing dependencies, for now just the first
                    break;
                }
                else
                {
                    unitInfo.DependencyUnitInfos.emplace_back(dependencyInfo.value());
                }
            }
        }

        if (!result)
        {
            m_result->ResultCode(WINGET_CONFIG_ERROR_MISSING_DEPENDENCY);
            return false;
        }

        if (!ProcessInternal(HasPreprocessed, &ConfigurationSetApplyProcessor::MarkPreprocessed))
        {
            // The preprocessing simulates processing as if every unit run was successful.
            // If it fails, this means that there are unit definitions whose dependencies cannot be satisfied.
            // The only reason for that is a cycle in the dependency graph somewhere.
            m_result->ResultCode(WINGET_CONFIG_ERROR_SET_DEPENDENCY_CYCLE);
            return false;
        }

        return true;
    }

    bool ConfigurationSetApplyProcessor::ProcessInternal(CheckDependencyPtr checkDependencyFunction, ProcessUnitPtr processUnitFunction, bool sendProgress)
    {
        // Create the set of units that need to be processed
        auto unitsToProcess = GetAllTopLevelUnitInfoReferences();

        // Always process the first item in the list that is available to be processed
        bool hasProcessed = true;
        bool hasFailure = false;
        while (hasProcessed)
        {
            hasProcessed = false;
            for (auto itr = unitsToProcess.begin(), end = unitsToProcess.end(); itr != end; ++itr)
            {
                UnitInfo& unitInfo = *itr;
                if (HasSatisfiedDependencies(unitInfo, checkDependencyFunction))
                {
                    if (!(this->*processUnitFunction)(unitInfo))
                    {
                        hasFailure = true;
                    }
                    unitsToProcess.erase(itr);
                    hasProcessed = true;
                    break;
                }
            }
        }

        // Mark all remaining items with intent as failed due to dependency
        bool hasRemainingDependencies = !unitsToProcess.empty();
        for (UnitInfo& unitInfo : unitsToProcess)
        {
            unitInfo.ResultInformation->Initialize(WINGET_CONFIG_ERROR_DEPENDENCY_UNSATISFIED, ConfigurationUnitResultSource::Precondition);
            if (sendProgress)
            {
                SendProgress(ConfigurationUnitState::Skipped, unitInfo);
            }
        }

        // Any failures are fatal, mark all other units as failed due to that
        if (hasFailure || hasRemainingDependencies)
        {
            if (hasFailure)
            {
                m_result->ResultCode(WINGET_CONFIG_ERROR_SET_APPLY_FAILED);
            }
            else // hasRemainingDependencies
            {
                m_result->ResultCode(WINGET_CONFIG_ERROR_DEPENDENCY_UNSATISFIED);
            }
            return false;
        }

        return true;
    }

    bool ConfigurationSetApplyProcessor::HasSatisfiedDependencies(
        const UnitInfo& unitInfo,
        CheckDependencyPtr checkDependencyFunction) const
    {
        bool result = true;

        for (UnitInfo& dependency : unitInfo.DependencyUnitInfos)
        {
            if (!checkDependencyFunction(dependency))
            {
                result = false;
                break;
            }
        }

        return result;
    }

    bool ConfigurationSetApplyProcessor::HasPreprocessed(const UnitInfo& unitInfo)
    {
        return unitInfo.PreProcessed;
    }

    bool ConfigurationSetApplyProcessor::MarkPreprocessed(UnitInfo& unitInfo)
    {
        unitInfo.PreProcessed = true;
        return true;
    }

    bool ConfigurationSetApplyProcessor::HasProcessedSuccessfully(const UnitInfo& unitInfo)
    {
        return unitInfo.Processed && SUCCEEDED(unitInfo.ResultInformation->ResultCode());
    }

    bool ConfigurationSetApplyProcessor::ProcessUnit(UnitInfo& unitInfo)
    {
        m_progress.ThrowIfCancelled();

        IConfigurationUnitProcessor unitProcessor;

        // Once we get this far, consider the unit processed even if we fail to create the actual processor.
        unitInfo.Processed = true;

        if (!unitInfo.Unit.IsActive())
        {
            // If the unit is requested to be skipped, we mark it with a failure to prevent any dependency from running.
            // But we return true from this function to indicate a successful "processing".
            unitInfo.ResultInformation->Initialize(WINGET_CONFIG_ERROR_MANUALLY_SKIPPED, ConfigurationUnitResultSource::Precondition);
            SendProgress(ConfigurationUnitState::Skipped, unitInfo);
            return true;
        }

        // Send a progress event that we are starting, and prepare one for completion when we exit the function
        SendProgress(ConfigurationUnitState::InProgress, unitInfo);
        auto sendCompletedProgress = wil::scope_exit([this, &unitInfo]() { SendProgress(ConfigurationUnitState::Completed, unitInfo); });

        try
        {
            unitProcessor = m_setProcessor.CreateUnitProcessor(unitInfo.Unit);
        }
        catch (...)
        {
            ExtractUnitResultInformation(std::current_exception(), unitInfo.ResultInformation);
            return false;
        }

        // As the process of creating the unit processor could take a while, check for cancellation again
        m_progress.ThrowIfCancelled();

        // Check for group handling capability for this unit if we think it is a group.
        // If it is a group but we don't think it is one, the only option we would have is to convert it to one
        // and create new configuration units dynamically. Without the child unit objects, there is no reason
        // to process it any differently than a normal unit.
        IConfigurationGroupProcessor groupProcessor = CreateGroupProcessor(unitInfo.Unit, unitProcessor);

        if (groupProcessor)
        {
            return ProcessGroup(unitInfo, groupProcessor);
        }
        else
        {
            return ProcessUnit(unitInfo, unitProcessor);
        }
    }

    bool ConfigurationSetApplyProcessor::ProcessUnit(UnitInfo & unitInfo, IConfigurationUnitProcessor& processor)
    {
        bool result = false;
        std::string_view action;

        try
        {
            action = TelemetryTraceLogger::TestAction;
            unitInfo.LastActionIntent = ConfigurationIntent::Assert;
            ITestSettingsResult testSettingsResult = processor.TestSettings();

            if (testSettingsResult.TestResult() == ConfigurationTestResult::Positive)
            {
                unitInfo.Result->PreviouslyInDesiredState(true);
                result = true;
            }
            else if (testSettingsResult.TestResult() == ConfigurationTestResult::Negative)
            {
                // Just in case testing took a while, check for cancellation before moving on to applying
                m_progress.ThrowIfCancelled();

                action = TelemetryTraceLogger::ApplyAction;
                unitInfo.LastActionIntent = ConfigurationIntent::Apply;
                IApplySettingsResult applySettingsResult = processor.ApplySettings();
                if (SUCCEEDED(applySettingsResult.ResultInformation().ResultCode()))
                {
                    unitInfo.Result->RebootRequired(applySettingsResult.RebootRequired());
                    result = true;
                }
                else
                {
                    unitInfo.ResultInformation->Initialize(applySettingsResult.ResultInformation());
                }
            }
            else if (testSettingsResult.TestResult() == ConfigurationTestResult::Failed)
            {
                unitInfo.ResultInformation->Initialize(testSettingsResult.ResultInformation());
            }
            else
            {
                unitInfo.ResultInformation->Initialize(E_UNEXPECTED, ConfigurationUnitResultSource::Internal);
            }
        }
        catch (...)
        {
            ExtractUnitResultInformation(std::current_exception(), unitInfo.ResultInformation);
        }

        m_telemetry.LogConfigUnitRunIfAppropriate(m_configurationSet.InstanceIdentifier(), unitInfo.Unit, ConfigurationIntent::Apply, action, *unitInfo.ResultInformation);
        return result;
    }

    IConfigurationGroupProcessor ConfigurationSetApplyProcessor::CreateGroupProcessor(const ConfigurationUnit& unit, IConfigurationUnitProcessor& processor)
    {
        if (unit.IsGroup())
        {
            std::wstring groupType = AppInstaller::Utility::ToLower(unit.Type());

            if (AppInstaller::Utility::ToLower(AssertionsGroup::Type()) == groupType)
            {
                return *make_self<wil::details::module_count_wrapper<AssertionsGroup>>(unit);
            }
            else
            {
                return processor.try_as<IConfigurationGroupProcessor>();
            }
        }
        else
        {
            return nullptr;
        }
    }

    bool ConfigurationSetApplyProcessor::ProcessGroup(UnitInfo& unitInfo, IConfigurationGroupProcessor& processor)
    {
        bool result = false;
        std::string_view action;

        try
        {
            action = TelemetryTraceLogger::TestAction;
            unitInfo.LastActionIntent = ConfigurationIntent::Assert;
            ITestGroupSettingsResult testSettingsResult = processor.TestGroupSettingsAsync().get();
            RecordUnitResults(testSettingsResult.UnitResults());

            if (testSettingsResult.TestResult() == ConfigurationTestResult::Positive)
            {
                unitInfo.Result->PreviouslyInDesiredState(true);
                result = true;
            }
            else if (testSettingsResult.TestResult() == ConfigurationTestResult::Negative)
            {
                // Just in case testing took a while, check for cancellation before moving on to applying
                m_progress.ThrowIfCancelled();

                action = TelemetryTraceLogger::ApplyAction;
                unitInfo.LastActionIntent = ConfigurationIntent::Apply;
                using ApplySettingsOperationType = Windows::Foundation::IAsyncOperationWithProgress<IApplyGroupSettingsResult, IApplySettingsResult>;
                ApplySettingsOperationType applySettingsOperation = processor.ApplyGroupSettingsAsync();

                applySettingsOperation.Progress([&](const ApplySettingsOperationType&, const IApplySettingsResult& applyResult)
                    {
                        RecordUnitResult(applyResult);
                    });

                IApplyGroupSettingsResult applySettingsResult = applySettingsOperation.get();
                RecordUnitResults(applySettingsResult.UnitResults());
                RecordSkippedUnits(unitInfo.Unit.Units());

                if (SUCCEEDED(applySettingsResult.ResultInformation().ResultCode()))
                {
                    unitInfo.Result->RebootRequired(applySettingsResult.RebootRequired());
                    result = true;
                }
                else
                {
                    unitInfo.ResultInformation->Initialize(applySettingsResult.ResultInformation());
                }
            }
            else if (testSettingsResult.TestResult() == ConfigurationTestResult::Failed)
            {
                unitInfo.ResultInformation->Initialize(testSettingsResult.ResultInformation());
            }
            else
            {
                unitInfo.ResultInformation->Initialize(E_UNEXPECTED, ConfigurationUnitResultSource::Internal);
            }
        }
        catch (...)
        {
            ExtractUnitResultInformation(std::current_exception(), unitInfo.ResultInformation);
        }

        m_telemetry.LogConfigUnitRunIfAppropriate(m_configurationSet.InstanceIdentifier(), unitInfo.Unit, ConfigurationIntent::Apply, action, *unitInfo.ResultInformation);
        return result;
    }

    void ConfigurationSetApplyProcessor::RecordUnitResults(const Windows::Foundation::Collections::IVector<ITestSettingsResult>& results)
    {
        for (const ITestSettingsResult& result : results)
        {
            UnitInfo& unitInfo = GetUnitInfo(result.Unit().InstanceIdentifier());
            unitInfo.LastActionIntent = ConfigurationIntent::Assert;

            if (result.TestResult() == ConfigurationTestResult::Positive)
            {
                unitInfo.Result->PreviouslyInDesiredState(true);
            }
            else if (result.TestResult() == ConfigurationTestResult::Negative)
            {
                // A negative test result just means we are going to apply shortly
            }
            else if (result.TestResult() == ConfigurationTestResult::Failed)
            {
                unitInfo.ResultInformation->Initialize(result.ResultInformation());
            }
            else
            {
                unitInfo.ResultInformation->Initialize(E_UNEXPECTED, ConfigurationUnitResultSource::Internal);
            }

            m_telemetry.LogConfigUnitRunIfAppropriate(m_configurationSet.InstanceIdentifier(), unitInfo.Unit, ConfigurationIntent::Apply, TelemetryTraceLogger::TestAction, *unitInfo.ResultInformation);
        }
    }

    void ConfigurationSetApplyProcessor::RecordUnitResult(const IApplySettingsResult& result)
    {
        UnitInfo& unitInfo = GetUnitInfo(result.Unit().InstanceIdentifier());

        if (unitInfo.Processed)
        {
            return;
        }

        unitInfo.Processed = true;

        if (SUCCEEDED(result.ResultInformation().ResultCode()))
        {
            unitInfo.Result->RebootRequired(result.RebootRequired());
        }
        else
        {
            unitInfo.ResultInformation->Initialize(result.ResultInformation());
        }

        m_telemetry.LogConfigUnitRunIfAppropriate(m_configurationSet.InstanceIdentifier(), unitInfo.Unit, ConfigurationIntent::Apply, TelemetryTraceLogger::ApplyAction, *unitInfo.ResultInformation);
        SendProgress(ConfigurationUnitState::Completed, unitInfo);
    }

    void ConfigurationSetApplyProcessor::RecordUnitResults(const Windows::Foundation::Collections::IVector<IApplySettingsResult>& results)
    {
        for (const IApplySettingsResult& result : results)
        {
            RecordUnitResult(result);
        }
    }

    void ConfigurationSetApplyProcessor::RecordSkippedUnits(const Windows::Foundation::Collections::IVector<ConfigurationUnit>& units)
    {
        // Create a copy of the set of configuration units
        std::vector<ConfigurationUnit> unitsToProcess{ units.Size() };
        units.GetMany(0, unitsToProcess);

        // Create the unit info vector from these units
        for (auto& unit : unitsToProcess)
        {
            if (unit.IsGroup())
            {
                RecordSkippedUnits(unit.Units());
            }

            UnitInfo& unitInfo = GetUnitInfo(unit.InstanceIdentifier());

            if (!unit.IsActive())
            {
                if (unitInfo.Processed)
                {
                    AICLI_LOG(Config, Crit, << "Inactive unit was processed: " << AppInstaller::Utility::ConvertToUTF8(unit.Identifier()));
                }
                else
                {
                    unitInfo.Processed = true;
                    unitInfo.ResultInformation->Initialize(WINGET_CONFIG_ERROR_MANUALLY_SKIPPED, ConfigurationUnitResultSource::Precondition);
                    SendProgress(ConfigurationUnitState::Skipped, unitInfo);
                }
            }
            else
            {
                if (!unitInfo.Processed)
                {
                    AICLI_LOG(Config, Warning, << "Unit a group was not processed: " << AppInstaller::Utility::ConvertToUTF8(unit.Identifier()));
                }
            }
        }
    }

    void ConfigurationSetApplyProcessor::SendProgress(ConfigurationSetState state)
    {
        try
        {
            m_progress.Progress(implementation::ConfigurationSetChangeData::Create(state));
        }
        CATCH_LOG();
    }

    void ConfigurationSetApplyProcessor::SendProgress(ConfigurationUnitState state, const UnitInfo& unitInfo)
    {
        unitInfo.Result->State(state);

        try
        {
            m_progress.Progress(implementation::ConfigurationSetChangeData::Create(state, *unitInfo.ResultInformation, unitInfo.Unit));
        }
        CATCH_LOG();
    }

    void ConfigurationSetApplyProcessor::SendProgressIfNotComplete(ConfigurationUnitState state, const UnitInfo& unitInfo)
    {
        if (unitInfo.Result->State() != ConfigurationUnitState::Completed)
        {
            SendProgress(state, unitInfo);
        }
    }

    TelemetryTraceLogger::ProcessingSummaryForIntent ConfigurationSetApplyProcessor::GetProcessingSummaryFor(ConfigurationIntent intent) const
    {
        TelemetryTraceLogger::ProcessingSummaryForIntent result{ intent, 0, 0, 0 };

        for (const auto& unitInfo : GetAllUnitInfos())
        {
            if (unitInfo.LastActionIntent == intent)
            {
                ++result.Count;

                if (unitInfo.Processed)
                {
                    ++result.Run;

                    if (FAILED(unitInfo.ResultInformation->ResultCode()))
                    {
                        ++result.Failed;
                    }
                }
            }
        }

        return result;
    }
}
