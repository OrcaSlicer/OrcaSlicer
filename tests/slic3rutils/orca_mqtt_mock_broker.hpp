#pragma once

// In-process plaintext MQTT-over-WebSocket broker for the OrcaMqtt tests.
//
// It speaks just enough of MQTT 3.1.1 to drive OrcaMqttConnection /
// OrcaPrinterAgent end to end without a real network: CONNECT/CONNACK,
// SUBSCRIBE/SUBACK, UNSUBSCRIBE/UNSUBACK, client PUBLISH (QoS 0), PINGREQ and
// DISCONNECT. The outbound PUBLISH frame is built with the production
// OrcaMqttConnection::make_publish_packet() so the tests never depend on a
// second, hand-rolled MQTT encoder.

#include <slic3r/Utils/OrcaMqttConnection.hpp>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace orca_mqtt_test {

namespace net   = boost::asio;
namespace beast = boost::beast;
namespace ws    = boost::beast::websocket;
using tcp       = boost::asio::ip::tcp;

// Decode an MQTT remaining-length varint starting at packet[offset].
// Returns {value, bytes_consumed}; bytes_consumed == 0 means malformed.
inline std::pair<std::size_t, std::size_t> mqtt_decode_remaining_length(const std::string& packet, std::size_t offset)
{
    std::size_t value      = 0;
    std::size_t multiplier = 1;
    std::size_t used       = 0;
    while (offset + used < packet.size() && used < 4) {
        const std::uint8_t byte = static_cast<std::uint8_t>(packet[offset + used]);
        value += static_cast<std::size_t>(byte & 0x7f) * multiplier;
        multiplier *= 128;
        ++used;
        if ((byte & 0x80) == 0)
            return {value, used};
    }
    return {0, 0};
}

inline bool mqtt_topic_is_request(const std::string& topic)
{
    static const std::string suffix = "/request";
    return topic.size() >= suffix.size() &&
           topic.compare(topic.size() - suffix.size(), suffix.size(), suffix) == 0;
}

class MockBroker
{
public:
    // refuse_auth: answer every CONNECT with CONNACK rc 5 (not authorized) and
    // close, so the reconnect/refusal paths can be exercised.
    explicit MockBroker(bool refuse_auth = false) : m_refuse_auth(refuse_auth), m_acceptor(m_io)
    {
        const tcp::endpoint endpoint(net::ip::make_address("127.0.0.1"), 0);
        m_acceptor.open(endpoint.protocol());
        m_acceptor.set_option(net::socket_base::reuse_address(true));
        m_acceptor.bind(endpoint);
        m_acceptor.listen(net::socket_base::max_listen_connections);
        m_port = std::to_string(m_acceptor.local_endpoint().port());
        // why: a non-blocking acceptor lets the accept loop poll a stop flag, so
        // the destructor never has to interrupt a blocking accept().
        m_acceptor.non_blocking(true);
        m_thread = std::thread([this] { run(); });
    }

    ~MockBroker()
    {
        m_stopping.store(true);
        drop_client();                     // unblocks the worker's blocking read
        if (m_thread.joinable())
            m_thread.join();
        boost::system::error_code ec;
        m_acceptor.close(ec);              // after join: the acceptor is worker-owned
        m_io.stop();
    }

    MockBroker(const MockBroker&)            = delete;
    MockBroker& operator=(const MockBroker&) = delete;

    std::string ws_url() const { return "ws://127.0.0.1:" + m_port + "/mqtt"; }

    std::pair<std::string, std::string> host_port() const { return {std::string("127.0.0.1"), m_port}; }

    // Server -> client PUBLISH on device/<dev_id>/report.
    void push_report(const std::string& dev_id, const std::string& payload)
    {
        const std::vector<std::uint8_t> packet =
            Slic3r::OrcaMqttConnection::make_publish_packet("device/" + dev_id + "/report", payload);
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_stream || !m_stream_ready)
            return;
        boost::system::error_code ec;
        m_stream->binary(true);
        m_stream->write(net::buffer(packet), ec); // a vanished client is not a test failure
    }

    // Force-close the live client socket; the worker's read returns an error and
    // the accept loop picks up the client's reconnect.
    void drop_client()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        close_client_locked();
    }

    // Payloads the client PUBLISHed to any device/<id>/request topic.
    std::vector<std::string> received_requests() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_received_requests;
    }

    // MQTT CONNECTs seen; increments again after a reconnect.
    int connect_count() const { return m_connect_count.load(); }

private:
    void run()
    {
        try {
            while (!m_stopping.load()) {
                tcp::socket               socket(m_io);
                boost::system::error_code ec;
                m_acceptor.accept(socket, ec);
                if (ec == net::error::would_block || ec == net::error::try_again) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }
                if (ec)
                    return;
                try {
                    serve(std::move(socket));
                } catch (...) {
                    // a client dying mid-session must not take the broker down
                }
                std::lock_guard<std::mutex> lock(m_mutex);
                close_client_locked();
                m_stream.reset();
            }
        } catch (...) {
            // never let an exception escape the broker thread
        }
    }

    void serve(tcp::socket socket)
    {
        ws::stream<beast::tcp_stream>* stream = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stream.emplace(std::move(socket));
            m_stream_ready = false;
            stream         = &*m_stream;
        }
        // why: no io_context is ever run here, so a tcp_stream timer would never
        // fire; the sync operations below carry no timeout of their own.
        beast::get_lowest_layer(*stream).expires_never();
        stream->set_option(ws::stream_base::decorator(
            [](ws::response_type& res) { res.set("Sec-WebSocket-Protocol", "mqtt"); }));

        boost::system::error_code ec;
        stream->accept(ec);
        if (ec)
            return;
        stream->binary(true);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stream_ready = true;
        }
        read_loop(*stream);
    }

    // The client sends every MQTT packet as one binary WebSocket message, so one
    // read yields exactly one packet.
    void read_loop(ws::stream<beast::tcp_stream>& stream)
    {
        beast::flat_buffer buffer;
        while (!m_stopping.load()) {
            boost::system::error_code ec;
            buffer.clear();
            stream.read(buffer, ec);
            if (ec)
                return;
            const std::string packet = beast::buffers_to_string(buffer.data());
            if (packet.empty())
                continue;
            if (!handle_packet(stream, packet))
                return;
        }
    }

    // Returns false when the session must be closed.
    bool handle_packet(ws::stream<beast::tcp_stream>& stream, const std::string& packet)
    {
        switch (static_cast<std::uint8_t>(packet[0]) & 0xf0) {
        case 0x10: { // CONNECT
            ++m_connect_count;
            if (m_refuse_auth) {
                write_packet(stream, {0x20, 0x02, 0x00, 0x05}); // CONNACK not authorized
                return false;
            }
            write_packet(stream, {0x20, 0x02, 0x00, 0x00}); // CONNACK accepted
            return true;
        }
        case 0x80: { // SUBSCRIBE (0x82) - packet id follows the remaining-length varint
            const auto id = packet_id(packet);
            if (id)
                write_packet(stream, {0x90, 0x03, id->first, id->second, 0x00}); // SUBACK, QoS 0
            return true;
        }
        case 0xa0: { // UNSUBSCRIBE (0xa2)
            const auto id = packet_id(packet);
            if (id)
                write_packet(stream, {0xb0, 0x02, id->first, id->second}); // UNSUBACK
            return true;
        }
        case 0x30: { // PUBLISH, QoS 0 (no packet identifier)
            record_publish(packet);
            return true;
        }
        case 0xc0: // PINGREQ
            write_packet(stream, {0xd0, 0x00});
            return true;
        case 0xe0: // DISCONNECT
            return false;
        default:
            return true;
        }
    }

    // The two packet-identifier bytes sitting right after the remaining-length varint.
    static std::optional<std::pair<std::uint8_t, std::uint8_t>> packet_id(const std::string& packet)
    {
        const auto varint = mqtt_decode_remaining_length(packet, 1);
        if (varint.second == 0)
            return std::nullopt;
        const std::size_t pos = 1 + varint.second;
        if (pos + 2 > packet.size())
            return std::nullopt;
        return std::make_pair(static_cast<std::uint8_t>(packet[pos]), static_cast<std::uint8_t>(packet[pos + 1]));
    }

    void record_publish(const std::string& packet)
    {
        const auto varint = mqtt_decode_remaining_length(packet, 1);
        if (varint.second == 0)
            return;
        std::size_t pos = 1 + varint.second;
        if (pos + 2 > packet.size())
            return;
        const std::size_t topic_len = (static_cast<std::size_t>(static_cast<std::uint8_t>(packet[pos])) << 8) |
                                      static_cast<std::uint8_t>(packet[pos + 1]);
        pos += 2;
        if (pos + topic_len > packet.size())
            return;
        const std::string topic = packet.substr(pos, topic_len);
        pos += topic_len;
        const std::size_t end = std::min(packet.size(), 1 + varint.second + varint.first);
        if (end < pos)
            return;
        if (!mqtt_topic_is_request(topic))
            return;
        std::lock_guard<std::mutex> lock(m_mutex);
        m_received_requests.push_back(packet.substr(pos, end - pos));
    }

    // Every write - the worker's own replies and push_report() from the test
    // thread - is serialised by m_mutex. beast permits a writer while the worker
    // is blocked in read(), which is the same arrangement OrcaMqttConnection uses.
    void write_packet(ws::stream<beast::tcp_stream>& stream, const std::vector<std::uint8_t>& packet)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        boost::system::error_code   ec;
        stream.binary(true);
        stream.write(net::buffer(packet), ec);
    }

    void close_client_locked()
    {
        if (!m_stream)
            return;
        m_stream_ready = false;
        boost::system::error_code ec;
        auto&                     socket = beast::get_lowest_layer(*m_stream).socket();
        socket.cancel(ec);
        // shutdown() before close() is what actually wakes a blocking read on the
        // worker thread; close() alone does not on POSIX.
        socket.shutdown(tcp::socket::shutdown_both, ec);
        socket.close(ec);
    }

    const bool                                   m_refuse_auth;
    net::io_context                              m_io;
    tcp::acceptor                                m_acceptor;
    std::string                                  m_port;
    std::thread                                  m_thread;
    std::atomic_bool                             m_stopping{false};
    std::atomic<int>                             m_connect_count{0};
    mutable std::mutex                           m_mutex;
    std::optional<ws::stream<beast::tcp_stream>> m_stream;      // guarded by m_mutex
    bool                                         m_stream_ready = false; // guarded by m_mutex
    std::vector<std::string>                     m_received_requests;    // guarded by m_mutex
};

} // namespace orca_mqtt_test
