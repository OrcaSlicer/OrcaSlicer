#include "McpApiServer.h"

#include <iostream>
#include <sstream>
#include <boost/log/trivial.hpp>

using boost::asio::ip::tcp;

namespace Slic3r { namespace GUI {

// ---------------------------------------------------------------------------
// McpApiServer
// ---------------------------------------------------------------------------

McpApiServer::McpApiServer(int port) : m_port(port) {}

McpApiServer::~McpApiServer() { stop(); }

void McpApiServer::set_handler(request_handler_fn handler) {
    m_handler = std::move(handler);
}

void McpApiServer::start() {
    if (m_running) return;
    if (!m_handler) return;

    m_listener = std::make_unique<Listener>(m_ioc, m_port, m_handler);
    m_listener->start_accept();
    m_running = true;
    m_thread = boost::thread([this]() { run_io(); });

    BOOST_LOG_TRIVIAL(info) << "MCP API server started on port " << m_port;
}

void McpApiServer::stop() {
    if (!m_running) return;
    m_running = false;
    if (m_listener) m_listener->stop();
    m_ioc.stop();
    if (m_thread.joinable()) m_thread.join();
    BOOST_LOG_TRIVIAL(info) << "MCP API server stopped";
}

void McpApiServer::run_io() {
    try {
        m_ioc.run();
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "MCP API server error: " << e.what();
    }
}

// ---------------------------------------------------------------------------
// Listener
// ---------------------------------------------------------------------------

McpApiServer::Listener::Listener(boost::asio::io_context& ioc, int port, request_handler_fn& handler)
    : m_ioc(ioc)
    , m_acceptor(ioc, tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port))
    , m_handler(handler)
{
    m_acceptor.set_option(boost::asio::socket_base::reuse_address(true));
}

void McpApiServer::Listener::start_accept() {
    m_acceptor.async_accept(
        [this](const boost::system::error_code& ec, tcp::socket socket) {
            if (!ec) {
                auto sess = std::make_shared<Session>(std::move(socket), m_handler);
                sess->start();
            }
            if (m_acceptor.is_open()) {
                start_accept();
            }
        });
}

void McpApiServer::Listener::stop() {
    boost::system::error_code ec;
    m_acceptor.close(ec);
}

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

McpApiServer::Session::Session(tcp::socket socket, request_handler_fn& handler)
    : m_socket(std::move(socket)), m_handler(handler) {}

void McpApiServer::Session::start() {
    read_request_line();
}

void McpApiServer::Session::read_request_line() {
    auto self = shared_from_this();
    boost::asio::async_read_until(m_socket, m_buf, "\r\n",
        [this, self](const boost::system::error_code& ec, std::size_t) {
            if (ec) return;
            std::istream stream(&m_buf);
            std::string line;
            std::getline(stream, line);
            // Remove trailing \r
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            std::istringstream iss(line);
            std::string version;
            iss >> m_method >> m_url >> version;

            read_headers();
        });
}

void McpApiServer::Session::read_headers() {
    auto self = shared_from_this();
    boost::asio::async_read_until(m_socket, m_buf, "\r\n",
        [this, self](const boost::system::error_code& ec, std::size_t) {
            if (ec) return;
            std::istream stream(&m_buf);
            std::string line;
            std::getline(stream, line);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            if (line.empty()) {
                // End of headers
                auto it = m_headers.find("content-length");
                int cl = 0;
                if (it != m_headers.end()) {
                    try { cl = std::stoi(it->second); } catch (...) {}
                }
                if (cl > 0) {
                    read_body(cl);
                } else {
                    process_request("");
                }
                return;
            }

            // Parse header: "Name: Value"
            auto colon = line.find(':');
            if (colon != std::string::npos) {
                std::string name = line.substr(0, colon);
                std::string value = line.substr(colon + 1);
                // Trim leading space from value
                if (!value.empty() && value[0] == ' ')
                    value = value.substr(1);
                // Lowercase the header name for case-insensitive lookup
                std::transform(name.begin(), name.end(), name.begin(), ::tolower);
                m_headers[name] = value;
            }

            // Read next header
            read_headers();
        });
}

void McpApiServer::Session::read_body(int content_length) {
    auto self = shared_from_this();

    // Some data may already be in the buffer from header reading
    size_t already = m_buf.size();
    if ((int)already >= content_length) {
        std::istream stream(&m_buf);
        std::string body(content_length, '\0');
        stream.read(&body[0], content_length);
        process_request(body);
        return;
    }

    int remaining = content_length - (int)already;
    boost::asio::async_read(m_socket, m_buf,
        boost::asio::transfer_at_least(remaining),
        [this, self, content_length](const boost::system::error_code& ec, std::size_t) {
            if (ec && ec != boost::asio::error::eof) return;
            std::istream stream(&m_buf);
            std::string body(content_length, '\0');
            stream.read(&body[0], content_length);
            process_request(body);
        });
}

void McpApiServer::Session::process_request(const std::string& body) {
    auto self = shared_from_this();

    BOOST_LOG_TRIVIAL(debug) << "MCP API: " << m_method << " " << m_url
                             << " body=" << body.size() << " bytes";

    // Handle OPTIONS preflight
    if (m_method == "OPTIONS") {
        Response resp;
        resp.status_code = 204;
        resp.status_text = "No Content";
        resp.body = "";
        send_response(resp);
        return;
    }

    Response resp;
    try {
        resp = m_handler(m_method, m_url, body, m_headers);
    } catch (const std::exception& e) {
        resp.status_code = 500;
        resp.status_text = "Internal Server Error";
        resp.content_type = "application/json";
        resp.body = "{\"ok\":false,\"error\":\"" + std::string(e.what()) + "\"}";
    }

    send_response(resp);
}

void McpApiServer::Session::send_response(const Response& resp) {
    auto self = shared_from_this();

    std::ostringstream ss;
    ss << "HTTP/1.1 " << resp.status_code << " " << resp.status_text << "\r\n";
    ss << "Access-Control-Allow-Origin: *\r\n";
    ss << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
    ss << "Access-Control-Allow-Headers: Content-Type, Accept, Mcp-Session-Id\r\n";
    ss << "Access-Control-Expose-Headers: Mcp-Session-Id\r\n";
    ss << "Connection: close\r\n";

    // Emit extra headers (e.g., Mcp-Session-Id)
    for (const auto& [key, value] : resp.extra_headers) {
        ss << key << ": " << value << "\r\n";
    }

    if (resp.is_binary) {
        ss << "Content-Type: " << resp.content_type << "\r\n";
        ss << "Content-Length: " << resp.binary_body.size() << "\r\n";
        ss << "\r\n";
        // Write header + binary body
        auto header_str = std::make_shared<std::string>(ss.str());
        auto bin_data = std::make_shared<std::vector<unsigned char>>(resp.binary_body);
        std::vector<boost::asio::const_buffer> buffers;
        buffers.push_back(boost::asio::buffer(*header_str));
        buffers.push_back(boost::asio::buffer(*bin_data));
        boost::asio::async_write(m_socket, buffers,
            [self, header_str, bin_data](const boost::system::error_code&, std::size_t) {
                boost::system::error_code ec;
                self->m_socket.shutdown(tcp::socket::shutdown_both, ec);
            });
    } else {
        ss << "Content-Type: " << resp.content_type << "\r\n";
        ss << "Content-Length: " << resp.body.size() << "\r\n";
        ss << "\r\n";
        ss << resp.body;
        auto data = std::make_shared<std::string>(ss.str());
        boost::asio::async_write(m_socket, boost::asio::buffer(*data),
            [self, data](const boost::system::error_code&, std::size_t) {
                boost::system::error_code ec;
                self->m_socket.shutdown(tcp::socket::shutdown_both, ec);
            });
    }
}

}} // namespace Slic3r::GUI
