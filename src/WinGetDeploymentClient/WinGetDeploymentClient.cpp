// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include <Windows.h>
#include "Public\WinGetDeploymentClient.h"
#include <objidl.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <mutex>
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

        struct AsyncState
        {
            std::mutex Lock;
            std::condition_variable Changed;
            bool Completed = false;
            std::function<void(bool, double)> Progress;
        };

        template <bool HasProgress = false, typename TAsync>
        auto WaitForOperation(const TAsync& operation, const UninstallCallbacks& callbacks)
        {
            // RPC failure can leave delegates outstanding after the session is destroyed.
            HMODULE module;
            winrt::check_bool(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(&CreatePackageManager), &module));
            auto state = std::make_shared<AsyncState>();
            state->Progress = callbacks.Progress;
            struct DisconnectProgress
            {
                std::shared_ptr<AsyncState> State;
                ~DisconnectProgress()
                {
                    std::lock_guard<std::mutex> lock{ State->Lock };
                    State->Progress = {};
                }
            } disconnect{ state };
            if constexpr (HasProgress)
            {
                operation.Progress([state](const auto&, const Deployment::UninstallProgress& progress)
                {
                    std::lock_guard<std::mutex> lock{ state->Lock };
                    if (state->Progress)
                    {
                        state->Progress(progress.State == Deployment::PackageUninstallProgressState::Queued,
                            progress.UninstallationProgress);
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

        std::string Lower(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(),
                [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            return value;
        }

        struct UninstallSession : IUninstallSession
        {
            UninstallSession() : m_manager(CreatePackageManager()) {}

            UninstallResult Execute(const UninstallRequest& request, const UninstallCallbacks& callbacks) override
            {
                if (m_started)
                {
                    throw winrt::hresult_illegal_method_call();
                }
                m_started = true;
                if (request.PackageId.empty())
                {
                    throw winrt::hresult_invalid_argument(L"A package identifier is required.");
                }
                if (callbacks.IsCancelled && callbacks.IsCancelled())
                {
                    throw winrt::hresult_canceled();
                }
                auto scope = Lower(request.Scope);
                if (!scope.empty() && scope != "user")
                {
                    throw winrt::hresult_error(E_ACCESSDENIED, L"Only user-scope uninstall is supported.");
                }
                auto composite = Activate<Deployment::CreateCompositePackageCatalogOptions>(
                    L"{" WINGET_OUTOFPROC_COM_CLSID_CreateCompositePackageCatalogOptions L"}");
                composite.InstalledScope(scope.empty() ? Deployment::PackageInstallScope::Any : Deployment::PackageInstallScope::User);
                composite.CompositeSearchBehavior(Deployment::CompositeSearchBehavior::LocalCatalogs);
                if (!request.SourceName.empty())
                {
                    auto reference = m_manager.GetPackageCatalogByName(winrt::to_hstring(request.SourceName));
                    if (!reference)
                    {
                        throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
                    }
                    reference.PackageCatalogBackgroundUpdateInterval(0s);
                    reference.AdditionalPackageCatalogArguments(winrt::to_hstring(request.SourceHeader));
                    auto authentication = Activate<Deployment::AuthenticationArguments>(
                        L"{" WINGET_OUTOFPROC_COM_CLSID_AuthenticationArguments L"}");
                    auto mode = Lower(request.AuthenticationMode);
                    if (mode == "interactive")
                    {
                        authentication.AuthenticationMode(Deployment::AuthenticationMode::Interactive);
                    }
                    else if (mode == "silent")
                    {
                        authentication.AuthenticationMode(Deployment::AuthenticationMode::Silent);
                    }
                    else if (mode == "silentpreferred")
                    {
                        authentication.AuthenticationMode(Deployment::AuthenticationMode::SilentPreferred);
                    }
                    else
                    {
                        throw winrt::hresult_invalid_argument(L"Invalid authentication mode.");
                    }
                    authentication.AuthenticationAccount(winrt::to_hstring(request.AuthenticationAccount));
                    reference.AuthenticationArguments(authentication);
                    if (!request.AcceptSourceAgreements && !reference.SourceAgreementsSatisfied())
                    {
                        throw winrt::hresult_error(E_ACCESSDENIED, L"Source agreements changed before delegation.");
                    }
                    reference.AcceptSourceAgreements(true);
                    auto connected = WaitForOperation(reference.ConnectAsync(), callbacks);
                    winrt::check_hresult(connected.ExtendedErrorCode());
                    if (!connected.PackageCatalog() || winrt::to_string(connected.PackageCatalog().Info().Id()) != request.SourceIdentifier)
                    {
                        throw winrt::hresult_error(E_INVALIDARG, L"The package source changed before delegation.");
                    }
                    composite.Catalogs().Append(reference);
                }
                auto connected = WaitForOperation(m_manager.CreateCompositePackageCatalog(composite).ConnectAsync(), callbacks);
                winrt::check_hresult(connected.ExtendedErrorCode());
                auto catalog = connected.PackageCatalog();
                if (!catalog)
                {
                    throw winrt::hresult_error(E_UNEXPECTED);
                }
                auto find = Activate<Deployment::FindPackagesOptions>(L"{" WINGET_OUTOFPROC_COM_CLSID_FindPackagesOptions L"}");
                auto filter = Activate<Deployment::PackageMatchFilter>(L"{" WINGET_OUTOFPROC_COM_CLSID_PackageMatchFilter L"}");
                filter.Field(Deployment::PackageMatchField::Id);
                filter.Option(Deployment::PackageFieldMatchOption::Equals);
                filter.Value(winrt::to_hstring(request.PackageId));
                find.Filters().Append(filter);
                auto found = WaitForOperation(catalog.FindPackagesAsync(find), callbacks);
                winrt::check_hresult(found.ExtendedErrorCode());
                if (found.Status() != Deployment::FindPackagesResultStatus::Ok || found.WasLimitExceeded())
                {
                    throw winrt::hresult_error(E_UNEXPECTED);
                }
                Deployment::CatalogPackage package{ nullptr };
                for (auto match : found.Matches())
                {
                    auto candidate = match.CatalogPackage();
                    if (candidate.InstalledVersion())
                    {
                        if (package)
                        {
                            throw winrt::hresult_error(E_INVALIDARG, L"The installed package target is ambiguous.");
                        }
                        package = candidate;
                    }
                }
                if (!package)
                {
                    throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), L"The installed package disappeared.");
                }
                using Field = Deployment::PackageVersionMetadataField;
                auto installed = package.InstalledVersion();
                auto type = Lower(winrt::to_string(installed.GetMetadata(Field::InstallerType)));
                if (Lower(winrt::to_string(installed.GetMetadata(Field::InstalledScope))) != "user" ||
                    (type != "exe" && type != "burn" && type != "inno" && type != "nullsoft" && type != "msi" && type != "wix"))
                {
                    throw winrt::hresult_error(E_ACCESSDENIED, L"The selected package is not a supported user-scope application.");
                }
                auto options = Activate<Deployment::UninstallOptions>(L"{" WINGET_OUTOFPROC_COM_CLSID_UninstallOptions L"}");
                options.PackageUninstallMode(request.Interactive ? Deployment::PackageUninstallMode::Interactive :
                    request.Silent ? Deployment::PackageUninstallMode::Silent : Deployment::PackageUninstallMode::Default);
                options.PackageUninstallScope(scope.empty() ? Deployment::PackageUninstallScope::Any : Deployment::PackageUninstallScope::User);
                options.Force(request.Force);
                options.LogOutputPath(winrt::to_hstring(request.LogOutputPath));
                options.CorrelationData(winrt::to_hstring(request.CorrelationData));
                auto result = WaitForOperation<true>(m_manager.UninstallPackageAsync(package, options), callbacks);
                return { result.ExtendedErrorCode(), result.UninstallerErrorCode(),
                    result.Status() == Deployment::UninstallResultStatus::Ok, result.RebootRequired() };
            }

            Deployment::PackageManager m_manager{ nullptr };
            bool m_started = false;
        };
    }

    IUninstallSession* __cdecl WinGetCreateUninstallSession()
    {
        return new UninstallSession();
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
