#include "OAuthJob.hpp"

#include "Http.hpp"
#include "ThreadSafeQueue.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "nlohmann/json.hpp"
#include <boost/algorithm/string.hpp>
#include <thread>

namespace Slic3r {
namespace GUI {

wxDEFINE_EVENT(EVT_OAUTH_COMPLETE_MESSAGE, wxCommandEvent);

OAuthJob::OAuthJob(const OAuthData& input) : local_authorization_server(input.params.callback_port), _data(input) {}

void OAuthJob::parse_token_response(const std::string& body, bool error, OAuthResult& result)
{
    const auto j = nlohmann::json::parse(body, nullptr, false, true);
    if (j.is_discarded()) {
        BOOST_LOG_TRIVIAL(warning) << "Invalid or no JSON data on token response: " << body;
        result.error_message = _u8L("Unknown error");
    } else if (error) {
        if (j.contains("error_description")) {
            j.at("error_description").get_to(result.error_message);
        } else {
            result.error_message = _u8L("Unknown error");
        }
    } else {
        j.at("access_token").get_to(result.access_token);
        j.at("refresh_token").get_to(result.refresh_token);
        result.success = true;
    }
}


void OAuthJob::process(Ctl& ctl)
{
    // Prepare auth process
    std::shared_ptr<ThreadSafeQueueSPSC<OAuthResult>> queue = std::make_shared<ThreadSafeQueueSPSC<OAuthResult>>();

    // Setup auth server to receive OAuth code from callback url
    local_authorization_server.set_request_handler([this, queue](const std::string& url) -> std::shared_ptr<HttpServer::Response> {
        if (boost::contains(url, "/callback")) {
            const auto code = url_get_param(url, "code");
            const auto state = url_get_param(url, "state");

            const auto handle_auth_fail = [this, queue](const std::string& message) -> std::shared_ptr<HttpServer::ResponseRedirect> {
                queue->push(OAuthResult{false, message});
                return std::make_shared<HttpServer::ResponseRedirect>(this->_data.params.auth_fail_redirect_url);
            };

            if (state != _data.params.state) {
                BOOST_LOG_TRIVIAL(warning) << "The provided state was not correct. Got " << state << " and expected " << _data.params.state;
                return handle_auth_fail(_u8L("The provided state is not correct."));
            }

            if (code.empty()) {
                const auto error_code = url_get_param(url, "error_code");
                if (error_code == "user_denied") {
                    BOOST_LOG_TRIVIAL(debug) << "User did not give the required permission when authorizing this application";
                    return handle_auth_fail(_u8L("Please give the required permissions when authorizing this application."));
                }

                BOOST_LOG_TRIVIAL(warning) << "Unexpected error when logging in. Error_code: " << error_code << ", State: " << state;
                return handle_auth_fail(_u8L("Something unexpected happened when trying to log in, please try again."));
            }


            OAuthResult r;
            // Request the access token from the authorization server.
            auto http = Http::post(_data.params.token_url);
            http.timeout_connect(5)
                .timeout_max(5)
                .form_add("client_id", _data.params.client_id)
                .form_add("redirect_uri", _data.params.callback_url)
                .form_add("grant_type", "authorization_code")
                .form_add("code", code)
                .form_add("code_verifier", _data.params.verification_code)
                .form_add("scope", _data.params.scope)
                .on_complete([&](std::string body, unsigned status) { parse_token_response(body, false, r); })
                .on_error([&](std::string body, std::string error, unsigned status) { parse_token_response(body, true, r); })
                .perform_sync();

            queue->push(r);
            return std::make_shared<HttpServer::ResponseRedirect>(r.success ? _data.params.auth_success_redirect_url :
                                                                              _data.params.auth_fail_redirect_url);
        } else {
            queue->push(OAuthResult{false});
            return std::make_shared<HttpServer::ResponseNotFound>();
        }
    });

    // Run the local server
    local_authorization_server.start();

    // Wait until we received the result
    bool received = false;
    while (!ctl.was_canceled() && !received ) {
        queue->consume_one(BlockingWait{1000}, [this, &received](const OAuthResult& result) {
            *_data.result = result;
            received      = true;
        });
    }

    // Handle timeout
    if (!received && ctl.was_canceled()) {
        _data.result->error_message = _u8L("User canceled.");
    } else {
        // Wait a while to ensure the response has sent
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    }
}

void OAuthJob::finalize(bool canceled, std::exception_ptr& e)
{
    // Make sure it's stopped
    local_authorization_server.stop();

    wxCommandEvent event(EVT_OAUTH_COMPLETE_MESSAGE);
    event.SetEventObject(m_event_handle);
    wxPostEvent(m_event_handle, event);
}
	
}} // namespace Slic3r::GUI
