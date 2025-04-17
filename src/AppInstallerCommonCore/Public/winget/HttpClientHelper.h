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
    namespace details
    {
        struct InvocationContext
        {
            std::unique_ptr<web::http::client::http_client> HttpClient;
            pplx::task<web::http::http_response> ResponseTask;
            web::http::client::native_handle RequestHandle = INVALID_HANDLE_VALUE;
            std::shared_ptr<void> RequestContextLifetime;

            void CaptureRequestContext(web::http::client::native_handle handle);
        };
    }

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

        details::InvocationContext Post(const utility::string_t& uri, const web::json::value& body, const HttpRequestHeaders& headers = {}, const HttpRequestHeaders& authHeaders = {}) const;

        std::optional<web::json::value> HandlePost(const utility::string_t& uri, const web::json::value& body, const HttpRequestHeaders& headers = {}, const HttpRequestHeaders& authHeaders = {}, const HttpResponseHandler& customHandler = {}) const;

        details::InvocationContext Get(const utility::string_t& uri, const HttpRequestHeaders& headers = {}, const HttpRequestHeaders& authHeaders = {}) const;

        std::optional<web::json::value> HandleGet(const utility::string_t& uri, const HttpRequestHeaders& headers = {}, const HttpRequestHeaders& authHeaders = {}, const HttpResponseHandler& customHandler = {}) const;

        void SetPinningConfiguration(const Certificates::PinningConfiguration& configuration);

    protected:
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
