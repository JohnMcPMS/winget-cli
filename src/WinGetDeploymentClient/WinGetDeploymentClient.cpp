// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include <Windows.h>
#include "Public\WinGetDeploymentClient.h"
#include <objidl.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <mutex>
#include <utility>
#include <winrt/Microsoft.Management.Deployment.h>
#include "..\Microsoft.Management.Deployment\Public\ComClsids.h"

namespace AppInstaller::Deployment::Client
{
    namespace
    {
        namespace Deployment = winrt::Microsoft::Management::Deployment;
        using namespace std::chrono_literals;

        template <typename T>
        T Activate(const wchar_t* classId)
        {
            CLSID clsid;
            winrt::check_hresult(CLSIDFromString(classId, &clsid));
            T result{ nullptr };
            winrt::check_hresult(CoCreateInstance(clsid, nullptr, CLSCTX_LOCAL_SERVER | CLSCTX_NO_CODE_DOWNLOAD,
                winrt::guid_of<T>(), winrt::put_abi(result)));
            return result;
        }

        Deployment::PackageManager CreatePackageManager()
        {
            winrt::com_ptr<IGlobalOptions> options;
            winrt::check_hresult(CoCreateInstance(CLSID_GlobalOptions, nullptr, CLSCTX_INPROC_SERVER,
                __uuidof(IGlobalOptions), options.put_void()));
            winrt::check_hresult(options->Set(COMGLB_UNMARSHALING_POLICY, COMGLB_UNMARSHALING_POLICY_STRONG));
            return Activate<Deployment::PackageManager>(L"{" WINGET_OUTOFPROC_COM_CLSID_PackageManager L"}");
        }

        template <typename TVector>
        std::vector<std::string> ToStrings(const TVector& values)
        {
            std::vector<std::string> result;
            for (const auto& value : values)
            {
                result.emplace_back(winrt::to_string(value));
            }
            std::sort(result.begin(), result.end());
            return result;
        }

        bool IsInstalledInstance(const Deployment::PackageVersionInfo& version, const InstalledPackage& expected)
        {
            using Field = Deployment::PackageVersionMetadataField;
            auto productCodes = expected.ProductCodes;
            auto familyNames = expected.PackageFamilyNames;
            std::sort(productCodes.begin(), productCodes.end());
            std::sort(familyNames.begin(), familyNames.end());
            auto scope = winrt::to_string(version.GetMetadata(Field::InstalledScope));
            if (scope == "System")
            {
                scope = "Machine";
            }
            return winrt::to_string(version.Id()) == expected.Id &&
                winrt::to_string(version.Version()) == expected.Version &&
                winrt::to_string(version.Channel()) == expected.Channel &&
                scope == expected.Scope &&
                winrt::to_string(version.GetMetadata(Field::InstalledArchitecture)) == expected.Architecture &&
                winrt::to_string(version.GetMetadata(Field::InstallerType)) == expected.InstallerType &&
                winrt::to_string(version.GetMetadata(Field::InstalledLocation)) == expected.InstallLocation &&
                ToStrings(version.ProductCodes()) == productCodes &&
                ToStrings(version.PackageFamilyNames()) == familyNames;
        }

        OperationProgress PercentProgress(OperationStage stage, double fraction)
        {
            if (!std::isfinite(fraction) || fraction < 0 || fraction > 1)
            {
                throw winrt::hresult_error(E_UNEXPECTED, L"Invalid operation progress.");
            }
            return { stage, static_cast<uint64_t>(fraction * 100), 100, false };
        }

        OperationProgress ConvertProgress(const Deployment::InstallProgress& progress)
        {
            switch (progress.State)
            {
            case Deployment::PackageInstallProgressState::Queued:
                return {};
            case Deployment::PackageInstallProgressState::Downloading:
                return { OperationStage::Download, progress.BytesDownloaded, progress.BytesRequired, true };
            case Deployment::PackageInstallProgressState::Installing:
                return PercentProgress(OperationStage::Execution, progress.InstallationProgress);
            default:
                return { OperationStage::PostExecution };
            }
        }

        OperationProgress ConvertProgress(const Deployment::UninstallProgress& progress)
        {
            if (progress.State == Deployment::PackageUninstallProgressState::Queued)
            {
                return {};
            }
            return PercentProgress(progress.State == Deployment::PackageUninstallProgressState::Uninstalling ?
                OperationStage::Execution : OperationStage::PostExecution, progress.UninstallationProgress);
        }

        OperationProgress ConvertProgress(const Deployment::RepairProgress& progress)
        {
            if (progress.State == Deployment::PackageRepairProgressState::Queued)
            {
                return {};
            }
            return PercentProgress(progress.State == Deployment::PackageRepairProgressState::Repairing ?
                OperationStage::Execution : OperationStage::PostExecution, progress.RepairCompletionProgress);
        }

        struct ProgressState
        {
            std::mutex Lock;
            std::condition_variable Changed;
            bool Completed = false;
            std::function<void(const OperationProgress&)> Callback;
        };

        template <bool HasProgress = true, typename TAsync>
        auto WaitForOperation(const TAsync& operation, const OperationCallbacks& callbacks)
        {
            // An outstanding async delegate can survive an RPC failure.
            HMODULE module;
            winrt::check_bool(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(&CreatePackageManager), &module));
            auto state = std::make_shared<ProgressState>();
            state->Callback = callbacks.Progress;
            struct DisconnectProgress
            {
                std::shared_ptr<ProgressState> State;
                ~DisconnectProgress()
                {
                    std::lock_guard<std::mutex> lock{ State->Lock };
                    State->Callback = {};
                }
            } disconnect{ state };
            if constexpr (HasProgress)
            {
                operation.Progress([state](const auto&, const auto& progress)
                {
                    std::lock_guard<std::mutex> lock{ state->Lock };
                    if (state->Callback)
                    {
                        state->Callback(ConvertProgress(progress));
                    }
                });
            }
            operation.Completed([state](const auto&, const auto&)
            {
                {
                    std::lock_guard<std::mutex> lock{ state->Lock };
                    state->Completed = true;
                }
                state->Changed.notify_one();
            });
            bool cancellationSent = false;
            std::unique_lock<std::mutex> lock{ state->Lock };
            while (!state->Changed.wait_for(lock, 100ms, [&]() { return state->Completed; }))
            {
                lock.unlock();
                if (!cancellationSent && callbacks.IsCancelled && callbacks.IsCancelled())
                {
                    operation.Cancel();
                    cancellationSent = true;
                }
                lock.lock();
            }
            lock.unlock();
            return operation.GetResults();
        }

        struct DeploymentSession : IDeploymentSession
        {
            explicit DeploymentSession(const OperationRequest& request) :
                m_request(request)
            {
                ValidateRequest(request);
                m_manager = CreatePackageManager();
            }

            static void ValidateRequest(const OperationRequest& request)
            {
                if ((request.Type == Operation::Install || request.Type == Operation::Upgrade) &&
                    (request.SourceName.empty() || request.SourceIdentifier.empty()))
                {
                    throw winrt::hresult_invalid_argument(L"Delegated installations require a catalog version.");
                }
                if (!request.Proxy.empty() || request.NoProxy)
                {
                    throw winrt::hresult_error(E_NOTIMPL, L"Explicit proxy overrides are not supported for delegated operations.");
                }
            }

            void ResolveTarget(const OperationCallbacks& callbacks)
            {
                const auto& request = m_request;
                auto compositeOptions = Activate<Deployment::CreateCompositePackageCatalogOptions>(
                    L"{" WINGET_OUTOFPROC_COM_CLSID_CreateCompositePackageCatalogOptions L"}");
                if (request.Type == Operation::Upgrade)
                {
                    compositeOptions.InstalledScope(request.Scope == "user" ? Deployment::PackageInstallScope::User :
                        request.Scope == "machine" ? Deployment::PackageInstallScope::System : Deployment::PackageInstallScope::Any);
                }
                auto catalogs = request.CorrelatedCatalogs;
                if (!request.SourceName.empty() && std::none_of(catalogs.begin(), catalogs.end(),
                    [&](const auto& catalog) { return catalog.Identifier == request.SourceIdentifier; }))
                {
                    catalogs.push_back({ request.SourceName, request.SourceIdentifier });
                }
                for (const auto& catalog : catalogs)
                {
                    auto reference = m_manager.GetPackageCatalogByName(winrt::to_hstring(catalog.Name));
                    if (!reference)
                    {
                        throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
                    }
                    reference.PackageCatalogBackgroundUpdateInterval(0s);
                    reference.AdditionalPackageCatalogArguments(winrt::to_hstring(request.SourceHeader));
                    reference.AuthenticationArguments(GetAuthenticationArguments());
                    if (!request.AcceptSourceAgreements && !reference.SourceAgreementsSatisfied())
                    {
                        throw winrt::hresult_error(E_ACCESSDENIED, L"Source agreements changed before delegation.");
                    }
                    reference.AcceptSourceAgreements(true);
                    auto connected = WaitForOperation<false>(reference.ConnectAsync(), callbacks);
                    winrt::check_hresult(connected.ExtendedErrorCode());
                    if (!connected.PackageCatalog() ||
                        winrt::to_string(connected.PackageCatalog().Info().Id()) != catalog.Identifier)
                    {
                        throw winrt::hresult_error(E_INVALIDARG, L"The package source changed before delegation.");
                    }
                    compositeOptions.Catalogs().Append(reference);
                }

                if (catalogs.empty())
                {
                    auto connected = WaitForOperation<false>(
                        m_manager.GetLocalPackageCatalog(Deployment::LocalPackageCatalog::InstalledPackages).ConnectAsync(), callbacks);
                    winrt::check_hresult(connected.ExtendedErrorCode());
                    m_catalog = connected.PackageCatalog();
                }
                else
                {
                    compositeOptions.CompositeSearchBehavior(Deployment::CompositeSearchBehavior::AllCatalogs);
                    auto connected = WaitForOperation<false>(m_manager.CreateCompositePackageCatalog(compositeOptions).ConnectAsync(), callbacks);
                    winrt::check_hresult(connected.ExtendedErrorCode());
                    m_catalog = connected.PackageCatalog();
                }
                if (!m_catalog)
                {
                    throw winrt::hresult_error(E_UNEXPECTED);
                }

                auto findOptions = Activate<Deployment::FindPackagesOptions>(L"{" WINGET_OUTOFPROC_COM_CLSID_FindPackagesOptions L"}");
                auto filter = Activate<Deployment::PackageMatchFilter>(L"{" WINGET_OUTOFPROC_COM_CLSID_PackageMatchFilter L"}");
                filter.Field(Deployment::PackageMatchField::Id);
                filter.Option(Deployment::PackageFieldMatchOption::Equals);
                filter.Value(winrt::to_hstring(request.PackageId));
                findOptions.Filters().Append(filter);
                auto found = WaitForOperation<false>(m_catalog.FindPackagesAsync(findOptions), callbacks);
                winrt::check_hresult(found.ExtendedErrorCode());
                if (found.Status() != Deployment::FindPackagesResultStatus::Ok || found.WasLimitExceeded())
                {
                    throw winrt::hresult_error(E_UNEXPECTED);
                }
                for (auto match : found.Matches())
                {
                    auto package = match.CatalogPackage();
                    if (request.Type == Operation::Install || request.Type == Operation::Upgrade)
                    {
                        auto installed = request.Type == Operation::Upgrade ? package.InstalledVersion() : nullptr;
                        if (request.Type == Operation::Upgrade && !installed)
                        {
                            continue;
                        }
                        if (m_package)
                        {
                            throw winrt::hresult_error(E_INVALIDARG, L"The package target is ambiguous.");
                        }
                        m_package = package;
                        m_installed = installed;
                    }
                    else
                    {
                        for (auto installed : package.InstalledVersions())
                        {
                            if (IsInstalledInstance(installed, request.Installed))
                            {
                                if (m_installed)
                                {
                                    throw winrt::hresult_error(E_INVALIDARG, L"The installed package target is ambiguous.");
                                }
                                m_package = package;
                                m_installed = installed;
                            }
                        }
                    }
                }
                if (!m_package || (request.Type != Operation::Install && !m_installed))
                {
                    throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), L"The selected installed package changed or disappeared.");
                }
                if (!request.SourceName.empty() &&
                    (request.Type == Operation::Install || request.Type == Operation::Upgrade || request.Type == Operation::Repair))
                {
                    for (auto version : m_package.AvailableVersions())
                    {
                        if (winrt::to_string(version.PackageCatalogId()) == request.SourceIdentifier &&
                            winrt::to_string(version.Version()) == request.Version &&
                            winrt::to_string(version.Channel()) == request.Channel)
                        {
                            if (m_version)
                            {
                                throw winrt::hresult_error(E_INVALIDARG);
                            }
                            m_version = version;
                        }
                    }
                    if (!m_version)
                    {
                        throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), L"The selected available version disappeared.");
                    }
                }
            }

            Deployment::AuthenticationArguments GetAuthenticationArguments()
            {
                auto result = Activate<Deployment::AuthenticationArguments>(L"{" WINGET_OUTOFPROC_COM_CLSID_AuthenticationArguments L"}");
                if (m_request.AuthenticationMode == "silent")
                {
                    result.AuthenticationMode(Deployment::AuthenticationMode::Silent);
                }
                else if (m_request.AuthenticationMode == "silentpreferred")
                {
                    result.AuthenticationMode(Deployment::AuthenticationMode::SilentPreferred);
                }
                else if (m_request.AuthenticationMode == "interactive")
                {
                    result.AuthenticationMode(Deployment::AuthenticationMode::Interactive);
                }
                else if (!m_request.AuthenticationMode.empty())
                {
                    throw winrt::hresult_invalid_argument(L"Invalid authentication mode.");
                }
                result.AuthenticationAccount(winrt::to_hstring(m_request.AuthenticationAccount));
                return result;
            }

            static winrt::Windows::System::ProcessorArchitecture GetArchitecture(std::string_view value)
            {
                using Architecture = winrt::Windows::System::ProcessorArchitecture;
                if (value == "x86")
                {
                    return Architecture::X86;
                }
                if (value == "x64")
                {
                    return Architecture::X64;
                }
                if (value == "arm")
                {
                    return Architecture::Arm;
                }
                if (value == "arm64")
                {
                    return Architecture::Arm64;
                }
                if (value == "neutral")
                {
                    return Architecture::Neutral;
                }
                throw winrt::hresult_invalid_argument(L"Invalid architecture.");
            }

            Deployment::PackageInstallerType GetInstallerType()
            {
                using Type = Deployment::PackageInstallerType;
                static constexpr std::pair<std::string_view, Type> types[] =
                {
                    { "", Type::Unknown },
                    { "unknown", Type::Unknown },
                    { "exe", Type::Exe },
                    { "inno", Type::Inno },
                    { "nullsoft", Type::Nullsoft },
                    { "burn", Type::Burn },
                    { "msi", Type::Msi },
                    { "wix", Type::Wix },
                    { "msix", Type::Msix },
                    { "msstore", Type::MSStore },
                    { "portable", Type::Portable },
                    { "zip", Type::Zip },
                    { "font", Type::Font },
                };
                for (const auto& [name, type] : types)
                {
                    if (m_request.InstallerType == name)
                    {
                        return type;
                    }
                }
                throw winrt::hresult_invalid_argument(L"Invalid installer type.");
            }

            template <typename TOptions>
            void SetSelectionOptions(const TOptions& options)
            {
                options.Locale(winrt::to_hstring(m_request.Locale));
                options.InstallerType(GetInstallerType());
                options.UseDefaultScopePreference(m_request.UseDefaultScopePreference);
                auto architectures = options.AllowedArchitectures();
                architectures.Clear();
                for (const auto& value : m_request.AllowedArchitectures)
                {
                    architectures.Append(GetArchitecture(value));
                }
            }

            OperationResult Execute(const OperationRequest& request, const OperationCallbacks& callbacks) override
            {
                if (m_executed)
                {
                    throw winrt::hresult_illegal_method_call();
                }
                m_executed = true;
                ValidateRequest(request);
                if (request.Type != m_request.Type)
                {
                    throw winrt::hresult_invalid_argument(L"The prepared operation type changed.");
                }
                m_request = request;
                auto normalize = [](std::string& value)
                {
                    std::transform(value.begin(), value.end(), value.begin(),
                        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                };
                normalize(m_request.Scope);
                normalize(m_request.InstallerType);
                normalize(m_request.AuthenticationMode);
                for (auto& architecture : m_request.AllowedArchitectures)
                {
                    normalize(architecture);
                }
                if (callbacks.IsCancelled && callbacks.IsCancelled())
                {
                    throw winrt::hresult_canceled();
                }
                ResolveTarget(callbacks);
                if (m_request.Type == Operation::Uninstall)
                {
                    auto options = Activate<Deployment::UninstallOptions>(L"{" WINGET_OUTOFPROC_COM_CLSID_UninstallOptions L"}");
                    options.InstalledVersion(m_installed);
                    options.Force(m_request.Force);
                    options.Purge(m_request.Purge);
                    options.Preserve(m_request.Preserve);
                    options.LogOutputPath(winrt::to_hstring(m_request.LogOutputPath));
                    options.CorrelationData(winrt::to_hstring(m_request.CorrelationData));
                    options.PackageUninstallMode(m_request.Interactive ? Deployment::PackageUninstallMode::Interactive :
                        m_request.Silent ? Deployment::PackageUninstallMode::Silent : Deployment::PackageUninstallMode::Default);
                    options.PackageUninstallScope(m_request.Scope == "machine" ? Deployment::PackageUninstallScope::System :
                        m_request.Scope == "user" ? Deployment::PackageUninstallScope::User : Deployment::PackageUninstallScope::Any);
                    auto result = WaitForOperation(m_manager.UninstallPackageAsync(m_package, options), callbacks);
                    return { result.ExtendedErrorCode(), result.UninstallerErrorCode(), result.Status() == Deployment::UninstallResultStatus::Ok, result.RebootRequired() };
                }
                if (m_request.Type == Operation::Repair)
                {
                    auto options = Activate<Deployment::RepairOptions>(L"{" WINGET_OUTOFPROC_COM_CLSID_RepairOptions L"}");
                    options.PackageVersionId(m_version);
                    options.InstalledVersion(m_installed);
                    SetSelectionOptions(options);
                    options.IgnoreLocalArchiveMalwareScan(m_request.IgnoreLocalArchiveMalwareScan);
                    options.Force(m_request.Force);
                    options.AcceptPackageAgreements(true);
                    options.AllowHashMismatch(false);
                    options.LogOutputPath(winrt::to_hstring(m_request.LogOutputPath));
                    options.CorrelationData(winrt::to_hstring(m_request.CorrelationData));
                    options.AuthenticationArguments(GetAuthenticationArguments());
                    options.BypassIsStoreClientBlockedPolicyCheck(m_request.BypassIsStoreClientBlockedPolicyCheck);
                    options.PackageRepairMode(m_request.Interactive ? Deployment::PackageRepairMode::Interactive :
                        m_request.Silent ? Deployment::PackageRepairMode::Silent : Deployment::PackageRepairMode::Default);
                    options.PackageRepairScope(m_request.Scope == "machine" ?
                        (m_request.AllowUnknownScope ? Deployment::PackageRepairScope::SystemOrUnknown : Deployment::PackageRepairScope::System) :
                        m_request.Scope == "user" ?
                        (m_request.AllowUnknownScope ? Deployment::PackageRepairScope::UserOrUnknown : Deployment::PackageRepairScope::User) :
                        Deployment::PackageRepairScope::Any);
                    auto result = WaitForOperation(m_manager.RepairPackageAsync(m_package, options), callbacks);
                    return { result.ExtendedErrorCode(), result.RepairerErrorCode(), result.Status() == Deployment::RepairResultStatus::Ok, result.RebootRequired() };
                }

                auto options = Activate<Deployment::InstallOptions>(L"{" WINGET_OUTOFPROC_COM_CLSID_InstallOptions L"}");
                options.PackageVersionId(m_version);
                SetSelectionOptions(options);
                options.Rename(winrt::to_hstring(m_request.Rename));
                options.UninstallPrevious(m_request.UninstallPrevious);
                options.IgnoreLocalArchiveMalwareScan(m_request.IgnoreLocalArchiveMalwareScan);
                options.Force(m_request.Force);
                options.SkipDependencies(true);
                options.AcceptPackageAgreements(true);
                // Delegation must not enable an override rejected by the elevated coordinator.
                options.AllowHashMismatch(false);
                options.AllowUpgradeToUnknownVersion(m_request.AllowUpgradeToUnknownVersion);
                options.LogOutputPath(winrt::to_hstring(m_request.LogOutputPath));
                options.CorrelationData(winrt::to_hstring(m_request.CorrelationData));
                options.PreferredInstallLocation(winrt::to_hstring(m_request.InstallLocation));
                options.ReplacementInstallerArguments(winrt::to_hstring(m_request.ReplacementArguments));
                options.AdditionalInstallerArguments(winrt::to_hstring(m_request.AdditionalArguments));
                options.AuthenticationArguments(GetAuthenticationArguments());
                options.BypassIsStoreClientBlockedPolicyCheck(m_request.BypassIsStoreClientBlockedPolicyCheck);
                options.PackageInstallMode(m_request.Interactive ? Deployment::PackageInstallMode::Interactive :
                    m_request.Silent ? Deployment::PackageInstallMode::Silent : Deployment::PackageInstallMode::Default);
                options.PackageInstallScope(m_request.Scope == "machine" ?
                    (m_request.AllowUnknownScope ? Deployment::PackageInstallScope::SystemOrUnknown : Deployment::PackageInstallScope::System) :
                    m_request.Scope == "user" ?
                    (m_request.AllowUnknownScope ? Deployment::PackageInstallScope::UserOrUnknown : Deployment::PackageInstallScope::User) :
                    Deployment::PackageInstallScope::Any);
                auto operation = m_request.Type == Operation::Upgrade ?
                    m_manager.UpgradePackageAsync(m_package, options) : m_manager.InstallPackageAsync(m_package, options);
                auto result = WaitForOperation(operation, callbacks);
                return { result.ExtendedErrorCode(), result.InstallerErrorCode(), result.Status() == Deployment::InstallResultStatus::Ok, result.RebootRequired() };
            }

            OperationRequest m_request;
            Deployment::PackageManager m_manager{ nullptr };
            Deployment::PackageCatalog m_catalog{ nullptr };
            Deployment::CatalogPackage m_package{ nullptr };
            Deployment::PackageVersionInfo m_installed{ nullptr };
            Deployment::PackageVersionId m_version{ nullptr };
            bool m_executed = false;
        };
    }

    IDeploymentSession* __cdecl WinGetCreateDeploymentSession(const OperationRequest& request)
    {
        return new DeploymentSession(request);
    }

    void RunPackageManagerActivationPlayground(
        const std::function<void(std::wstring_view, const std::vector<GUID>&)>& callback)
    {
        auto instance = CreatePackageManager();
        // Metadata-based proxies need IInspectable's own proxy for these calls.
        const auto inspectable = instance.as<winrt::Windows::Foundation::IInspectable>();
        const auto runtimeClassName = winrt::get_class_name(inspectable);
        if (runtimeClassName != winrt::name_of<winrt::Microsoft::Management::Deployment::PackageManager>())
        {
            throw winrt::hresult_error(E_UNEXPECTED, L"Activation did not return a PackageManager.");
        }

        const auto interfaces = winrt::get_interfaces(inspectable);
        std::vector<GUID> interfaceIds;
        interfaceIds.reserve(interfaces.size());
        for (const auto& interfaceId : interfaces)
        {
            interfaceIds.push_back(interfaceId);
        }

        callback(runtimeClassName, interfaceIds);
    }
}
