#ifndef __OAuthJob_HPP__
#define __OAuthJob_HPP__

#include "Job.hpp"
#include "slic3r/GUI/HttpServer.hpp"
#include <boost/nowide/fstream.hpp>
#include <boost/nowide/cstdio.hpp>
#include <boost/log/trivial.hpp>

namespace Slic3r {
namespace GUI {

class Plater;

struct OAuthParams
{
    std::string login_url;
    std::string client_id;
    boost::asio::ip::port_type callback_port;
    std::string callback_url;
    std::string scope;
    std::string response_type;
    std::string auth_success_redirect_url;
    std::string auth_fail_redirect_url;
    std::string token_url;
    std::string verification_code;
    std::string state;
};

struct OAuthResult
{
    bool        success{false};
    std::string error_message{""};
    std::string access_token{""};
    std::string refresh_token{""};
};

struct OAuthData
{
    OAuthParams params;
    std::shared_ptr<OAuthResult> result;
};

class OAuthJob : public Job
{
    HttpServer local_authorization_server;
    OAuthData  _data;
    wxWindow*  m_event_handle{nullptr};

public:
    explicit OAuthJob(const OAuthData& input);
    
    void process(Ctl& ctl) override;
    void finalize(bool canceled, std::exception_ptr& e) override;

    void set_event_handle(wxWindow* hanle) { m_event_handle = hanle; }

    static void parse_token_response(const std::string& body, bool error, OAuthResult& result);
};

wxDECLARE_EVENT(EVT_OAUTH_COMPLETE_MESSAGE, wxCommandEvent);

}} // namespace Slic3r::GUI

#endif // OAUTHJOB_HPP
