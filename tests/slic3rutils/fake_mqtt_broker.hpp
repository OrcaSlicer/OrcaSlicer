#pragma once

#include "slic3r/Utils/MqttWebSocketClient.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/websocket/rfc6455.hpp>
#include <boost/beast/websocket/stream.hpp>
#include <boost/beast/websocket/stream_base.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace mqtt_test {
namespace net       = boost::asio;
namespace beast     = boost::beast;
namespace http      = beast::http;
namespace websocket = beast::websocket;
using tcp           = net::ip::tcp;

class FakeMqttBroker
{
public:
    class Session
    {
    public:
        Slic3r::Mqtt::Packet read_packet()
        {
            for (;;) {
                if (auto packet = m_decoder.next()) {
                    if (auto publish = Slic3r::Mqtt::parse_publish(*packet)) {
                        std::lock_guard<std::mutex> lock(m_broker.m_mutex);
                        m_broker.m_topics.push_back(publish->topic);
                    }
                    return std::move(*packet);
                }
                if (m_decoder.failed())
                    throw std::runtime_error("Invalid MQTT packet");
                beast::flat_buffer message;
                m_ws.read(message);
                if (!m_ws.got_binary())
                    throw std::runtime_error("Expected a binary WebSocket message");
                m_decoder.feed(static_cast<const std::uint8_t*>(message.data().data()), message.size());
            }
        }

        void send(const std::vector<std::uint8_t>& bytes) { m_ws.write(net::buffer(bytes)); }
        void close() { m_ws.close(websocket::close_code::normal); }
        const std::string& subprotocol() const { return m_subprotocol; }
        void stall() { m_broker.stall(); }

    private:
        friend class FakeMqttBroker;
        Session(FakeMqttBroker& broker, tcp::socket socket) : m_broker(broker), m_ws(std::move(socket)) {}

        void handshake()
        {
            beast::flat_buffer buffer;
            http::request<http::empty_body> request;
            http::read(m_ws.next_layer(), buffer, request);
            m_subprotocol = std::string(request[http::field::sec_websocket_protocol]);
            m_ws.set_option(websocket::stream_base::decorator(
                [](websocket::response_type& response) { response.set(http::field::sec_websocket_protocol, "mqtt"); }));
            m_ws.accept(request);
            m_ws.binary(true);
        }

        FakeMqttBroker& m_broker;
        websocket::stream<tcp::socket> m_ws;
        Slic3r::Mqtt::PacketDecoder m_decoder;
        std::string m_subprotocol;
    };

    enum class Handshake { Accept, Stall };

    explicit FakeMqttBroker(std::function<void(Session&)> script, Handshake handshake = Handshake::Accept)
        : m_thread([this, script = std::move(script), handshake] {
            try {
                // Accepted on the broker's own context, so join() and the destructor can cancel it when no client comes.
                tcp::socket socket{m_ioc};
                beast::error_code accepted;
                m_acceptor.async_accept(socket, [&](beast::error_code ec) { accepted = ec; });
                m_ioc.run();
                if (accepted || m_stopping)
                    return;
                Session session{*this, std::move(socket)};
                if (handshake == Handshake::Stall) {
                    stall();
                    return;
                }
                session.handshake();
                script(session);
            } catch (...) {
                m_error = std::current_exception();
            }
        })
    {}

    ~FakeMqttBroker()
    {
        if (m_thread.joinable()) {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_stopping = true;
            }
            m_changed.notify_all();
            stop_accepting();
            m_thread.join();
        }
    }

    std::string port() const { return std::to_string(m_endpoint.port()); }

    std::vector<std::string> published_topics() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_topics;
    }

    // Waits for the script, or stops waiting for a client that never came. Rethrows what the script threw.
    void join()
    {
        stop_accepting();
        if (m_thread.joinable())
            m_thread.join();
        if (m_error)
            std::rethrow_exception(m_error);
    }

private:
    // A no-op once the client was accepted: the context has then run out of work and never runs the handler.
    void stop_accepting()
    {
        net::post(m_ioc, [this] {
            beast::error_code ignored;
            m_acceptor.close(ignored);
        });
    }

    void stall()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_changed.wait_for(lock, std::chrono::seconds(2), [this] { return m_stopping.load(); });
    }

    net::io_context m_ioc;
    tcp::acceptor m_acceptor{m_ioc, {net::ip::make_address("127.0.0.1"), 0}};
    const tcp::endpoint m_endpoint = m_acceptor.local_endpoint();
    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    std::atomic<bool> m_stopping{false};
    std::vector<std::string> m_topics;
    std::exception_ptr m_error;
    std::thread m_thread;
};

} // namespace mqtt_test
