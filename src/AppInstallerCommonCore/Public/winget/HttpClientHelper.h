// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include <winget/Certificates.h>
#include <cpprest/http_client.h>
#include <cpprest/json.h>
#include <memory>
#include <optional>
#include <vector>

namespace AppInstaller::Http
{
    struct HttpClientHelper
    {
        using HttpRequestHeaders = std::unordered_map<utility::string_t, utility::string_t>;

        struct HttpResponseHandlerResult
        {
            // The custom response handler result. Default is empty.
            std::optional<web::json::value> Result = std::nullopt;

            // Indicates whether to use default handling logic by HttpClientHelper instead (i.e. the custom response handler does not handle the specific response).
            bool UseDefaultHandling = false;
        };

        using HttpResponseHandler = std::function<HttpResponseHandlerResult(const web::http::http_response&)>;

        HttpClientHelper(std::shared_ptr<web::http::http_pipeline_stage> = {});

        std::optional<web::json::value> HandlePost(const utility::string_t& uri, const web::json::value& body, const HttpRequestHeaders& headers = {}, const HttpRequestHeaders& authHeaders = {}, const HttpResponseHandler& customHandler = {}) const;

        std::optional<web::json::value> HandleGet(const utility::string_t& uri, const HttpRequestHeaders& headers = {}, const HttpRequestHeaders& authHeaders = {}, const HttpResponseHandler& customHandler = {}) const;

        void SetPinningConfiguration(const Certificates::PinningConfiguration& configuration);

    protected:
        struct InvocationContext : public std::enable_shared_from_this<InvocationContext>
        {
            std::unique_ptr<std::weak_ptr<InvocationContext>> Self;
            std::unique_ptr<web::http::client::http_client> HttpClient;
            pplx::task<web::http::http_response> ResponseTask;

            web::http::client::native_handle SessionHandle = INVALID_HANDLE_VALUE;
            void* PreviousCallback = nullptr;
            DWORD_PTR PreviousContext = 0;

            Certificates::PinningConfiguration PinningConfiguration;
            std::exception_ptr PinningValidationException = nullptr;

            void CaptureSessionHandle(web::http::client::native_handle sessionHandle);
            void InstallStatusCallback(web::http::client::native_handle requestHandle);

            static void _stdcall ValidatePinningConfigurationCallback(
                IN LPVOID hInternet,
                IN DWORD_PTR dwContext,
                IN DWORD dwInternetStatus,
                IN LPVOID lpvStatusInformation OPTIONAL,
                IN DWORD dwStatusInformationLength);
        };

        std::shared_ptr<InvocationContext> GetClient(const utility::string_t& uri) const;

        std::shared_ptr<InvocationContext> Post(const utility::string_t& uri, const web::json::value& body, const HttpRequestHeaders& headers = {}, const HttpRequestHeaders& authHeaders = {}) const;

        std::shared_ptr<InvocationContext> Get(const utility::string_t& uri, const HttpRequestHeaders& headers = {}, const HttpRequestHeaders& authHeaders = {}) const;

        std::optional<web::json::value> ValidateAndExtractResponse(const web::http::http_response& response) const;

        std::optional<web::json::value> ExtractJsonResponse(const web::http::http_response& response) const;

    private:
        // Translates a cpprestsdk http_exception to a WIL exception.
        static void RethrowAsWilException(const web::http::http_exception& exception);

        std::shared_ptr<web::http::http_pipeline_stage> m_defaultRequestHandlerStage;
        web::http::client::http_client_config m_clientConfig;
        std::optional<Certificates::PinningConfiguration> m_pinningConfiguration;
    };
}
