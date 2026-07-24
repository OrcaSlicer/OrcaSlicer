#ifndef WEB_SOCKET_CLIENT_HPP_
#define WEB_SOCKET_CLIENT_HPP_
#include "libslic3r/Thread.hpp"

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/log/trivial.hpp>
#include <iostream>
#include <string>
#include <chrono>
#include <utility>
namespace beast     = boost::beast;     // from <boost/beast.hpp>
namespace http      = beast::http;      // from <boost/beast/http.hpp>
namespace websocket = beast::websocket; // from <boost/beast/websocket.hpp>
namespace net       = boost::asio;      // from <boost/asio.hpp>
using tcp           = net::ip::tcp;     // from <boost/asio/ip/tcp.hpp>

class WebSocketClient
{
public:
    WebSocketClient() : m_resolver(m_ioctx), m_ws(m_ioctx) {}

    virtual ~WebSocketClient()
    {
        if (!m_ws.is_open())
            return;
        try {
            // Close the WebSocket connection
            m_ws.close(websocket::close_code::normal);
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "Failed to close websocket gracefully: " << e.what() << std::endl;
        }
    }

    virtual void connect(std::string host, const std::string& port, const std::string& path)
    {
        if (m_ws.is_open())
            return;

        // if host last char is  '/', remove it
        if (!host.empty() && host[host.size() - 1] == '/') {
            host[host.size() - 1] = '\0';
        }

        // Look up the domain name
        const auto results = m_resolver.resolve(host, port);

        // Make the connection on the IP address we get from a lookup
        beast::get_lowest_layer(m_ws).connect(results);

        // Set a decorator to change the User-Agent of the handshake
        m_ws.set_option(
            websocket::stream_base::decorator([](websocket::request_type& req) { req.set(http::field::user_agent, SLIC3R_APP_NAME); }));

        // Perform the WebSocket handshake
        m_ws.handshake(host, path);
    }

    void close() { m_ws.close(websocket::close_code::normal); }

    void send(const std::string& message)
    {
        // Send a message
        m_ws.write(net::buffer(message));
    }

    std::string receive()
    {
        // This buffer will hold the incoming message
        beast::flat_buffer buffer;

        // Read a message into our buffer
        m_ws.read(buffer);

        // Return the message as a string
        return beast::buffers_to_string(buffer.data());
    }

    [[nodiscard]] bool is_connected() const { return m_ws.is_open(); }

protected:
    net::io_context                      m_ioctx;
    tcp::resolver                        m_resolver;
    websocket::stream<beast::tcp_stream> m_ws;
};

class AsyncWebSocketClient : public WebSocketClient
{
public:
    typedef std::function<void(const beast::error_code& /*ec*/)>                                WSOnConnectFn;
    typedef std::function<void(const beast::error_code& /*ec*/, std::size_t /*bytes_written*/)> WSOnSendFn;
    typedef std::function<void(const std::string& /*message*/, const beast::error_code& /*ec*/, std::size_t /*bytes_written*/)> WSOnReceiveFn;
    typedef std::function<void(const websocket::close_reason& /*reason*/, const bool /*client_initiated_disconnect*/)> WSOnCloseFn;

    AsyncWebSocketClient()
    {
        // Async options are implicitly thread safe in this implementation
        // All async work for the ioctx will be serialized and run only on the following thread
        m_async_thread = std::thread([this] {
            Slic3r::set_current_thread_name("Async Websocket Client");
            // keep the thread running, even if there is no work to do
            while (true) {
                auto work_guard = boost::asio::make_work_guard(m_ioctx);
                try {
                    m_ioctx.run();
                    break; // break out of loop if run finishes with no errors
                } catch (std::exception& e) {
                    // An error here likely means an error within a handler
                    BOOST_LOG_TRIVIAL(error) << "AsyncWebSocketClient runner thread exception: " << e.what();
                }
            }
        });
    }

    ~AsyncWebSocketClient() override
    {
        m_ioctx.stop();
        if (m_async_thread.joinable())
            m_async_thread.join();
    }

    void connect(std::string host, const std::string& port, const std::string& path = "/") override
    {
        WebSocketClient::connect(std::move(host), port, path);
        m_disconnect_handled = false;
    };

    void async_connect(std::string host, std::string port, std::string path = "/")
    {
        if (m_ws.is_open())
            return;
        m_connecting = true;

        // if host last char is  '/', remove it
        if (!host.empty() && host[host.size() - 1] == '/') {
            host[host.size() - 1] = '\0';
        }

        // Handle making the connection
        std::make_shared<ConnectionManager>(this, std::move(host), std::move(port), std::move(path))->start();
    }

    void async_close()
    {
        m_client_requested_disconnect = true;
        m_ws.async_close(websocket::close_code::normal, [&](const beast::error_code&) { on_close(); });
    }

    void async_send(const std::string& message)
    { m_ws.async_write(net::buffer(message), beast::bind_front_handler(&AsyncWebSocketClient::on_send, this)); }

    void async_receive() { m_ws.async_read(m_receive_buffer, beast::bind_front_handler(&AsyncWebSocketClient::on_receive, this)); }

    void set_on_connect_fn(const WSOnConnectFn& fn) { m_on_connect_fn = fn; }
    void set_on_close_fn(const WSOnCloseFn& fn) { m_on_close_fn = fn; }
    void set_on_send_fn(const WSOnSendFn& fn) { m_on_send_fn = fn; }
    void set_on_receive_fn(const WSOnReceiveFn& fn) { m_on_receive_fn = fn; }

    [[nodiscard]] bool is_connecting() const { return m_connecting; }
    [[nodiscard]] bool ready_to_connect() const { return !m_connecting && !is_connected(); }

protected:
    void on_connect(const beast::error_code& ec)
    {
        m_connecting = false;
        if (check_for_close())
            return;
        m_disconnect_handled = false;
        if (m_on_connect_fn)
            m_on_connect_fn(ec);
    }

    void on_close()
    {
        if (m_on_close_fn)
            m_on_close_fn(m_ws.reason(), m_client_requested_disconnect);
        m_client_requested_disconnect = false;
        m_disconnect_handled          = true;
    }

    void on_send(const beast::error_code& ec, const std::size_t bytes_transferred)
    {
        if (check_for_close())
            return;
        if (m_on_send_fn)
            m_on_send_fn(ec, bytes_transferred);
    }

    void on_receive(const beast::error_code& ec, const std::size_t bytes_transferred)
    {
        if (check_for_close())
            return;
        const auto message = beast::buffers_to_string(m_receive_buffer.data());
        m_receive_buffer.consume(m_receive_buffer.size());
        if (m_on_receive_fn)
            m_on_receive_fn(message, ec, bytes_transferred);
    }

    bool check_for_close()
    {
        // Determine if the server has been closed
        // Call on_close() if so
        if (!m_ws.is_open()) {
            if (!m_disconnect_handled)
                on_close();
            return true;
        }
        return false;
    }

    WSOnConnectFn      m_on_connect_fn;
    WSOnCloseFn        m_on_close_fn;
    WSOnSendFn         m_on_send_fn;
    WSOnReceiveFn      m_on_receive_fn;
    beast::flat_buffer m_receive_buffer;
    std::thread        m_async_thread;
    bool               m_connecting                  = false;
    bool               m_client_requested_disconnect = false;

    // Upon connecting, this flag is set to false
    // If an async operation completes and the socket is not connected afterward, the disconnect is handled once,
    // then this flag is set back to true until another connection is made
    bool m_disconnect_handled = true;

    // Handle the async connection process
    struct ConnectionManager : std::enable_shared_from_this<ConnectionManager>
    {
        ConnectionManager(AsyncWebSocketClient* client, std::string host, std::string port, std::string path)
            : m_client(client), m_host(std::move(host)), m_port(std::move(port)), m_path(std::move(path))
        {}

        void start()
        {
            m_client->m_resolver.async_resolve(m_host, m_port,
                                               beast::bind_front_handler(&ConnectionManager::on_resolve, shared_from_this()));
        }

        void on_resolve(const beast::error_code& ec, const tcp::resolver::results_type& result)
        {
            if (ec) {
                m_client->on_connect(ec);
                return;
            }
            beast::get_lowest_layer(m_client->m_ws)
                .async_connect(result, beast::bind_front_handler(&ConnectionManager::on_connect, shared_from_this()));
        }

        void on_connect(const beast::error_code& ec, const tcp::resolver::results_type::endpoint_type&)
        {
            if (ec) {
                m_client->on_connect(ec);
                return;
            }

            // Set a decorator to change the User-Agent of the handshake
            m_client->m_ws.set_option(
                websocket::stream_base::decorator([](websocket::request_type& req) { req.set(http::field::user_agent, SLIC3R_APP_NAME); }));

            // Perform the WebSocket handshake
            m_client->m_ws.async_handshake(m_host, m_path, beast::bind_front_handler(&ConnectionManager::on_handshake, shared_from_this()));
        }

        void on_handshake(const beast::error_code& ec) const { m_client->on_connect(ec); }

        AsyncWebSocketClient* m_client;
        std::string           m_host;
        std::string           m_port;
        std::string           m_path;
    };
};
#endif // WEB_SOCKET_CLIENT_HPP_