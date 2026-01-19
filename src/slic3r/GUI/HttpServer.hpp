#ifndef slic3r_Http_App_hpp_
#define slic3r_Http_App_hpp_

#include <iostream>
#include <mutex>
#include <stack>

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio.hpp>
#include <boost/thread.hpp>
#include <string>
#include <set>
#include <memory>

#define LOCALHOST_PORT      13618
#define LOCALHOST_URL       "http://localhost:"

namespace Slic3r { namespace GUI {

class session;

class http_headers
{
    std::string method;
    std::string url;
    std::string version;

    std::map<std::string, std::string> headers;

    friend class session;
public:
    std::string get_url() { return url; }

    int content_length()
    {
        auto request = headers.find("content-length");
        if (request != headers.end()) {
            std::stringstream ssLength(request->second);
            int               content_length;
            ssLength >> content_length;
            return content_length;
        }
        return 0;
    }

    void on_read_header(std::string line)
    {
        // std::cout << "header: " << line << std::endl;

        std::stringstream ssHeader(line);
        std::string       headerName;
        std::getline(ssHeader, headerName, ':');

        std::string value;
        std::getline(ssHeader, value);
        headers[headerName] = value;
    }

    void on_read_request_line(std::string line)
    {
        std::stringstream ssRequestLine(line);
        ssRequestLine >> method;
        ssRequestLine >> url;
        ssRequestLine >> version;

        std::cout << "request for resource: " << url << std::endl;
    }
};

class HttpServer
{
    boost::asio::ip::port_type port;

public:
    class Response
    {
    public:
        virtual ~Response()                                   = default;
        virtual void write_response(std::stringstream& ssOut) = 0;
    };

    class ResponseNotFound : public Response
    {
    public:
        ~ResponseNotFound() override = default;
        void write_response(std::stringstream& ssOut) override;
    };

    class ResponseRedirect : public Response
    {
        const std::string location_str;

    public:
        ResponseRedirect(const std::string& location) : location_str(location) {}
        ~ResponseRedirect() override = default;
        void write_response(std::stringstream& ssOut) override;
    };

    HttpServer(boost::asio::ip::port_type port = LOCALHOST_PORT);

    boost::thread m_http_server_thread;
    bool          start_http_server = false;

    bool is_started() { return start_http_server; }
    void start();
    void stop();
    void set_request_handler(const std::function<std::shared_ptr<Response>(const std::string&)>& m_request_handler);

    static std::shared_ptr<Response> bbl_auth_handle_request(const std::string& url);

private:
    class IOServer
    {
    public:
        HttpServer&                        server;
        boost::asio::io_service            io_service;
        boost::asio::ip::tcp::acceptor     acceptor;
        std::set<std::shared_ptr<session>> sessions;

        IOServer(HttpServer& server) : server(server), acceptor(io_service, {boost::asio::ip::tcp::v4(), server.port}) {}

        void do_accept();

        void start(std::shared_ptr<session> session);
        void stop(std::shared_ptr<session> session);
        void stop_all();
    };
    friend class session;

    std::unique_ptr<IOServer> server_{nullptr};

    std::function<std::shared_ptr<Response>(const std::string&)> m_request_handler{&HttpServer::bbl_auth_handle_request};
};

class session : public std::enable_shared_from_this<session>
{
    HttpServer::IOServer& server;
    boost::asio::ip::tcp::socket socket;

    boost::asio::streambuf buff;
    http_headers headers;

    void read_first_line();
    void read_next_line();
    void read_body();

public:
    session(HttpServer::IOServer& server, boost::asio::ip::tcp::socket socket) : server(server), socket(std::move(socket)) {}

    void start();
    void stop();
};

std::string url_get_param(const std::string& url, const std::string& key);

}};

#endif
