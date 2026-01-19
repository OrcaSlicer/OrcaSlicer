#include "HttpServer.hpp"
#include <boost/log/trivial.hpp>
#include "GUI_App.hpp"
#include "slic3r/Utils/Http.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"

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
               [this, self](const boost::beast::error_code& e, std::size_t s) { server.stop(self); });
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

void HttpServer::start()
{
    BOOST_LOG_TRIVIAL(info) << "start_http_service...";
    start_http_server    = true;
    m_http_server_thread = create_thread([this] {
        set_current_thread_name("http_server");
        server_ = std::make_unique<IOServer>(*this);
        server_->acceptor.listen();

        server_->do_accept();

        server_->io_service.run();
    });
}

void HttpServer::stop()
{
    start_http_server = false;
    if (server_) {
        server_->acceptor.close();
        server_->stop_all();
        server_->io_service.stop();
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
    BOOST_LOG_TRIVIAL(info) << "thirdparty_login: get_response";

    if (boost::contains(url, "access_token")) {
        std::string   redirect_url           = url_get_param(url, "redirect_url");
        std::string   access_token           = url_get_param(url, "access_token");
        std::string   refresh_token          = url_get_param(url, "refresh_token");
        std::string   expires_in_str         = url_get_param(url, "expires_in");
        std::string   refresh_expires_in_str = url_get_param(url, "refresh_expires_in");
        NetworkAgent* agent                  = wxGetApp().getAgent();

        unsigned int http_code;
        std::string  http_body;
        int          result = agent->get_my_profile(access_token, &http_code, &http_body);
        if (result == 0) {
            std::string user_id;
            std::string user_name;
            std::string user_account;
            std::string user_avatar;
            try {
                json user_j = json::parse(http_body);
                if (user_j.contains("uidStr"))
                    user_id = user_j["uidStr"].get<std::string>();
                if (user_j.contains("name"))
                    user_name = user_j["name"].get<std::string>();
                if (user_j.contains("avatar"))
                    user_avatar = user_j["avatar"].get<std::string>();
                if (user_j.contains("account"))
                    user_account = user_j["account"].get<std::string>();
            } catch (...) {
                ;
            }
            json j;
            j["data"]["refresh_token"]      = refresh_token;
            j["data"]["token"]              = access_token;
            j["data"]["expires_in"]         = expires_in_str;
            j["data"]["refresh_expires_in"] = refresh_expires_in_str;
            j["data"]["user"]["uid"]        = user_id;
            j["data"]["user"]["name"]       = user_name;
            j["data"]["user"]["account"]    = user_account;
            j["data"]["user"]["avatar"]     = user_avatar;
            agent->change_user(j.dump());
            if (agent->is_user_login()) {
                wxGetApp().request_user_login(1);
            }
            GUI::wxGetApp().CallAfter([] { wxGetApp().ShowUserLogin(false); });
            std::string location_str = (boost::format("%1%?result=success") % redirect_url).str();
            return std::make_shared<ResponseRedirect>(location_str);
        } else {
            std::string error_str    = "get_user_profile_error_" + std::to_string(result);
            std::string location_str = (boost::format("%1%?result=fail&error=%2%") % redirect_url % error_str).str();
            return std::make_shared<ResponseRedirect>(location_str);
        }
    } else {
        return std::make_shared<ResponseNotFound>();
    }
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
    const std::string sHTML = "<html><body><p>redirect to url </p></body></html>";
    ssOut << "HTTP/1.1 302 Found" << std::endl;
    ssOut << "Location: " << location_str << std::endl;
    ssOut << "content-type: text/html" << std::endl;
    ssOut << "content-length: " << sHTML.length() << std::endl;
    ssOut << std::endl;
    ssOut << sHTML;
}

} // GUI
} //Slic3r
