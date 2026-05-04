#include "HttpServer.hpp"
#include <boost/log/trivial.hpp>
#include "GUI_App.hpp"
#include "slic3r/Utils/Http.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"
#include "slic3r/Utils/BBLNetworkPlugin.hpp"

namespace Slic3r {
namespace GUI {

std::string url_get_param(const std::string& url, const std::string& key)
{
    size_t start = url.find(key);
    if (start == std::string::npos) return "";
    size_t eq = url.find('=', start);
    if (eq == std::string::npos) return "";
    std::string key_str = url.substr(start, eq - start);
    if (key_str != key)
        return "";
    start += key.size() + 1;
    size_t end = url.find('&', start);
    if (end == std::string::npos) end = url.length(); // Last param
    std::string result = url.substr(start, end - start);
    return result;
}

std::string json_string_first(const json& j, std::initializer_list<const char*> keys)
{
    for (const char* key : keys) {
        if (j.contains(key) && !j[key].is_null()) {
            if (j[key].is_string())
                return j[key].get<std::string>();
            return j[key].dump();
        }
    }
    return "";
}

json build_canonical_login_payload(const json& token_j, const json& profile_j)
{
    const std::string token = json_string_first(token_j, {"accessToken", "access_token", "token"});
    const std::string refresh_token = json_string_first(token_j, {"refreshToken", "refresh_token"});
    const std::string expires_in = json_string_first(token_j, {"expiresIn", "expires_in"});
    const std::string refresh_expires_in = json_string_first(token_j, {"refreshExpiresIn", "refresh_expires_in"});
    const std::string uid = json_string_first(profile_j, {"uidStr", "uid", "id"});
    const std::string user_name = json_string_first(profile_j, {"name"});
    const std::string account = json_string_first(profile_j, {"account"});
    const std::string avatar = json_string_first(profile_j, {"avatar"});

    json out;
    out["command"] = "user_login";
    out["data"]["token"] = token;
    out["data"]["access_token"] = token;
    out["data"]["refresh_token"] = refresh_token;
    out["data"]["expires_in"] = expires_in;
    out["data"]["refresh_expires_in"] = refresh_expires_in;
    out["data"]["user_id"] = uid;
    out["data"]["uidStr"] = uid;
    out["data"]["user"]["id"] = uid;
    out["data"]["user"]["uid"] = uid;
    out["data"]["user"]["uidStr"] = uid;
    out["data"]["user"]["name"] = user_name;
    out["data"]["user"]["account"] = account;
    out["data"]["user"]["avatar"] = avatar;
    return out;
}


std::string TicketLoginTask::perform_sync(const std::string& ticket)
{
    auto login_info = do_request_login_info(ticket);
    if (login_info.has_value())
        return login_info.value();

    return std::string();
}

void TicketLoginTask::perform_async(const std::string& ticket, std::function<void(std::string)> cb)
{
    boost::thread([cb = std::move(cb), ticket]() {
        auto login_info = do_request_login_info(ticket);
        auto result = login_info ? *login_info : std::string();
        if (wxTheApp) {
            wxTheApp->CallAfter([cb, result = std::move(result)]() mutable { cb(std::move(result)); });
        } else {
            cb(result);
        }
    }).detach();
}

std::optional<std::string> TicketLoginTask::do_request_login_info(const std::string& ticket)
{
    NetworkAgent* agent = wxGetApp().getAgent();
    if (!agent)
        return std::nullopt;

    unsigned int token_http_code = 0;
    std::string token_http_body;
    if (agent->get_my_token(ticket, &token_http_code, &token_http_body) < 0)
        return std::nullopt;

    json token_j;
    try {
        token_j = json::parse(token_http_body);
    } catch (...) {
        return std::nullopt;
    }

    const std::string access_token = json_string_first(token_j, {"accessToken", "access_token", "token"});
    if (access_token.empty())
        return std::nullopt;

    unsigned int profile_http_code = 0;
    std::string profile_http_body;
    if (agent->get_my_profile(access_token, &profile_http_code, &profile_http_body) < 0)
        return std::nullopt;

    json profile_j;
    try {
        profile_j = json::parse(profile_http_body);
    } catch (...) {
        return std::nullopt;
    }

    return build_canonical_login_payload(token_j, profile_j).dump();
}

void session::start()
{
    read_first_line();
}

void session::stop()
{
    boost::system::error_code ignored_ec;
    socket.shutdown(boost::asio::socket_base::shutdown_both, ignored_ec);
    socket.close(ignored_ec);
}

void session::read_first_line()
{
    auto self(shared_from_this());

    async_read_until(socket, buff, '\r', [this, self](const boost::beast::error_code& e, std::size_t s) {
        if (!e) {
            std::string  line, ignore;
            std::istream stream{&buff};
            std::getline(stream, line, '\r');
            std::getline(stream, ignore, '\n');
            headers.on_read_request_line(line);
            read_next_line();
        } else if (e != boost::asio::error::operation_aborted) {
            server.stop(self);
        }
    });
}

void session::read_body()
{
    auto self(shared_from_this());

    int                                nbuffer = 1000;
    std::shared_ptr<std::vector<char>> bufptr  = std::make_shared<std::vector<char>>(nbuffer);
    async_read(socket, boost::asio::buffer(*bufptr, nbuffer),
               [this, self, bufptr](const boost::beast::error_code& e, std::size_t s) { server.stop(self); });
}

void session::read_next_line()
{
    auto self(shared_from_this());

    async_read_until(socket, buff, '\r', [this, self](const boost::beast::error_code& e, std::size_t s) {
        if (!e) {
            std::string  line, ignore;
            std::istream stream{&buff};
            std::getline(stream, line, '\r');
            std::getline(stream, ignore, '\n');
            headers.on_read_header(line);

            if (line.length() == 0) {
                if (headers.content_length() == 0) {
                    std::cout << "Request received: " << headers.method << " " << headers.get_url();
                    if (headers.method == "OPTIONS") {
                        // Ignore http OPTIONS
                        server.stop(self);
                        return;
                    }

                    const std::string url_str = Http::url_decode(headers.get_url());
                    const auto        resp    = server.server.m_request_handler(url_str);
                    std::stringstream ssOut;
                    resp->write_response(ssOut);
                    std::shared_ptr<std::string> str = std::make_shared<std::string>(ssOut.str());
                    async_write(socket, boost::asio::buffer(str->c_str(), str->length()),
                                [this, self, str](const boost::beast::error_code& e, std::size_t s) {
                        std::cout << "done" << std::endl;
                        server.stop(self);
                    });
                } else {
                    read_body();
                }
            } else {
                read_next_line();
            }
        } else if (e != boost::asio::error::operation_aborted) {
            server.stop(self);
        }
    });
}

void HttpServer::IOServer::do_accept()
{
    acceptor.async_accept([this](boost::system::error_code ec, boost::asio::ip::tcp::socket socket) {
        if (!acceptor.is_open()) {
            return;
        }

        if (!ec) {
            const auto ss = std::make_shared<session>(*this, std::move(socket));
            start(ss);
        }

        do_accept();
    });
}

void HttpServer::IOServer::start(std::shared_ptr<session> session)
{
    sessions.insert(session);
    session->start();
}

void HttpServer::IOServer::stop(std::shared_ptr<session> session)
{
    sessions.erase(session);
    session->stop();
}

void HttpServer::IOServer::stop_all()
{
    for (auto s : sessions) {
        s->stop();
    }
    sessions.clear();
}


HttpServer::HttpServer(boost::asio::ip::port_type port) : port(port) {}

HttpServer::~HttpServer()
{
    stop();
}

void HttpServer::start()
{
    if (start_http_server)
        return;

    BOOST_LOG_TRIVIAL(info) << "start_http_service...";
    server_             = std::make_unique<IOServer>(*this);
    IOServer* io_server = server_.get();
    start_http_server   = true;
    m_http_server_thread = create_thread([io_server] {
        set_current_thread_name("http_server");
        io_server->acceptor.listen();

        io_server->do_accept();

        io_server->io_service.run();
    });
}

void HttpServer::stop()
{
    start_http_server = false;
    if (server_) {
        IOServer* io_server = server_.get();
        boost::asio::post(io_server->io_service, [io_server] {
            boost::system::error_code ec;
            io_server->acceptor.cancel(ec);
            io_server->acceptor.close(ec);
            io_server->stop_all();
            io_server->io_service.stop();
        });
    }
    if (m_http_server_thread.joinable())
        m_http_server_thread.join();
    server_.reset();
}

void HttpServer::set_request_handler(const std::function<std::shared_ptr<Response>(const std::string&)>& request_handler)
{
    this->m_request_handler = request_handler;
}

std::shared_ptr<HttpServer::Response> HttpServer::bbl_auth_handle_request(const std::string& url)
{
    return auth_handle_request(url, BBL_CLOUD_PROVIDER);
}

std::shared_ptr<HttpServer::Response> HttpServer::auth_handle_request(const std::string& url, const std::string& provider)
{
    BOOST_LOG_TRIVIAL(info) << "thirdparty_login: get_response";

    NetworkAgent* agent = wxGetApp().getAgent();
    if (!agent)
        return std::make_shared<ResponseNotFound>();

    const std::string ticket = url_get_param(url, "ticket");
    if (!ticket.empty()) {
        unsigned int token_http_code = 0;
        std::string token_http_body;
        const int token_result = agent->get_my_token(ticket, &token_http_code, &token_http_body);
        BOOST_LOG_TRIVIAL(info) << "thirdparty_login ticket exchange result=" << token_result << ", http_code=" << token_http_code;

        json token_j;
        json profile_j;
        std::string access_token;

        if (token_result == 0) {
            try {
                token_j = json::parse(token_http_body);
                access_token = json_string_first(token_j, {"accessToken", "access_token", "token"});
            } catch (...) {}
        }

        unsigned int profile_http_code = 0;
        std::string profile_http_body;
        const int profile_result = access_token.empty() ? -1 : agent->get_my_profile(access_token, &profile_http_code, &profile_http_body);
        BOOST_LOG_TRIVIAL(info) << "thirdparty_login profile result=" << profile_result << ", http_code=" << profile_http_code;

        if (token_result == 0 && profile_result == 0) {
            try {
                profile_j = json::parse(profile_http_body);
            } catch (...) {}

            const json payload = build_canonical_login_payload(token_j, profile_j);
            agent->change_user(payload.dump());
            const bool login_ok = agent->is_user_login();
            if (login_ok) {
                wxGetApp().request_user_login(1);
                GUI::wxGetApp().CallAfter([] { wxGetApp().ShowUserLogin(false); });
            }

            const std::string title = login_ok ? "Authentication complete" : "Authentication failed";
            const std::string message = login_ok
                ? "You can return to OrcaSlicer. This window will close automatically."
                : "Something went wrong. Please return to OrcaSlicer and try again.";
            const std::string html =
                "<html><head><meta charset=\"utf-8\">"
                "<style>body{font-family:Arial,sans-serif;background:#f7f7f7;color:#222;margin:32px;}"
                "a.button{display:inline-block;padding:10px 16px;margin-top:12px;background:#0f8bff;color:#fff;text-decoration:none;border-radius:6px;}"
                "</style></head><body><div class=\"container\">"
                "<h2>" + title + "</h2>"
                "<p>" + message + "</p>"
                "<script>setTimeout(function(){try{window.close();}catch(e){}},1500);</script>"
                "</div></body></html>";
            return std::make_shared<ResponseHtml>(html);
        }

        const std::string html =
            "<html><head><meta charset=\"utf-8\"></head><body>"
            "<h2>Authentication failed</h2>"
            "<p>Something went wrong. Please return to OrcaSlicer and try again.</p>"
            "</body></html>";
        return std::make_shared<ResponseHtml>(html);
    }

    const std::string auth_code = url_get_param(url, "code");
    if (!auth_code.empty()) {
        std::string state = url_get_param(url, "state");
        json payload;
        payload["command"] = "user_login";
        payload["data"]["code"] = auth_code;
        payload["data"]["state"] = state;

        agent->change_user(payload.dump(), provider);
        const bool login_ok = agent->is_user_login(provider);
        if (login_ok) {
            wxGetApp().request_user_login(1, provider);
            GUI::wxGetApp().CallAfter([] { wxGetApp().ShowUserLogin(false); });
        }

        const std::string title = login_ok ? "Authentication complete" : "Authentication failed";
        const std::string message = login_ok
            ? "You can return to OrcaSlicer. This window will close automatically."
            : "Something went wrong. Please return to OrcaSlicer and try again.";
        const std::string html =
            "<html><head><meta charset=\"utf-8\">"
            "<style>body{font-family:Arial,sans-serif;background:#f7f7f7;color:#222;margin:32px;}"
            "a.button{display:inline-block;padding:10px 16px;margin-top:12px;background:#0f8bff;color:#fff;text-decoration:none;border-radius:6px;}"
            "</style></head><body><div class=\"container\">"
            "<h2>" + title + "</h2>"
            "<p>" + message + "</p>"
            "<script>setTimeout(function(){try{window.close();}catch(e){}},1500);</script>"
            "</div></body></html>";
        return std::make_shared<ResponseHtml>(html);
    }

    if (boost::contains(url, "access_token")) {
        std::string   redirect_url           = url_get_param(url, "redirect_url");
        std::string   access_token           = url_get_param(url, "access_token");
        std::string   refresh_token          = url_get_param(url, "refresh_token");
        std::string   expires_in_str         = url_get_param(url, "expires_in");
        std::string   refresh_expires_in_str = url_get_param(url, "refresh_expires_in");

        unsigned int http_code;
        std::string  http_body;
        int          result = agent->get_my_profile(access_token, &http_code, &http_body, provider);
        if (result == 0) {
            json profile_j;
            try {
                profile_j = json::parse(http_body);
            } catch (...) {}
            json token_j;
            token_j["accessToken"] = access_token;
            token_j["refreshToken"] = refresh_token;
            token_j["expiresIn"] = expires_in_str;
            token_j["refreshExpiresIn"] = refresh_expires_in_str;
            const json j = build_canonical_login_payload(token_j, profile_j);
            agent->change_user(j.dump());
            if (agent->is_user_login()) {
                wxGetApp().request_user_login(1);
            }
            GUI::wxGetApp().CallAfter([] { wxGetApp().ShowUserLogin(false); });
            std::string location_str = (boost::format("%1%?result=success") % redirect_url).str();
            return std::make_shared<ResponseRedirect>(location_str);
        }

        std::string error_str    = "get_user_profile_error_" + std::to_string(result);
        std::string location_str = (boost::format("%1%?result=fail&error=%2%") % redirect_url % error_str).str();
        return std::make_shared<ResponseRedirect>(location_str);
    }

    return std::make_shared<ResponseNotFound>();
}

void HttpServer::ResponseNotFound::write_response(std::stringstream& ssOut)
{
    const std::string sHTML = "<html><body><h1>404 Not Found</h1><p>There's nothing here.</p></body></html>";
    ssOut << "HTTP/1.1 404 Not Found" << std::endl;
    ssOut << "content-type: text/html" << std::endl;
    ssOut << "content-length: " << sHTML.length() << std::endl;
    ssOut << std::endl;
    ssOut << sHTML;
}

void HttpServer::ResponseRedirect::write_response(std::stringstream& ssOut)
{
    const std::string sHTML =
        "<html><head><meta charset=\"utf-8\">"
        "<meta http-equiv=\"refresh\" content=\"0;url=" + location_str + "\">"
        "<style>body{font-family:Arial,sans-serif;background:#f7f7f7;color:#222;margin:32px;}"
        "a.button{display:inline-block;padding:10px 16px;margin-top:12px;background:#0f8bff;color:#fff;text-decoration:none;border-radius:6px;}"
        "</style></head><body><div class=\"container\">"
        "<h2>Authentication complete</h2>"
        "<p>You can return to OrcaSlicer. If your browser does not redirect automatically, use the button below.</p>"
        "<a class=\"button\" href=\"" + location_str + "\">Continue</a>"
        "<script>setTimeout(function(){try{window.close();}catch(e){}},1500);</script>"
        "</div></body></html>";
    ssOut << "HTTP/1.1 302 Found" << std::endl;
    ssOut << "Location: " << location_str << std::endl;
    ssOut << "content-type: text/html" << std::endl;
    ssOut << "content-length: " << sHTML.length() << std::endl;
    ssOut << std::endl;
    ssOut << sHTML;
}

void HttpServer::ResponseHtml::write_response(std::stringstream& ssOut)
{
    ssOut << "HTTP/1.1 200 OK" << std::endl;
    ssOut << "content-type: text/html" << std::endl;
    ssOut << "content-length: " << html.length() << std::endl;
    ssOut << std::endl;
    ssOut << html;
}

} // GUI
} //Slic3r
